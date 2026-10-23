# Overlay fonts

The overlay embeds two fonts as compressed C arrays, so nothing is read from disk at run time. `theme.cpp` includes the `.inc` files and loads them with `ImFontAtlas::AddFontFromMemoryCompressedTTF`.

| file | font | use |
|---|---|---|
| `nunito_bold.inc` | Nunito, weight 700 | body text, widgets, tooltips |
| `fredoka_semibold.inc` | Fredoka, weight 600 | sidebar, tabs, card headings |

Both are SIL Open Font License 1.1 (`OFL-Nunito.txt`, `OFL-Fredoka.txt`). The OFL allows embedding and modified copies; the subsets below keep the original names, which the licence allows because these are not distributed as standalone font files.

## How they were made

Sources: `ofl/nunito/Nunito[wght].ttf` and `ofl/fredoka/Fredoka[wdth,wght].ttf` from https://github.com/google/fonts (main branch, 2026-10-23).

```sh
pip install fonttools
python -m fontTools.varLib.instancer "Nunito[wght].ttf" wght=700 -o Nunito-Bold-full.ttf
python -m fontTools.varLib.instancer "Fredoka[wdth,wght].ttf" wght=600 wdth=100 -o Fredoka-SemiBold-full.ttf
U="U+0020-007E,U+00A0-017F,U+0218-021B,U+2010-2027,U+2030-203A,U+20AC,U+2122,U+2190-2193,U+2212,U+00B0"
python -m fontTools.subset Nunito-Bold-full.ttf --unicodes="$U" --layout-features='kern,liga' --no-hinting --desubroutinize --output-file=Nunito-Bold.ttf
python -m fontTools.subset Fredoka-SemiBold-full.ttf --unicodes="$U" --layout-features='kern,liga' --no-hinting --desubroutinize --output-file=Fredoka-SemiBold.ttf
binary_to_compressed_c -u32 Nunito-Bold.ttf nunito_bold > nunito_bold.inc
binary_to_compressed_c -u32 Fredoka-SemiBold.ttf fredoka_semibold > fredoka_semibold.inc
```

`binary_to_compressed_c` is `misc/fonts/binary_to_compressed_c.cpp` from Dear ImGui. Characters outside the subset (Basic Latin, Latin-1, Latin Extended-A, common punctuation) fall back to Segoe UI, which `theme.cpp` merges into the body font when it is installed.
