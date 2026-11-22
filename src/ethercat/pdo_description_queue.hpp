// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#pragma once
#include "model/types.hpp"
#include <map>

namespace niceecatm {
enum class PdoDescriptionStatus { Available, Missing, Failed };

struct PdoDescription {
    PdoDescriptionStatus status;
    uint16_t dataType=0;
    unsigned bitLength=0;
    std::string name;
};

// The mapping is immutable until rescan or invalidation. Multiple PDOs can
// refer to the same dictionary entry, including in different directions.
class PdoDescriptionQueue {
    struct Object {
        uint16_t index;
        uint8_t subindex;
        std::vector<size_t> rows;
    };
    std::vector<Object> objects_;
    size_t next_=0, unavailable_=0;
    unsigned consecutiveFailures_=0;
public:
    explicit PdoDescriptionQueue(const std::vector<PdoEntry>& entries) {
        std::map<uint32_t,size_t> positions;
        for(size_t row=0;row<entries.size();++row) {
            const auto& entry=entries[row];
            if(entry.padding)continue;
            const uint32_t key=(uint32_t(entry.index)<<8)|entry.subindex;
            const auto [at,inserted]=positions.emplace(key,objects_.size());
            if(inserted)objects_.push_back({entry.index,entry.subindex,{}});
            objects_[at->second].rows.push_back(row);
        }
    }
    bool pending() const {return next_<objects_.size();}
    size_t unavailable() const {return unavailable_;}
    bool stoppedAfterFailures() const {return consecutiveFailures_>=3;}

    template<class Reader>
    bool step(std::vector<PdoEntry>& entries,Reader&& read) {
        if(!pending())return false;
        const auto& object=objects_[next_++];
        const auto description=read(object.index,object.subindex);
        if(description.status!=PdoDescriptionStatus::Failed)consecutiveFailures_=0;
        if(description.status!=PdoDescriptionStatus::Available) {
            ++unavailable_;
            if(description.status==PdoDescriptionStatus::Failed)++consecutiveFailures_;
            if(stoppedAfterFailures()) {
                unavailable_+=objects_.size()-next_;
                next_=objects_.size();
            }
            return false;
        }
        bool changed=false;
        for(const size_t row:object.rows) {
            auto& entry=entries[row];
            if(description.bitLength>=entry.bitLength) {
                const auto type=pdoType(description.dataType,entry.bitLength);
                if(!type.empty()&&entry.type!=type){entry.type=type;changed=true;}
            }
            if(!description.name.empty()&&entry.names.empty()) {
                entry.names.push_back({0,description.name});
                changed=true;
            }
        }
        return changed;
    }
};
} // namespace niceecatm
