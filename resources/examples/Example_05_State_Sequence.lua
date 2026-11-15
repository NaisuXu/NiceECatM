-- Copyright (C) 2026 Naisu Xu
-- SPDX-License-Identifier: GPL-3.0-only

-- NiceECatM example: explicit AL-state sequence for a commissioning test.
-- A one-shot timer returns the bus to PRE-OP and disconnects; the script then stops.

local bus = ecat.current()
assert(bus:status().connected, "open or attach a bus first")

bus:state("all", "INIT")
log.info("all slaves are in INIT")
bus:state("all", "PRE-OP")
log.info("all slaves are in PRE-OP; mailbox services are available when declared")
bus:state("all", "SAFE-OP")
log.info("all slaves are in SAFE-OP; outputs are prepared")
bus:state("all", "OP")
log.info("all slaves are in OP; cyclic I/O is running")

timer.after(1000, function()
    bus:state("all", "PRE-OP")
    log.info("returned all slaves to PRE-OP")
    bus:disconnect()
    log.info("state sequence complete")
end)
