# Third-party material

- `phono-core`: dynamically loaded through the C ABI of the local checkout; its Apache-2.0 license accompanies the source at `../phono-core/LICENSE`. No inference implementation is linked into the TSF DLL.
- The worker, core orchestration, invalid input presentation and shuangpin conversion semantics are adapted from local `phono-fcitx5-addon`, Apache-2.0. Its license is in `data/licenses/PHONO-ADDON-APACHE-2.0.txt`.
- `data/libime.tsv`: 300,234 entries from the official `dict_sc.txt` in [libime dict-20260703](https://download.fcitx-im.org/data/dict-20260703.tar.zst), LGPL-2.1-or-later, copyright 2002–2005 Yuking and 2014–2026 CSSlayer. See [upstream REUSE metadata](https://github.com/fcitx/libime/blob/master/REUSE.toml) and `data/licenses/LGPL-2.1-or-later.txt`. The original pronunciations and log10 weights are retained; `tools/import_libime.py` reproduces the conversion.
- `data/zh_CN.lm`: generated from [official libime lm_sc.arpa-20260629](https://download.fcitx-im.org/data/lm_sc.arpa-20260629.tar.zst), LGPL-2.1-or-later, copyright 2022–2026 CSSlayer. `tools/prepare_libime_lm.py` uses the upstream quantized trie conversion parameters.
- KenLM is supplied by the fixed vcpkg manifest because conda-forge has no win-64 package. It is linked only into the broker / engine, with no Fcitx runtime. KenLM and its dependent library copyright files are included under `licenses/vcpkg` in packaged output. See [KenLM upstream](https://github.com/kpu/kenlm) and [the Windows vcpkg port](https://github.com/microsoft/vcpkg/tree/master/ports/kenlm).
- `src/engine/shuangpin_tables.h`: only the seven key mapping arrays from [libime shuangpindata.h](https://github.com/fcitx/libime/blob/master/src/libime/pinyin/shuangpindata.h), LGPL-2.1-or-later, copyright 2011–2020 CSSlayer. The Windows engine and TSF implementation are original code.
- `nlohmann_json`: MIT, supplied by pixi/conda-forge; the copyright notice is in its headers and the license is retained in `data/licenses/NLOHMANN-JSON-MIT.txt`.
- WebView2 SDK: [Microsoft.Web.WebView2 1.0.4191.47](https://www.nuget.org/packages/Microsoft.Web.WebView2/1.0.4191.47), fetched from the official NuGet endpoint. SDK license files remain in `.deps/webview2`; the SDK's loader is packaged beside the configuration program.

API references, used for individual interface contracts rather than importing another input method project:

- [Microsoft TSF edit sessions](https://learn.microsoft.com/en-us/windows/win32/tsf/edit-sessions)
- [Microsoft TSF compositions](https://learn.microsoft.com/en-us/windows/win32/tsf/compositions)
- [Microsoft TSF registration and 64-bit considerations](https://learn.microsoft.com/en-us/windows/win32/tsf/64-bit-platform-considerations)
- [Microsoft named pipe security](https://learn.microsoft.com/en-us/windows/win32/ipc/named-pipe-security-and-access-rights)
- [Microsoft UTF-8 process code page](https://learn.microsoft.com/en-us/windows/apps/design/globalizing/use-utf8-code-page), enabled only for the broker and engine test executable to let KenLM open Unicode model paths.
- [Microsoft WebView2 WebMessageReceived](https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2webmessagereceivedeventargs)
- [Microsoft Per-Monitor V2](https://learn.microsoft.com/en-us/windows/win32/hidpi/dpi-awareness-context), including native popup menu scaling.
- [ManagedShell tray interface declarations](https://github.com/cairoshell/ManagedShell/blob/master/src/ManagedShell.WindowsTray/TrayNotify.cs): only COM identifiers, interface method order and notification-item field layout were consulted for Shell compatibility. The tray and menu implementation are Euphony's own.
