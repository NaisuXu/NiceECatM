// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
#include "ethercat/cyclic_state_poll.hpp"

// Replace the SOEM read, not the monitor: these tests exercise the production
// wrapper against lost AL replies without a NIC, capture driver or real sleep.
namespace {
using Monitor=niceecatm::CyclicStatePoll;
using Kind=Monitor::EventKind;
using Time=Monitor::Clock::time_point;
struct Reply {uint16_t state,code=0;};
std::vector<Reply> replies;
int checks=0,reads=0;
void check(bool condition,const char* message) {
    ++checks;
    if(!condition)throw std::runtime_error(message);
}
Time at(int milliseconds) {return Time{}+std::chrono::milliseconds(milliseconds);}
std::unique_ptr<ecx_contextt> context(int count) {
    auto result=std::make_unique<ecx_contextt>();
    result->slavecount=count;
    for(int s=1;s<=count;++s)result->slavelist[s].state=EC_STATE_OPERATIONAL;
    return result;
}
void event(Monitor& monitor,Kind kind,int slave,const char* message) {
    const auto events=monitor.takeEvents();
    check(events.size()==1&&events[0].kind==kind&&events[0].slave==slave,message);
    check(monitor.takeEvents().empty(),"Diagnostic events are consumed once");
}
void transientFailure() {
    auto bus=context(2);Monitor monitor;
    bus->slavelist[1].ALstatuscode=0x001b;
    replies={{EC_STATE_NONE},{EC_STATE_OPERATIONAL}};
    const int before=reads;
    monitor.read(*bus,true,at(10000));
    check(reads==before+1,"A failed AL poll does not synchronously retry or add reads");
    check(bus->slavelist[1].state==EC_STATE_OPERATIONAL&&bus->slavelist[1].ALstatuscode==0x001b,
          "One lost AL reply preserves both native state and its associated code with complete PDO WKC");
    check(bus->slavelist[2].state==EC_STATE_OPERATIONAL,"Another slave's successful reply is retained");
    check(bus->slavelist[0].state==EC_STATE_OPERATIONAL&&bus->slavelist[0].ALstatuscode==0x001b,
          "The aggregate state/code agree with restored per-slave state");
    check(monitor.pending()&&monitor.failures()==1,"Failed AL sampling is tracked independently of PDO WKC");
    event(monitor,Kind::Failed,1,"The first failure emits one diagnostic for the affected slave");
    monitor.expire(*bus,true,at(10999));
    check(bus->slavelist[1].state==EC_STATE_OPERATIONAL&&monitor.takeEvents().empty(),
          "The next valid PDO cycle does not turn a transient AL failure into a false drop");
    replies={{EC_STATE_OPERATIONAL},{EC_STATE_OPERATIONAL}};
    monitor.read(*bus,true,at(11000));
    check(!monitor.pending()&&monitor.failures()==1,"A successful AL read clears pending status but keeps its failure count");
    check(bus->slavelist[1].ALstatuscode==0,"Fresh AL data replaces the preserved status code");
    event(monitor,Kind::Recovered,1,"Recovery emits one diagnostic");
    monitor.expire(*bus,true,at(15000));
    check(bus->slavelist[1].state==EC_STATE_OPERATIONAL&&monitor.takeEvents().empty(),
          "A completed recovery leaves no stale expiry timer");
}
void actualStateChanges() {
    auto bus=context(3);Monitor monitor;
    replies={{EC_STATE_INIT},{EC_STATE_SAFE_OP|EC_STATE_ERROR,0x001b},{EC_STATE_BOOT}};
    monitor.read(*bus,true,at(10000));
    check(bus->slavelist[1].state==EC_STATE_INIT,"Genuine INIT is never replaced by cached OP");
    check(bus->slavelist[2].state==(EC_STATE_SAFE_OP|EC_STATE_ERROR)&&bus->slavelist[2].ALstatuscode==0x001b,
          "A genuine SAFE-OP error and its AL code are immediately adopted");
    check(bus->slavelist[3].state==EC_STATE_BOOT,"A valid BOOT reply is not mistaken for read failure");
    check(bus->slavelist[0].state==EC_STATE_INIT&&bus->slavelist[0].ALstatuscode==0x001b,
          "Mixed actual states retain the lowest state and combined AL code");
    check(!monitor.pending()&&monitor.failures()==0&&monitor.takeEvents().empty(),
          "Real state transitions are not counted as read timeouts");
    replies={{EC_STATE_NONE},{EC_STATE_NONE},{EC_STATE_ERROR,0x0011}};
    monitor.read(*bus,true,at(11000));
    check(bus->slavelist[1].state==EC_STATE_INIT,"A later timeout retains INIT instead of reviving historical OP");
    check(bus->slavelist[2].state==(EC_STATE_SAFE_OP|EC_STATE_ERROR)&&bus->slavelist[2].ALstatuscode==0x001b,
          "A timeout cannot clear an existing real error flag or AL code");
    check(bus->slavelist[3].state==EC_STATE_ERROR&&bus->slavelist[3].ALstatuscode==0x0011,
          "Only exact NONE is treated as an empty reply, not a masked state with an error bit");
}
void independentDeadlines() {
    auto bus=context(2);Monitor monitor;
    replies={{EC_STATE_NONE},{EC_STATE_PRE_OP}};
    monitor.read(*bus,true,at(10000));
    event(monitor,Kind::Failed,1,"Only the failed station enters the grace period");
    check(bus->slavelist[2].state==EC_STATE_PRE_OP&&bus->slavelist[0].state==EC_STATE_PRE_OP,
          "A different station's PRE-OP transition is not hidden by a pending OP cache");
    replies={{EC_STATE_NONE},{EC_STATE_OPERATIONAL}};
    monitor.read(*bus,true,at(11000));
    check(monitor.failures()==2&&monitor.takeEvents().empty(),"Repeated failures are counted without repeating the transition log");
    monitor.expire(*bus,true,at(11999));
    check(bus->slavelist[1].state==EC_STATE_OPERATIONAL,"The cache remains valid immediately before its deadline");
    monitor.expire(*bus,true,at(12000));
    check(bus->slavelist[1].state==EC_STATE_NONE&&bus->slavelist[1].ALstatuscode==0,
          "A continuously unreadable AL state becomes unknown exactly at the two-second deadline");
    check(bus->slavelist[2].state==EC_STATE_OPERATIONAL&&bus->slavelist[0].state==EC_STATE_NONE,
          "A successful neighbouring station neither resets nor expires another station's cache");
    event(monitor,Kind::Unavailable,1,"Expiry emits one unavailable-state diagnostic");
    monitor.expire(*bus,true,at(12500));
    monitor.read(*bus,true,at(13000));
    check(bus->slavelist[1].state==EC_STATE_NONE&&monitor.failures()==3&&monitor.takeEvents().empty(),
          "Repeated AL failures cannot extend the deadline or resurrect an expired OP state");
}
void processDataFailure() {
    auto bus=context(1);Monitor monitor;
    replies={{EC_STATE_NONE}};
    monitor.read(*bus,true,at(10000));monitor.takeEvents();
    monitor.expire(*bus,false,at(10001));
    check(bus->slavelist[1].state==EC_STATE_NONE,"Invalid PDO WKC immediately invalidates a temporarily retained AL state");
    event(monitor,Kind::Unavailable,1,"PDO invalidation is reported once");
    monitor.expire(*bus,true,at(10002));
    monitor.read(*bus,true,at(11000));
    check(bus->slavelist[1].state==EC_STATE_NONE&&monitor.takeEvents().empty(),
          "PDO recovery alone cannot revive old OP without a new AL reply");
    replies={{EC_STATE_SAFE_OP}};monitor.read(*bus,true,at(11500));
    check(bus->slavelist[1].state==EC_STATE_SAFE_OP&&!monitor.pending(),
          "Only a fresh state reply establishes a state after PDO invalidation");
    event(monitor,Kind::Recovered,1,"The fresh AL reply completes recovery");

    monitor.reset();bus->slavelist[1].state=EC_STATE_OPERATIONAL;replies={{EC_STATE_NONE}};
    monitor.read(*bus,false,at(20000));
    check(bus->slavelist[1].state==EC_STATE_NONE,"An AL read made with incomplete PDO WKC does not retain previous OP");
    check(monitor.pending()&&monitor.failures()==1,"A rejected cache still records the failed AL read");
}
void partialRecovery() {
    auto bus=context(2);Monitor monitor;
    replies={{EC_STATE_NONE},{EC_STATE_NONE}};
    monitor.read(*bus,true,at(10000));
    check(monitor.takeEvents().size()==2&&monitor.failures()==2,"Two lost station replies create separate failure episodes");
    replies={{EC_STATE_OPERATIONAL},{EC_STATE_NONE}};
    monitor.read(*bus,true,at(11000));
    event(monitor,Kind::Recovered,1,"Only the recovered station emits a recovery event");
    check(monitor.pending()&&monitor.failures()==3,"One recovery does not clear another station's pending failure");
    monitor.expire(*bus,true,at(12000));
    check(bus->slavelist[1].state==EC_STATE_OPERATIONAL&&bus->slavelist[2].state==EC_STATE_NONE,
          "Partial recovery leaves the still-unreadable station on its original deadline");
    event(monitor,Kind::Unavailable,2,"Only the still-unreadable station expires");
}
void deadlineOnRead() {
    auto bus=context(1);Monitor monitor;
    replies={{EC_STATE_NONE}};
    monitor.read(*bus,true,at(10000));monitor.takeEvents();
    // No intervening expire call: a delayed polling read must still report the
    // old cache becoming unavailable before processing its next failed reply.
    monitor.read(*bus,true,at(12000));
    check(bus->slavelist[1].state==EC_STATE_NONE,"A poll on the deadline cannot renew old OP");
    event(monitor,Kind::Unavailable,1,"Expiry during read produces the same single transition as per-cycle expiry");
    monitor.expire(*bus,true,at(12001));
    check(monitor.takeEvents().empty(),"The following cycle does not duplicate a read-time expiry event");

    monitor.reset();bus->slavelist[1].state=EC_STATE_OPERATIONAL;
    monitor.read(*bus,true,at(20000));monitor.takeEvents();
    monitor.read(*bus,false,at(21000));
    check(bus->slavelist[1].state==EC_STATE_NONE,"An invalid-PDO read cannot silently renew retained OP");
    event(monitor,Kind::Unavailable,1,"Invalidation at read time also emits one transition");
}
void connectionReset() {
    auto bus=context(1);Monitor monitor;
    replies={{EC_STATE_NONE}};monitor.read(*bus,true,at(10000));
    check(monitor.pending()&&monitor.failures()==1,"The original connection has a pending failed poll");
    monitor.reset();
    check(!monitor.pending()&&monitor.failures()==0&&monitor.takeEvents().empty(),
          "Reset clears prior connection failure timers, counters and events");
    bus->slavelist[1].state=EC_STATE_INIT;bus->slavelist[1].ALstatuscode=0;
    monitor.read(*bus,true,at(20000));
    check(bus->slavelist[1].state==EC_STATE_INIT&&monitor.failures()==1,
          "A new connection cannot inherit cached OP from the previous connection");
    event(monitor,Kind::Failed,1,"The new connection starts its own failure episode");
    monitor.expire(*bus,true,at(21999));
    check(bus->slavelist[1].state==EC_STATE_INIT,"The new failure episode receives its own bounded grace period");
}
}

extern "C" int ecx_readstate(ecx_contextt* context) {
    ++reads;
    if(replies.size()!=size_t(context->slavecount))throw std::runtime_error("Missing mock AL reply");
    int lowest=0xff;
    for(int s=1;s<=context->slavecount;++s) {
        context->slavelist[s].state=replies[size_t(s-1)].state;
        context->slavelist[s].ALstatuscode=replies[size_t(s-1)].code;
        if((context->slavelist[s].state&0x0f)<lowest)lowest=context->slavelist[s].state&0x0f;
    }
    // Deliberately leave aggregate fields inconsistent. The wrapper must
    // rebuild them after it restores any native per-slave state/code.
    context->slavelist[0].state=0xffff;context->slavelist[0].ALstatuscode=0xffff;
    return lowest;
}

int main() {
    try {
        transientFailure();actualStateChanges();independentDeadlines();
        processDataFailure();partialRecovery();deadlineOnRead();connectionReset();
        std::cout<<checks<<" cyclic AL-state fault-injection checks passed.\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr<<"Cyclic AL-state check failed: "<<error.what()<<'\n';
        return 1;
    }
}
