// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace niceecatm::sii {
// Category offsets address payload bytes, rather than their four-byte header.
struct Category { uint16_t type=0; size_t offset=0,bytes=0; };
struct Image {
    std::vector<uint8_t> bytes;
    uint32_t vendor=0,product=0,revision=0,serial=0;
    uint16_t alias=0;
    size_t capacity=0,usedBytes=0,sourceBytes=0;
    uint8_t crc=0;
    std::vector<Category> categories;
};
struct Device { std::string name; size_t ordinal=0; };
// These functions leave the destination unchanged on failure. ESI strings use
// English, then an unqualified name, then the first available language. GUI
// language is deliberately not an input to EEPROM serialization.
bool listDevices(const std::filesystem::path&,std::vector<Device>&,std::string& error);
bool fromEsi(const std::filesystem::path&,size_t ordinal,Image&,std::string& error);
bool fromBin(const std::vector<uint8_t>&,Image&,std::string& error);
bool loadBin(const std::filesystem::path&,Image&,std::string& error);
bool saveBin(const std::filesystem::path&,const Image&,std::string& error);
// CRC-8: polynomial 0x07, initial remainder 0xFF, first 14 config bytes.
uint8_t configCrc(const std::vector<uint8_t>&);
} // namespace niceecatm::sii
