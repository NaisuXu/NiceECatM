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
    virtual Result scanProgramming(const Connection&,const EepromControl&) {return {false,"Programming scan is unavailable in this backend.",{}};}
    virtual Result readEeprom(int,size_t,const EepromControl&) {return {false,"EEPROM read is unavailable in this backend.",{}};}
    virtual Result writeEeprom(int,const std::vector<uint8_t>&,const EepromControl&) {return {false,"EEPROM programming is unavailable in this backend.",{}};}
    virtual Result foeTransfer(int,FoeDirection,const FoeOptions&,const FoeControl&) {return {false,"FoE transfer is unavailable in this backend.",{}};}
    virtual Result state(int slave, AlState)=0;
    virtual Result setCycle(int cycleUs)=0;
    virtual Result start()=0;
    virtual Result stop()=0;
    virtual Result readRegister(int slave, uint16_t offset, int length)=0;
    virtual Result writeRegister(int slave, uint16_t offset, const std::vector<uint8_t>&)=0;
    virtual Result readSdo(int slave, uint16_t index, uint8_t sub, int capacity)=0;
    virtual Result writeSdo(int slave, uint16_t index, uint8_t sub, const std::vector<uint8_t>&)=0;
    virtual Result readSdoDictionary(int) {return {false,"Online SDO dictionary discovery is unavailable.",{}};}
    // Each idle service call may query one PDO object's description.
    virtual bool hasPendingPdoDescriptions() const {return false;}
    virtual bool servicePdoDescriptions() {return false;}
    virtual Result writeOutput(int slave, int offset, const std::vector<uint8_t>&)=0;
    virtual void cycle()=0;
    // Trend selection is applied by the I/O owner before the next cycle.
    virtual void setTrendSelection(const std::vector<std::string>&) {}
    // Called by the I/O owner after each cyclic exchange. Implementations
    // decide whether the exchange is valid before recording selected values.
    virtual void sampleTrend() {}
    virtual TrendHistory trendHistory() const {return {};}
    virtual void clearTrend() {}
    virtual void close()=0;
    virtual Snapshot snapshot() const=0;
    // Asynchronous transport notices are drained by the sole I/O owner and
    // assigned global log sequence numbers by the controller.
    virtual std::vector<Result> takeDiagnostics() {return {};}
};
std::unique_ptr<Backend> makeSimulation();
std::unique_ptr<Backend> makeSoem();
} // namespace niceecatm
