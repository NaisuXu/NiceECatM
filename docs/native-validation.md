# Validation and release checks

**English** | [简体中文](native-validation.zh-CN.md) | [User guide](native.md)

This document describes reproducible checks for the current desktop application. Software regression and simulated GUI checks do not establish operation or timing against a particular EtherCAT installation.

## Windows build and tests

```powershell
./scripts/build-windows.ps1 -Clean -Jobs 4
./scripts/smoke-windows.ps1
```

The clean option resets CMake configuration and rebuilds targets from `third_party/`. All dependency sources remain available when `build/` is removed, so a clean configure/build can run offline after the platform prerequisites are installed. The preset isolates output in `build/windows-mingw-release/`. To test an existing build directly, use `ctest --preset windows-mingw-release`.

| CTest suite | Coverage |
| --- | --- |
| `core` | Typed data conversion, field boundaries and simulation |
| `controller` | Queues, generations, ownership, state/cycle behavior and snapshots |
| `script_bridge` | Script/controller requests and mapped signal operations |
| `esi_catalog` | XML parsing, identity/layout matching, metadata fallback and offline SDO dictionaries (scalars, records, arrays, permissions, conflicts and invalidation) |
| `pdo_layout` | Process-image extents, failed mapping rejection and valid zero-byte mapping |
| `script_runtime` | Lua limits, host operations, timer/cycle scheduling and cancellation |
| `update_check` | Release JSON and timestamp validation, version comparison, HTTP failures, one-shot worker lifecycle and cancellation, using injected responses without network access |
| `cyclic_state_poll` | Deterministic AL-read fault injection: transient NONE, real state/error replies, per-slave recovery, bounded cache lifetime, PDO invalidation and reconnect isolation |
| `soem_segmented_sdo` | 57 deterministic SDO scenarios: upload/download, Complete Access acknowledgements, invalid requests, loop deadlines, malformed responses and mailbox ownership |
| `soem_sdo_information` | 49 mock mailbox scenarios: description identity/length, list fragmentation, incomplete/skipped fragments, limits, errors and ownership |
| `sdo_dictionary` | 18 enumeration checks: sparse subindices, partial dictionaries, string types, error isolation/classification, immutable metadata and time/request/object limits |

The GUI smoke script uses SDL's dummy video driver and software renderer. It checks all seven pages in English/Chinese, PDO at three window sizes, populated diagnostic history layouts, populated SDO dictionaries at three window sizes, a simulated scan and explicit missing-capture-driver behavior. SDO checks require discovery to complete and the page/dictionary header containers to have no scroll overflow. It never scans a real bus.

Six additional offline checks inject available/current/unavailable release results in English and Chinese at the minimum window size. They verify header scroll bounds and the absence of popups; the available result displays the release hint. `--ui-smoke-update available|current|unavailable` always uses a fixture and never contacts GitHub.

Online dictionary discovery has not been validated against a physical CoE slave with SDO Information. The bundled AX58100 and LAN9252 ESI files describe PDOs only; their absence of dictionary definitions is tested as an empty-description case, not treated as successful online enumeration.

The smoke application is copied to `build/windows-mingw-release/smoke/app/`; screenshots are written under `smoke/`. This separates test preferences and script files from the installed application.

## Package inspection

After the build and smoke checks, run `./scripts/package-windows.ps1` from the source checkout. Verify its `dist/NiceECatM-<yyyymmddhhmm>-windows-x64.zip` against the adjacent `.zip.sha256`, then inspect and test the extracted folder. The clean install stage excludes local development preferences and user scripts.

Check the installed application's filename, icon, 12-digit version and startup without dependency DLLs. Its ESI files, example scripts and third-party notices should match the source resources. The application creates `preferences.ini` and user scripts beside the executable; these are user data, not required build inputs.

Ensure docs links resolve, no build cache or temporary probe program is included in a source package, and a copied Windows package starts from a different working directory. Real adapter access requires Npcap/WinPcap; missing drivers must not prevent simulation or the GUI from opening.

## Hardware acceptance

The target machine, adapters, driver, slave models and cabling define the result. Record them with the requested cycle, measured timing, actual AL states and WKC.

| Area | Required target-hardware evidence |
| --- | --- |
| Scan and ESI | Correct identities, PRE-OP result, mapped directions, bit offsets and description matches |
| State control | Actual INIT/PRE-OP/SAFE-OP/OP, failed transitions and retained error information |
| PDO | Verified input values, output application in OP and safe behavior in SAFE-OP |
| ESC / SDO | Valid and rejected addresses, timeouts and transfer error reporting |
| Mapping | Both normal and overlapped operation with actual image extents/WKC |
| Redundancy | Both interfaces initialize; single-path loss, recovery and dual-path loss are measured |
| Timing | Sustained operation at intended cycles, scheduling deviations and WKC faults |
| Script lifecycle | Repeated runs, cancellation, bus loss and bounded callback failures |

**2026-11-14 short hardware check:** Windows 11 LTSC build 26100; Npcap 1.90; Intel I219-LM, driver 20.0.3.28, 100 Mb/s; AX58100 and LAN9252 slaves, expected WKC 6. Exchanges used SAFE-OP only, ending in PRE-OP followed by disconnect. OP and output writes were not tested.

Both slaves initially reported AL Control/Status `0x12`, AL code `0`. Scanning now finishes with a plain `0x02` request and unmasked-state confirmation; repeated scans returned `0x02`. The old build waited about 15.6 ms after rescanning; the high-resolution waitable timer fixes this scheduling issue. Windows per-frame receive budget is now `clamp(cycleUs, 2000, 10000)` us.

Each new-build run below lasted 15 seconds; cells show communication errors / cycles.

| Target cycle | Normal mapping | Overlapped mapping |
| --- | --- | --- |
| 10 ms | 0 / 1493 | 0 / 1496 |
| 1 ms | 70 / 10453 | 71 / 9900 |
| 500 us | 67 / 12293 | 70 / 14434 |

A 3-second old-build baseline recorded 4 / 189, 141 / 189 and 180 / 189 at 10 ms, 1 ms and 500 us. These short, unequal windows are not a strict performance benchmark. Significant skipped cycles persist at 1 ms/500 us; neither target is guaranteed on this PC.

NIC RX/TX errors/discards stayed zero. ESC CRC/lost-link counters did not increase, but one LAN9252 forwarded-error count of 1 prevents ruling out physical-link issues. Interrupt moderation and EEE were enabled; their causal impact was not tested. Dual-port redundancy remains untested.

Subsequent OP tests include output-pattern submissions, normal/overlapped mapping and the new 5 ms target. Results and remaining timing limits are recorded in [Communication performance](performance.md#op-tests-on-this-machine); the earlier table above describes the SAFE-OP-only runs.

**2026-11-15 cyclic AL-state regression:** the Intel test bus with AX58100 and LAN9252 ran the indexed Lua `cycle.every(100)` output script at a 10 ms target with normal mapping for 60.0013 seconds. It completed 5,974 cycles, 59 callbacks and 118 successful signal writes; all 118 main-process output-image comparisons matched, with no script/host-write errors or sampled unknown AL states. Both stations remained OP in the recorded samples, expected WKC was 6, and the maximum measured cycle deviation was 10,279 us. There were still 27 incomplete PDO WKC cycles. No AL-read failure was observed in this short hardware run; the intermittent failure/recovery and expiration paths are covered by 60 deterministic fault-injection checks instead. These image checks do not independently measure electrical output pins. Cleanup zeroed both output images, confirmed PRE-OP and disconnected. The probe changed no NIC settings and is retained only under `build/communication-diagnostics/`.

The pre-release organization/build cleanup does not itself perform hardware acceptance. Ubuntu x86/ARM binaries and platform behavior also require separate builds and tests on their target environments. FoE, EoE, SoE, VoE, BOOT and DC trigger configuration are outside the implemented service set.
