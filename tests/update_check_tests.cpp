// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#include "platform/update_check.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace niceecatm::updates;
using namespace std::chrono_literals;
int checks = 0;
constexpr char Current[] = "202611181200";

void check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

Response release(const std::string& tag, const std::string& extra = {}) {
    return {200, "{\"tag_name\":\"" + tag + "\",\"draft\":false,\"prerelease\":false" + extra + "}", {}};
}

void expect(const Response& response, State state, const char* message) {
    check(evaluate(Current, response).state == state, message);
}

template <class Predicate>
bool waitFor(Predicate predicate) {
    const auto until = std::chrono::steady_clock::now() + 2s;
    do {
        if (predicate()) return true;
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < until);
    return predicate();
}

void parsingTests() {
    const auto newer = evaluate(Current, release("202611181201"));
    check(newer.state == State::Available && newer.latestVersion == "202611181201", "newer release is available");
    expect(release(Current), State::Current, "equal release is current");
    expect(release("202611171200"), State::Current, "local development build can be newer");
    const auto prefixed = evaluate(Current, release("v202611181201"));
    check(prefixed.state == State::Available && prefixed.latestVersion == "202611181201", "single v prefix is normalized");
    check(evaluate("v202611181200", release("202611181201")).state == State::Available, "current timestamp prefix is supported");
    check(evaluate("not-a-version", release("202611181201")).state == State::Unavailable, "invalid local version is not reported current");
    for (const auto& tag : {"", "20261118120", "2026111812000", "vv202611181201", "release-202611181201", " 202611181201", "202611181201 ", "2026111812a1", "000011181201", "202600181201", "202613181201", "202611001201", "202611311201", "202602291201", "210002291201", "202611182401", "202611181260", "202611181201-beta"}) {
        expect(release(tag), State::Unavailable, "invalid release timestamp is rejected");
    }
    expect(release("202402291201"), State::Current, "leap day is accepted");
    expect(release("200002291201"), State::Current, "400-year leap day is accepted");
    expect(release("202612312359"), State::Available, "calendar and time upper bounds are accepted");
    expect({404, "not json", {}}, State::NoRelease, "404 means no release");
    for (int status : {0, 301, 304, 401, 403, 429, 500, 503}) {
        expect({status, release("209901010000").body, {}}, State::Unavailable, "HTTP failures are not current");
    }
    expect({200, release("209901010000").body, "Timed out"}, State::Unavailable, "transport failure is not current");
    expect({404, {}, "Connection failed"}, State::Unavailable, "transport failure takes precedence over status");
    expect({200, R"({"tag_name":"209901010000","draft":true,"prerelease":false})", {}}, State::NoRelease, "draft is filtered");
    expect({200, R"({"tag_name":"209901010000","draft":false,"prerelease":true})", {}}, State::NoRelease, "prerelease is filtered");

    // Unknown fields are valid JSON but cannot override the root release tag.
    expect(release("202611171200", R"(,"other":{"tag_name":"209901010000"},"body":"tag_name: 209901010000")"), State::Current, "nested metadata and release notes cannot supply version");
    expect({200, R"({"draft":false,"prerelease":false,"other":{"tag_name":"209901010000"}})", {}}, State::Unavailable, "nested tag does not replace missing root tag");
    expect({200, R"({"tag\u005fname":"\u0032\u0030\u0032\u0036\u0031\u0031\u0031\u0038\u0031\u0032\u0030\u0031","dr\u0061ft":false,"pre\u0072elease":false})", {}}, State::Available, "escaped keys and timestamp are decoded");
    expect(release("202611181201", R"(,"name":"\u4e2d\u6587 \ud83d\ude00","body":"\"\\\/\b\f\n\r\t","data":[null,true,false,0,-10,0.01,-1.5e+3,2E-4,{},[]])"), State::Available, "unicode and all JSON value types are accepted");
    expect(release("202611181201", u8",\"name\":\"中文 😀\""), State::Available, "UTF-8 text is accepted");
    expect({200, " \r\n\t" + release("202611181201").body + "\r\n ", {}}, State::Available, "JSON whitespace is accepted");

    const std::vector<std::string> malformed = {
        "", "null", "[]", "{", "{}", R"({"tag_name":"209901010000"})",
        R"({"tag_name":"209901010000","draft":false})",
        R"({"tag_name":209901010000,"draft":false,"prerelease":false})",
        R"({"tag_name":null,"draft":false,"prerelease":false})",
        R"({"tag_name":"209901010000","draft":"false","prerelease":false})",
        R"({"tag_name":"209901010000","draft":false,"prerelease":0})",
        R"({"tag_name":"209901010000","draft":false,"prerelease":false,})",
        R"({"tag_name":"209901010000","tag_name":"202611171200","draft":false,"prerelease":false})",
        R"({"tag_name":"209901010000","tag\u005fname":"202611171200","draft":false,"prerelease":false})",
        R"({"tag_name":"209901010000","draft":false,"draft":true,"prerelease":false})",
        R"({"tag_name":"209901010000","draft":false,"prerelease":false,"prerelease":true})",
        release("202611181201").body + " null",
        release("202611181201", R"(,"other":[1,])").body,
        release("202611181201", R"(,"other":{"a":1,})").body,
        release("202611181201", R"(,"other":01)").body,
        release("202611181201", R"(,"other":-)").body,
        release("202611181201", R"(,"other":.5)").body,
        release("202611181201", R"(,"other":1.)").body,
        release("202611181201", R"(,"other":1e+)").body,
        release("202611181201", R"(,"other":NaN)").body,
        release("202611181201", R"(,"other":truefalse)").body,
        release("202611181201", R"(,"other":"\x20")").body,
        release("202611181201", R"(,"other":"\ud800")").body,
        release("202611181201", R"(,"other":"\udc00")").body,
        release("202611181201", R"(,"other":"\ud800\u0041")").body,
        release("202611181201", R"(,"other":"\u00zz")").body,
        release("202611181201", ",\"other\":\"line\nbreak\"").body,
        release("202611181201", std::string(",\"other\":\"") + char(0xc0) + char(0xaf) + "\"").body,
        release("202611181201", std::string(",\"other\":\"") + char(0xed) + char(0xa0) + char(0x80) + "\"").body,
        release("202611181201", std::string(",\"other\":\"") + char(0xf4) + char(0x90) + char(0x80) + char(0x80) + "\"").body
    };
    for (const auto& body : malformed) expect({200, body, {}}, State::Unavailable, "malformed or incomplete JSON is rejected");
    expect(release("202611181201", ",\"other\":" + std::string(31, '[') + "null" + std::string(31, ']')), State::Available, "32 container levels are accepted");
    expect(release("202611181201", ",\"other\":" + std::string(32, '[') + "null" + std::string(32, ']')), State::Unavailable, "excessive JSON depth is rejected");
    auto maximum = release("202611181201", ",\"body\":\"\"");
    maximum.body.insert(maximum.body.size() - 2, 256 * 1024 - maximum.body.size(), 'a');
    expect(maximum, State::Available, "body size limit itself is accepted");
    maximum.body.insert(maximum.body.size() - 2, 1, 'a');
    expect(maximum, State::Unavailable, "oversized body is rejected");
}

void workerTests() {
    const auto uiThread = std::this_thread::get_id();
    std::atomic<int> calls{0};
    std::atomic<bool> entered{false}, onWorker{false}, allowReturn{false};
    Checker checker;
    check(checker.snapshot().state == State::Idle, "checker initially idle");
    const Fetch fetch = [&](const std::atomic<bool>& canceled) {
        ++calls;
        onWorker.store(std::this_thread::get_id() != uiThread);
        entered.store(true);
        waitFor([&] { return allowReturn.load() || canceled.load(); });
        return release("209901010000");
    };
    checker.start(Current, fetch);
    check(waitFor([&] { return entered.load(); }), "background request begins");
    check(onWorker.load(), "fetch does not run on calling thread");
    check(checker.snapshot().state == State::Checking, "checking remains observable during request");
    checker.start(Current, fetch);
    allowReturn.store(true);
    check(waitFor([&] { return checker.snapshot().state != State::Checking; }), "background check completes");
    check(checker.snapshot().state == State::Available, "background result is published");
    checker.start(Current, fetch);
    checker.stop();
    checker.start(Current, fetch);
    check(calls.load() == 1, "repeated starts never repeat the startup request");

    std::atomic<bool> canceledObserved{false}, returning{false};
    {
        Checker canceledChecker;
        canceledChecker.start(Current, [&](const std::atomic<bool>& canceled) {
            entered.store(true);
            waitFor([&] { return canceled.load(); });
            canceledObserved.store(canceled.load());
            returning.store(true);
            return release("209901010000");
        });
        canceledChecker.stop();
        check(canceledObserved.load() && returning.load(), "stop cancels and joins the worker");
        check(canceledChecker.snapshot().state == State::Idle, "late result is not published after cancellation");
        canceledChecker.stop();
    }
    canceledObserved.store(false);
    {
        Checker destructed;
        destructed.start(Current, [&](const std::atomic<bool>& canceled) {
            waitFor([&] { return canceled.load(); });
            canceledObserved.store(canceled.load());
            return release("209901010000");
        });
    }
    check(canceledObserved.load(), "destructor cancels and joins without detach");
    for (bool standard : {false, true}) {
        Checker failed;
        failed.start(Current, [standard](const std::atomic<bool>&) -> Response {
            if (standard) throw std::runtime_error("injected failure");
            throw 7;
        });
        check(waitFor([&] { return failed.snapshot().state == State::Unavailable; }), "fetch exceptions become unavailable");
    }
    Checker emptyFetch;
    emptyFetch.start(Current, {});
    check(waitFor([&] { return emptyFetch.snapshot().state == State::Unavailable; }), "empty fetch does not terminate application");
    Checker stopped;
    stopped.stop();
    stopped.start(Current, [&](const std::atomic<bool>&) { ++calls; return release(Current); });
    check(stopped.snapshot().state == State::Idle && calls.load() == 1, "stopped checker cannot start new work");
}
} // namespace

int main() {
    try {
        parsingTests();
        workerTests();
        std::cout << checks << " update checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Update check failed: " << error.what() << '\n';
        return 1;
    }
}
