# Third-party code and data

NanoSync's own code is licensed under the GNU General Public License, version 3 (`LICENSE`). The following third-party material is included or required.

## Included in this repository

| Component | Files | Author / source | License | Notes |
|---|---|---|---|---|
| HashAB | `core/calcHashAB.c`, `core/calcHashAB.h`, `core/cipher.c`, `core/cipher.h`, `core/cipher_tables.c`, `core/cipher_tables.h`, `core/expand.c`, `core/expand.h`, `core/hash.c`, `core/hash.h`, `core/reduce.c`, `core/reduce.h`, `core/byteorder.h` | [dstaley/hashab](https://github.com/dstaley/hashab) | The Unlicense (public domain) | License text: `core/LICENSE.hashab`. |
| EDON-R 256 | `core/edonr.c`, `core/edonr.h` | Aleksey Kravchenko (RHash), as bundled by dstaley/hashab | ISC-style permissive license in the file header | The header of `core/edonr.c` must stay with the file. |
| TJpgDec R0.03 | `core/tjpgd.c`, `core/tjpgd.h` (`core/tjpgdcnf.h` is NanoSync's configuration) | ChaN, (C) 2021 | Permissive, "no restriction on use"; source redistributions must retain the copyright notice | License text: `core/LICENSE.tjpgd`. The notice at the top of `core/tjpgd.c` is unchanged. |

Everything else under `core/` (`mdb_*`, `sha1.c`) and `src/`, `tools/`, `tests/` was written for this project and is under
the project's license (GPL-3.0). The permissive components above are compatible with it.

## Not included, required to build

| Component | Where | License | Notes |
|---|---|---|---|
| NanoApps SDK (`sdk/`, `tools/`, `lvgl` fork) | [nfzerox/NanoApps](https://github.com/nfzerox/NanoApps); the fork inside [olievans123/nano7-untethered](https://github.com/olievans123/nano7-untethered) under `nanoapps/` | MIT, (c) 2026 nfzerox (`nanoapps/LICENSE`) | **Not copied here.** `tools/build.sh` copies `src/` and `core/` into `<sdk>/apps/nanosync` and runs the SDK's own `make`. LVGL is MIT as well. |
| GNU Arm Embedded toolchain | your system | GPL/LGPL, used as a tool | Not distributed. |
| Untethered firmware and loader | olievans123/nano7-untethered | see that repository (its firmware builder is GPL-3.0) | Not included. NanoSync is only an app that the loader starts. |

## Used only as a reference, no code copied

- Apple's iTunesCDB / ArtworkDB / SQLite library formats were studied from files produced by a stock iPod and, as a
  correctness check during development, compared with the output of [iOpenPod](https://github.com/TheRealSavi/iOpenPod)
  (GPL-3.0). No iOpenPod source file is part of this repository and no test in this repository depends on it.

## Provenance notes

- `core/mdb_tags_alias.h` is a table of tag field aliases (ID3 frame ids, MP4 atoms, Vorbis/ASF names) and the lookup
  priority in `core/mdb_tags.c` follows the behavior of iOpenPod (GPL-3.0).
- `core/mdb_schema.h` (table and column definitions of the three `.itdb` SQLite databases) and `core/mdb_tmpl.h` (zeroed
  template of a track record) were generated from blank library data produced by iOpenPod. They describe Apple's file
  formats and contain no program code.
- `core/mdb_sqlgen.c` and `core/mdb_cdb.c` reproduce the same CDB to SQLite projection and sort orders as iOpenPod,
  verified by comparing outputs during development.

For these reasons the project is released under GPL-3.0, the same license as iOpenPod.
