# 图标资源 / Artwork

`euphony.svg` 是本项目原创标志，设置页面也使用它；`euphony.png` 是 512 px 透明导出，`euphony.ico` 包含 16 至 256 px 的 Windows 图标尺寸，嵌入程序与 DLL。`window-controls.svg` 记录 Win32 窗口边框绘制的标题栏符号。

`euphony.svg` is the project’s original mark, also used by settings. `euphony.png` is its 512 px transparent export, and `euphony.ico` contains Windows icon sizes from 16 to 256 px for programs and DLLs. `window-controls.svg` records the caption glyphs drawn by the Win32 frame.

`mode-chinese.svg` 与 `mode-english.svg` 是原创中英状态标志，其 PNG / ICO 导出用于语言栏、后台托盘和临时模式提示。

`mode-chinese.svg` and `mode-english.svg` are original Chinese / English status marks. Their PNG / ICO exports are used by the language bar, broker tray, and temporary mode hint.

重新生成 PNG / ICO：

Regenerate PNG / ICO files:

```powershell
pixi run python tools/generate_icons.py
```

生成器读取 SVG 的圆角矩形与多边形，仅使用 Python 标准库。所有图标为本项目原创，使用项目许可证。

The generator reads rounded rectangles and polygons from SVG and uses only Python’s standard library. All artwork is original and covered by the project license.
