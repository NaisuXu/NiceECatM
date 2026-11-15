-- Copyright (C) 2026 Naisu Xu
-- SPDX-License-Identifier: GPL-3.0-only

-- NiceECatM example: write the first mapped output and verify the request.
-- Writes are direct when the state, mapping and signal permissions allow them.
-- It performs one write and then stops; use Example_03 for a persistent task.

local bus = ecat.current()
local status = bus:status()
assert(status.connected and status.mapped, "scan and map a connected bus first")

for position = 1, status.slaves do
    for _, item in ipairs(bus:signals(position)) do
        if not item.input then
            local output = bus:signal(position, item.index, item.subindex)
            output:write(0)
            log.info(string.format("slave %d output %s set to zero", position, item.name))
            return
        end
    end
end
error("no mapped output signal found")
