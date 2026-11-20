-- Copyright (C) 2026 Naisu Xu
-- SPDX-License-Identifier: GPL-3.0-only

-- NiceECatM example: complete host lifecycle.
-- Lists adapters, opens the first wired interface, scans the bus, reads
-- identity and PDO information, changes state, then closes the bus.
-- Select Hardware and disconnect any existing bus before running this example.
-- Adapter enumeration itself never switches the selected operating mode.
-- This finite script stops after the disconnect completes.

assert(ecat.current():status().mode == "hardware", "Select Hardware before running this example")

local adapters = ecat.adapters()
assert(#adapters > 0, "no wired Ethernet adapters found")
for index, adapter in ipairs(adapters) do
    log.info(string.format("adapter %d: %s (%s)", index, adapter.name, adapter.description))
end

local bus = ecat.open({ interface = adapters[1].name, simulation = false, cycle_us = 10000 })
local status = bus:status()
log.info(string.format("opened: connected=%s cycle=%dus", tostring(status.connected), 10000))

local devices = bus:scan()
assert(#devices > 0, "no EtherCAT slaves found")
for _, device in ipairs(devices) do
    log.info(string.format("slave %d: %s [%s] DC=%s", device.position, device.name, device.state, tostring(device.dc)))
    local signals = bus:signals(device.position)
    log.info(string.format("slave %d exposes %d mapped signals", device.position, #signals))
end

bus:state("all", "PRE-OP")
log.info("all slaves confirmed PRE-OP")

-- Enter OP to start cyclic exchange, then return to PRE-OP before closing.
bus:state("all", "OP")
local running = bus:status()
log.info(string.format("cyclic exchange: running=%s WKC=%d/%d", tostring(running.running), running.wkc, running.expected_wkc))
bus:state("all", "PRE-OP")
bus:disconnect()
log.info("lifecycle complete: bus disconnected")
