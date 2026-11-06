# tools/psp

Host-side helpers for the PSP port. **Nothing here is used by the normal build** (`make`,
`build_psp.ps1`); the game is compiled from your own ROMs exactly as before.

## make_release.py

Splits a finished build into a template that contains no data taken from the Nintendo ROM, plus a
small manifest that says where that data has to be copied from. An installer (on the PSP, or
`--install` on a PC) combines the template, the manifest and the user's own ROM into the same
`SM64.PRX` the normal build produces, byte for byte.

```
# 1. build the version as usual (it must be built, this tool only reads the result)
# 2. split it
python3 tools/psp/make_release.py --version us --verify --leak-check
# -> dist/psp_release/us/{SM64.template.prx, manifest.bin, report.txt}

# reference installer (what the on-device installer has to do)
python3 tools/psp/make_release.py --install dist/psp_release/us --rom us=baserom.us.z64
# -> dist/psp_release/us/SM64.PRX
```

* One package per version (`us`, `jp`, `eu`, `sh`). Shindou needs both the SH and the JP ROM.
* Only the Python standard library is required. Each run takes a couple of minutes.
* `--verify` rebuilds the module from ROM + template + manifest and compares it with the original.
* `--leak-check` scans the template for anything that still matches the ROM assets (should be 0).
* The template and manifest belong to one exact build: after any code change, build again and
  run the tool again.
* The ROM may be `.z64`, `.v64` or `.n64`; it is identified by its SHA-1, never by its file name.

The manifest format is documented at the top of the script.

### Release that needs no PC (on-device installer)

`psp/installer/` is a small replacement for `psp/launcher/` used only by this kind of release. Its EBOOT finds
the user's ROM (in `data/` or next to the EBOOT, any name, any byte order), checks its SHA-1, rebuilds
`SM64.PRX` from `SM64.template.prx` + `manifest.bin`, verifies the result and starts the game. Later starts go
straight to the game. If `manifest.bin` is not there it behaves exactly like the normal launcher.
It has its own Makefile and the main `Makefile` is not involved.

```
# one command, the same one as the normal build with `release` instead of `pbp`:
docker run --rm -v "${PWD}:/sm64" -w /sm64 sm64psp make TARGET_PSP=1 release VERSION=us -j8
# or with the PowerShell script:   .\build_psp.ps1 -Version us -Release
# -> dist/psp_release/us/PSP/GAME/SM64_US/{EBOOT.PBP, SM64.template.prx, manifest.bin, data/README.txt}
# (add FAST=1 to skip the slow leak check; -Fast in the script)

# several versions in a row, no Makefile involved:
docker run --rm -v "${PWD}:/sm64" -w /sm64 sm64psp sh tools/psp/release.sh all

# or, by hand, after the regular build of a version:
python3 tools/psp/make_release.py --version us --verify --leak-check --pack
```

The `release` target of the Makefile (and `release.sh`) only run the existing `make TARGET_PSP=1 pbp VERSION=<v>` and then
this tool; the normal build is not changed. `release.sh` also takes `FAST=1` (skip the slow leak check), `SKIP_BUILD=1`
(repeat only the packaging) and `JOBS=8`. The release always uses the default `build/` folder (not the -O2 one).

`--pack` runs `make -C psp/installer VERSION=us` (PSP toolchain needed) or takes a ready EBOOT with
`--installer path/EBOOT.PBP`. Copy the `PSP` folder to the Memory Stick; the user puts their ROM in `data/`.
The installer shows its messages in the language of the PSP: English, French, German, Spanish, Italian and Japanese
(anything else falls back to English). The texts are in `psp/installer/installer_text.h`. The PSP debug font is plain
ASCII, so the Latin texts have no accents and the Japanese ones are drawn from bitmaps: after changing a Japanese
text run `python3 tools/psp/gen_installer_ja.py` (needs Pillow and a Japanese font) to regenerate
`psp/installer/text_ja.inc`. `data/README.txt` of the package is written in the same six languages.
The French, German, Italian and Japanese texts were written without native review; corrections are welcome.

`make -C psp/installer host-test` builds a PC driver (`build/host_test DIR OUT ROM [ROM]`) for the same installer code.
