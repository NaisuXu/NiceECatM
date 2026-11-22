// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#include "ethercat/pdo_description_queue.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
using namespace niceecatm;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void appendObject(std::vector<PdoEntry>& entries, bool input, uint16_t index,
                  std::initializer_list<unsigned> widths, unsigned& offset) {
    uint8_t subindex = 0;
    for (auto width : widths) {
        PdoEntry entry;
        entry.input = input;
        entry.index = index;
        entry.subindex = ++subindex;
        entry.bitOffset = offset;
        entry.bitLength = width;
        entry.source = "CoE";
        entries.push_back(entry);
        offset += width;
    }
}

void largeDevice() {
    // TTS7612 maps 44 outputs followed by 166 inputs. The first reported
    // missing field, 0x6080:04, is request 129 at input byte 217.
    std::vector<PdoEntry> entries;
    unsigned outBits = 0, inBits = 0;
    appendObject(entries, false, 0x7000, {8, 8}, outBits);
    for (unsigned group = 1; group <= 14; ++group)
        appendObject(entries, false, 0x7000 + 16 * group, {8, 8, 32}, outBits);
    appendObject(entries, true, 0x6000, {8, 8, 8, 8}, inBits);
    for (unsigned group = 1; group <= 14; ++group)
        appendObject(entries, true, 0x6000 + 16 * group, {8, 8, 8, 8, 32, 32, 32, 32, 16, 32, 32}, inBits);
    appendObject(entries, true, 0x60f0, {32, 64, 64, 8, 8, 8, 8, 16}, inBits);
    check(entries.size() == 210 && outBits == 688 && inBits == 3600, "Unexpected TTS7612 layout");
    check(entries[127].index == 0x6080 && entries[127].subindex == 3 &&
          entries[128].bitOffset == 217 * 8, "Unexpected location of old lookup cutoff");

    PdoDescriptionQueue queue(entries);
    size_t reads = 0;
    auto read = [&](uint16_t index, uint8_t subindex) {
        ++reads;
        if (index == 0x6080 && subindex == 4)
            return PdoDescription{PdoDescriptionStatus::Available, 5, 8, "sample_valid"};
        if (index == 0x6080 && subindex == 5)
            return PdoDescription{PdoDescriptionStatus::Available, 4, 32, "org_value_uv"};
        return PdoDescription{PdoDescriptionStatus::Available, 5, 8, "channel"};
    };
    for (unsigned n = 0; n < 128; ++n)
        check(queue.pending() && queue.step(entries, read), "Lost initial entry description");
    check(queue.pending() && entries[128].names.empty(), "Must retain unread metadata after initial batch");
    while (queue.pending()) queue.step(entries, read);
    check(reads == 210, "One online description read per unique mapped field");
    check(std::all_of(entries.begin(), entries.end(), [](const PdoEntry& entry) { return !entry.names.empty(); }),
          "Every successfully described field must have a name");
    check(pdoEntryName(entries[128], false) == "sample_valid" && entries[128].type == "UINT8",
          "Restore field 129 immediately after old cutoff");
    check(pdoEntryName(entries[129], false) == "org_value_uv" && entries[129].type == "INT32" &&
          entries[129].bitOffset == 218 * 8 && entries[129].bitLength == 32,
          "Resolve 32-bit field as a single named signed value at its mapped offset");
}

void unavailableEntries() {
    std::vector<PdoEntry> entries;
    unsigned offset = 0;
    appendObject(entries, true, 0x6100, {8, 8, 8, 8}, offset);
    entries.push_back(entries.back()); // Same OD field mapped again.
    entries.back().bitOffset = offset;
    PdoEntry padding;
    padding.padding = true;
    entries.push_back(padding);

    PdoDescriptionQueue queue(entries);
    std::vector<unsigned> reads;
    auto read = [&](uint16_t, uint8_t subindex) {
        reads.push_back(subindex);
        if (subindex == 1) return PdoDescription{PdoDescriptionStatus::Missing};
        if (subindex == 3) return PdoDescription{PdoDescriptionStatus::Failed};
        return PdoDescription{PdoDescriptionStatus::Available, 5, 8, "present"};
    };
    while (queue.pending()) queue.step(entries, read);
    check((reads == std::vector<unsigned>{1, 2, 3, 4}), "Deduplicate requests and skip padding");
    check(entries[0].names.empty() && entries[2].names.empty() && entries[5].names.empty(),
          "Missing and failed descriptions must leave the data unnamed");
    check(pdoEntryName(entries[1], false) == "present" && pdoEntryName(entries[3], false) == "present" &&
          pdoEntryName(entries[4], false) == "present", "Continue past a failed entry and annotate all occurrences");

    PdoDescriptionQueue unavailable(entries);
    unsigned failures = 0;
    auto fail = [&](uint16_t, uint8_t) {
        ++failures;
        return PdoDescription{PdoDescriptionStatus::Failed};
    };
    while (unavailable.pending()) unavailable.step(entries, fail);
    check(failures == 3, "An unresponsive slave must not time out once per mapped field");
}
}

int main() {
    try {
        largeDevice();
        unavailableEntries();
        std::cout << "PDO description checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "PDO description check failed: " << error.what() << '\n';
        return 1;
    }
}
