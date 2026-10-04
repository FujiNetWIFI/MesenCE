# FujiNet NES cartridge — vendored firmware sources

These files are the FujiNet NES cartridge's own firmware sources, copied
verbatim from `fujinet-firmware` (`pico/nes/firmware`, branch
`nes-bringup`) by `sync.sh`, so that MesenCE runs the protocol and the
mapper engine the cartridge actually ships rather than a lookalike — the
same arrangement as the cartridge's MAME device (`pico/nes/emu`).

| File | What it is |
|---|---|
| `fujimail.c/.h` | the mailbox service: hotspot decode, the SEQ/ACKSEQ interlock, the DBC ROM-push receiver |
| `fujibus.c/.h` | the SLIP-framed FujiBus codec spoken to FujiNet |
| `nesmap.c/.h` | the iNES/NES 2.0 header parser and the mapper engine (mappers 0 1 2 3 4 7 11 30 34 66 71 206) |
| `nes_cart.h` | the bus decode: what the cartridge serves and what a write means |
| `fuji_mailbox.h` | the mailbox address map, the single source of truth |
| `nesloaderrom.h` | the 2K loader ROM served at `$5800` |
| `fujiconfigrom.h` | the CONFIG client: `fujinet-config`'s `nes/` build, converted by `pico/nes/tools/mkromh.py` |
| `fujins.h` | generated: renames the shared symbols so they cannot clash with an in-process FujiNet |
| `SOURCES` | the commits the copies came from |

The only edit is the `#include "fujins.h"` line `sync.sh` puts at the top of
`fujimail` and `fujibus` (as the MAME graft does). The C files are compiled as
C++ through `../FujiNet_*.cpp`, one translation unit each. The transport is
not vendored: `../FujiNetLink.cpp` replaces `pico/nes/emu/fujitcp.c` with a
non-blocking, abortable link that runs on the cartridge's worker thread.

To refresh, build `fujinet-config/nes` (`make`), then run `./sync.sh
[path-to-fujinet-firmware] [path-to-fujinet-config]` and rebuild.
