# Third-party code

| File | What | Version | Licence |
|---|---|---|---|
| `stb_image.h` | PNG decoder for the badge pipeline (launcher side only; never in the module) | v2.30, github.com/nothings/stb commit `f58f558c120e9b32c217290b80bad1a0729fbb2c`, SHA-256 `594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3` | public domain or MIT (dual), text at the end of the file |
| `misc-fixed/5x7.bdf` | the pop-ups' font (`display/src/dsd_font_small.c`, made by `display/tools/bdf2font.py`) | X11 misc-fixed, gitlab.freedesktop.org/xorg/font/misc-misc `5x7.bdf` (ucs-fonts 1.38, 2006-01-05), SHA-256 `57197fb80351bd95d00110eb710158706cca883f517bdf3c4db373332ef894ca` | public domain ("Public domain font. Share and enjoy.") |

When the badge pipeline ships in the launcher, add stb_image's licence to `release/licenses/`.
