# NiceECatM user guide

**English** | [简体中文](README.zh-CN.md)

NiceECatM is a graphical EtherCAT tool for scanning slaves, controlling states, testing PDO signals, accessing ESC registers and CoE SDO objects, transferring FoE files, programming EEPROM and running Lua scripts.

Project: [NiceECatM on GitHub](https://github.com/NaisuXu/NiceECatM)

## Start here

1. Extract the Windows x64 ZIP into a writable folder and run `NiceECatM.exe`. Keep the supplied folders beside the executable.
2. For real Windows devices, install [Npcap](https://npcap.com/#download) separately, or use a compatible WinPcap installation. If driver access is restricted to administrators, run with the corresponding privileges.
3. Use **EN** / **中文** at the upper right to change language. English is the default.

Click the version at the upper right to open [GitHub Releases](https://github.com/NaisuXu/NiceECatM/releases). A silent background check runs once at startup; **New version** appears beside the version when a newer stable release exists. There are no update popups or Log messages, including when offline. Download and install releases manually.

For a hardware-free trial, choose **Simulation** in **CONNECTION** while disconnected, then **Scan devices**. The default is **Hardware**. Linux needs its corresponding binary and raw Ethernet permissions; the current ZIP contains only the Windows x64 application.

## Windows / Intel adapter setting

For the Intel wired adapter dedicated to EtherCAT, set **Interrupt Moderation** to **Disabled** to reduce receive latency.

Open **Device Manager → Network adapters → your Intel adapter → Properties → Advanced**, select **Interrupt Moderation**, choose **Disabled** and apply. Wait for the link to return, then scan again. The option's name depends on the driver; skip this setting if it is unavailable.

Tests on an Intel I219-LM showed lower typical receive latency. Results depend on the adapter and driver; this setting does not guarantee the target cycle or eliminate WKC errors. Check actual timing and WKC in **Diagnostics**.

## Basic operation

1. Choose **Hardware**, **Single port**, **Normal mapping** and the wired interface connected to your slaves, then **Scan devices**. A successful scan leaves the slaves in **PRE-OP** with cyclic I/O stopped.
2. Select a slave in the left topology. Its signals appear in **Buffer · PDO**; inputs are read-only and mapped outputs can be edited directly.
3. **SAFE-OP** starts cyclic exchange with slave outputs kept safe. **OP** allows output data to be applied. **INIT** / **PRE-OP** stop cyclic exchange; **Disconnect** also closes the connection.

Recent trends are hidden until the play icon is clicked for a signal. A monitored signal stores one `float` sample for each valid I/O cycle in a 2,000-sample ring. The small range control below the play/stop icon switches between data min/max and the signal type limits. The I/O worker samples at the target cycle, while the workbench refreshes the visible plot about every 100 ms. At 500 us this is about 1 second of history, and at 10 ms it is about 20 seconds.

Request **PRE-OP** before synchronous ESC register or CoE SDO access, which requires cyclic exchange to be stopped. Use **Diagnostics** and the shared **Log** to check actual states and errors; select a log entry and press **Ctrl+C** to copy it.

**CoE / SDO** lists objects from a matching ESI and, when the slave supports SDO Information, discovers online descriptions in PRE-OP. Select an object to fill its address/type, then read or write explicitly. **Refresh dictionary** reloads online descriptions; it does not read all values. Sources and partial results are marked. ESI defaults are not current values; manual addresses remain available when no dictionary is provided.

The target cycle defaults to **10 ms**; options are **500 us, 1, 2, 5, 10, 20, 50 and 100 ms**. Change it while disconnected, or while cyclic exchange is stopped and all slaves are in INIT/PRE-OP. Check actual timing in Diagnostics.

**Disconnect before changing** Simulation / Hardware / Programming mode, interfaces, redundancy or mapping mode. Stop scripts before changing modes. Dual-port redundancy connects two interfaces to opposite ends of the **same slave chain**. Normal/overlapped mapping is independent of port count.

For signal descriptions, put matching manufacturer ESI XML files in the `esi/` directory beside the executable, restart and scan again. If a description does not match, the log shows the directory and reason. English-only device names remain English in the Chinese UI.

## FoE file transfer

1. In **Hardware**, scan and select the target in the left topology. Stop scripts and request **PRE-OP** for all slaves to stop cyclic I/O. **FoE** uses this selection.
2. For ordinary FoE, stay in PRE-OP. If the device's update procedure requires BOOT, select the intended current/all-slave scope in the top toolbar and click **BOOT**. Every target must have valid bootstrap mailboxes; an unsupported target rejects the initial request without changing any state. BOOT entry passes through INIT. Programming mode is reserved for EEPROM.
3. Choose **Download · master → slave** or **Upload · slave → master**, enter the vendor's device filename and 32-bit password (decimal or `0x`), then select the local file or save destination. The device filename is independent of the local filename.
4. Click **Start download/upload**. Progress and errors appear on the page and in the shared Log. Uploads have no known total length; the size limit is a receive bound. Advanced settings control response/no-progress/total timeouts and retries. Passwords are not saved or logged.
5. Completion leaves the bus in its actual state. After BOOT, use INIT/PRE-OP and **Scan devices** again before cyclic I/O. Follow the vendor's firmware activation or power-cycle procedure; OP is never restored automatically.

For slave-reported errors, the page and global Log show the protocol code, a localized standard reason and any original slave text; generic or vendor-specific errors cannot reliably distinguish an unsupported filename from an incorrect password.

**FoE completion confirms file transfer; whether firmware is activated is determined by the slave.** Cancellation does not roll back data already accepted by the slave. A failed/cancelled upload preserves an existing local destination. EEPROM support does not imply FoE support; the slave's application/bootloader must implement it.

In **Simulation**, virtual slave 4 provides `firmware.bin` and `demo.bin`; downloads can be uploaded again for readback testing. No hardware is accessed. FoE transfer is currently a GUI feature; Lua snippets do not expose file-transfer APIs.

## EEPROM programming

1. Stop scripts, disconnect, choose **Programming** on the left, select one interface and **Scan**. This scan confirms **INIT** without PDO mapping and can enumerate blank SII devices. Select the physical target in the left topology.
2. Only the **EEPROM** page can be selected in **Programming**; all other pages are disabled. Use **Open ESI** and choose its device description, or **Open BIN**. Preparation and BIN export work before connecting. ESI generation supports the supplied AX58100/LAN9252 fixed-device features; unsupported layouts are rejected. A display-compatible ESI is not necessarily supported for image generation.
3. **Read & save** reads the selected EEPROM. If its size is unknown, enter a bounded length suitable for the hardware.
4. **Program EEPROM** asks you to confirm the target, image and capacity, then writes the prepared image, including Alias/Serial and its padding, and compares the full readback. Different Vendor IDs or Product Codes are allowed and shown in this confirmation; matching identifiers alone do not prove hardware compatibility. The displayed SII capacity is a declaration, not a physical measurement.

**Export ESI** saves a basic ESI XML from a loaded image or a validated EEPROM read, without connecting. ESI export includes only EEPROM/SII data and cannot recover the full object dictionary or initialization commands. Keep the original BIN as the EEPROM backup.

Cancelling after a write starts can leave a partially programmed EEPROM; there is no rollback. Completion, failure and cancellation retain Programming mode. Follow the device's reload or power-cycle procedure; to resume normal operation, disconnect, choose **Hardware**, scan to PRE-OP and request OP as needed. OP is never restored automatically.

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
| `Example_01_Full_Lifecycle.lua` | Select Hardware and start disconnected; enumerates real adapters, uses the first one and current redundancy/mapping settings, scans, inspects devices, enters OP, returns to PRE-OP and disconnects; adjust its connection options for your bus |
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
