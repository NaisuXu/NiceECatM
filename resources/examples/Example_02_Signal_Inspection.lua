-- Copyright (C) 2026 Naisu Xu
-- SPDX-License-Identifier: GPL-3.0-only

-- NiceECatM example: inspect mapped signal metadata and read values.
-- This uses PDO index/subindex identities so duplicate display names are safe.
-- It is a finite inspection and stops after the last signal is read.

local bus = ecat.current()
local status = bus:status()
assert(status.connected and status.mapped, "scan and map a connected bus first")

for position = 1, status.slaves do
    local signals = bus:signals(position)
    log.info(string.format("slave %d", position))
    for _, item in ipairs(signals) do
        log.info(string.format("  %s %s %s (%d bit) PDO 0x%04X:%02X",
            item.input and "IN" or "OUT", item.name, item.type,
            item.bit_length, item.index, item.subindex))
        if item.input then
            local value = bus:signal(position, item.index, item.subindex):read()
            log.info("  value = " .. value)
        end
    end
end
