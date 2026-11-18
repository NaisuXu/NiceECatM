# NiceECatM user guide

**English** | [简体中文](README.zh-CN.md)

NiceECatM is a graphical EtherCAT tool for scanning slaves, controlling states, testing PDO signals, accessing ESC registers and CoE SDO objects, and running Lua scripts.

Project: [NiceECatM on GitHub](https://github.com/NaisuXu/NiceECatM)

## Start here

1. Extract the Windows x64 ZIP into a writable folder and run `NiceECatM.exe`. Keep the supplied folders beside the executable.
2. For real Windows devices, install [Npcap](https://npcap.com/#download) separately, or use a compatible WinPcap installation. If driver access is restricted to administrators, run with the corresponding privileges.
3. Use **EN** / **中文** at the upper right to change language. English is the default.

For a hardware-free trial, choose **Simulation** in **CONNECTION** while disconnected, then **Scan devices**. The default is **Hardware**. Linux needs its corresponding binary and raw Ethernet permissions; the current ZIP contains only the Windows x64 application.

## Windows / Intel adapter setting

For the Intel wired adapter dedicated to EtherCAT, set **Interrupt Moderation** to **Disabled** to reduce receive latency.

Open **Device Manager → Network adapters → your Intel adapter → Properties → Advanced**, select **Interrupt Moderation**, choose **Disabled** and apply. Wait for the link to return, then scan again. The option's name depends on the driver; skip this setting if it is unavailable.

Tests on an Intel I219-LM showed lower typical receive latency. Results depend on the adapter and driver; this setting does not guarantee the target cycle or eliminate WKC errors. Check actual timing and WKC in **Diagnostics**.

## Basic operation

1. Choose **Hardware**, **Single port**, **Normal mapping** and the wired interface connected to your slaves, then **Scan devices**. A successful scan leaves the slaves in **PRE-OP** with cyclic I/O stopped.
2. Select a slave in the left topology. Its signals appear in **Buffer · PDO**; inputs are read-only and mapped outputs can be edited directly.
3. **SAFE-OP** starts cyclic exchange with slave outputs kept safe. **OP** allows output data to be applied. **INIT** / **PRE-OP** stop cyclic exchange; **Disconnect** also closes the connection.

Request **PRE-OP** before synchronous ESC register or CoE SDO access, which requires cyclic exchange to be stopped. Use **Diagnostics** and the shared **Log** to check actual states and errors; select a log entry and press **Ctrl+C** to copy it.

**CoE / SDO** lists objects from a matching ESI and, when the slave supports SDO Information, discovers online descriptions in PRE-OP. Select an object to fill its address/type, then read or write explicitly. **Refresh dictionary** reloads online descriptions; it does not read all values. Sources and partial results are marked. ESI defaults are not current values; manual addresses remain available when no dictionary is provided.

The target cycle defaults to **10 ms**; options are **500 us, 1, 2, 5, 10, 20, 50 and 100 ms**. Change it while disconnected, or while cyclic exchange is stopped and all slaves are in INIT/PRE-OP. Check actual timing in Diagnostics.

**Disconnect before changing** hardware/simulation mode, interfaces, redundancy or mapping mode. Dual-port redundancy connects two interfaces to opposite ends of the **same slave chain**. Normal/overlapped mapping is independent of port count.

For signal descriptions, put matching manufacturer ESI XML files in `esi/`, restart and scan again. English-only device names remain English in the Chinese UI.

## Lua scripts

Scripts are UTF-8 `.lua` files in the adjacent `scripts/` folder. The Scripts page includes an editor, file list, formatting and API snippets. Snippets insert code; they do not execute it.

1. Select a file, or click **New** to create a draft with a unique name.
2. Double-click a filename in the list to rename it. A leading `*` marks an unsaved file or edit.
3. Edit and **Save**, then **Run**. Run rejects unsaved source; it does not run an older saved copy silently.
4. Read output or errors in the shared **Log**. Only one script runs at a time.
5. Use the script **Stop** button to stop its callbacks and tasks. It does not disconnect the bus or stop cyclic I/O automatically.

Unsaved edits exist only in memory and are lost on exit. **Open / Reload** loads the selected file from disk and discards its cached edits. Selecting a different file does not stop a running script.

To inspect the current workbench connection without opening another one, save and run:

```lua
local bus = ecat.current()
local status = bus:status()
assert(status.connected, "scan the bus first")
log.info(string.format("state=%s WKC=%d/%d",
    status.state, status.wkc, status.expected_wkc))
```

The packaged examples have different effects:

| File | Behavior |
| --- | --- |
| `Example_01_Full_Lifecycle.lua` | Start disconnected; enumerates real adapters, uses the first one and current redundancy/mapping settings, scans, inspects devices, enters OP, returns to PRE-OP and disconnects; adjust its connection options for your bus |
| `Example_02_Signal_Inspection.lua` | Requires a connected, mapped bus; inspects signals, reads input values from the current process image and finishes; stopped I/O gives cached values |
| `Example_03_Cycle_Automation.lua` | Requires running cyclic I/O; reports every ten observed cycles plus a periodic heartbeat; continues until stopped |
| `Example_04_Output_Write_Test.lua` | Requires a mapped connection and a writable output; writes zero to the first output found, then finishes; check that target before running |
| `Example_05_State_Sequence.lua` | Requires a connected, mapped bus; steps through INIT/PRE-OP/SAFE-OP/OP, then returns to PRE-OP and disconnects after a one-shot timer |

An example error is reported in the Log and may leave the bus connected in its last reached state. Check the actual bus state and use PRE-OP or Disconnect as needed.

`ecat.current()` attaches to the workbench connection. `ecat.open(...)` selects connection options; `bus:scan()` performs the scan. Bus calls return their result directly, without an `await` wrapper. Use a unique signal name or its Index/SubIndex instead of assuming a name such as `Input` is unique.

Timers use milliseconds; cycle callbacks use a positive **cycle count**. `cycle.every(1, fn)` follows observed cycles, and `cycle.every(2, fn)` waits for two elapsed cycles. Both timer and cycle registrations return task IDs that can be cancelled. They run on the script worker and can skip intermediate bus cycles; they do not replace a hard real-time control loop.

A repeating timer or cycle task keeps a script running after its top-level statements finish. The script ends when no tasks remain, or when stopped. `bus:stop()` stops bus I/O; the script Stop button stops the script. Use `timer.cancel(id)` or `cycle.cancel(id)` to cancel one task. More code is available in the page's API snippets and packaged examples.

## Files and upgrades

- `scripts/`: saved Lua files and five examples; `esi/`: manufacturer XML descriptions.
- `preferences.ini`: saved UI language only. These files are resolved from the executable's folder; connection settings must be selected again after restarting.
- Before upgrading, close the application and back up `scripts/`, custom `esi/` files and `preferences.ini`. Extract the new release into a new folder, then copy the data you want to keep.

## License

Copyright (C) 2026 Naisu Xu. NiceECatM's own code uses **GPL-3.0-only**; see [LICENSE](LICENSE). Third-party notices are in [THIRD_PARTY.md](THIRD_PARTY.md) and `licenses/`.
