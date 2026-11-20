// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#pragma once
#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>

namespace niceecatm::foe {
struct MailboxStateWire {
    std::function<int(uint16_t)> write;
    std::function<int(uint16_t& state,uint16_t& code)> read;
    std::function<uint64_t()> nowMs;
    std::function<void(unsigned)> waitMs;
};
inline bool requestMailboxState(const MailboxStateWire& wire,uint16_t desired,unsigned timeoutMs) {
    if(!wire.write||!wire.read||!wire.nowMs||!wire.waitMs||!timeoutMs)return false;
    const auto started=wire.nowMs();
    uint16_t actual=0,code=0;
    if(wire.read(actual,code)!=1)return false;
    const auto state=actual&15;
    if(state!=1&&state!=2&&state!=3&&state!=4&&state!=8)return false;
    if(desired==1&&(actual&0x10)) {
        // ACK only a confirmed error, then finish with plain INIT. Some ESCs
        // mirror the ACK bit in AL Status; never mistake that for confirmed
        // INIT or leave it latched after recovering a rejected BOOT request.
        if(wire.write(0x11)!=1)return false;
        do {
            wire.waitMs(1);
            if(wire.read(actual,code)==1&&(!(actual&0x10)||code==0))break;
        }while(wire.nowMs()-started<std::min(100u,timeoutMs));
        if(wire.nowMs()-started>=timeoutMs)return false;
    }
    if(wire.write(desired)!=1)return false;
    do {
        if(wire.read(actual,code)==1&&actual==desired)return true;
        wire.waitMs(1);
    }while(wire.nowMs()-started<timeoutMs);
    return false;
}
using MailboxImage=std::array<uint8_t,16>;
struct MailboxRegisters {
    std::function<int(uint16_t,size_t,const void*)> write;
    std::function<int(uint16_t,size_t,void*)> read;
};
// Both SMs are written and read back before the caller publishes the new
// context offsets. Status/PDI bytes are volatile; compare address, size,
// control and the activation bit, never accept a zero/multiple-target WKC.
inline bool programMailbox(const MailboxRegisters& wire,const MailboxImage& image) {
    if(!wire.write||!wire.read)return false;
    if(wire.write(0x0800,8,image.data())!=1||wire.write(0x0808,8,image.data()+8)!=1)return false;
    for(size_t sm=0;sm<2;++sm) {
        std::array<uint8_t,8> actual{};
        if(wire.read(uint16_t(0x0800+sm*8),8,actual.data())!=1)return false;
        const auto* expected=image.data()+sm*8;
        for(size_t i=0;i<5;++i)if(actual[i]!=expected[i])return false;
        if((actual[6]&1)!=(expected[6]&1))return false;
    }
    return true;
}

// The normal configuration survives partial writes and failed BOOT requests.
// Only an explicit, confirmed INIT + complete normal-SM restoration clears it.
// Config is opaque, allowing native context fields and raw SM bytes to stay
// together without exposing SOEM objects outside the I/O owner.
template<class Config> class BootstrapSession {
    std::optional<Config> normal_;
public:
    using State=std::function<bool(uint16_t)>;
    using Configure=std::function<bool(const Config&)>;
    bool active() const {return normal_.has_value();}
    bool enter(const Config& normal,const Config& bootstrap,const State& state,const Configure& configure) {
        if(!normal_)normal_=normal;
        return state(1)&&configure(bootstrap)&&state(3);
    }
    bool leave(const State& state,const Configure& configure) {
        if(!normal_)return true;
        if(!state(1)||!configure(*normal_))return false;
        normal_.reset();return true;
    }
};
} // namespace niceecatm::foe
