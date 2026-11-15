-- Copyright (C) 2026 Naisu Xu
-- SPDX-License-Identifier: GPL-3.0-only

-- NiceECatM example: monitor the running bus and report every ten cycles.
-- Leave the script running while the test is active; press Stop to finish.
-- The repeating cycle and timer callbacks intentionally keep this script alive.

local bus = ecat.current()
local status = bus:status()
assert(status.connected and status.running, "request OP before starting this example")

local reports = 0
cycle.every(10, function(frame)
    reports = reports + 1
    log.info(string.format("report %d: cycle=%d WKC=%d/%d missed=%d",
        reports, frame.cycles, frame.wkc, frame.expected_wkc, frame.missed))
end)

timer.every(1000, function()
    local current = bus:status()
    log.info(string.format("heartbeat: running=%s cycles=%d WKC=%d/%d",
        tostring(current.running), current.cycles, current.wkc, current.expected_wkc))
end)
