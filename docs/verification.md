# 开发、验证与发布 / Development, validation, and releases

[安装与使用 / Installation and usage](../README.md)

## 架构与交付 / Architecture and distribution

x64 和 x86 应用通过各自的 TSF 前端连接同一个 x64 后台，后台调用 phono-core 生成模型候选。设置界面使用 WebView2。交付物为 Inno Setup EXE 安装包和 portable ZIP 包；前者自动完成注册与卸载，后者通过管理脚本注册输入法。

TSF frontends for x64 and x86 applications connect to one x64 broker, which calls phono-core to generate model candidates. Settings use WebView2. Distribution includes an Inno Setup EXE installer with registration and uninstallation, plus a portable ZIP registered through the management script.

## 安装包构建与验证 / Installer build and validation

安装 Inno Setup 6.3+ 后运行 `pixi run installer`，复用 `package` 的干净载荷，同时保留 ZIP。已打包时可运行 `pixi run python tools/workflow.py installer`，通过 `--iscc` 指定编译器路径；省略时从 PATH 或 Inno Setup 6 默认目录查找。CI 使用 Windows runner 预装的 Inno Setup 6。

Install Inno Setup 6.3+ and run `pixi run installer` to reuse the clean `package` payload and retain the ZIP. After packaging, run `pixi run python tools/workflow.py installer`; use `--iscc` to specify the compiler path, or omit it to search PATH and the default Inno Setup 6 directory. CI uses the Windows runner's preinstalled Inno Setup 6.

`installer/Euphony.iss` 使用 `regserver` 和明确的 `64bit` / `32bit` 标记调用已有 TSF DLL 入口，安装和卸载不启动 PowerShell。安装器要求在同一账户下提权，记录现有 `HKCU\Software\Euphony` 发现路径。覆盖安装沿用原目录；文件占用时由 Inno 安排重启替换与注册，卸载可延迟到重启删除占用文件。用户配置和模型不属于安装器载荷。

`installer/Euphony.iss` uses `regserver` with explicit `64bit` / `32bit` flags to call existing TSF DLL entry points, without launching PowerShell during installation or removal. Elevate under the same account; the installer records the existing `HKCU\Software\Euphony` discovery paths. Upgrades keep the installation directory. Inno schedules locked-file replacement and registration for restart, and can defer deletion of locked files until restart. User preferences and models are outside the installer payload.

Release 或手动勾选 `release_checks` 时，CI 在一次性 Windows runner 上运行 `pixi run python tools/installer_test.py`：静默安装到含空格与中文的目录，逐文件对比载荷，检查两种 COM / TSF 注册及 Windows 卸载入口，覆盖安装后再次检查载荷和注册，再卸载并检查注册清理和配置、学习数据保留。后台 IPC 行为由日常 smoke 检查，不在安装生命周期中重复执行。普通 PR 仍构建安装器，但跳过安装生命周期检查。日志与结果在 `build/test-results/installer/`。该测试会修改机器注册，要求管理员权限，并拒绝在已有 Euphony 注册的机器上运行。

For Releases or manual runs with `release_checks` selected, CI runs `pixi run python tools/installer_test.py` on its disposable Windows runner: silently install into a path containing spaces and Chinese characters, compare every payload file, check both COM / TSF registrations and the Windows uninstall entry, reinstall and recheck the payload and registrations, then uninstall and verify registration cleanup and retention of preferences and learning data. Broker IPC behavior is covered by routine smoke checks and is not repeated during the installer lifecycle. Ordinary PRs still build the installer but skip lifecycle checks. Logs and results are in `build/test-results/installer/`. This test changes machine registrations, requires elevation, and refuses hosts with an existing Euphony registration.

## 候选生成策略 / Candidate generation

词典候选先返回，模型候选异步补充；用户开始浏览候选后，该轮列表停止更新。默认开启的 `prefer_unigram_single_syllable` 在无上文且仅有一个完整音节时优先使用 KenLM 单字词频；有上文、连续拼音或关闭选项时使用模型。KenLM 不可用时回到通常的候选生成流程。模型未配置、禁用或加载失败时保留词典候选。

Dictionary candidates return first, followed asynchronously by model candidates. Once the user starts browsing, the list stops updating for that composition. The default `prefer_unigram_single_syllable` option prefers KenLM single-character frequency for one complete syllable without context. Contextual input, continuous pinyin, or disabling the option uses the model. If KenLM is unavailable, the usual candidate generation path applies. Dictionary candidates remain available if the model is unconfigured, disabled, or fails to load.

## 验证范围 / Validation scope

测试包括日常检查、安装器生命周期、发布验证和桌面交互，均复用本仓库脚本。日常与桌面验证使用隔离用户配置和管道；安装器检查在一次性 runner 上注册并卸载输入法。模型通过参数或环境变量传入。

同一 PR 的新提交会取消旧的 CI 执行；Release 与手动执行使用各自的运行分组，不受 PR 取消影响。

Validation includes routine checks, installer lifecycle, release validation, and desktop interaction, all using this repository’s scripts. Routine and desktop checks use isolated user preferences and pipes. Installer checks register and remove the input method on a disposable runner. Models are supplied by arguments or environment variables.

New commits to a PR cancel its older CI runs. Release and manual runs use their own run groups and are unaffected by PR cancellation.

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

CTest 运行 x64 的 `common`、`engine`、`tsf_policy`、`tsf_smoke` 以及 x86 的两项 TSF 测试。直接使用 CTest 的退出码和 `--no-tests=error` 判定结果，JUnit 报告与执行日志保存在 `build/<arch>/test-results`。

CTest runs x64 `common`, `engine`, `tsf_policy`, `tsf_smoke` and both x86 TSF tests. Results use CTest's exit code and `--no-tests=error` directly. JUnit reports and execution logs are saved under `build/<arch>/test-results`.

## SDK 与缓存 / SDK and caches

`dependencies.json` 固定 `phono-project/phono-core` 的 Windows x64 Release SDK，当前资产为 `phono-core-sdk-0.1.1-windows-x64.zip`。`tools/download_core.py` 下载到 `.deps/phono-core-0.1.1-windows-x64`，并检查包内版本与固定版本一致。构建使用 SDK 的 C ABI 头文件、DLL 与许可证，不拉取 core 源码。第一次完整构建前，固定 core Release 必须已发布资产。

`dependencies.json` pins the Windows x64 Release SDK from `phono-project/phono-core`, currently `phono-core-sdk-0.1.1-windows-x64.zip`. `tools/download_core.py` downloads to `.deps/phono-core-0.1.1-windows-x64` and checks that the installed version matches the pinned version. Builds use the SDK’s C ABI header, DLL, and licenses without fetching core sources. The pinned core Release must publish the asset before a first complete build.

WebView2 SDK、词库来源和 KenLM 构建依赖使用固定版本。CI 只缓存 `.deps/vcpkg-installed`、`.deps/webview2`、core SDK 目录和 `data/zh_CN.lm`，不保存 vcpkg 构建树、下载目录或解压后的 ARPA 中间文件。已有语言模型可直接复用，无需下载源归档；`setup` 仍可从无缓存状态准备完整依赖。显式 SDK 覆盖可通过 `PHONO_CORE_SDK_DIR` 或构建入口的 `--core-sdk` 参数传入。

WebView2 SDK, dictionary sources, and KenLM build dependencies use pinned versions. CI caches only `.deps/vcpkg-installed`, `.deps/webview2`, the core SDK directory, and `data/zh_CN.lm`, excluding vcpkg build trees, download directories, and extracted ARPA intermediates. An existing language model is reused without downloading its source archive; `setup` still prepares complete dependencies from an empty cache. Supply an explicit SDK override through `PHONO_CORE_SDK_DIR` or the build entry point’s `--core-sdk` argument.

```powershell
pixi run python tools/workflow.py build --core-sdk C:\SDKs\phono-core
```

## 发布验证 / Release validation

更新固定的 core SDK 依赖后，需在 Euphony 仓库运行验证并单独发布；core 发布不自动触发 Euphony 发布。

After updating the pinned core SDK dependency, validate and release Euphony separately; core releases do not automatically publish Euphony.

`pixi.toml` 的 workspace 版本为唯一来源，CMake、资源文件、管理脚本、包目录、ZIP 和 EXE 安装包均读取该版本。Release 使用 `vX.Y.Z` 标签并校验标签与包版本一致。创建标签并发布对应 GitHub Release 后，`release.published` 检出标签，再构建、测试、打包和上传；正式版与预发布使用同一路径，仅推送标签不触发。上传任务单独授予写权限。

The workspace version in `pixi.toml` is the single source read by CMake, resources, manager scripts, package directory, ZIP, and EXE installer. Releases use `vX.Y.Z` tags and validate matching tag and package versions. Create the tag and publish its GitHub Release; `release.published` checks out the tag, then builds, tests, packages, and uploads. Stable releases and prereleases share this path, while pushing a tag alone does not trigger it. Write permission is granted separately to the upload job.

手动入口的 `release_checks` 可执行安装器生命周期、真实模型和解压 ZIP 验证，`clean_dependencies` 可跳过依赖缓存以验证首次构建，`desktop_checks` 可在适合的桌面环境运行交互检查。ZIP 与 EXE 检查产物均保存在 Actions；只有已发布 Release 的执行向 Release 上传两种附件。

The manual entry point’s `release_checks` runs installer lifecycle, real-model and extracted-ZIP validation, `clean_dependencies` skips dependency caches to check a first build, and `desktop_checks` runs interaction checks in a suitable desktop environment. Both ZIP and EXE artifacts are retained in Actions. Only published Release runs attach both to a Release.

打包每次从干净的暂存目录开始，包含两个 TSF、后台、设置、core、词库、KenLM、装卸脚本、版本及许可证。Release 验证解压 ZIP 后只运行真实模型的加载、推理、候选提交和配置持久化检查，不重复日常无模型 IPC 行为测试；模型缺失或不完整会失败。模型包不随 ZIP 发布。

Packaging starts with a clean staging directory each time and includes both TSF DLLs, broker, settings, core, dictionary, KenLM, management scripts, version information, and licenses. Release validation extracts the ZIP and checks only real-model loading, inference, candidate commits, and configuration persistence, without repeating routine model-free IPC behavior tests. Missing or incomplete models fail validation. Model packages are separate from the ZIP.

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
