# 开发、验证与发布 / Development, validation, and releases

[安装与使用 / Installation and usage](../README.md)

## 架构与交付 / Architecture and distribution

x64 和 x86 应用通过各自的 TSF 前端连接同一个 x64 后台，后台调用 phono-core 生成模型候选。设置界面使用 WebView2。当前交付物为 portable ZIP 包，需通过管理脚本注册输入法。

TSF frontends for x64 and x86 applications connect to one x64 broker, which calls phono-core to generate model candidates. Settings use WebView2. The current distribution is a portable ZIP package that requires registration through the management script.

## 候选生成策略 / Candidate generation

词典候选先返回，模型候选异步补充；用户开始浏览候选后，该轮列表停止更新。默认开启的 `prefer_unigram_single_syllable` 在无上文且仅有一个完整音节时优先使用 KenLM 单字词频；有上文、连续拼音或关闭选项时使用模型。KenLM 不可用时回到通常的候选生成流程。模型未配置、禁用或加载失败时保留词典候选。

Dictionary candidates return first, followed asynchronously by model candidates. Once the user starts browsing, the list stops updating for that composition. The default `prefer_unigram_single_syllable` option prefers KenLM single-character frequency for one complete syllable without context. Contextual input, continuous pinyin, or disabling the option uses the model. If KenLM is unavailable, the usual candidate generation path applies. Dictionary candidates remain available if the model is unconfigured, disabled, or fails to load.

## 验证范围 / Validation scope

测试分为日常检查、发布验证和桌面交互三个部分，均复用本仓库脚本。自动验证使用隔离用户配置和管道，不注册输入法，也不修改日常配置；模型通过参数或环境变量传入。

Validation has three parts: routine checks, release validation, and desktop interaction, all using this repository’s scripts. Automated checks use isolated user preferences and pipes, without registering the input method or changing daily settings. Models are supplied by arguments or environment variables.

## 日常检查 / Routine checks

准备 README 所列工具后，在仓库根目录运行：

After preparing the tools listed in the README, run from the repository root:

```powershell
pixi install --locked
pixi run python tools/download_core.py
pixi run package
pixi run verify
```

构建树为 `build/x64` 和 `build/x86`。x86 只构建 TSF，通过隔离 IPC 连接 x64 后台，不需要 x86 core。`verify` 运行两个架构的 CTest、无模型的后台冒烟测试和注册辅助进程测试；PR 与手动检查复用这些入口。

Build trees are `build/x64` and `build/x86`. x86 builds only TSF and connects to the x64 broker through isolated IPC; no x86 core is needed. `verify` runs CTest for both architectures, model-free broker smoke tests, and registration-helper process tests. Pull requests and manual checks reuse these entry points.

`pixi run test` 会先构建，再运行两个架构的 CTest、无模型 IPC 和注册辅助检查。无模型检查验证词典和 KenLM 路线、配置与学习、取消、私密字段、IPC 生命周期和 TSF 输入行为；不将缺失深度模型当作发布推理成功。

`pixi run test` builds first, then runs CTest for both architectures, model-free IPC and registration-helper checks. Model-free checks cover dictionary and KenLM paths, settings and learning, cancellation, private fields, IPC lifecycle, and TSF input behavior. Missing neural models are never counted as successful release inference.

CTest 要求 x64 的 `common`、`engine`、`tsf_policy`、`tsf_smoke` 以及 x86 的两项 TSF 测试存在并通过；允许新增测试，必要测试缺失或跳过则失败。报告与发现日志保存在 `build/<arch>/test-results`。

CTest requires x64 `common`, `engine`, `tsf_policy`, `tsf_smoke` and both x86 TSF tests to exist and pass. Additional tests are allowed; missing or skipped required tests fail verification. Reports and discovery logs are saved under `build/<arch>/test-results`.

## SDK 与缓存 / SDK and caches

`dependencies.json` 固定 `phono-project/phono-core` 的 Windows x64 Release SDK，当前资产为 `phono-core-sdk-0.1.1-windows-x64.zip`。`tools/download_core.py` 校验对应 `SHA256SUMS-windows-x64.txt`，下载到 `.deps/phono-core-0.1.1-windows-x64`，并以 `core-dependency.json` 记录实际 SHA-256。构建使用 SDK 的 C ABI 头文件、DLL 与许可证，不拉取 core 源码。第一次完整构建前，固定 core Release 必须已发布资产。

`dependencies.json` pins the Windows x64 Release SDK from `phono-project/phono-core`, currently `phono-core-sdk-0.1.1-windows-x64.zip`. `tools/download_core.py` verifies `SHA256SUMS-windows-x64.txt`, downloads to `.deps/phono-core-0.1.1-windows-x64`, and records the actual SHA-256 in `core-dependency.json`. Builds use the SDK’s C ABI header, DLL, and licenses without fetching core sources. The pinned core Release must publish the asset before a first complete build.

WebView2 SDK、词库来源和 KenLM 构建依赖使用固定版本；下载与生成结果缓存在 `.deps` 和 `data/zh_CN.lm`。CI 可复用缓存，但 `setup` 负责从无缓存状态准备完整依赖。显式 SDK 覆盖可通过 `PHONO_CORE_SDK_DIR` 或构建入口的 `--core-sdk` 参数传入。

WebView2 SDK, dictionary sources, and KenLM build dependencies use pinned versions. Downloads and generated results are cached in `.deps` and `data/zh_CN.lm`. CI may reuse caches, while `setup` prepares complete dependencies from an empty cache. Supply an explicit SDK override through `PHONO_CORE_SDK_DIR` or the build entry point’s `--core-sdk` argument.

```powershell
pixi run python tools/workflow.py build --core-sdk C:\SDKs\phono-core
```

## 发布验证 / Release validation

更新固定的 core SDK 依赖后，需在 Euphony 仓库运行验证并单独发布；core 发布不自动触发 Euphony 发布。

After updating the pinned core SDK dependency, validate and release Euphony separately; core releases do not automatically publish Euphony.

`pixi.toml` 的 workspace 版本为唯一来源，CMake、资源文件、管理脚本、包目录和 ZIP 均读取该版本。Release 使用 `vX.Y.Z` 标签并校验标签与包版本一致。创建标签并发布对应 GitHub Release 后，`release.published` 检出标签，再构建、测试、打包和上传；正式版与预发布使用同一路径，仅推送标签不触发。上传任务单独授予写权限。

The workspace version in `pixi.toml` is the single source read by CMake, resources, manager scripts, package directory, and ZIP. Releases use `vX.Y.Z` tags and validate matching tag and package versions. Create the tag and publish its GitHub Release; `release.published` checks out the tag, then builds, tests, packages, and uploads. Stable releases and prereleases share this path, while pushing a tag alone does not trigger it. Write permission is granted separately to the upload job.

手动入口的 `release_checks` 可执行真实模型和解压 ZIP 验证，`clean_dependencies` 可跳过依赖缓存以验证首次构建，`desktop_checks` 可在适合的桌面环境运行交互检查。检查产物保存在 Actions；只有已发布 Release 的执行向 Release 上传附件。

The manual entry point’s `release_checks` runs real-model and extracted-ZIP validation, `clean_dependencies` skips dependency caches to check a first build, and `desktop_checks` runs interaction checks in a suitable desktop environment. Checked artifacts are retained in Actions. Only published Release runs attach them to a Release.

打包每次从干净的暂存目录开始，包含两个 TSF、后台、设置、core、词库、KenLM、装卸脚本、版本及许可证。Release 验证解压 ZIP 后运行真实模型的加载和推理，模型缺失或不完整会失败。模型包不随 ZIP 发布。

Packaging starts with a clean staging directory each time and includes both TSF DLLs, broker, settings, core, dictionary, KenLM, management scripts, version information, and licenses. Release validation extracts the ZIP and tests real-model loading and inference. Missing or incomplete models fail validation. Model packages are separate from the ZIP.

本地下载程序将固定修订的 W8A8 模型保存到本仓库 `.cache/models/phonop2c_v2_2_base_w8a8_model`，随后向验证入口显式传入目录：

The local downloader saves the pinned W8A8 revision to this repository’s `.cache/models/phonop2c_v2_2_base_w8a8_model`. Then pass that directory explicitly to validation:

```powershell
pixi run python tools/download_models.py
pixi run python tools/workflow.py release --model-dir .cache/models/phonop2c_v2_2_base_w8a8_model
```

也可设置 `PHONO_TEST_MODEL_DIR`。路径属于调用方，不设相邻仓库默认值。自动下载的发布模型应使用依赖文件固定的 Hugging Face 修订；用户模型下载命令见 README。

Alternatively, set `PHONO_TEST_MODEL_DIR`. The caller owns that path, with no adjacent-repository default. Automatically downloaded release models should use the Hugging Face revision pinned by the dependency file. See the README for user model download commands.

## 诊断 / Diagnostics

工作流即使失败也会尝试将已生成的报告、日志、截图和 smoke 失败配置上传为 `euphony-diagnostics-<run_id>-<attempt>`，保留 7 天。smoke 运行阶段捕获的异常记录在 `build/test-results/temporary-smoke/<instance>/failure.json`；更早的失败请查看 job 日志。

Even on failure, the workflow attempts to upload generated reports, logs, screenshots and failed smoke profiles as `euphony-diagnostics-<run_id>-<attempt>`, retained for seven days. Exceptions caught during smoke execution are recorded in `build/test-results/temporary-smoke/<instance>/failure.json`; consult the job log for earlier failures.

## 桌面验证 / Desktop validation

在有交互桌面及 WebView2 Runtime 的 Windows 环境中，打包后单独运行：

On Windows with an interactive desktop and WebView2 Runtime, run separately after packaging:

```powershell
pixi run python tools/workflow.py desktop
pixi run python tools/tray_test.py --visible
```

桌面检查验证实际设置页面、配置保存、候选外观、原生托盘菜单及图标。报告和设置截图位于 `build/test-results`；部分 TSF 候选和模式提示截图位于对应架构的 `bin` 目录。桌面环境要求与 PR 的无模型检查分开安排。

Desktop checks exercise the actual settings page, saved preferences, candidate appearance, native tray menu, and icons. Reports and settings screenshots live in `build/test-results`; some TSF candidate and mode-hint screenshots live in the corresponding architecture’s `bin` directory. Desktop environment requirements are scheduled separately from model-free PR checks.

安装后在实际应用中检查：空白文本框输入 `ni` 和 `nihao`，确认单字词频与模型路线；提交中文后再输入，检查上文；测试数字和 Space 选词、Enter 提交、Escape 取消、Shift 中英切换；调整横纵候选与模型颜色；确认密码框不推理。最后运行 `Status` 确认后台与 TSF 路径属于同一版本。安装、更新和卸载步骤见 README。

After installation, check real applications: type `ni` and `nihao` in an empty field to exercise unigram and model paths; commit Chinese and type again to check context; test number keys and Space, Enter, Escape, and Shift language switching; change candidate orientation and model color; confirm password fields do not infer. Finally, run `Status` to confirm broker and TSF paths belong to the same version. See the README for installation, updates, and removal.

三个项目独立版本，core 更新通过普通依赖提交验证。若未来自动创建 Release，应将构建和上传留在同一工作流，不依赖跨仓库连锁发布。

The three projects have independent versions, and core updates are validated through normal dependency commits. If Release creation is later automated, keep building and uploading in the same workflow without relying on chained cross-repository releases.
