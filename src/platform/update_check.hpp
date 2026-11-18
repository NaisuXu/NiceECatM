// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace niceecatm::updates {
inline constexpr char ReleasesUrl[]="https://github.com/NaisuXu/NiceECatM/releases";
inline constexpr char ApiUrl[]="https://api.github.com/repos/NaisuXu/NiceECatM/releases/latest";
enum class State { Idle, Checking, Current, Available, NoRelease, Unavailable };
struct View {State state=State::Idle;std::string latestVersion,detail;};
struct Response {int status=0;std::string body,error;};
using Fetch=std::function<Response(const std::atomic<bool>&)>;
// System HTTPS transport; TLS verification is always enabled. No bus access.
Response fetchLatest(const std::atomic<bool>& canceled);
// Pure bounded release parsing/version comparison, shared with offline tests.
View evaluate(const std::string& currentVersion,const Response&);
class Checker {
public:
    Checker();
    ~Checker();
    Checker(const Checker&)=delete;
    Checker& operator=(const Checker&)=delete;
    void start(std::string currentVersion,Fetch fetch=fetchLatest);
    void stop();
    View snapshot() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
