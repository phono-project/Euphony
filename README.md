# Euphony

Euphony 是适用于 Windows 10（1903+）和 Windows 11 的中文输入法，支持全拼和七种双拼方案。输入时同时提供词典候选和结合上文生成的整句候选，支持逐词选择或整句输入。

Euphony is a Chinese input method for Windows 10 (1903+) and Windows 11, supporting Pinyin and seven Shuangpin schemes. It offers dictionary candidates alongside sentence candidates generated from preceding text, so you can select words individually or commit a whole sentence.

## 安装包安装 / Installer installation

从 [GitHub Releases](https://github.com/phono-project/Euphony/releases) 下载 `Euphony-<version>-windows-x64-setup.exe` 并运行安装向导。安装器支持 Windows 10（1903+）和 Windows 11 x64，自动注册 x64 与 x86 输入法，添加开始菜单设置入口及 Windows「已安装的应用」卸载入口。安装和卸载不调用 PowerShell。

Download `Euphony-<version>-windows-x64-setup.exe` from [GitHub Releases](https://github.com/phono-project/Euphony/releases) and run the wizard. It supports Windows 10 (1903+) and Windows 11 x64, registers the input method for x64 and x86 applications, and adds Start menu settings and Windows Installed apps uninstall entries. Installation and removal do not invoke PowerShell.

安装需要管理员权限，请使用准备使用输入法的同一 Windows 账户批准提权。安装前保存输入内容，退出旧设置窗口及托盘中的 Euphony。安装后按提示重启；未提示重启时，注销再登录。按 `Win+Space` 选择 Euphony，从开始菜单打开 **Euphony Settings**。设置需要下方链接的 WebView2 Runtime，整句候选的模型需单独下载。

Administrator privileges are required; approve elevation under the Windows account that will use Euphony. Save any composition and exit the old settings window and Euphony tray first. Restart if prompted; otherwise sign out and back in. Select Euphony with `Win+Space` and open **Euphony Settings** from Start. Settings require the WebView2 Runtime linked below; download the sentence model separately.

更新时直接运行新版本安装包，沿用原目录。卸载时先退出设置和托盘程序，在 Windows「已安装的应用」中卸载 Euphony，然后重启以释放应用载入的 TSF DLL。配置、学习数据和单独下载的模型保留。

To update, run the new installer over the existing directory. To uninstall, exit settings and the tray, remove Euphony through Windows Installed apps, then restart to unload TSF DLLs. Preferences, learned vocabulary, and separately downloaded models are retained.

从便携版切换时，先按下方便携版步骤卸载注册、注销再登录，再运行安装包。若旧配置中的 core、词库或 KenLM 路径指向便携目录，请在设置中改为新安装目录内的 `phono_core.dll`、`data/libime.tsv` 和 `data/zh_CN.lm`，再删除旧目录。

To switch from portable, unregister it using the removal steps below, sign out and back in, then run the installer. If saved core, dictionary, or KenLM paths point to the portable directory, change them in Settings to `phono_core.dll`, `data/libime.tsv`, and `data/zh_CN.lm` in the new installation before deleting the old directory.

## Portable 包安装 / Portable package installation

同时提供 portable ZIP 包，以下步骤适用于便携包；便携包仍需运行注册命令，将输入法添加到 Windows。

A portable ZIP package is also available; the steps below apply to that package. The portable package still requires a registration command to add the input method to Windows.

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
| 单独 Shift 或 Ctrl+Space / Shift alone or Ctrl+Space | 中英切换 / Toggle Chinese and English |
| Ctrl+, | 全半角切换 / Toggle full-width and half-width input |

## 常用设置 / Settings

单击托盘图标打开设置；右键菜单提供方案、候选数、排列和常用开关。设置在下一轮输入生效。默认每页 5 个候选，纵向排列，模型候选使用淡绿色背景；可在设置中调整。应用若使用自己的候选界面，其显示样式由应用控制。

Click the tray icon to open settings. Its context menu provides schemes, candidate count, layout, and common switches. Changes apply to the next composition. Defaults are five candidates per page, vertical layout, and a pale green background for model candidates; these can be adjusted in settings. Applications that provide their own candidate UI control its appearance.

“无上下文单字优先词频”选项默认开启：没有上文、只输入一个完整音节时，优先使用单字词频候选；关闭后使用模型生成。候选生成策略见 [开发说明](docs/verification.md#候选生成策略--candidate-generation)。

The “无上下文单字优先词频” option (prefer frequency for a single syllable without context) is enabled by default: when there is no preceding context and only one complete syllable, single-character frequency candidates take priority. Disable it to use model generation instead. See [candidate generation](docs/verification.md#候选生成策略--candidate-generation) for details.

## 上文与用户数据 / Context and user data

Euphony 在本机处理拼音和上文，使用应用提供的光标前文本生成候选。可在设置中关闭读取应用上文；应用不能提供上文时使用会话内连续提交的历史。密码与私密字段不读取上下文、不推理、不学习。

Euphony processes pinyin and context locally, using text before the cursor supplied by the application to generate candidates. You can disable application context in settings. When context is unavailable, it uses consecutive commits within the session. Password and private fields disable context reading, inference, and learning.

「仅精确匹配时优先用户词」默认开启：用户词只匹配输入前缀、简拼或未完成音节时，排在模型结果之后。关闭后可优先显示前缀用户词；多字输入中的单字记忆始终不前置。高亮用户词后按 Delete 可删除记忆并重新生成候选。

“Prioritize user words only for exact matches” is enabled by default. User words matching a prefix, abbreviation, or unfinished syllable follow model results. Disabling it allows prefix user words to lead; single-character memories never receive priority for longer input. Highlight a user word and press Delete to forget it and regenerate candidates.

「启用用户词」和「用户词优先」默认开启。关闭「用户词优先」后，用户词排在模型结果之后；关闭「启用用户词」后，不使用用户词和记忆排序，保留已有记录。「记住词库选择」独立控制是否继续学习新词。

User words are enabled and shown before model results by default. Disable “User words first” to place them after model results, or disable “Enable user words” to stop using remembered vocabulary and ranking while retaining saved records. “Remember dictionary selections” independently controls new learning.

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

安装 [Inno Setup 6.3+](https://jrsoftware.org/isdl.php) 后运行 `pixi run installer`，可额外生成 `dist/Euphony-<version>-windows-x64-setup.exe`，同时保留便携 ZIP。CI 自动构建、验证并保存两种产物，发布 Release 时上传两者。

With [Inno Setup 6.3+](https://jrsoftware.org/isdl.php) installed, run `pixi run installer` to also create `dist/Euphony-<version>-windows-x64-setup.exe`, retaining the portable ZIP. CI builds, validates, and saves both formats, and attaches both to published Releases.

## 许可证 / License

本项目使用 [Apache License 2.0](LICENSE)，第三方材料保留各自许可证。

This project uses [Apache License 2.0](LICENSE). Third-party material retains their respective licenses.

### 候选窗与回退分词

候选窗的预编辑框默认隐藏，可在设置中开启。颜色模式支持遵循系统（默认）、亮色与暗色；两套配色独立保存，可分别调整背景、模型词、用户词和选定词颜色。暗色默认使用暗灰背景、暗绿模型词、暗紫用户词和深蓝选定词。

字典回退对不超过 6 个字符的输入保留多种读法，并降低简拼和过度拆分路径的排序。更长输入通过动态规划优先选择完整音节较少的路径，末尾允许未完成音节；手动输入的分隔符会被保留。无法找到全拼路径时仍支持原有简拼回退。该策略参考 [libime 的拼音解析实现](https://github.com/fcitx/libime/blob/master/src/libime/pinyin/pinyinencoder.cpp)，不新增运行依赖。

中文标点模式支持 `\` / `/` → `、`、`|` → `·`、`$` → `¥`（开启全角功能符号时为 `￥`）、`^` → `……`、`_` → `——`、`{` / `}` → `『』`。常用映射参考 [Rime 标点配置](https://github.com/rime/rime-prelude/blob/master/punctuation.yaml)，保留现有括号配对与数字标点处理。关闭中文标点后仍可直接输入 ASCII 符号。

全角模式可分别配置“全角数字”和“全角功能符号”，默认均关闭。关闭时数字与 `~ @ # ¥ % & * - = + | /` 的输入结果与半角模式一致；开启时相应字符转换为全角。中文标点设置仍独立生效。
