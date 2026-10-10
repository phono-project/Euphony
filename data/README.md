Euphony ships the official libime simplified Chinese dictionary text export as
`libime.tsv`. It contains 300,234 word/pronunciation records imported from:
https://download.fcitx-im.org/data/dict-20260703.tar.zst

The `dict_sc.txt` payload is converted by `tools/import_libime.py`; spelling,
word text and log10 weights are retained. The Euphony C++ engine constructs its
own lookup tries. No Fcitx or libime runtime library is included or required.
Single-character, phrase, incomplete-syllable and initials lookup are supported.
The word-combination beam uses KenLM conditional probabilities, exported
dictionary weights and local explicit dictionary selections. It compares whole
words with split paths, including phrases present in the lexicon. The decoder
is Euphony's bounded beam, rather than libime's complete decoder; fuzzy pinyin
and libime's separate prediction trie are not implemented.

`tools/prepare_libime_lm.py` downloads the official libime language model:
https://download.fcitx-im.org/data/lm_sc.arpa-20260629.tar.zst
and builds `data/zh_CN.lm` with KenLM's `-s -a 22 -q 4 trie` options, matching
the binary type used by libime. The generated model is included in packages.
The model is LGPL-2.1-or-later, copyright 2022-2026 CSSlayer, as documented in
the same upstream REUSE metadata. Its download / ARPA source remains under
`.deps/libime-lm`; the shipped model can be reproduced without Fcitx APIs.

The default `prefer_unigram_single_syllable` setting uses only null-context
unigram probabilities when there is no surrounding or locally committed
context and the complete input is one syllable with single-character dictionary
candidates. Phono inference is not queued for that input. A full shuangpin pair
expanded to one syllable follows the same rule. Initial-only, incomplete,
multiple-syllable, or contextual input retains Phono inference. Turning the
setting off also restores Phono inference for a single syllable. If the language
model cannot load, status reports the error and input retains the usual
dictionary/deep-model fallback.

Dictionary data licensing, as documented by upstream `REUSE.toml`:
https://github.com/fcitx/libime/blob/master/REUSE.toml

- SPDX-License-Identifier: LGPL-2.1-or-later
- SPDX-FileCopyrightText: 2002-2005 Yuking <yuking_net@sohu.com>
- SPDX-FileCopyrightText: 2014-2026 CSSlayer <wengxt@gmail.com>

`src/engine/shuangpin_tables.h` retains only the seven profile mapping arrays
from `libime/src/libime/pinyin/shuangpindata.h`, retrieved on 2026-10-09.
https://github.com/fcitx/libime/blob/master/src/libime/pinyin/shuangpindata.h
They are LGPL-2.1-or-later, copyright 2011-2020 CSSlayer. Euphony's profile table
construction and native lookup implementation use no Fcitx APIs.

The worker, core C ABI orchestration, invalid-input presentation, and fixed-pair
conversion semantics are adapted from the local `phono-fcitx5-addon` project.
Its Apache-2.0 license is preserved in `licenses/PHONO-ADDON-APACHE-2.0.txt`.
Changes replace link-time core calls with explicit DLL symbol loading, remove
the Fcitx runtime dependency, and apply worker results through the broker's
request thread. LGPL text is in `licenses/LGPL-2.1-or-later.txt`.

Refresh from the official data source with:
`pixi run python tools/import_libime.py --download --output data/libime.tsv`
An existing text export can be supplied as the positional source argument.
The importer validates record shape, UTF-8, pronunciations and finite weights.
