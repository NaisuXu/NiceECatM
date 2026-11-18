// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#pragma once
#include "model/types.hpp"
#include <memory>
namespace niceecatm {
// Called exclusively by one I/O owner; no SOEM object crosses this boundary.
class Backend {
public:
    virtual ~Backend()=default;
    virtual Result adapters()=0;
    virtual Result scan(const Connection&)=0;
    virtual Result state(int slave, AlState)=0;
    virtual Result setCycle(int cycleUs)=0;
    virtual Result start()=0;
    virtual Result stop()=0;
    virtual Result readRegister(int slave, uint16_t offset, int length)=0;
    virtual Result writeRegister(int slave, uint16_t offset, const std::vector<uint8_t>&)=0;
    virtual Result readSdo(int slave, uint16_t index, uint8_t sub, int capacity)=0;
    virtual Result writeSdo(int slave, uint16_t index, uint8_t sub, const std::vector<uint8_t>&)=0;
    virtual Result readSdoDictionary(int) {return {false,"Online SDO dictionary discovery is unavailable.",{}};}
    virtual Result writeOutput(int slave, int offset, const std::vector<uint8_t>&)=0;
    virtual void cycle()=0;
    virtual void close()=0;
    virtual Snapshot snapshot() const=0;
    // Asynchronous transport notices are drained by the sole I/O owner and
    // assigned global log sequence numbers by the controller.
    virtual std::vector<Result> takeDiagnostics() {return {};}
};
std::unique_ptr<Backend> makeSimulation();
std::unique_ptr<Backend> makeSoem();
} // namespace niceecatm
