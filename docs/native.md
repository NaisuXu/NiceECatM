# Build and user guide

**English** | [简体中文](native.zh-CN.md) | [README](../README.md)

## Build prerequisites

Windows x64 requires CMake 3.28+, MinGW-w64 with a C++17 compiler, Make and windres on PATH. Run:

```powershell
./scripts/build-windows.ps1 -Jobs 4
```

The script configures, builds, runs CTest and installs to `dist/windows-x64/`. Add `-Clean` to reset configuration and rebuild objects from the vendored sources. The application is `NiceECatM.exe`; its application dependencies and fonts are statically linked or embedded. Windows system libraries remain runtime prerequisites.

Ubuntu requires a C++17 compiler, Make, CMake 3.28+ and development libraries for the SDL display backend available on the machine. The script builds for the host CPU:

```sh
./scripts/build-linux.sh
```

The result is installed to `dist/linux-$(uname -m)/NiceECatM`. Add `--clean` for a configuration reset and target rebuild; set `NICEECATM_JOBS` to choose parallelism. Real EtherCAT access requires permission to use raw Ethernet sockets. Linux builds and hardware operation require validation on the target x86 or ARM machine.

Background release detection uses Windows WinHTTP or optional libcurl on Linux. Install `libcurl4-openssl-dev` before configuring Ubuntu to enable it. Without libcurl, the Linux build keeps the clickable Releases link and skips network detection. Normal desktop launches check GitHub once; CLI and UI smoke runs make no update requests. Detection never uses the EtherCAT transport, blocks the UI, opens a dialog or writes to the Log. Release tags must use the build version `yyyymmddhhmm` (an optional `v` prefix is accepted); drafts and prereleases are excluded.

A Linux executable linked against shared libcurl also needs the matching runtime library on its destination system (normally Ubuntu's `libcurl4` package); build-time fallback does not remove that runtime dependency from an already linked binary.

SOEM, SDL, Dear ImGui, Lua, TinyXML2 and the code editor are provided at fixed versions in `third_party/`. Configuration and compilation use these local sources and need no dependency downloads once the platform prerequisites are installed. Dependency setup is centralized in `cmake/dependencies.cmake`; versions, source archive hashes and adaptations are recorded in [THIRD_PARTY.md](../THIRD_PARTY.md).

## Build directories

`windows-mingw-release` is a CMake preset describing the platform, compiler and build type. Keeping `build/windows-mingw-release/` separate from `build/linux-release/` prevents caches and object files from different toolchains from being mixed.

CMake build directories are generated; `build/<preset>/_deps/` contains compiled dependency outputs. Removing them keeps the dependency sources in `third_party/`, so rebuilding does not require network access. `dist/` contains installed applications and may also contain user-created files beside the executable. Preserve any manually stored backups before clearing `build/`, and user files before replacing an installation.

Equivalent manual commands are:

```powershell
cmake --preset windows-mingw-release
cmake --build --preset windows-mingw-release --parallel 4
ctest --preset windows-mingw-release
cmake --install build/windows-mingw-release --prefix dist/windows-x64 --strip
```

Use `-DNICEECATM_BUILD_GUI=OFF` at configuration to build the CLI without SDL or ImGui. `NICEECATM_BUILD_TESTS` controls the test targets.

## Create a Windows release package

Run from a source checkout after the Release build and tests succeed:

```powershell
./scripts/package-windows.ps1
```

The script uses the existing build and installs into a clean private stage. It creates `dist/NiceECatM-<yyyymmddhhmm>-windows-x64.zip` and `.zip.sha256`. The archive contains a named application folder with the executable, concise English/Chinese user README files, the project license, release third-party notices, packaged ESI and example scripts. Detailed build, architecture, validation, ESI and Lua guides stay in the source checkout; the package has no `docs/` directory. It excludes local `preferences.ini` and user scripts from the development installation, and leaves `dist/windows-x64/` intact.

Release README files and third-party notices are authored in `resources/release/` and installed at the package root. Their relative links are written for that installed layout. The source checkout's root README and third-party inventory remain development documentation.

Extract the archive to a writable directory and start its `NiceECatM.exe`. Release packaging does not install Npcap or change the system driver.

## Portable files

| File or directory beside the executable | Purpose |
| --- | --- |
| `NiceECatM.exe` / `NiceECatM` | Application |
| `LICENSE` | Project copyright and license: Naisu Xu; GPL-3.0-only |
| `licenses/` | Third-party license notices |
| `README.md`, `README.zh-CN.md` | Concise English and Chinese user guides with a script section |
| `THIRD_PARTY.md` | Third-party notices |
| `esi/` | Manufacturer ESI XML files |
| `scripts/` | Lua files and packaged examples |
| `preferences.ini` | UI language, created or updated by the application |

The application resolves these paths from the executable directory, independently of the current working directory. Use a writable directory when changing preferences or scripts. ESI XML and Lua files are editable data; no additional language runtime is installed.

Software versions use local build time in `yyyymmddhhmm`. The UI, CLI help and Windows version resource share the generated value. The Windows icon is compiled with windres and does not require rcedit.

## Connecting a bus

Windows hardware access requires an installed Npcap or compatible WinPcap driver. Driver installers and capture DLLs are not bundled. Without the driver, the GUI and built-in simulation remain available, and hardware enumeration reports the failure.

Select a physical wired adapter and scan. Single-port operation uses one interface. Same-bus dual-port redundancy uses two different interfaces connected to opposite ends of the slave chain. Normal and overlapped mapping are independent of port count. Disconnect before changing Simulation/Hardware/Programming, connection mode, interfaces or mapping, then scan again. Stop scripts before changing the operating mode.

Scanning is an active SOEM configuration and mapping operation. It leaves responding slaves in PRE-OP; it is not a passive packet capture. Select a discovered slave in the topology to inspect its signals, identity, registers or SDO objects.

## States and cyclic I/O

| Action | Result |
| --- | --- |
| BOOT | With cyclic I/O and scripts stopped, validates the target scope and enters BOOT through INIT using bootstrap mailboxes; invalidates PDO mapping |
| INIT | Requests INIT and stops cyclic exchange |
| PRE-OP | Requests PRE-OP and stops cyclic exchange |
| SAFE-OP | Requests SAFE-OP and starts cyclic exchange; slave outputs remain in their safe state |
| OP | Requests OP and starts cyclic exchange; valid output PDOs may be applied by the slave |
| Disconnect | Stops exchange and closes the master connection |

A state request can fail; check actual AL state and the Log rather than assuming the request succeeded. The top controls are **BOOT / INIT / PRE-OP / SAFE-OP / OP**. The target selector applies a request to all slaves or the topology selection. BOOT validates every target before changing state and confirms each target individually. Exiting BOOT restores normal mailboxes through INIT; scan again before restarting cyclic I/O. The actual slave state includes AL error flags.

The target cycle can be changed while exchange is stopped and all slaves are in INIT/PRE-OP. Available values are 500 us, 1, 2, 5, 10, 20, 50 and 100 ms; the default is 10 ms. Timing is measured on the host and is subject to operating-system and driver scheduling.

Windows cyclic scheduling uses a high-resolution waitable timer. The Npcap receive budget per frame is `min(max(target cycle, 2 ms), 10 ms)`. This budget does not guarantee the target cycle; actual timing deviations and skipped cycles are still recorded.

Background AL-state monitoring distinguishes an unreadable `NONE` result from a confirmed state change. If PDO WKC is complete, a failed AL read may retain that slave's last confirmed state for at most 2 seconds from the first failed read. Invalid PDO WKC or expiration clears that retained state; a fresh AL reply is then required before writing outputs. Real INIT, SAFE-OP and error replies are adopted immediately. Explicit state requests always require fresh confirmation. The Log records the first failed AL read, cache invalidation and recovery; these notices are separate from script callback errors and PDO WKC failures. Continuous PDO failure still stops exchange after 2 seconds.

## Workbench pages

- **Device / DC:** device identity, mapped layout information and declared capabilities. DC trigger configuration/readback is not connected.
- **ESC registers:** physical ESC offset and byte data. Writes are limited by the implemented register rules and current state.
- **BufferPDO:** signals for the selected slave and direct output editing. Trend plots are hidden by default. Click **Monitor** for a signal to capture one `float` sample on every valid I/O cycle in a 2,000-sample ring, then click **Stop** to remove that signal from capture. The workbench refreshes the displayed snapshot about every 100 ms, while the I/O worker keeps the per-cycle samples. At 500 us this is about 1 second of history, and at 10 ms it is about 20 seconds. Input fields are read-only. Cached/stopped data retains its previous trend.
- **CoE / SDO:** searchable object dictionary with always-visible subindices, descriptions, data types, bit lengths, access flags, source and explicitly read values. Group headers use a tinted background; their entries are indented, with no collapse controls. Matching ESI descriptions are available without mailbox requests. Entering this page in confirmed PRE-OP discovers online descriptions once when SDO Information is declared and no script is running; Refresh dictionary retries explicitly. Online metadata takes priority and ESI supplements missing descriptions. Discovery is bounded, may be partial, and never reads all values or changes AL state. Manual Index/SubIndex access remains available. Unknown encodings use raw bytes.
- **FoE:** selected-target file upload/download in confirmed PRE-OP or BOOT, with progress, cancellation and bounded timeouts/retries.
- **Scripts:** Lua editor, saved files, API snippets and automation. See the [script guide](scripts.md).
- **EEPROM:** Programming-only image preparation/export, EEPROM read/save and selected-target programming with complete readback comparison.
- **Diagnostics:** WKC, host timing, bus context and a 200-sample deviation history; its tab follows EEPROM.
- **Log:** shared application and script messages with Follow and Clear controls. Select one entry and press Ctrl+C to copy its complete displayed line.

FoE follows CoE / SDO, and Diagnostics follows EEPROM. Programming selects EEPROM and disables every other page; EEPROM is disabled in Hardware/Simulation.

Writes do not require a global unlock. State, direction, size, mapping and object permissions are always enforced. In PRE-OP/SAFE-OP an output-image write prepares the master image; it does not establish physical output activation. Synchronous register and SDO operations require cyclic exchange to be stopped.

## FoE file transfer

In Hardware/Simulation, stop scripts and move all slaves to PRE-OP before transferring. Select the target, upload/download direction, vendor-defined device filename and 32-bit password, then the local file or save destination. Use the shared BOOT control only if the device's procedure requires it. Files are limited to 128 MiB; uploads default to a 16 MiB receive limit. Defaults are 3 s response, 30 s no-progress, 300 s total timeout and 3 retries. The page shows progress and cancellation; details use the global Log, and passwords are not saved or logged.

For slave-reported errors, the page and global Log show the protocol code, a localized standard reason and any original slave text; generic or vendor-specific errors cannot reliably distinguish an unsupported filename from an incorrect password.

**FoE completion confirms file transfer; whether firmware is activated is determined by the slave.** Cancellation cannot undo accepted slave data; failed/cancelled uploads preserve the original local destination. EEPROM support does not establish FoE support. See the [release workflow](../resources/release/README.md#foe-file-transfer) for steps and the [architecture](design.md#foe-transfer-and-boot-sessions) for protocol boundaries. Lua/CLI file-transfer APIs are not implemented.

## EEPROM programming

Stop scripts, disconnect and select **Programming** to enable EEPROM. Open a supported ESI device description or SII BIN; preparing/exporting an image works without a connection. Select one adapter and scan to INIT without PDO mapping, then choose the physical target in the left topology. **Read & save** reads its EEPROM. **Program EEPROM** confirms the target, image and capacity, writes the prepared image including Alias/Serial and padding, then compares the full readback. Vendor ID/Product Code changes are shown and allowed; matching identifiers do not prove hardware compatibility. Capacity from SII is a declaration, not a physical measurement. See [image support and validation](esi.md#eeprom-images).

Use **Export ESI** to save a basic ESI XML from the loaded image or a validated EEPROM read; it works offline. ESI export includes only EEPROM/SII data and cannot recover the full object dictionary or initialization commands.

Cancelling after writing starts may leave a partial image; there is no rollback. The mode remains Programming. Follow the device's reload or power-cycle requirements, disconnect, select Hardware and scan again for normal I/O; OP is not resumed automatically.

## CLI

```powershell
./dist/windows-x64/NiceECatM.exe --help
./dist/windows-x64/NiceECatM.exe --adapters
./dist/windows-x64/NiceECatM.exe --simulate --scan
./dist/windows-x64/NiceECatM.exe --scan --interface "<adapter name>"
./dist/windows-x64/NiceECatM.exe --read-register 0x130 --length 6 --slave 1 --interface "<adapter name>"
./dist/windows-x64/NiceECatM.exe --sdo-read 0x1008 --sub 0 --length 128 --slave 1 --interface "<adapter name>"
./dist/windows-x64/NiceECatM.exe --programming-scan --interface "<adapter name>"
./dist/windows-x64/NiceECatM.exe --eeprom-read backup.bin --slave 1 --interface "<adapter name>"
./dist/windows-x64/NiceECatM.exe --sii-export device.xml --esi-device 0 --output device.bin
./dist/windows-x64/NiceECatM.exe --esi-export backup.bin --output device.xml
```

CLI operations return JSON. Hardware operations require an explicitly selected adapter. Ordinary operations scan/configure before the requested operation and close the backend afterwards; the CLI is not a persistent cyclic runner. `--programming-scan` and `--eeprom-read` use the dedicated INIT inventory path without PDO mapping. `--sii-export` (ESI to BIN) and `--esi-export` (BIN to basic ESI) are offline. Options include `--secondary NAME`, `--overlap` and `--cycle-us`. GUI language can be selected with `--language en` or `--language zh-CN`; `--esi-image file.xml` (or `file.bin`) opens the offline Programming page without scanning or opening a capture connection.

See [validation](native-validation.md) for reproducible checks and [architecture](design.md) for implemented boundaries.
