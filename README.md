# Euphony

Euphony 是 Windows 10（1903+）/11 的中文输入法，支持全拼和双拼，将 Phono 模型的整句候选与词典逐词输入结合。x64 和 x86 应用通过各自的 TSF 前端连接同一个 x64 后台，推理在本机运行。设置界面使用 WebView2，输入法无需 Fcitx5 运行环境。

Euphony is a Chinese input method for Windows 10 (1903+) / 11. It supports Pinyin and Shuangpin, combining sentence candidates from the Phono model with word-by-word dictionary input. TSF frontends for x64 and x86 applications connect to the same x64 broker, with inference running locally. Settings use WebView2; the input method needs no Fcitx5 runtime.

## 安装 / Installation

从 [GitHub Releases](https://github.com/phono-project/Euphony/releases) 下载 `Euphony-<version>-windows-x64.zip`，解压到准备长期保留的目录。ZIP 包含 x64 / x86 TSF、x64 后台、设置程序、phono-core、词库、KenLM 语言模型、许可证和装卸脚本。Phono 深度模型单独下载。

Download `Euphony-<version>-windows-x64.zip` from [GitHub Releases](https://github.com/phono-project/Euphony/releases) and extract it to a directory you will keep. The ZIP includes x64 / x86 TSF, the x64 broker, settings, phono-core, dictionary, KenLM language model, licenses, and installation scripts. Download the Phono neural model separately.

设置程序需要 [Microsoft WebView2 Evergreen Runtime](https://developer.microsoft.com/en-us/microsoft-edge/webview2/#download-section)。若尚未安装，先使用微软提供的安装程序安装 Runtime；ZIP 内的 `WebView2Loader.dll` 不能替代 Runtime。

Settings require the [Microsoft WebView2 Evergreen Runtime](https://developer.microsoft.com/en-us/microsoft-edge/webview2/#download-section). If it is missing, install the Runtime using Microsoft’s installer first. The ZIP’s `WebView2Loader.dll` does not replace the Runtime.

保存正在输入的内容，关闭旧设置窗口。在当前 Windows 账户的管理员 PowerShell 中进入解压目录，执行以下命令。更新也使用同一安装入口，无需先卸载。

Save any composition in progress and close the old settings window. Open an administrator PowerShell under your current Windows account, enter the extracted directory, and run the command below. Updates use the same installation entry point without prior uninstallation.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\manage.ps1 Install
```

安装完成后注销 Windows，再登录，使应用重新载入新版 TSF DLL。随后在普通 PowerShell 中打开设置，用 `Win+Space` 选择 Euphony。将解压目录作为安装目录保留。

After installation, sign out of Windows and sign back in so applications reload the updated TSF DLLs. Then open settings from a normal PowerShell and select Euphony with `Win+Space`. Keep the extracted directory as your installation directory.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\manage.ps1 Settings
```

## 模型配置 / Model setup

下载一个完整的 v2.2 模型包：[W4A8](https://huggingface.co/afirelily/phonop2c_v2_2_base_w4a8_model) 或 [W8A8](https://huggingface.co/afirelily/phonop2c_v2_2_base_w8a8_model)。安装 Hugging Face CLI 后可使用下列命令，模型保存到用户选定的固定目录。

Download a complete v2.2 model package: [W4A8](https://huggingface.co/afirelily/phonop2c_v2_2_base_w4a8_model) or [W8A8](https://huggingface.co/afirelily/phonop2c_v2_2_base_w8a8_model). With the Hugging Face CLI installed, use either command below and keep the model in a permanent directory of your choice.

```powershell
hf download afirelily/phonop2c_v2_2_base_w4a8_model --local-dir .\phonop2c_v2_2_base_w4a8_model
# 或选择 W8A8 / Or choose W8A8
hf download afirelily/phonop2c_v2_2_base_w8a8_model --local-dir .\phonop2c_v2_2_base_w8a8_model
```

在设置中将「模型目录」指向下载目录的绝对路径。该目录应直接包含 `config.json`、`bins/` 和 `vocabs/`。启用模型并保存；首次使用会加载模型。模型没有配置、被禁用或加载失败时，仍可使用词典候选，设置中的状态信息会显示资源情况。

In settings, point “Model directory” to the absolute path of the downloaded directory. It must directly contain `config.json`, `bins/`, and `vocabs/`. Enable the model and save; first use loads it. Dictionary candidates remain available when the model is unconfigured, disabled, or fails to load. Resource status is shown in settings.

默认使用安装目录中的 core、词库和 KenLM；不需要源码仓库。模型目录保存在用户配置中，更新输入法不会替换模型。移动模型目录后，请重新选择路径。

Core, dictionary, and KenLM default to resources in the installation directory; source repositories are unnecessary. The model directory is stored in user settings, and input-method updates do not replace models. Select the new path after moving a model.

## 使用 / Usage

输入拼音时，词典候选立即出现，模型整句候选随后显示。选择词典候选可继续逐词选择；选择模型候选提交整句。浏览候选时，该轮列表停止异步更新。默认每页 5 个候选，纵向排列，模型候选使用淡绿色背景；候选数、排列和颜色可在设置中调整。

Dictionary candidates appear immediately while typing, followed by model sentence candidates. Choose dictionary entries to continue word by word, or a model candidate to commit a sentence. The list stops updating asynchronously while you browse it. Defaults are five candidates per page, vertical layout, and a pale green background for model candidates. Settings let you adjust candidate count, layout, and colors.

支持全拼及小鹤、自然码、微软、紫光、智能 ABC、中文之星、拼音加加七种双拼。无上文的单音节输入默认优先使用 KenLM 单字词频；有上文、连续拼音或关闭该选项时使用模型。KenLM 不可用时自动回退。

Pinyin and seven Shuangpin schemes are supported: Xiaohe, Natural Code, Microsoft, Purple Light, Smart ABC, Chinese Star, and Pinyin Jiajia. A single syllable without preceding context defaults to KenLM single-character frequency. Contextual input, continuous pinyin, or disabling that option uses the model. Unavailable KenLM triggers fallback.

| 按键 / Key | 操作 / Action |
| --- | --- |
| 数字 / Number keys、Space | 选择候选 / Select a candidate |
| `=`、`+`、Tab、PageDown | 下一页 / Next page |
| `-`、`_`、Shift+Tab、PageUp | 上一页 / Previous page |
| ↑ / ↓ | 移动候选选择 / Move candidate selection |
| ← / →、Home / End | 编辑拼音光标 / Move the pinyin cursor |
| Backspace / Delete | 删除或退回逐词选择 / Delete or return to word selection |
| Enter / Escape | 提交 / 取消 / Commit / Cancel |
| 单独 Shift / Shift alone | 中英切换 / Toggle Chinese and English |

单击托盘图标打开设置；右键菜单提供方案、候选数、排列和常用开关。设置在下一轮输入生效。应用若使用自己的候选界面，其显示样式由应用控制。

Click the tray icon to open settings. Its context menu provides schemes, candidate count, layout, and common switches. Changes apply to the next composition. Applications that provide their own candidate UI control its appearance.

## 上文与用户数据 / Context and user data

Euphony 使用应用提供的光标前文本生成符合上文的候选，可在设置中关闭读取应用上文。应用不能提供上文时使用会话内连续提交的历史。密码与私密字段不读取上下文、不推理、不学习。

Euphony uses text before the cursor supplied by the application to generate contextual candidates. You can disable application context in settings. When context is unavailable, it uses consecutive commits within the session. Password and private fields disable context reading, inference, and learning.

用户数据保存在 `%LOCALAPPDATA%\Euphony`：`config.json` 为配置，`learning.json` 保存明确选中的词典词条，`webview/` 为页面缓存。更新安装自带资源路径前会保留 `config.json.before-update`。模型保存在自行选择的目录。

User data lives in `%LOCALAPPDATA%\Euphony`: `config.json` holds preferences, `learning.json` records explicitly selected dictionary entries, and `webview/` stores page cache. Updating packaged resource paths preserves `config.json.before-update`. Models remain in the directory you select.

## 状态、更新与卸载 / Status, updates, and removal

在普通 PowerShell 中运行 `Status`，检查后台版本、两种 TSF 注册路径及 core / 词库 / KenLM / 模型路径。它不会启动后台；需要时先打开设置或开始输入。若更新后仍显示旧 DLL，注销再登录。

Run `Status` in a normal PowerShell to inspect the broker version, both TSF registration paths, and core / dictionary / KenLM / model paths. It does not start the broker; open settings or type first if needed. If old DLLs remain after an update, sign out and back in.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\manage.ps1 Status
```

卸载时，在同一 Windows 账户的管理员 PowerShell 中运行下列命令。配置、学习数据和模型会保留。注销再登录后可删除旧安装目录；已注册或仍被应用载入的 DLL 不应手动删除。

To uninstall, run the command below in an administrator PowerShell under the same Windows account. Preferences, learning data, and models are retained. Delete the old installation directory after signing out and back in; do not manually delete registered DLLs or DLLs still loaded by applications.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\manage.ps1 Uninstall
```

管理入口还支持 `Start`、`Stop` 和 `Settings`。日常启动后台和设置使用普通窗口，无需 Python。源码中的注册脚本由管理入口调用。

The manager also supports `Start`, `Stop`, and `Settings`. Run the broker and settings from a normal window; Python is unnecessary. The manager invokes the underlying registration scripts.

## 开发与贡献 / Development and contributions

源码构建需要 Pixi、Visual Studio C++ 桌面工作负载和 Windows SDK，以及位于 PATH 或由 `VCPKG_ROOT` 指定的 vcpkg。独立克隆即可使用：先下载并校验固定的 Windows x64 core Release SDK；`setup` 准备固定 WebView2 SDK、官方词库和 KenLM，生成语言模型并校验下载来源。

Source builds require Pixi, Visual Studio’s C++ Desktop workload and Windows SDK, plus vcpkg on PATH or specified by `VCPKG_ROOT`. An independent checkout is sufficient: first download and verify the pinned Windows x64 core Release SDK. `setup` prepares the pinned WebView2 SDK, official dictionary, and KenLM, generates the language model, and verifies downloaded sources.

```powershell
pixi install --locked
pixi run python tools/download_core.py
pixi run package
pixi run verify
```

`package` 构建 x64 主程序及 x86 TSF，从干净暂存目录生成 `dist/Euphony-<version>-windows-x64.zip`。`verify` 运行两个架构的 CTest 和隔离 IPC 验证，日常检查不需要深度模型或桌面交互。桌面测试、真实模型验证与 Release 复现见 [验证与发布文档](docs/verification.md)。

`package` builds the x64 programs and x86 TSF, then creates `dist/Euphony-<version>-windows-x64.zip` from a clean staging directory. `verify` runs CTest for both architectures and isolated IPC checks; routine validation needs no neural model or desktop interaction. See [validation and release documentation](docs/verification.md) for desktop tests, real-model validation, and Release reproduction.

版本只在 `pixi.toml` 的 workspace 中维护。前端通过普通提交更新固定 core 依赖并验证，core 发布不自动触发 Euphony 发布。贡献代码前运行相关检查；有关依赖来源、词典行为和图标的说明分别见 [THIRD_PARTY.md](THIRD_PARTY.md)、[data/README.md](data/README.md) 和 [img/README.md](img/README.md)。

The workspace in `pixi.toml` is the single application version source. Update the pinned core dependency in a normal frontend commit and validate it; core releases do not automatically publish Euphony. Run relevant checks before contributing. Dependency provenance, dictionary behavior, and artwork are documented in [THIRD_PARTY.md](THIRD_PARTY.md), [data/README.md](data/README.md), and [img/README.md](img/README.md).

## 许可证 / License

本项目使用 [Apache License 2.0](LICENSE)，第三方材料保留各自许可证。

This project uses [Apache License 2.0](LICENSE). Third-party material retains their respective licenses.
