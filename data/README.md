# 词库与语言模型 / Dictionary and language model

Euphony 交付官方 libime 简体中文词库的文本转换 `libime.tsv`，包含 300,234 条词与读音记录，来自 [dict-20260703.tar.zst](https://download.fcitx-im.org/data/dict-20260703.tar.zst) 的 `dict_sc.txt`。`tools/import_libime.py` 保留词语、原读音及 log10 权重，验证 UTF-8、记录格式、读音和有限权重。

Euphony ships `libime.tsv`, a text conversion of the official libime simplified Chinese dictionary with 300,234 word / pronunciation records, from `dict_sc.txt` in [dict-20260703.tar.zst](https://download.fcitx-im.org/data/dict-20260703.tar.zst). `tools/import_libime.py` preserves word text, original pronunciations, and log10 weights, validating UTF-8, record shape, pronunciations, and finite weights.

引擎自行构建查询 Trie，支持单字、词组、不完整音节和声母查询，不需要 Fcitx5 或 libime 运行库。组合词束搜索使用 KenLM 条件概率、词库权重及本地明确选词。解码采用 Euphony 自身的有界 beam，未实现模糊拼音或 libime 独立预测 Trie。

The engine builds its own lookup tries for characters, phrases, incomplete syllables, and initials without Fcitx5 or libime runtime libraries. Its word-combination beam uses KenLM conditional probabilities, dictionary weights, and explicit local selections. Decoding uses Euphony’s bounded beam; fuzzy pinyin and libime’s separate prediction trie are not implemented.

`tools/prepare_libime_lm.py` 从 [lm_sc.arpa-20260629.tar.zst](https://download.fcitx-im.org/data/lm_sc.arpa-20260629.tar.zst) 获取官方语言模型，用 KenLM 的 `-s -a 22 -q 4 trie` 参数生成 `zh_CN.lm` 并随安装包交付。源文件缓存在 `.deps/libime-lm`，KenLM 工具由固定 vcpkg manifest 安装。

`tools/prepare_libime_lm.py` obtains the official language model from [lm_sc.arpa-20260629.tar.zst](https://download.fcitx-im.org/data/lm_sc.arpa-20260629.tar.zst), generates `zh_CN.lm` with KenLM’s `-s -a 22 -q 4 trie` options, and includes it in packages. Sources are cached in `.deps/libime-lm`; the pinned vcpkg manifest installs the KenLM tool.

词库与语言模型的下载来源、版本和 SHA-256 在 `dependencies.json` 中固定。日常构建直接使用已提交的 `data/libime.tsv`；`pixi run setup` 准备 KenLM 和语言模型。更新词库时显式运行转换脚本并提交生成结果；独立复现词库或模型可运行：

`dependencies.json` pins download sources, versions, and SHA-256 for the dictionary and language model. Routine builds use the committed `data/libime.tsv`; `pixi run setup` prepares KenLM and the language model. When updating the dictionary, run the conversion script explicitly and commit the output. To reproduce the dictionary or model separately, run:

```powershell
pixi run python tools/import_libime.py --download --output data/libime.tsv
pixi run python tools/prepare_libime_lm.py
```

默认 `prefer_unigram_single_syllable` 只在无应用上文、无会话提交历史且完整输入为单音节时，按单字词典候选的一元概率排序，不提交 Phono 推理。双拼完整两键展开为一个音节时同样适用。简拼、不完整音节、多音节或有上文的输入仍使用 Phono；关闭选项也恢复单音节推理。语言模型加载失败时报告错误并保留词典 / 深度模型回退。

The default `prefer_unigram_single_syllable` setting ranks single-character dictionary candidates by unigram probability only for a complete single syllable without application context or local commit history, without queuing Phono inference. A complete Shuangpin pair expanded to one syllable follows the same rule. Initials, incomplete syllables, multiple syllables, and contextual input retain Phono. Disabling the setting restores single-syllable inference. Language-model load failures report an error and retain dictionary / neural-model fallback.

## 许可与来源 / Licensing and provenance

词库使用 LGPL-2.1-or-later，copyright 2002–2005 Yuking `<yuking_net@sohu.com>`、2014–2026 CSSlayer `<wengxt@gmail.com>`。语言模型使用 LGPL-2.1-or-later，copyright 2022–2026 CSSlayer。许可依据见 [上游 REUSE.toml](https://github.com/fcitx/libime/blob/master/REUSE.toml)，文本在源码的 `data/licenses/LGPL-2.1-or-later.txt` 及安装包的 `licenses/LGPL-2.1-or-later.txt`。

The dictionary uses LGPL-2.1-or-later, copyright 2002–2005 Yuking `<yuking_net@sohu.com>` and 2014–2026 CSSlayer `<wengxt@gmail.com>`. The language model uses LGPL-2.1-or-later, copyright 2022–2026 CSSlayer. See [upstream REUSE.toml](https://github.com/fcitx/libime/blob/master/REUSE.toml); the license text is in the source tree’s `data/licenses/LGPL-2.1-or-later.txt` and the package’s `licenses/LGPL-2.1-or-later.txt`.

七组双拼键位数组来自 libime 的 [shuangpindata.h](https://github.com/fcitx/libime/blob/master/src/libime/pinyin/shuangpindata.h)，使用 LGPL-2.1-or-later，copyright 2011–2020 CSSlayer。worker、core C ABI 调度、非法输入展示和固定两键转换语义改编自 [phono-fcitx5-addon](https://github.com/phono-project/phono-fcitx5-addon)，Apache-2.0 许可证保留在 `licenses/PHONO-ADDON-APACHE-2.0.txt`。其余依赖声明见 [THIRD_PARTY.md](../THIRD_PARTY.md)。

Seven Shuangpin key arrays come from libime’s [shuangpindata.h](https://github.com/fcitx/libime/blob/master/src/libime/pinyin/shuangpindata.h), under LGPL-2.1-or-later, copyright 2011–2020 CSSlayer. Worker behavior, core C ABI orchestration, invalid-input presentation, and fixed-pair conversion semantics are adapted from [phono-fcitx5-addon](https://github.com/phono-project/phono-fcitx5-addon), with its Apache-2.0 license retained in `licenses/PHONO-ADDON-APACHE-2.0.txt`. See [THIRD_PARTY.md](../THIRD_PARTY.md) for other dependency notices.
