# 验证与发布 / Validation and releases

[安装与使用 / Installation and usage](../README.md)

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

`pixi run test` 仅运行两个架构的 CTest。无模型检查验证词典和 KenLM 路线、配置与学习、取消、私密字段、IPC 生命周期和 TSF 输入行为；不将缺失深度模型当作发布推理成功。

`pixi run test` runs only CTest for both architectures. Model-free checks cover dictionary and KenLM paths, settings and learning, cancellation, private fields, IPC lifecycle, and TSF input behavior. Missing neural models are never counted as successful release inference.

日常检查要求 x64 的 `common`、`engine`、`tsf_policy`、`tsf_smoke` 以及 x86 的两项 TSF 测试存在并通过；允许增加测试，但必要测试缺失或跳过会失败。CTest 列表、JUnit 和输出日志保存在 `build/<arch>/test-results`。可用 `pixi run python -m unittest discover -s tests -p test_verification.py -v` 单独检查验证脚本的失败路径。

Routine checks require x64 `common`, `engine`, `tsf_policy`, `tsf_smoke` and both x86 TSF tests to exist and pass. Additional tests are allowed; missing or skipped required tests fail verification. CTest listings, JUnit and output logs are saved under `build/<arch>/test-results`. Run `pixi run python -m unittest discover -s tests -p test_verification.py -v` to test verification failure paths separately.

## SDK 与缓存 / SDK and caches

`dependencies.json` 固定 `phono-project/phono-core` 的 Windows x64 Release SDK，当前资产为 `phono-core-sdk-0.1.1-windows-x64.zip`。`tools/download_core.py` 校验对应 `SHA256SUMS-windows-x64.txt`，下载到 `.deps/phono-core-0.1.1-windows-x64`，并以 `core-dependency.json` 记录实际 SHA-256。构建使用 SDK 的 C ABI 头文件、DLL 与许可证，不拉取 core 源码。第一次完整构建前，固定 core Release 必须已发布资产。

`dependencies.json` pins the Windows x64 Release SDK from `phono-project/phono-core`, currently `phono-core-sdk-0.1.1-windows-x64.zip`. `tools/download_core.py` verifies `SHA256SUMS-windows-x64.txt`, downloads to `.deps/phono-core-0.1.1-windows-x64`, and records the actual SHA-256 in `core-dependency.json`. Builds use the SDK’s C ABI header, DLL, and licenses without fetching core sources. The pinned core Release must publish the asset before a first complete build.

WebView2 SDK、词库来源和 KenLM 构建依赖使用固定版本；下载与生成结果缓存在 `.deps` 和 `data/zh_CN.lm`。CI 可复用缓存，但 `setup` 负责从无缓存状态准备完整依赖。显式 SDK 覆盖可通过 `PHONO_CORE_SDK_DIR` 或构建入口的 `--core-sdk` 参数传入。

WebView2 SDK, dictionary sources, and KenLM build dependencies use pinned versions. Downloads and generated results are cached in `.deps` and `data/zh_CN.lm`. CI may reuse caches, while `setup` prepares complete dependencies from an empty cache. Supply an explicit SDK override through `PHONO_CORE_SDK_DIR` or the build entry point’s `--core-sdk` argument.

```powershell
pixi run python tools/workflow.py build --core-sdk C:\SDKs\phono-core
```

## 发布验证 / Release validation

`pixi.toml` 的 workspace 版本为唯一来源，CMake、资源文件、管理脚本、包目录和 ZIP 均读取该版本。Release 使用 `vX.Y.Z` 标签并校验标签与包版本一致。创建标签并发布对应 GitHub Release 后，`release.published` 检出标签，再构建、测试、打包和上传；正式版与预发布使用同一路径，仅推送标签不触发。上传任务单独授予写权限。

The workspace version in `pixi.toml` is the single source read by CMake, resources, manager scripts, package directory, and ZIP. Releases use `vX.Y.Z` tags and validate matching tag and package versions. Create the tag and publish its GitHub Release; `release.published` checks out the tag, then builds, tests, packages, and uploads. Stable releases and prereleases share this path, while pushing a tag alone does not trigger it. Write permission is granted separately to the upload job.

手动入口的 `release_checks` 可执行真实模型和解压 ZIP 验证，`clean_dependencies` 可跳过依赖缓存以验证首次构建，`desktop_checks` 可在适合的桌面环境运行交互检查。检查产物保存在 Actions；只有已发布 Release 的执行上传附件。

The manual entry point’s `release_checks` runs real-model and extracted-ZIP validation, `clean_dependencies` skips dependency caches to check a first build, and `desktop_checks` runs interaction checks in a suitable desktop environment. Checked artifacts are retained in Actions. Only published Release runs attach them to a Release.

打包每次从干净的暂存目录开始，包含两个 TSF、后台、设置、core、词库、KenLM、装卸脚本、版本及许可证。Release 验证解压 ZIP 后运行真实模型的加载和推理，模型缺失或不完整会失败。模型包不随 ZIP 发布。

Packaging starts with a clean staging directory each time and includes both TSF DLLs, broker, settings, core, dictionary, KenLM, management scripts, version information, and licenses. Release validation extracts the ZIP and tests real-model loading and inference. Missing or incomplete models fail validation. Model packages are separate from the ZIP.

`dependencies.json` 同时记录发布模型文件的大小和 SHA-256。下载器检查已有文件及新下载，发布验证也在启动推理前检查显式模型目录。校验失败会指出文件，不自动覆盖已有模型；请检查所选目录，或移走错误文件后重新下载。更新模型时应一起更新 revision 和文件清单。直接运行 `smoke.py --model-dir` 仍可试验自定义模型。

`dependencies.json` also records release model file sizes and SHA-256 hashes. The downloader checks existing files and new downloads; release verification checks the supplied model directory before inference. A mismatch identifies the file without overwriting existing models. Check the selected directory, or move the incorrect file aside before downloading again. Update the revision and file manifest together. Direct `smoke.py --model-dir` calls still support custom models.

本地下载程序将固定修订的 W8A8 模型保存到本仓库 `.cache/models/phonop2c_v2_2_base_w8a8_model`，随后向验证入口显式传入目录：

The local downloader saves the pinned W8A8 revision to this repository’s `.cache/models/phonop2c_v2_2_base_w8a8_model`. Then pass that directory explicitly to validation:

```powershell
pixi run python tools/download_models.py
pixi run python tools/workflow.py release --model-dir .cache/models/phonop2c_v2_2_base_w8a8_model
```

也可设置 `PHONO_TEST_MODEL_DIR`。路径属于调用方，不设相邻仓库默认值。自动下载的发布模型应使用依赖文件固定的 Hugging Face 修订；用户模型下载命令见 README。

Alternatively, set `PHONO_TEST_MODEL_DIR`. The caller owns that path, with no adjacent-repository default. Automatically downloaded release models should use the Hugging Face revision pinned by the dependency file. See the README for user model download commands.

## 桌面验证 / Desktop validation

工作流即使失败也尝试上传 `euphony-diagnostics-<run_id>-<attempt>`，保留 7 天，包括 CTest 报告、日志、已有截图和失败 smoke 的隔离配置。smoke 成功时清理临时配置，失败时将错误写入对应 `build/test-results/temporary-smoke/<instance>/failure.json` 并保留配置；控制台输出仍由 Actions 保存。上传失败或 runner 丢失时无法保证文件送达；尚未生成报告的早期失败以 job 日志为准。

The workflow attempts to upload `euphony-diagnostics-<run_id>-<attempt>` even on failure, retaining CTest reports, logs, existing screenshots and failed smoke profiles for seven days. Successful smoke tests clean up their temporary profiles; failures retain the profile and write `build/test-results/temporary-smoke/<instance>/failure.json`. Actions continues to retain console output. Artifact delivery cannot be guaranteed after runner loss or upload failure; early failures without reports are diagnosed from job logs.

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
