# 第三方材料 / Third-party material

依赖下载版本及校验和由 `dependencies.json` 固定，Python 工具环境由 `pixi.lock` 固定，KenLM 由 `vcpkg.json` 的 baseline 固定。安装包保留对应许可文件。

`dependencies.json` pins dependency download versions and checksums, `pixi.lock` locks the Python tool environment, and the baseline in `vcpkg.json` pins KenLM. Packages retain the corresponding license files.

## 推理与输入逻辑 / Inference and input logic

[phono-core](https://github.com/phono-project/phono-core) 通过固定 Release SDK 的 C ABI 动态加载，许可证及依赖声明来自 SDK 的 `share/licenses/phono-core`，随包保留在 `licenses/phono-core`。TSF DLL 不包含推理实现；x86 TSF 连接 x64 后台。

[phono-core](https://github.com/phono-project/phono-core) is loaded dynamically through the C ABI from a pinned Release SDK. Its license and dependency notices come from the SDK’s `share/licenses/phono-core` and are retained in the package’s `licenses/phono-core` directory. TSF DLLs contain no inference implementation; x86 TSF connects to the x64 broker.

worker、core 调度、非法输入展示和双拼转换语义改编自 [phono-fcitx5-addon](https://github.com/phono-project/phono-fcitx5-addon)，使用 Apache-2.0，其许可证在 `data/licenses/PHONO-ADDON-APACHE-2.0.txt`。改编改为显式 DLL 符号加载、移除 Fcitx5 运行时依赖，并通过后台请求线程应用结果。

Worker behavior, core orchestration, invalid-input presentation, and Shuangpin conversion semantics are adapted from [phono-fcitx5-addon](https://github.com/phono-project/phono-fcitx5-addon), licensed under Apache-2.0 with its license in `data/licenses/PHONO-ADDON-APACHE-2.0.txt`. Adaptations use explicit DLL symbol loading, remove the Fcitx5 runtime dependency, and apply results through the broker request thread.

`src/engine/shuangpin_tables.h` 只保留 [libime shuangpindata.h](https://github.com/fcitx/libime/blob/master/src/libime/pinyin/shuangpindata.h) 的七组键位映射数组，使用 LGPL-2.1-or-later，copyright 2011–2020 CSSlayer。Windows 引擎及 TSF 实现为本项目代码。

`src/engine/shuangpin_tables.h` retains only seven key-mapping arrays from [libime shuangpindata.h](https://github.com/fcitx/libime/blob/master/src/libime/pinyin/shuangpindata.h), licensed under LGPL-2.1-or-later, copyright 2011–2020 CSSlayer. The Windows engine and TSF implementation are this project’s code.

## 词库与语言模型 / Dictionary and language model

`data/libime.tsv` 来自 [libime dict-20260703](https://download.fcitx-im.org/data/dict-20260703.tar.zst) 的官方 `dict_sc.txt`，含 300,234 条词条，保留原读音及 log10 权重。许可为 LGPL-2.1-or-later，copyright 2002–2005 Yuking、2014–2026 CSSlayer；见 [上游 REUSE 声明](https://github.com/fcitx/libime/blob/master/REUSE.toml) 和 `data/licenses/LGPL-2.1-or-later.txt`。`tools/import_libime.py` 可复现转换。

`data/libime.tsv` contains 300,234 entries from the official `dict_sc.txt` in [libime dict-20260703](https://download.fcitx-im.org/data/dict-20260703.tar.zst), retaining original pronunciations and log10 weights. It is LGPL-2.1-or-later, copyright 2002–2005 Yuking and 2014–2026 CSSlayer; see [upstream REUSE metadata](https://github.com/fcitx/libime/blob/master/REUSE.toml) and `data/licenses/LGPL-2.1-or-later.txt`. `tools/import_libime.py` reproduces the conversion.

`data/zh_CN.lm` 由 [libime lm_sc.arpa-20260629](https://download.fcitx-im.org/data/lm_sc.arpa-20260629.tar.zst) 生成，使用 LGPL-2.1-or-later，copyright 2022–2026 CSSlayer。`tools/prepare_libime_lm.py` 使用上游量化 trie 转换参数生成包内语言模型。

`data/zh_CN.lm` is generated from [libime lm_sc.arpa-20260629](https://download.fcitx-im.org/data/lm_sc.arpa-20260629.tar.zst), licensed under LGPL-2.1-or-later, copyright 2022–2026 CSSlayer. `tools/prepare_libime_lm.py` uses upstream quantized-trie conversion parameters to generate the packaged language model.

[KenLM](https://github.com/kpu/kenlm) 通过固定的 [vcpkg Windows port](https://github.com/microsoft/vcpkg/tree/master/ports/kenlm) 提供，只链接到后台 / 引擎。安装包的 `licenses/vcpkg` 保留 KenLM 与依赖库的版权文件。词库和语言模型的复现说明见 [data/README.md](data/README.md)。

[KenLM](https://github.com/kpu/kenlm) is supplied through the pinned [vcpkg Windows port](https://github.com/microsoft/vcpkg/tree/master/ports/kenlm) and linked only into the broker / engine. The package’s `licenses/vcpkg` retains KenLM and dependency copyright files. See [data/README.md](data/README.md) for dictionary and language-model reproduction.

## 设置与工具依赖 / Settings and tool dependencies

`nlohmann_json` 由 Pixi / conda-forge 提供，使用 MIT；头文件保留版权声明，许可文件在 `data/licenses/NLOHMANN-JSON-MIT.txt`。

`nlohmann_json` is supplied by Pixi / conda-forge under MIT. Its headers retain copyright notices, and the license is in `data/licenses/NLOHMANN-JSON-MIT.txt`.

[Microsoft.Web.WebView2 1.0.4191.47](https://www.nuget.org/packages/Microsoft.Web.WebView2/1.0.4191.47) 来自官方 NuGet 端点，下载校验和固定在 `dependencies.json`。SDK 许可缓存在 `.deps/webview2`，loader、LICENSE 和 NOTICE 随设置程序交付。WebView2 Runtime 是用户另行安装的前置条件。

[Microsoft.Web.WebView2 1.0.4191.47](https://www.nuget.org/packages/Microsoft.Web.WebView2/1.0.4191.47) comes from the official NuGet endpoint, with its download checksum pinned in `dependencies.json`. SDK licenses are cached in `.deps/webview2`; the loader, LICENSE, and NOTICE accompany the settings program. Users install the WebView2 Runtime separately as a prerequisite.

## 接口参考 / Interface references

以下资料用于单独的接口契约和平台行为参考：

The following material is used as reference for individual interface contracts and platform behavior:

- [Microsoft TSF edit sessions](https://learn.microsoft.com/en-us/windows/win32/tsf/edit-sessions)
- [Microsoft TSF compositions](https://learn.microsoft.com/en-us/windows/win32/tsf/compositions)
- [Microsoft TSF registration and 64-bit considerations](https://learn.microsoft.com/en-us/windows/win32/tsf/64-bit-platform-considerations)
- [Microsoft named pipe security](https://learn.microsoft.com/en-us/windows/win32/ipc/named-pipe-security-and-access-rights)
- [Microsoft UTF-8 process code page](https://learn.microsoft.com/en-us/windows/apps/design/globalizing/use-utf8-code-page)
- [Microsoft WebView2 WebMessageReceived](https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2webmessagereceivedeventargs)
- [Microsoft Per-Monitor V2](https://learn.microsoft.com/en-us/windows/win32/hidpi/dpi-awareness-context)
- [ManagedShell tray interface declarations](https://github.com/cairoshell/ManagedShell/blob/master/src/ManagedShell.WindowsTray/TrayNotify.cs)

UTF-8 进程代码页只用于后台和引擎测试，使 KenLM 能读取 Unicode 路径。ManagedShell 资料只用于 Shell 兼容所需的 COM 标识、接口方法顺序和通知项字段布局；托盘与菜单由 Euphony 实现。

The UTF-8 process code page is enabled only for the broker and engine tests so KenLM can open Unicode paths. ManagedShell material was consulted only for COM identifiers, interface method order, and notification-item field layout needed for Shell compatibility. Euphony implements its own tray and menus.
