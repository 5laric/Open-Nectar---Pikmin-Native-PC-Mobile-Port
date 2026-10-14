# Vendored netplay libraries

Source-only copies for online co-op. Nothing here is built by default: CMake
only builds them with `-DPIKMIN_NETPLAY_BUILD=ON`.

| Directory | Upstream | Pinned commit | License |
|---|---|---|---|
| `gekkonet/` | https://github.com/HeatXD/GekkoNet | `3b21722ad09824638ed6eb43282c463008053a91` (2026-09-02, release v20260902101441-3b21722) | BSD-2-Clause (`gekkonet/LICENSE`) |
| `gekkonet/GekkoLib/thirdparty/zpp/` | zpp_bits, as bundled by GekkoNet | same | MIT (`gekkonet/GekkoLib/thirdparty/zpp/LICENSE`) |

These libraries keep their own licenses. The repository's CC0 waiver does not
cover them.

## What was copied

Files were copied with `git archive` at the pinned commit. Nothing in them has
been modified (line endings are normalised to LF by git).

- `LICENSE`, `README.md`, `GekkoLib/include`, `GekkoLib/src`, `GekkoLib/thirdparty/zpp`
- Excluded: the bundled ASIO (`GekkoLib/thirdparty/asio`), `Examples/`, `docs/`, the MSVC project files and
  the CI workflows.

## How it is built

- `GEKKONET_NO_ASIO` and `GEKKONET_STATIC`. The game supplies its own transport through `GekkoNetAdapter`
  (`pc_port/netplay/pc_netplay_udp.cpp`).
- The library needs C++20 (`-std=gnu++20`), so it is its own static target outside the game's C++17 options.
- Its quoted includes are searched with `-iquote` first: on case-insensitive file systems the game's
  `include/Event.h` would otherwise shadow GekkoNet's private `event.h`.
- The game's own code includes only `gekkonet.h`, which is C-compatible.
