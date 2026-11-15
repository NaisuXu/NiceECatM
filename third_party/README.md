# Dependency layout

Build integration is centralized in [dependencies.cmake](../cmake/dependencies.cmake).
Versions, upstream sources and licenses are recorded in [THIRD_PARTY.md](../THIRD_PARTY.md).

| Directory | Component | Version |
| --- | --- | --- |
| `soem/` | EtherCAT master | 2.0.1, commit `49fc34923b973c17700592319159af22157350c9` |
| `lua/` | Script runtime | 5.4.7 |
| `tinyxml2/` | ESI XML parser | 11.0.0 |
| `imgui_color_text_edit/` | Lua editor widget | Vendored source with its MIT license |
| `sdl/` | Windowing and rendering platform | 3.2.20, commit `96292a5b464258a2b926e0a3d72f8b98c2a81aa6` |
| `imgui/` | Immediate-mode GUI | 1.92.5, commit `6d910d5487d11ca567b61c7824b0c78c569d62f0` |

SOEM's upstream source remains unchanged. The checked SDO patch in
`cmake/patches/soem-sdo.cmake` produces `build/<preset>/generated/soem/ec_coe.c`;
the library and regression tests both compile that generated copy.
The adaptation retains v2.0.1's length and capacity checks and adds segmented-loop
deadlines, response validation and mailbox ownership cleanup on early failure.
In-flight send/receive calls retain their own timeouts.

The SOEM release is identified by its `v2.0.1` tag and pinned commit. Its
upstream `CMakeLists.txt` still declares `VERSION 2.0.0`, so that value alone
must not be used to identify the release.

All code dependencies are supplied in this directory. With the platform's
compiler, CMake and system development libraries installed, even a clean build
can run offline. `build/<preset>/_deps/` contains compiled dependency outputs;
removing the build directory leaves the dependency sources intact.

The SOEM, SDL and Dear ImGui copies retain their upstream sources and licenses.
Their pinned archive URLs and SHA256 provenance are recorded in
[THIRD_PARTY.md](../THIRD_PARTY.md). CMake uses these local source directories
directly and does not download or unpack dependencies during configuration.

Embedded UI and editor fonts and their licenses are in `resources/fonts/`.
