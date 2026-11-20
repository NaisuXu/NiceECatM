// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace niceecatm::eeprom {
// Register callbacks return the actual datagram WKC. An AP operation must
// address exactly one responding ESC; a zero-filled failed read is not data.
struct Wire {
    std::function<int(uint16_t,size_t,void*)> read;
    std::function<int(uint16_t,size_t,const void*)> write;
};

// Holds EEPROM arbitration only, never a SOEM context or a second master.
// finish() reports cleanup errors; the destructor is an additional guard.
class Session {
public:
    explicit Session(Wire wire,std::function<bool()> cancelled={}):wire_(std::move(wire)),cancelled_(std::move(cancelled)){}
    ~Session();
    Session(const Session&)=delete;
    Session& operator=(const Session&)=delete;
    bool begin(std::string& error);
    bool finish(std::string& error);
    bool read(size_t offset,size_t length,size_t capacity,std::vector<uint8_t>& data,std::string& error);
    bool writeWord(size_t byteOffset,uint16_t value,std::string& error);
    bool writeAttempted() const {return written_;}
    static constexpr size_t MaxCapacity=131072;
private:
    Wire wire_;
    std::function<bool()> cancelled_;
    bool saved_=false,written_=false;
    uint8_t owner_=0;
    size_t readWidth_=4;
    bool status(uint16_t& value,std::string& error);
    bool readRegister(uint16_t address,size_t size,void* data,std::string& error);
    bool writeRegister(uint16_t address,size_t size,const void* data,std::string& error,unsigned retries=3);
    bool ready(uint16_t& value,std::string& error);
    bool prepare(std::string& error);
    bool command(uint16_t command,size_t byteOffset,std::string& error);
};
} // namespace niceecatm::eeprom
