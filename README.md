# Euphony

Euphony 是适用于 Windows 10（1903+）和 Windows 11 的中文输入法，支持全拼和七种双拼方案。输入时同时提供词典候选和结合上文生成的整句候选，支持逐词选择或整句输入。

Euphony is a Chinese input method for Windows 10 (1903+) and Windows 11, supporting Pinyin and seven Shuangpin schemes. It offers dictionary candidates alongside sentence candidates generated from preceding text, so you can select words individually or commit a whole sentence.

## Portable 包安装 / Portable package installation

当前版本以 portable ZIP 包提供，以下步骤适用于便携包；便携包仍需运行注册命令，将输入法添加到 Windows。

The current release is a portable ZIP package; the steps below apply to that package. The portable package still requires a registration command to add the input method to Windows.

从 [GitHub Releases](https://github.com/phono-project/Euphony/releases) 下载 `Euphony-<version>-windows-x64.zip`，解压到准备长期保留的目录。需要 64 位 Windows，可在 x64 和 x86 应用中使用。整句候选使用的 Phono 模型需单独下载，见下方模型配置。

Download `Euphony-<version>-windows-x64.zip` from [GitHub Releases](https://github.com/phono-project/Euphony/releases) and extract it to a directory you will keep. It requires 64-bit Windows and supports both x64 and x86 applications. Download the Phono model for sentence candidates separately, as described below.

设置程序需要 [Microsoft WebView2 Evergreen Runtime](https://developer.microsoft.com/en-us/microsoft-edge/webview2/#download-section)，若尚未安装，请先安装。

Settings require the [Microsoft WebView2 Evergreen Runtime](https://developer.microsoft.com/en-us/microsoft-edge/webview2/#download-section). Install it first if it is missing.

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

模型目录保存在用户配置中，更新输入法不会替换模型。移动模型目录后，请重新选择路径。

The model directory is stored in user settings, and input-method updates do not replace models. Select the new path after moving a model.

## 使用 / Usage

输入拼音时，词典候选立即出现，模型整句候选随后显示。选择词典候选可继续逐词选择；选择模型候选提交整句。开始浏览候选后，该轮候选列表保持不变。

Dictionary candidates appear immediately while typing, followed by model sentence candidates. Choose dictionary entries to continue word by word, or a model candidate to commit a sentence. Once you start browsing candidates, the list stays unchanged for that composition.

支持全拼及小鹤、自然码、微软、紫光、智能 ABC、中文之星、拼音加加七种双拼。

Pinyin and seven Shuangpin schemes are supported: Xiaohe, Natural Code, Microsoft, Purple Light, Smart ABC, Chinese Star, and Pinyin Jiajia.

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

## 常用设置 / Settings

单击托盘图标打开设置；右键菜单提供方案、候选数、排列和常用开关。设置在下一轮输入生效。默认每页 5 个候选，纵向排列，模型候选使用淡绿色背景；可在设置中调整。应用若使用自己的候选界面，其显示样式由应用控制。

Click the tray icon to open settings. Its context menu provides schemes, candidate count, layout, and common switches. Changes apply to the next composition. Defaults are five candidates per page, vertical layout, and a pale green background for model candidates; these can be adjusted in settings. Applications that provide their own candidate UI control its appearance.

“无上下文单字优先词频”选项默认开启：没有上文、只输入一个完整音节时，优先使用单字词频候选；关闭后使用模型生成。候选生成策略见 [开发说明](docs/verification.md#候选生成策略--candidate-generation)。

The “无上下文单字优先词频” option (prefer frequency for a single syllable without context) is enabled by default: when there is no preceding context and only one complete syllable, single-character frequency candidates take priority. Disable it to use model generation instead. See [candidate generation](docs/verification.md#候选生成策略--candidate-generation) for details.

## 上文与用户数据 / Context and user data

Euphony 在本机处理拼音和上文，使用应用提供的光标前文本生成候选。可在设置中关闭读取应用上文；应用不能提供上文时使用会话内连续提交的历史。密码与私密字段不读取上下文、不推理、不学习。

Euphony processes pinyin and context locally, using text before the cursor supplied by the application to generate candidates. You can disable application context in settings. When context is unavailable, it uses consecutive commits within the session. Password and private fields disable context reading, inference, and learning.

用户数据保存在 `%LOCALAPPDATA%\Euphony`：`config.json` 为配置，`learning.json` 保存明确选中的词典词条，`webview/` 为页面缓存。更新安装自带资源路径前会保留 `config.json.before-update`。模型保存在自行选择的目录。

User data lives in `%LOCALAPPDATA%\Euphony`: `config.json` holds preferences, `learning.json` records explicitly selected dictionary entries, and `webview/` stores page cache. Updating packaged resource paths preserves `config.json.before-update`. Models remain in the directory you select.

## Portable 包更新与卸载 / Portable package updates and removal

更新时，将新版本 ZIP 解压到准备保留的目录，再按上方安装步骤运行 `Install` 并注销、重新登录。配置、学习数据和单独下载的模型会保留。

To update, extract the new ZIP into the directory you intend to keep, then follow the installation steps above to run `Install` and sign out and back in. Preferences, learning data, and separately downloaded models are retained.

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

管理入口还支持 `Start`、`Stop` 和 `Settings`，在普通 PowerShell 窗口中运行。

The manager also supports `Start`, `Stop`, and `Settings`; run these in a normal PowerShell window.

## 开发与贡献 / Development and contributions

源码构建需要 Pixi、Visual Studio C++ 桌面工作负载和 Windows SDK，以及位于 PATH 或由 `VCPKG_ROOT` 指定的 vcpkg。在仓库根目录运行：

Source builds require Pixi, Visual Studio’s C++ Desktop workload and Windows SDK, plus vcpkg on PATH or specified by `VCPKG_ROOT`. Run from the repository root:

```powershell
pixi install --locked
pixi run python tools/download_core.py
pixi run package
pixi run verify
```

`package` 生成 portable 包 `dist/Euphony-<version>-windows-x64.zip`；`verify` 运行自动检查。架构、依赖准备、桌面测试和发布流程见 [开发、验证与发布文档](docs/verification.md)。

`package` creates the portable archive `dist/Euphony-<version>-windows-x64.zip`; `verify` runs automated checks. See the [development, validation, and release guide](docs/verification.md) for architecture, dependency setup, desktop tests, and releases.

贡献代码前运行相关检查。依赖来源、词典行为和图标说明见 [THIRD_PARTY.md](THIRD_PARTY.md)、[data/README.md](data/README.md) 和 [img/README.md](img/README.md)。

Run relevant checks before contributing. Dependency provenance, dictionary behavior, and artwork are documented in [THIRD_PARTY.md](THIRD_PARTY.md), [data/README.md](data/README.md), and [img/README.md](img/README.md).

## 许可证 / License

本项目使用 [Apache License 2.0](LICENSE)，第三方材料保留各自许可证。

This project uses [Apache License 2.0](LICENSE). Third-party material retains their respective licenses.
