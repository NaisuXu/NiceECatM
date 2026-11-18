# Third-party components

This inventory records the dependencies used by NiceECatM. NiceECatM's own source code is licensed under [GPL-3.0-only](LICENSE), GNU General Public License version 3 only. Third-party code, fonts and manufacturer ESI files retain their respective licenses and notices; the project's license does not replace them.

The project's own copyright notice is Copyright (C) 2026 Naisu Xu; see [LICENSE](LICENSE).

| Component | Version / source | Location | License notice |
| --- | --- | --- | --- |
| SOEM | 2.0.1, commit `49fc34923b973c17700592319159af22157350c9` | `third_party/soem/` | GPLv3 / commercial notice, `LICENSE.md` |
| Lua | 5.4.7 | `third_party/lua/` | MIT-style Lua license, `COPYRIGHT` |
| TinyXML2 | 11.0.0 | `third_party/tinyxml2/` | zlib, `LICENSE.txt` |
| ImGuiColorTextEdit | Vendored `TextEditor.cpp` / `TextEditor.h` snapshot | `third_party/imgui_color_text_edit/` | MIT, `LICENSE` |
| SDL | 3.2.20, commit `96292a5b464258a2b926e0a3d72f8b98c2a81aa6` | `third_party/sdl/` | zlib, `LICENSE.txt` |
| Dear ImGui | 1.92.5, commit `6d910d5487d11ca567b61c7824b0c78c569d62f0` | `third_party/imgui/` | MIT, `LICENSE.txt` |
| Noto Sans SC | Embedded regular-font subset | `resources/fonts/workbench_font.cpp` | SIL OFL 1.1, `resources/fonts/OFL.txt` |
| Cousine | Bundled regular TTF and embedded code-editor font | `resources/fonts/Cousine-Regular.ttf`, `cousine_font.cpp` | SIL OFL 1.1, `resources/fonts/Cousine-OFL.txt` |

## Dependency management

`cmake/dependencies.cmake` owns dependency targets. All code dependencies are vendored under `third_party/` at the versions listed above. Configuration and compilation use local sources, and generated files stay inside the active build preset. Removing `build/` does not remove dependency source or require a dependency download on the next build.

The SOEM, SDL and Dear ImGui source copies come from the upstream commit archives below. These SHA256 values record their provenance; CMake does not download these archives during a build.

| Component | Source archive | SHA256 |
| --- | --- | --- |
| SOEM | [Pinned upstream archive](https://codeload.github.com/OpenEtherCATsociety/SOEM/tar.gz/49fc34923b973c17700592319159af22157350c9) | `a13d201373019e0d9d6178fe0eccc41a26920231583bd08498600e808678cd0d` |
| SDL | [Pinned upstream archive](https://codeload.github.com/libsdl-org/SDL/tar.gz/96292a5b464258a2b926e0a3d72f8b98c2a81aa6) | `4ae5eba49e7e346f742bea4c9966f4cc871face4b999513fcc1eb1f14d4dec30` |
| Dear ImGui | [Pinned upstream archive](https://codeload.github.com/ocornut/imgui/tar.gz/6d910d5487d11ca567b61c7824b0c78c569d62f0) | `b715a3226320aaebaea79dc6af6405ae37cdcb479817fddaef12a17d8bd15a3a` |

SOEM's version follows its upstream `v2.0.1` release tag and pinned commit. That release's unchanged `CMakeLists.txt` still declares `VERSION 2.0.0`; that value does not identify the vendored release.

The vendored Lua image excludes file, OS, package and debug libraries and the command-line interpreters. The script runtime exposes only its selected standard libraries and registered host APIs.

## Local adaptations

- `cmake/patches/soem-sdo.cmake` checks the normalized SHA256 of the pinned SOEM CoE source and produces a patched copy in `build/<preset>/generated/soem/ec_coe.c`. It retains v2.0.1's unsigned length accounting and segmented-capacity checks, and adds segmented-loop deadlines, response length/identity/command/toggle validation and mailbox ownership cleanup on early failure. In-flight send/receive calls retain their own timeouts, so completion can exceed the remaining loop budget. Configuration leaves the vendored upstream file unchanged.
- Its companion `cmake/patches/soem-sdoinfo.cmake` validates SDO Information response lengths, service and object identity, and bounds multipart object-list discovery. These application guards preserve the upstream API and are covered by mock mailbox tests; no current object values are uploaded during dictionary discovery.
- `src/platform/pcap_runtime.c` replaces Windows hard imports of wpcap/Packet with a narrow runtime binding to the installed system capture driver. Capture uses absolute system paths and an EtherCAT filter.
- `src/ethercat/soem_backend.cpp` verifies primary and secondary port initialization separately and manages SOEM through the controller's single I/O owner. Mapping validation rejects failed or over-capacity segment results while accepting a valid zero-byte map. Mailbox queue cleanup is delegated to upstream `ecx_close()`.
- The code editor is built from its vendored C++ source and receives application-owned Lua text and editor settings.
- Noto Sans SC was subset and embedded. Its ascent/descent uses the original typographic metrics for compact rendering. Cousine is compressed and embedded for code editing. Regeneration inputs/notices are documented in `resources/fonts/README.md`.

Third-party license files are installed in `licenses/` beside the executable. Capture drivers are separately installed system components; no Npcap/WinPcap installer, capture DLL or SDK binary is bundled.

`resources/esi/` contains manufacturer descriptions supplied for the ASIX AX58100 and Microchip LAN9252 test devices. They are installed as editable XML data and retain their original content. `resources/examples/` contains the application's Lua examples.
