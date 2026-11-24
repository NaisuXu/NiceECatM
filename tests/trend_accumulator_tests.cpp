// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#include "ethercat/trend_accumulator.hpp"
#include <iostream>
#include <stdexcept>

namespace {
using namespace niceecatm;

void check(bool condition,const char* message) {if(!condition)throw std::runtime_error(message);}

Slave squareWaveSlave(bool high) {
    Slave slave;slave.position=1;slave.inputBits=1;slave.inputLayoutKnown=true;slave.inputs={uint8_t(high?1:0)};
    PdoEntry entry;entry.input=true;entry.bitOffset=0;entry.bitLength=1;entry.type="BOOL";slave.pdoEntries.push_back(std::move(entry));
    return slave;
}

void selectedSignalCapturesEveryCycle() {
    TrendAccumulator accumulator;
    std::vector<Slave> slaves;
    slaves={squareWaveSlave(false)};
    accumulator.setSelection({"1/in/0/1"},slaves);
    for(unsigned cycle=0;cycle<400;++cycle) {
        slaves[0].inputs[0]=uint8_t((cycle/100)%2!=0);
        accumulator.observe(slaves,500,true);
    }
    const auto history=accumulator.history();
    const auto found=history.find("1/in/0/1");
    check(found!=history.end()&&found->second.size()==400,"Selected signal must retain every valid cycle");
    check(found->second[0]==0&&found->second[99]==0&&found->second[100]==1&&found->second[199]==1,
          "Per-cycle square-wave samples must preserve their order");
}

void ringIsBoundedAndInvalidCyclesAreSkipped() {
    TrendAccumulator accumulator;std::vector<Slave> slaves{squareWaveSlave(true)};
    accumulator.setSelection({"1/in/0/1"},slaves);
    for(unsigned cycle=0;cycle<2500;++cycle)accumulator.observe(slaves,500,cycle%11!=0);
    const auto history=accumulator.history();
    const auto found=history.find("1/in/0/1");
    check(found!=history.end()&&found->second.size()==2000,"Trend ring must retain at most 2000 samples");
    check(found->second.front()==1&&found->second.back()==1,"Valid samples must remain after skipped WKC cycles");
}

void cycleChangeClearsHistory() {
    TrendAccumulator accumulator;std::vector<Slave> slaves{squareWaveSlave(true)};
    accumulator.setSelection({"1/in/0/1"},slaves);accumulator.observe(slaves,500,true);
    check(accumulator.history().at("1/in/0/1").size()==1,"Initial sample missing");
    accumulator.setCycle(1000);
    check(accumulator.history().empty(),"Changing the target cycle must clear old time-axis data");
}

void rawFallbackCanBeMonitored() {
    TrendAccumulator accumulator;Slave slave;slave.position=1;slave.inputBits=16;slave.inputs={0x12,0x34};
    std::vector<Slave> slaves{slave};accumulator.setSelection({"1/in/0/8","1/in/8/8"},slaves);
    accumulator.observe(slaves,10000,true);
    const auto history=accumulator.history();
    check(history.count("1/in/0/8")&&history.count("1/in/8/8"),"Raw process-image fields must be selectable");
}

} // namespace

int main() {
    try {
        selectedSignalCapturesEveryCycle();
        ringIsBoundedAndInvalidCyclesAreSkipped();
        cycleChangeClearsHistory();
        rawFallbackCanBeMonitored();
        std::cout<<"trend accumulator tests passed\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
