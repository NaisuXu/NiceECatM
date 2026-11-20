# Lua scripts and automation

**English** | [简体中文](scripts.zh-CN.md) | [User guide](../README.md)

The Scripts page runs Lua 5.4 on a separate worker. It can scan devices, control states, inspect signals and automate tests through the same controller used by the UI. It does not call SOEM directly and does not provide a hard real-time PLC loop.

## Editing and running

Scripts are UTF-8 `.lua` files in `scripts/` beside the executable. New adds a uniquely named draft immediately. Double-click its list name to rename it. A leading `*` identifies an unsaved draft or edit, including while another file is selected.

Save before Run: unsaved edits are rejected rather than running older source. Open / Reload reloads the selected file from disk and discards its cached edits. Unsaved buffers remain only in memory and are lost when the application closes.

The editor provides line numbers, syntax colors, a monospaced font and Format. API snippets insert code without executing it. Script messages and failures appear in the shared Log. One script runs at a time; the editor may show a different file while that run continues.

Example files removed by the user are not recreated on startup. If `scripts/` is empty, the editor shows an unsaved `untitled.lua` draft; Save creates its file.

Five `Example_*.lua` files are included in `scripts/`: full hardware lifecycle, signal inspection, cycle automation, output-write testing and a delayed state sequence. The hardware lifecycle enumerates actual physical interfaces and requires a capture driver. Use explicit simulation for hardware-free experiments:

```lua
local bus = ecat.open({ simulation = true, cycle_us = 10000 })
local devices = bus:scan()
log.info("devices: " .. #devices)
bus:disconnect()
```

## Connection and operations

`ecat.current()` attaches a Lua handle to the workbench connection. `ecat.open(options)` copies the current connection settings and overrides supplied options; it creates a handle without scanning. Options are `interface` (or `primary`), `secondary`, `redundant`, `overlap`, `simulation` and `cycle_us`.

Select **Simulation** or **Hardware** before running a script. Mode changes require stopped scripts and a disconnected bus; Programming mode does not run scripts. For the hardware lifecycle example, select Hardware first. For the hardware-free example above, select Simulation first. Adapter enumeration returns physical interface metadata without changing the selected mode or disconnecting a bus.

Bus operations wait for their host result before returning. They raise a Lua error on failure; no `await` wrapper is required.

| API | Behavior |
| --- | --- |
| `ecat.adapters()` | Enumerates physical wired interfaces, returning `name` and `description`; does not switch modes, open a bus or scan slaves |
| `ecat.current()` | Gets a handle using the current workbench connection settings |
| `ecat.open(options)` | Gets a handle with explicit connection-setting overrides |
| `bus:scan()` | Configures/maps slaves, leaves PRE-OP, returns an array of `position`, `name`, `state`, `dc` |
| `bus:disconnect()` | Closes the current bus connection |
| `bus:state(target, state)` | Requests INIT, PRE-OP, SAFE-OP or OP for `"all"` or a numeric slave position; SAFE-OP/OP start cycles |
| `bus:start()` | Starts cyclic exchange in SAFE-OP/OP |
| `bus:stop()` | Stops cyclic exchange and lowers OP slaves to SAFE-OP |
| `bus:cycle(cycle_us)` | Changes the target while stopped and all slaves are INIT/PRE-OP |
| `bus:status()` | Reads a published status snapshot without a bus transaction |
| `bus:signals(slave)` | Lists mapped named non-padding signal descriptors |
| `bus:signal(slave, name)` | Gets a PDO signal handle; ambiguous names are rejected when resolving the operation |
| `bus:signal(slave, index, subindex)` | Gets a PDO signal handle by object identity |
| `signal:read()` | Reads a decoded PDO value as a string |
| `signal:write(value)` | Writes a typed output value; a read value can be passed through unchanged |
| `bus:register(slave, offset)` | Gets an ESC register handle; `:read(length)` returns hex bytes, `:write(hex_bytes)` writes bytes |
| `bus:sdo(slave, index, subindex)` | Gets a CoE SDO handle with the same byte read/write methods |
| `log.info(text)`, `log.warn(text)`, `log.error(text)` | Writes to the shared Log; `print(text)` is an info alias |

Slave positions start at 1. Use `"all"` for an explicit bus-wide state request. Cycle periods are in microseconds: 500, 1000, 2000, 5000, 10000, 20000, 50000 or 100000.

`bus:status()` returns `mode` (`simulation`, `hardware` or `programming`), `connected`, `mapped`, `running`, `busy`, `generation`, `cycles`, `wkc`, `expected_wkc`, `slaves` (a count), `state` and `status`. Signal descriptors contain `name`, `input`, `type`, `bit_offset`, `bit_length`, `index` and `subindex`.

Signal names and object addresses are device-specific. Inspect the descriptors instead of assuming an ESI name such as Input is unique:

```lua
local bus = ecat.current()
assert(bus:status().connected, "scan the bus first")
for _, signal in ipairs(bus:signals(1)) do
    if signal.input then
        local value = bus:signal(1, signal.index, signal.subindex):read()
        log.info(signal.name .. " = " .. value)
    end
end
```

Connection generation protects queued commands from obsolete contexts. Lua handles themselves are not generation-bound addresses. Discover and bind signals again after scanning or a mapping change. Synchronous ESC/SDO calls require cyclic exchange to be stopped. Writes always obey the normal state, direction, mapping and range checks; no write-enable switch is required.

## Timers and cycle callbacks

| API | Behavior |
| --- | --- |
| `timer.after(duration, fn)` | Registers a one-shot callback and returns its numeric task ID |
| `timer.every(duration, fn)` | Registers a repeating callback and returns its task ID |
| `timer.cancel(id)` | Removes an after/every task |
| `cycle.every(cycles, fn)` | Registers a callback after each N elapsed cycles observed through snapshots; returns its task ID |
| `cycle.cancel(id)` | Removes a cycle task |

Numeric timer durations are milliseconds. Strings such as `"1s"` or `"1000us"` are accepted when the result is at least 1 ms. Periods below 1 ms are rejected. Host timers are suitable for polling and timeouts; they do not run exactly at an EtherCAT cycle edge.

The cycle parameter is a positive cycle count: 1 follows received cycle snapshots, 2 waits for at least two elapsed cycles, and so on. The callback receives `frame`, containing `cycles`, `missed`, `wkc`, `expected_wkc`, `connected`, `running`, `status` and a `slaves` array with input/output hex snapshots. Intermediate cycles may be skipped when the worker or UI is delayed; skipped callbacks are not replayed.

Callbacks run serially. Up to 64 timer/cycle tasks may coexist by default. Cancellation returns `true` for a live task of the matching kind, or `false` for an absent, expired, already cancelled or other-kind ID. IDs must be positive whole numbers of numeric type; invalid arguments raise a Lua error.

A callback may cancel itself or another task. Cancellation stops later invocations; it does not interrupt the current invocation. Declare a self-captured local ID before assigning it:

```lua
local count = 0
local timerId
timerId = timer.every(100, function()
    count = count + 1
    log.info("tick " .. count)
    if count >= 10 then timer.cancel(timerId) end
end)

local cycleId = cycle.every(2, function(frame)
    log.info("cycle " .. frame.cycles)
end)
timer.after(2000, function()
    cycle.cancel(cycleId)
end)
```

The script ends naturally when top-level code has returned and no registered tasks remain. A repeating task keeps it running, including a cycle task waiting for a stopped/disconnected bus. The script Stop button cancels all tasks and pending waits. `bus:stop()` stops bus I/O, not the script.

## Long-running scripts and errors

Use short initialization followed by finite callbacks. Lua supports `while` and `repeat`, but a busy infinite loop prevents other callbacks from running and reaches the execution guard. Captured local variables retain their values between callback invocations.

```lua
local bus = ecat.current()
local polls = 0
timer.every(1000, function()
    local status = bus:status()
    polls = polls + 1
    log.info(string.format("poll %d: %s WKC=%d/%d",
        polls, status.state, status.wkc, status.expected_wkc))
end)
```

Each run creates a fresh Lua state on the script thread. Internal states are Empty, Ready, Running, Waiting, Stopped and Error; the UI shows the active/stopped indicator. A top-level parse/runtime error ends the run. Callback errors are logged; a repeating callback remains registered until cancelled or stopped. Catch expected failures with `pcall`:

```lua
local ok, result = pcall(function()
    return ecat.current():register(1, 0x0130):read(2)
end)
if ok then log.info(result) else log.error(result) end
```

Stopping a script does not disconnect the bus unless the script explicitly does so. It prevents further scheduling but cannot undo a write already accepted by the controller.

The editor accepts source up to its 64 KiB buffer limit. The runtime limits memory, source, logs and registered tasks, has a default 30-second host-request timeout and resets the execution budget for the top-level chunk and each callback. File, OS, package, debug and network libraries are not exposed. Long-running healthy callbacks do not exhaust a lifetime instruction budget.
