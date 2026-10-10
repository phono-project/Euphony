# Euphony 0.2.0 验证与装卸说明

当前发布目录 `dist/Euphony-0.2.0`；ZIP `dist/Euphony-win64.zip`。只使用这一版本的 `tools/manage.ps1`。源码构建树统一为 `build/x64` 和 `build/x86`。

已完成的验证：

| 检查 | 结果 |
| --- | --- |
| x64 CTest：策略、真实 TSF、配置、引擎 | 4/4 通过 |
| x86 CTest：策略、真实 TSF，连接 x64 后台 | 2/2 通过 |
| 候选窗：无布局 / 零矩形、首次位置、连续输入、横纵排列和鼠标选择 | 两个架构通过 |
| 中英切换：英文不吞字母、图标提示显示 / 自动隐藏、切回中文 | 两个架构通过 |
| 发布包：IPC、隔离 / 取消、私密字段、配置和学习保存 | 通过 |
| libime 官方词库、KenLM、单字一元路线 / 关闭路线 / 有上文路线 | 通过 |
| phono_core.dll 动态加载指定 W8A8 模型 | 通过；生成 你好 / 你号 / 泥嚎 |
| 实际 WebView2：设置绑定、颜色 / 排列、校验、资源选择、自绘边框与图标 | 通过，保留截图 |
| 原生托盘菜单：快捷配置保存与恢复，图标添加重试 / 重建 | 通过 |
| 日常后台：正常停止旧进程、资源路径迁移、0.2.0 后台启动与版本诊断 | 通过 |
| 日常托盘：仅自身 GUID 的 IsPromoted=1，Shell 返回有效图标矩形 | 通过 |
| 日常菜单：实际鼠标处的原生菜单，逐显示器 DPI 与灰度文字 | 已实际显示并捕获检查 |
| 注册辅助进程：真实 GUI 进程等待、退出 0/5、空格 / 中文路径、发布脚本语法 | 通过，不写系统注册表 |

当前系统已经卸载 TSF 注册，日常后台已换成 0.2.0。ChatGPT 与 Explorer 仍载入原始 DLL，因此 `dist/Euphony` 暂时保留；不会强制结束这些应用。已清理 build-release / build-release-x86 / build-x86、原 build 根目录的旧产物、旧 ZIP、旧 0.1.1 发布包和重复测试配置。保留实际回归测试，移除重复任务入口；修正两个架构共用测试实例与跨进程窗口查找造成的测试串扰。日后注销再登录，运行 clean 可去掉最后的旧包。

## 从源码目录重复测试

```powershell
pixi run package
pixi run verify
# 可选：短暂创建并移除独立 Shell 测试图标
pixi run python tools/tray_test.py --visible
```

自动测试使用独立用户目录与管道，不注册输入法，不修改日常设置。临时测试配置自动清理。报告在 `build/test-results`；设置截图在其 `settings` 子目录，菜单截图为 `native-menu.png`。候选与中英提示截图在各架构的 `bin` 子目录。模型固定使用相邻的 `phonop2c_v2_2_base_w8a8_model`。

## 安装、验收、卸载

从 Euphony 源码目录，在**同一 Windows 账户的管理员 PowerShell** 执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File dist/Euphony-0.2.0/tools/manage.ps1 Install
```

安装前检查四个组件的文件版本都是 0.2.0；正常停止旧后台，注册 x64 / x86；仅迁移原安装自带的 core、词库和 KenLM 路径，保留外部自定义路径、模型选择和其他设置。修改配置前保留一份备份并原子替换。

**注销 Windows，再登录**。卸载 / 重注册不会卸载已经载入应用的 DLL。之后在普通 PowerShell 打开设置，再用 Win+Space 选择 Euphony：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File dist/Euphony-0.2.0/tools/manage.ps1 Settings
powershell -NoProfile -ExecutionPolicy Bypass -File dist/Euphony-0.2.0/tools/manage.ps1 Status
```

Status 应显示两个 TSF 路径和后台路径都位于 0.2.0、后台版本 0.2.0，没有 old-path=True。若后台尚未启动，打开设置或输入后再检查。

空文本框测试 ni 的单字词频路线和 nihao 的淡绿色模型候选；修改颜色并重新输入；Shift 测试英文直输与状态图标；检查横纵候选、连续打字无原点闪现，右键菜单在鼠标附近且没有彩色文字边缘；关闭设置后托盘仍常显。完整手动清单见 README.md。

卸载在同账户的管理员 PowerShell 中执行，保留用户配置和学习数据：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File dist/Euphony-0.2.0/tools/manage.ps1 Uninstall
```

注销再登录后才能删除旧包。源码清理：

```powershell
pixi run clean
```

本轮没有重新写系统 TSF 注册，也没有通过自动化注销或关闭用户应用。实际注册后的不同应用验收仍需按上述步骤完成。Windows 11 的托盘常显已在本机验证；旧 Shell 的兼容接口已实现，但没有独立 Windows 10 环境进行实际验证。