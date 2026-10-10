# Euphony 0.2.0

Windows 10（1903+）/11 中文输入法，使用 C++20、TSF 和 IPC。x64 / x86 的 TSF 前端连接同一个 x64 后台；后台动态加载本地 phono-core，结合 libime 来源词库和官方 KenLM 模型。没有 Fcitx5 运行时或扩展系统。设置使用本地 WebView2 页面和自绘窗口边框。

## 安装、更新与卸载

**本次唯一发布入口：`dist/Euphony-0.2.0`，压缩包：`dist/Euphony-win64.zip`。** 安装包已经包含两个架构、phono_core.dll、词库、KenLM 和设置前端。模型保留在指定位置，不复制进安装包。

下面从源码项目 `Euphony` 目录执行。如果使用解压后的安装包，将脚本路径换成 `tools/manage.ps1`。

1. 保存正在输入的内容，关闭旧设置窗口。
2. 在**当前 Windows 账户的管理员 PowerShell** 中安装新版。无需先卸载；此入口正常停止旧后台，注册 x64 / x86，并迁移项目自带资源的旧路径：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File dist/Euphony-0.2.0/tools/manage.ps1 Install
```

3. **注销 Windows，再登录。** 已经载入 ChatGPT、Explorer、QQ 等进程的旧 TSF DLL 不会因卸载或重新注册自动退出。单独重开后台不能解决前端仍然旧版的问题。注销能保证所有进程重新载入 DLL。
4. 在普通 PowerShell 打开新版设置：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File dist/Euphony-0.2.0/tools/manage.ps1 Settings
```

5. 用 `Win+Space` 选择 Euphony。设置里确认模型目录为 `phonop2c_v2_2_base_w8a8_model`，词库和 KenLM 已就绪。首次推理会加载 / 预热模型。

检查实际使用的版本与路径，普通 PowerShell 即可；管理员窗口能检查更多进程：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File dist/Euphony-0.2.0/tools/manage.ps1 Status
```

应看到两个架构的 TSF 注册目录、后台路径都指向 `Euphony-0.2.0`，后台版本为 `0.2.0`，已载入模块没有 `old-path=True`。状态检查不启动后台；输入或打开设置后再检查。它同时列出实际的 core / 词库 / KenLM / 模型路径。受保护进程可能无法枚举。

卸载同样使用当前账户的**管理员 PowerShell**，保留配置和学习数据：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File dist/Euphony-0.2.0/tools/manage.ps1 Uninstall
```

卸载后注销再登录，然后才能删除旧安装目录。不要手动删除已注册或仍被应用载入的 DLL。源码里的 `register.ps1` / `unregister.ps1` 是管理入口调用的底层实现，日常只使用 `manage.ps1`。

管理入口还支持 `Start`（正常停止旧后台并启动所选版本）、`Stop`、`Settings` 和 `Status`，无需 Python。`Start` 与 `Settings` 在普通窗口执行；避免把日常后台作为管理员运行。

## 使用与手动测试

- 默认每页 5 个候选，纵向排列；设置和原生托盘菜单都可改为横向，窄屏会换行。
- 模型候选使用淡绿色背景，颜色可在设置中调整；选中项有描边。不再在候选末尾显示来源文字。应用使用自己候选界面时，其配色由应用控制。
- 单独按 Shift 切换中 / 英，语言栏和托盘图标随模式更新；切换时短暂显示对应图标。原始 SVG、PNG 和多尺寸 ICO 在 `img`。
- 单击托盘打开设置；右键原生 Windows 菜单在鼠标附近打开，支持方案、候选数、排列和常用开关。逐显示器 DPI 缩放避免菜单被系统放大成模糊位图；通过原生主题 API 绘制菜单，文字使用灰度抗锯齿，避免 ClearType 彩色边缘，不修改系统全局字体设置。
- 后台只提升自己的图标为常显，Explorer 重建后自动恢复。Windows 11 使用自身 IconGuid 的 IsPromoted，兼容接口处理旧 Shell；不会修改其他图标或全局托盘设置。这些 Shell 兼容机制可能受后续 Windows 更新影响。
- 设置页面完成载入前显示空白底色，之后呈现完整页面，没有居中的加载文字。
- 模型请求没有防抖。新输入立即提交并取消过时任务；35ms 的候选轮询用于接收结果，不延迟推理提交。

安装并重新登录后，按以下顺序验收：

1. 在空白文本框输入 `ni`：默认“无上下文单字优先词频”开启，应使用 libime KenLM 一元词概率；输入 `nihao` 则应出现深度模型候选与淡绿色背景。
2. 先提交一段中文，再输入 `ni`，或关闭单字词频选项，应继续使用深度模型。KenLM 未就绪时单字路线自动退回深度模型。
3. 数字 / Space 选词，Enter 提交，Escape 取消。连续打字，确认候选窗不从屏幕原点闪现；横 / 纵排列均可鼠标选词。
4. 修改模型候选颜色并保存；新一轮输入应采用新颜色，候选末尾没有 AI / 词文字，也没有翻页 / Tab 提示。
5. Shift 切换中 / 英，检查短暂提示、语言栏和托盘图标。切到英文后字母直接输入。
6. 右键托盘，确认菜单在鼠标附近且文字清晰；切换常用设置，再退出设置窗口，后台和常显图标应继续保留。
7. 在密码框输入，Euphony 不读取上文或发送推理输入。
8. 运行 Status，确认后台版本、资源与 TSF 路径一致。卸载后重新登录，确认 Windows 输入菜单中没有 Euphony。

## 构建、自动测试和清理

需要 pixi、Visual Studio 的 C++ 桌面工作负载和 Windows SDK。pixi 管理 CMake、Ninja、Python 和 JSON；仅 KenLM 使用固定 vcpkg manifest 后备方案，需要 vcpkg 位于 PATH 或设置 VCPKG_ROOT。`setup` 会准备微软官方 WebView2 SDK 和官方 libime 语言模型，之后复用已有依赖。

在源码 Euphony 目录：

```powershell
pixi install --locked
pixi run package   # 构建 x64 + x86，生成统一目录与 ZIP
pixi run verify    # 一次运行两个架构的回归测试及发布包的完整验证
pixi run clean     # 清理旧构建、旧 ZIP 和临时产物；保留已载入 / 注册 / 配置引用的输出
```

`verify` 检查真实 TSF 编辑会话、输入 / 选词 / 取消 / 私密字段、候选窗定位和横纵布局、IPC 生命周期、libime / KenLM、指定 W8A8 模型的动态加载、实际 WebView2 设置页面、原生托盘菜单和注册脚本的 GUI 进程退出码。测试使用隔离配置，不注册输入法，也不更改日常设置。`pixi run test` 只运行两个架构的 CTest。

可选的真实 Shell 图标测试：

```powershell
pixi run python tools/tray_test.py --visible
```

该测试短暂添加独立图标，验证添加重试和恢复后移除，不重启 Explorer。自动测试报告与设置截图统一在 `build/test-results`，临时配置自动清理。候选窗截图在 `build/x64/bin`、`build/x86/bin`。清理后仍留下的旧发布目录意味着它仍被使用；注销再登录后重新运行 clean。`build/x64`、`build/x86` 是唯一日常构建树。`pixi run settings / broker / status` 使用已安装目录，不再运行旧 build/bin。

默认使用相邻 `../phono-core/interface` 和 `../phono-core/results/phono_core.dll`。深度测试固定使用 `../phonop2c_v2_2_base_w8a8_model`。重新编译底层库按本地 phono-core 的文档操作。

## 输入和配置

支持全拼及小鹤、自然码、微软、紫光、智能 ABC、中文之星、拼音加加七种双拼。词库选择可逐词继续，模型选择提交整句。浏览候选时冻结该轮异步更新；正在输入的一轮保留原配置，新配置在下一轮应用。

| 按键 | 行为 |
| --- | --- |
| 数字 / Space | 选择候选 |
| = / + / Tab / PageDown | 下一页 |
| - / _ / Shift+Tab / PageUp | 上一页 |
| ↑ / ↓ | 移动候选选择 |
| ← / → / Home / End | 编辑拼音光标 |
| Backspace / Delete | 删除或退回逐词选择 |
| Enter / Escape | 提交 / 取消 |
| 单独 Shift / 语言栏按钮 | 中英切换 |

唯一用户目录为 `%LOCALAPPDATA%/Euphony`：`config.json` 配置、`learning.json` 明确选中的词库词条、`webview/` 页面缓存。更新自带资源路径前保留一份 `config.json.before-update`。模型仍在用户选定的目录。配置保存采用原子替换；损坏的配置保留至用户主动保存。

可关闭读取应用上文；应用无法提供时使用会话连续提交历史。焦点 / 编辑变化使旧结果失效。密码与私密输入不读取上下文、不推理、不学习。IPC 限定本地当前用户和登录会话，有大小与时限约束。解码器由 Euphony 实现，因此不保证所有排序细节与 Linux 插件完全一致。词库、KenLM、图标与参考接口的来源见 data/README.md、img/README.md 和 THIRD_PARTY.md。