#include "common/config.h"
#include "common/ipc.h"
#include "engine/engine.h"
#include "tray.h"
#include <imm.h>
#include <atomic>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace {
using nlohmann::json;
std::atomic_bool stopping{false};
std::atomic_int clients{0};
std::mutex state_mutex;
std::mutex config_mutex;
std::shared_ptr<euphony::Engine> engine;
std::unique_ptr<euphony::ConfigStore> config;
std::uint64_t config_epoch = 1;
euphony::BrokerTray* active_tray = nullptr;
struct Route {
    std::shared_ptr<euphony::Engine> engine;
    std::uint64_t epoch;
    json preferences;
    bool composing = false;
};
std::unordered_map<std::string, Route> routes;

json dispatch(const json& request, std::vector<std::shared_ptr<euphony::Engine>>& retired) {
    if (request.value("version", 0) != 1) throw std::runtime_error("Unsupported IPC version");
    const auto op = request.value("op", "");
    if (op == "set_mode") {
        if (!request.contains("chinese") || !request["chinese"].is_boolean()) throw std::runtime_error("Invalid input mode");
        if (active_tray) active_tray->set_mode(request.at("chinese").get<bool>());
        return {{"ok",true},{"version",1}};
    }
    if (op == "shutdown") {
        stopping.store(true);
        return {{"ok", true}, {"version", 1}};
    }
    if (op == "get_config") {
        std::lock_guard lock(state_mutex);
        auto status = engine->handle({{"version", 1}, {"op", "status"}});
        status["app_version"] = EUPHONY_VERSION;
        status["executable_directory"] = euphony::utf8(euphony::module_directory().wstring());
        status["pid"] = GetCurrentProcessId();
        if (!config->load_error().empty()) status["config_error"] = config->load_error();
        return {{"ok", true}, {"version", 1}, {"config", config->get()}, {"status", status}, {"config_epoch", config_epoch}};
    }
    if (op == "set_config") {
        std::lock_guard transaction(config_mutex);
        json next;
        {
            std::lock_guard lock(state_mutex);
            next = config->prepare(request.at("config"));
            if (next == config->get()) {
                if (!config->load_error().empty()) config->save(next);
                return {{"ok", true}, {"version", 1}, {"config", next},
                    {"status", engine->handle({{"version", 1}, {"op", "status"}})}, {"config_epoch", config_epoch}};
            }
        }
        auto replacement = std::make_shared<euphony::Engine>(next);
        json response;
        {
            std::lock_guard lock(state_mutex);
            config->save(next);
            engine.swap(replacement);
            ++config_epoch;
            response = {{"ok", true}, {"version", 1}, {"config", next}, {"status", engine->handle({{"version", 1}, {"op", "status"}})}, {"config_epoch", config_epoch}};
        }
        // Cancel/free the old worker without blocking keystrokes on the new engine.
        retired.push_back(std::move(replacement));
        return response;
    }
    if (op != "key" && op != "poll" && op != "reset" && op != "close" && op != "status")
        throw std::runtime_error("Unknown request operation");
    if (op == "status") {
        std::lock_guard lock(state_mutex);
        return engine->handle(request);
    }
    if (op != "status") {
        const auto session = request.value("session", "");
        if (session.empty() || session.size() > 128) throw std::runtime_error("Invalid session");
        if (!request.contains("seq") || !request["seq"].is_number_unsigned()) throw std::runtime_error("Invalid request sequence");
        if (request.value("surrounding", "").size() > 65536) throw std::runtime_error("Surrounding text is too long");
        if (op == "key" && request.value("key", "").size() > 32) throw std::runtime_error("Invalid key");
    }
    // Keep each active composition on the configuration with which it began.
    // Settings take effect at the next empty composition rather than losing keys.
    std::shared_ptr<euphony::Engine> selected;
    std::unique_lock lock(state_mutex);
    const auto session = request.value("session", "");
    auto route = routes.find(session);
    if (route != routes.end() && route->second.engine != engine && !route->second.composing) {
        retired.push_back(route->second.engine);
        route->second.engine->handle({{"version", 1}, {"op", "close"}, {"session", session},
            {"seq", request.value("seq", std::uint64_t{})}});
        routes.erase(route);
        route = routes.end();
    }
    if (route == routes.end()) {
        if (routes.size() >= 256) throw std::runtime_error("Too many active input sessions");
        route = routes.emplace(session, Route{engine, config_epoch, config->get()}).first;
    }
    selected = route->second.engine;
    auto response = selected->handle(request);
    route->second.composing = !response.value("preedit", "").empty();
    response["version"] = 1;
    response["config_epoch"] = route->second.epoch;
    response["use_surrounding"] = route->second.preferences.at("use_surrounding");
    response["history_characters"] = route->second.preferences.at("history_characters");
    response["full_width"] = route->second.preferences.at("full_width");
    response["chinese_punctuation"] = route->second.preferences.at("chinese_punctuation");
    if (op == "reset" || op == "close" || (response.value("preedit", "").empty() && route->second.engine != engine)) {
        if (op != "close") selected->handle({{"version", 1}, {"op", "close"}, {"session", session},
            {"seq", request.value("seq", std::uint64_t{}) + 1}});
        routes.erase(route);
        retired.push_back(selected);
    }
    lock.unlock();
    return response;
}
void serve(HANDLE raw) {
    euphony::UniqueHandle pipe(raw);
    json request;
    std::vector<std::shared_ptr<euphony::Engine>> retired;
    if (euphony::read_frame(pipe.get(), request, 3000)) {
        json response;
        try { response = dispatch(request, retired); }
        catch (const std::exception& error) {
            response = {{"ok", false}, {"version", 1}, {"error", error.what()}, {"eaten", false}};
            if (request.contains("session")) response["session"] = request["session"];
            if (request.contains("seq")) response["seq"] = request["seq"];
        }
        if (euphony::write_frame(pipe.get(), response, 2000)) {
            json acknowledgment;
            euphony::read_frame(pipe.get(), acknowledgment, 2000);
        }
    }
    DisconnectNamedPipe(pipe.get());
    retired.clear();
    --clients;
}
int run() {
    euphony::PipeSecurity security;
    const auto mutex_name = L"Local\\Euphony-Broker-" + euphony::instance_name();
    euphony::UniqueHandle singleton(CreateMutexW(security.get(), FALSE, mutex_name.c_str()));
    if (!singleton) return 2;
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    config = std::make_unique<euphony::ConfigStore>();
    engine = std::make_shared<euphony::Engine>(config->get());
    euphony::BrokerTray tray(stopping);
    active_tray = &tray;
    const auto pipe_name = euphony::pipe_name();
    while (!stopping.load()) {
        // Leave a listener slot available instead of exiting when clients fill
        // the pipe's instance limit. Every slow client has a read deadline.
        while (clients.load() >= 63 && !stopping.load()) Sleep(10);
        if (stopping.load()) break;
        euphony::UniqueHandle pipe(CreateNamedPipeW(pipe_name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            64, 65536, 65536, 2000, security.get()));
        if (!pipe) return 3;
        euphony::UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        OVERLAPPED connection{};
        connection.hEvent = event.get();
        BOOL connected = ConnectNamedPipe(pipe.get(), &connection);
        const auto error = connected ? ERROR_SUCCESS : GetLastError();
        if (!connected && error == ERROR_IO_PENDING) {
            while (!stopping.load() && WaitForSingleObject(event.get(), 250) == WAIT_TIMEOUT) {}
            if (stopping.load()) {
                CancelIoEx(pipe.get(), &connection);
                DWORD ignored = 0;
                GetOverlappedResult(pipe.get(), &connection, &ignored, TRUE);
                break;
            }
            DWORD ignored = 0;
            connected = GetOverlappedResult(pipe.get(), &connection, &ignored, FALSE);
        } else if (error == ERROR_PIPE_CONNECTED) connected = TRUE;
        if (connected) {
            if (clients.load() >= 64) { DisconnectNamedPipe(pipe.get()); continue; }
            ++clients;
            std::thread(serve, pipe.release()).detach();
        }
    }
    while (clients.load()) Sleep(10);
    active_tray = nullptr;
    routes.clear(); engine.reset();
    return 0;
}
}
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    ImmDisableIME(static_cast<DWORD>(-1));
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    try { return run(); }
    catch (const std::exception& error) {
        // Startup diagnostics contain no input, context, or candidate text.
        OutputDebugStringA((std::string("Euphony broker: ") + error.what() + "\n").c_str());
        return 1;
    }
}
