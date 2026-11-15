// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#pragma once
#include <chrono>
#include <cstdint>
#include <vector>
extern "C" {
#include "soem/soem.h"
}

namespace niceecatm {
// Only for background monitoring. Explicit state requests must obtain a fresh
// confirmation and must never use this grace period to confirm a transition.
class CyclicStatePoll {
public:
    using Clock=std::chrono::steady_clock;
    static constexpr auto FailureGrace=std::chrono::seconds(2);
    enum class EventKind { Failed, Unavailable, Recovered };
    struct Event { EventKind kind; int slave; uint16_t previousState; };
private:
    struct Entry {
        bool pending=false,retaining=false;
        Clock::time_point failedSince{};
        uint16_t beforeState=0,beforeCode=0;
    };
    std::vector<Entry> entries_;
    std::vector<Event> events_;
    uint64_t failures_=0;

    static bool known(uint16_t state) {
        switch(state&0x0f) {
        case EC_STATE_INIT:case EC_STATE_PRE_OP:case EC_STATE_BOOT:
        case EC_STATE_SAFE_OP:case EC_STATE_OPERATIONAL:return true;
        default:return false;
        }
    }
    static void aggregate(ecx_contextt& context) {
        uint16_t lowest=0xff,code=0;
        for(int s=1;s<=context.slavecount;++s) {
            const auto& slave=context.slavelist[s];
            if((slave.state&0x0f)<lowest)lowest=slave.state&0x0f;
            code|=slave.ALstatuscode;
        }
        context.slavelist[0].state=context.slavecount?lowest:0;
        context.slavelist[0].ALstatuscode=code;
    }
public:
    void reset() {entries_.clear();events_.clear();failures_=0;}
    void read(ecx_contextt& context,bool processValid,Clock::time_point now) {
        if(entries_.size()!=size_t(context.slavecount+1))entries_.assign(size_t(context.slavecount+1),{});
        expire(context,processValid,now);
        for(int s=1;s<=context.slavecount;++s) {
            entries_[s].beforeState=context.slavelist[s].state;
            entries_[s].beforeCode=context.slavelist[s].ALstatuscode;
        }
        // SOEM's return value is the lowest AL state, not a transport WKC.
        // A failed per-slave read is represented by an exact NONE (zero).
        ecx_readstate(&context);
        for(int s=1;s<=context.slavecount;++s) {
            auto& entry=entries_[s];auto& slave=context.slavelist[s];
            if(slave.state!=EC_STATE_NONE) {
                if(entry.pending)events_.push_back({EventKind::Recovered,s,slave.state});
                entry.pending=entry.retaining=false;
                continue;
            }
            ++failures_;
            if(!entry.pending) {
                entry.pending=true;entry.failedSince=now;
                events_.push_back({EventKind::Failed,s,entry.beforeState});
            }
            entry.retaining=processValid&&known(entry.beforeState)&&now-entry.failedSince<FailureGrace;
            if(entry.retaining) {
                slave.state=entry.beforeState;slave.ALstatuscode=entry.beforeCode;
            }
        }
        aggregate(context);
    }
    // Check every PDO cycle, so neither the one-second poll interval nor an
    // intervening PDO failure can extend the lifetime of a cached state.
    void expire(ecx_contextt& context,bool processValid,Clock::time_point now) {
        bool changed=false;
        for(int s=1;s<=context.slavecount&&size_t(s)<entries_.size();++s) {
            auto& entry=entries_[s];
            if(entry.retaining&&(!processValid||now-entry.failedSince>=FailureGrace)) {
                events_.push_back({EventKind::Unavailable,s,context.slavelist[s].state});
                context.slavelist[s].state=EC_STATE_NONE;context.slavelist[s].ALstatuscode=0;
                entry.retaining=false;changed=true;
            }
        }
        if(changed)aggregate(context);
    }
    bool pending() const {for(const auto& entry:entries_)if(entry.pending)return true;return false;}
    uint64_t failures() const {return failures_;}
    std::vector<Event> takeEvents() {std::vector<Event> result;result.swap(events_);return result;}
};
} // namespace niceecatm
