# Third-party code

Small libraries that are copied into the repository so that `core/` and `handheld/` build with
nothing to download. Only those two folders use these copies; the web app, Kerf Lite (`plugin/`)
and the desktop app do not.

| File | What it is | Version | Licence |
|--|--|--|--|
| `dr_wav.h` | WAV decoder | 0.14.6 | public domain or MIT-0, your choice |
| `dr_mp3.h` | MP3 decoder | 0.7.4 | public domain or MIT-0, your choice |
| `dr_flac.h` | FLAC decoder | 0.13.4 | public domain or MIT-0, your choice |
| `font8x8_basic.h`, `font8x8_ext_latin.h` | 8x8 bitmap font (the text of the handheld screen) | commit 8e279d2 | public domain |

Where they come from:

- dr_wav, dr_mp3 and dr_flac: https://github.com/mackron/dr_libs (David Reid), commit
  `b4264c5c1f61c1d460aced4cf4d8a8dec197ca48`. The copies are unchanged.
- font8x8: https://github.com/dhepper/font8x8 (Daniel Hepper), commit
  `8e279d2d864e79128e96188a6b9526cfa3fbfef9`. One edit in each of the two files: the table is
  declared `const unsigned char` instead of `char`, because the original does not compile as C++
  (the values above 127 do not fit a signed `char`).

The licence texts are in `licenses/`. SDL2 (zlib licence) is not part of the sources: it is used
as a shared library. The Windows package ships `SDL2.dll`, so `licenses/SDL2-LICENSE.txt` is
included for it; on a handheld the system's own SDL2 is used.
