# dr_libs (dr_mp3, dr_wav)

Single-file decoders from https://github.com/mackron/dr_libs, unmodified:

| file | version | tag / commit |
|---|---|---|
| `dr_mp3.h` | v0.7.3 (2026-01-17) | `mp3-0.7.3`, `5690d4671d7ad07ae6021756d7222eb159745f06` |
| `dr_wav.h` | v0.14.5 (2026-03-03) | `wav-0.14.5`, `fa931f3285ced10ace628f7f1ac951e1951e7ea6` |

Licence: public domain (Unlicense) or MIT No Attribution, your choice (`LICENSE`, also at the
end of each header).

Used by the `slime_dj` module to decode `.mp3` and `.wav` songs. The implementations are
compiled once, in `modules/slime_dj/codecs.c` (CMake target `sr2_codecs`, C, warnings off),
together with `third_party/stb/stb_vorbis.c`.
