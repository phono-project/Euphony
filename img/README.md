# Euphony artwork

`euphony.svg` is the original vector mark, also used by the settings page.
`euphony.png` is its 512 px transparent export. `euphony.ico` contains native
Windows sizes from 16 to 256 px and is embedded in all three programs / DLLs.
`window-controls.svg` records the caption glyphs drawn by the Win32 frame.
`mode-chinese.svg` / `mode-english.svg` are the original Chinese / English
status marks. Their PNG and ICO exports are used by the language bar,
broker tray and temporary mode hint.

Regenerate PNG / ICO with `pixi run python tools/generate_icons.py`.
The generator reads the SVG's rounded rectangles / polygons and needs only Python's
standard library. All artwork is original and covered by the project license.
