# stb_image, stb_vorbis

From https://github.com/nothings/stb, commit `003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5`,
unmodified. Licence: MIT or public domain, your choice (`LICENSE.txt`, also at the end of each
file).

- `stb_image.h` v2.30: used by `ui/mod_icons.cpp` to decode the embedded PNG icons. That file is
  the only translation unit that defines `STB_IMAGE_IMPLEMENTATION`, with only the PNG decoder
  enabled (`STBI_ONLY_PNG`, `STBI_NO_STDIO`).
- `stb_image_write.h` v1.16 (same commit, added for the test harness): PNG encoding of screenshots in
  `modules/autotest/png.cpp`, the only translation unit that defines `STB_IMAGE_WRITE_IMPLEMENTATION`
  (`STBI_WRITE_NO_STDIO`; the file is written by the module).
- `stb_vorbis.c` v1.22: Ogg Vorbis decoder for the `slime_dj` module (`.ogg` songs). Compiled
  in `modules/slime_dj/codecs.c` (CMake target `sr2_codecs`, C, warnings off) with
  `STB_VORBIS_NO_PUSHDATA_API`.
