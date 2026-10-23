# stb_image

`stb_image.h` v2.30 from https://github.com/nothings/stb, commit
`003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5`, unmodified. Licence: MIT or public domain, your
choice (`LICENSE.txt`, also at the end of the header).

Used by `ui/mod_icons.cpp` to decode the embedded PNG icons. That file is the only
translation unit that defines `STB_IMAGE_IMPLEMENTATION`, with only the PNG decoder enabled
(`STBI_ONLY_PNG`, `STBI_NO_STDIO`).
