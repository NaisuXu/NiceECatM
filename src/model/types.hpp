// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace niceecatm {
enum class AlState : uint16_t { Init=1, PreOp=2, SafeOp=4, Op=8 };
const char* stateName(uint16_t state);
struct Adapter { std::string name, description; };
struct Connection {
    bool simulation=true, redundant=false, overlap=false;
    std::string primary, secondary;
    int cycleUs=10000;
};
struct LocalizedText { uint32_t lcid=0; std::string text; };
// CoE object descriptions are metadata, never cached live values. Access bits
// are CoE read PRE-OP/SAFE-OP/OP (0..2), write PRE-OP/SAFE-OP/OP (3..5).
struct SdoEntry {
    uint8_t subindex=0;
    uint16_t dataType=0, access=0;
    unsigned bitLength=0;
    bool accessKnown=false, online=false, esi=false;
    std::string type;
    std::vector<LocalizedText> names;
};
struct SdoObject {
    uint16_t index=0;
    uint8_t objectCode=0, maxSubindex=0;
    bool online=false, esi=false;
    std::vector<LocalizedText> names;
    std::vector<SdoEntry> entries;
};
struct SdoDictionary {
    std::vector<SdoObject> objects;
    bool complete=false;
    std::string status;
};
// All offsets are relative to the compact per-slave IN or OUT image, in bits.
// Padding participates in layout validation but is not an editable signal.
struct PdoEntry {
    bool input=true, padding=false;
    unsigned bitOffset=0, bitLength=0;
    uint16_t pdoIndex=0, index=0;
    uint8_t subindex=0;
    int syncManager=-1;
    std::string type, source;
    std::vector<LocalizedText> names;
};
std::string pdoEntryName(const PdoEntry&, bool chinese);
std::string pdoType(const std::string& name, unsigned bits);
std::string pdoType(uint16_t ethercatType, unsigned bits);
struct Slave {
    int position=0;
    std::string name;
    uint32_t vendor=0, product=0, revision=0, serial=0;
    uint16_t state=1, requested=1, alCode=0, address=0, mailboxProtocols=0;
    bool dc=false;
    unsigned inputBits=0, outputBits=0, inputStartBit=0, outputStartBit=0;
    int inputOffset=-1, outputOffset=-1;
    std::vector<uint8_t> inputs, outputs;
    bool inputLayoutKnown=false, outputLayoutKnown=false;
    std::vector<PdoEntry> pdoEntries;
    std::string pdoIssue, esiFile, esiStatus;
    bool sdoInfo=false;
    // Immutable descriptions are shared by GUI/script snapshots, keeping large
    // dictionaries out of per-cycle deep copies. ESI never overwrites live OD.
    std::shared_ptr<const SdoDictionary> sdoDictionary, esiSdoDictionary;
};
std::vector<SdoObject> mergedSdoObjects(const Slave&);
std::string sdoName(const std::vector<LocalizedText>&, bool chinese);
struct LogEntry { uint64_t sequence=0; std::string time, message; bool error=false; };
struct Result {
    bool ok=false;
    std::string message;
    std::vector<uint8_t> data;
};
struct Snapshot {
    bool simulation=true, connected=false, mapped=false, running=false, busy=false;
    Connection connection;
    uint64_t generation=0, completed=0, cycles=0, missed=0, errors=0;
    int wkc=0, expectedWkc=0;
    double actualCycleUs=0, maxJitterUs=0;
    std::string status="Ready", driver;
    std::string esiDirectory, esiStatus;
    size_t esiFiles=0, esiDevices=0;
    uint64_t descriptionRevision=0;
    std::vector<Adapter> adapters;
    std::vector<Slave> slaves;
    std::vector<LogEntry> logs;
    Result result;
};
enum class CommandKind { RefreshAdapters, Scan, Disconnect, State, SetCycle, Start, Stop, ReadRegister, WriteRegister, ReadSdo, WriteSdo, ReadSdoDictionary, WriteOutput };
struct Command {
    CommandKind kind=CommandKind::RefreshAdapters;
    uint64_t generation=0;
    Connection connection;
    int slave=0;
    AlState state=AlState::PreOp;
    uint16_t address=0, index=0;
    uint8_t subindex=0;
    int length=1, offset=0;
    std::vector<uint8_t> data;
};
// Strict byte and bit conversion is shared by the GUI, CLI and simulation.
bool parseHex(const std::string& text, std::vector<uint8_t>& bytes, std::string& error);
std::string hexBytes(const std::vector<uint8_t>& bytes);
bool writeBits(std::vector<uint8_t>& bytes, unsigned bit, unsigned width, uint64_t value, std::string& error);
uint64_t readBits(const std::vector<uint8_t>& bytes, unsigned bit, unsigned width);
bool encodeValue(const std::string& text, const std::string& type, bool hex, std::vector<uint8_t>& bytes, std::string& error);
std::string decodeValue(const std::vector<uint8_t>& bytes, const std::string& type, bool hex=false);
} // namespace niceecatm
