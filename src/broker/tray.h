#pragma once
#include "common/ipc.h"
#include <atomic>
#include <memory>

namespace euphony {
// A separate Win32 message thread leaves inference and pipe dispatch responsive.
class BrokerTray {
public:
    explicit BrokerTray(std::atomic_bool& stopping);
    ~BrokerTray();
    void set_mode(bool chinese);
    BrokerTray(const BrokerTray&) = delete;
    BrokerTray& operator=(const BrokerTray&) = delete;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
