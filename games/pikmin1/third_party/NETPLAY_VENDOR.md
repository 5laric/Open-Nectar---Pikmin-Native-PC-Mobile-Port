# Vendored netplay libraries

Source-only copies for online co-op. Nothing here is built by default: CMake
only builds them with `-DPIKMIN_NETPLAY_BUILD=ON`.

| Directory | Upstream | Pinned commit | License |
|---|---|---|---|
| `gekkonet/` | https://github.com/HeatXD/GekkoNet | `3b21722ad09824638ed6eb43282c463008053a91` (2026-09-02, release v20260902101441-3b21722) | BSD-2-Clause (`gekkonet/LICENSE`) |
| `gekkonet/GekkoLib/thirdparty/zpp/` | zpp_bits, as bundled by GekkoNet | same | MIT (`gekkonet/GekkoLib/thirdparty/zpp/LICENSE`) |
| `libjuice/` | https://github.com/paullouisageneau/libjuice | `b89c792e3612faf2f12cf35bcc56857313a06be3` (tag v1.7.4) | MPL-2.0 (`libjuice/LICENSE`) |

These libraries keep their own licenses. The repository's CC0 waiver does not
cover them.

## What was copied

Files were copied with `git archive` at the pinned commits. Nothing in them has
been modified (line endings are normalised to LF by git).

GekkoNet:
- `LICENSE`, `README.md`, `GekkoLib/include`, `GekkoLib/src`, `GekkoLib/thirdparty/zpp`
- Excluded: the bundled ASIO (`GekkoLib/thirdparty/asio`), `Examples/`, `docs/`, the MSVC project files and
  the CI workflows.

libjuice:
- `LICENSE`, `README.md`, `include`, `src`, `CMakeLists.txt` (kept for reference; the game uses its own target)
- Excluded: `test/`, `fuzzer/`, `cmake/`, `Makefile` and CI.

MPL-2.0 is a file-level copyleft. If a libjuice file is ever modified, that file
stays under MPL-2.0 and its source must remain available; keep such patches as
separate, clearly marked commits.

## How it is built

GekkoNet:
- `GEKKONET_NO_ASIO` and `GEKKONET_STATIC`. The game supplies its own transport through `GekkoNetAdapter`
  (`pc_port/netplay/pc_netplay_udp.cpp`).
- The library needs C++20 (`-std=gnu++20`), so it is its own static target outside the game's C++17 options.
- Its quoted includes are searched with `-iquote` first: on case-insensitive file systems the game's
  `include/Event.h` would otherwise shadow GekkoNet's private `event.h`.
- The game's own code includes only `gekkonet.h`, which is C-compatible.

libjuice:
- Its own static C11 target from `libjuice/src/*.c`, with `JUICE_STATIC` and `USE_NETTLE=0` (the built-in
  SHA/HMAC, no Nettle). It links `ws2_32`, `bcrypt` and the platform threads.
- The game's ICE transport (`pc_port/netplay/pc_netplay_ice.cpp`) includes only `juice/juice.h`.
