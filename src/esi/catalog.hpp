// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#pragma once
#include "model/types.hpp"
#include <filesystem>
#include <memory>

namespace niceecatm {
// An offline metadata library. It neither changes PDO assignments nor sends
// startup commands; live layout discovery stays with the EtherCAT owner.
class EsiCatalog {
public:
    explicit EsiCatalog(std::filesystem::path directory);
    Result reload();
    void decorate(Snapshot&) const;
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
}
