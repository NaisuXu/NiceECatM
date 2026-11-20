// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#pragma once
#include "model/types.hpp"
#include <functional>

namespace niceecatm::foe {
enum class Opcode : uint8_t { Read=1, Write=2, Data=3, Ack=4, Error=5, Busy=6 };
struct Packet {
    Opcode opcode=Opcode::Error;
    uint32_t number=0;
    std::vector<uint8_t> data;
};
// Payload sizes exclude both the six-byte mailbox and six-byte FoE headers.
// A callback performs a bounded transaction. receive: 1 packet, 0 none, -1
// unavailable transport. An empty DATA packet is valid and terminates a file.
struct Wire {
    size_t txPayload=0,rxPayload=0;
    std::function<bool(const Packet&,int timeoutMs)> send;
    std::function<int(Packet&,int timeoutMs)> receive;
    std::function<uint64_t()> nowMs;
    std::function<void(unsigned)> waitMs;
};
constexpr size_t MaxFileBytes=128u*1024*1024;
bool validBootstrapMailbox(uint16_t txOffset,uint16_t txLength,uint16_t rxOffset,uint16_t rxLength,size_t transportCapacity);
// Executes on the sole I/O owner. File contents never enter a Snapshot or
// command Result. Failed/cancelled uploads preserve an existing destination.
Result run(Wire,int slave,FoeDirection,const FoeOptions&,const FoeControl&);
} // namespace niceecatm::foe
