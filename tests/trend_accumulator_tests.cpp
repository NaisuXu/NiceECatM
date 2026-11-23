// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#include "ethercat/trend_accumulator.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
using namespace niceecatm;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

Slave squareWaveSlave(bool high) {
    Slave slave;
    slave.position=1;
    slave.inputBits=1;
    slave.inputLayoutKnown=true;
    slave.inputs={uint8_t(high?1:0)};
    PdoEntry entry;
    entry.input=true;
    entry.bitOffset=0;
    entry.bitLength=1;
    entry.type="BOOL";
    slave.pdoEntries.push_back(std::move(entry));
    return slave;
}

void squareWaveIsCapturedBeforeUiSampling() {
    TrendAccumulator accumulator;
    std::vector<Slave> slaves;
    for (unsigned cycle=0;cycle<400;++cycle) {
        // A 50 ms square wave at a 500 us I/O period has 100 samples per half-cycle.
        slaves={squareWaveSlave((cycle/100)%2!=0)};
        accumulator.observe(slaves,500,true);
    }
    const auto& history=accumulator.history();
    const auto found=history.find("1/in/0/1");
    check(found!=history.end()&&found->second.size()==2,"Expected two 100 ms trend buckets");
    check(found->second[0].minimum==0&&found->second[0].maximum==1&&found->second[0].latest==1,
          "The first bucket must preserve both levels and its final value");
    check(found->second[1].minimum==0&&found->second[1].maximum==1&&found->second[1].latest==1,
          "The second bucket must preserve both levels and its final value");
}

void historyIsBounded() {
    TrendAccumulator accumulator;
    std::vector<Slave> slaves;
    for (unsigned cycle=0;cycle<50000;++cycle) {
        slaves={squareWaveSlave((cycle&1)!=0)};
        accumulator.observe(slaves,10000,true);
    }
    const auto found=accumulator.history().find("1/in/0/1");
    check(found!=accumulator.history().end()&&found->second.size()==200,"Trend history must retain 200 buckets");
}

void rawFallbackKeepsByteFields() {
    TrendAccumulator accumulator;
    Slave slave;
    slave.position=1;
    slave.inputBits=16;
    slave.inputs={0x12,0x34};
    slave.inputLayoutKnown=false;
    std::vector<Slave> slaves{slave};
    for (unsigned cycle=0;cycle<100;++cycle) accumulator.observe(slaves,1000,true);
    check(accumulator.history().count("1/in/0/8")&&accumulator.history().count("1/in/8/8"),
          "Raw process-image bytes must have trend fields when the PDO layout is unavailable");
}

void pauseDiscardsPartialBucketOnly() {
    TrendAccumulator accumulator;
    std::vector<Slave> slaves;
    for (unsigned cycle=0;cycle<50;++cycle) {
        slaves={squareWaveSlave(true)};
        accumulator.observe(slaves,1000,true);
    }
    accumulator.pause();
    for (unsigned cycle=0;cycle<100;++cycle) {
        slaves={squareWaveSlave(false)};
        accumulator.observe(slaves,1000,true);
    }
    const auto found=accumulator.history().find("1/in/0/1");
    check(found!=accumulator.history().end()&&found->second.size()==1&&found->second[0].minimum==0&&
          found->second[0].maximum==0,"Stopping must discard an incomplete bucket without clearing history");
}

} // namespace

int main() {
    try {
        squareWaveIsCapturedBeforeUiSampling();
        historyIsBounded();
        rawFallbackKeepsByteFields();
        pauseDiscardsPartialBucketOnly();
        std::cout<<"trend accumulator tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<error.what()<<'\n';
        return 1;
    }
}
