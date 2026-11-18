// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#include "platform/update_check.hpp"

#include <exception>
#include <mutex>
#include <thread>
#include <utility>

namespace niceecatm::updates {
namespace {
constexpr std::size_t MaxBody = 256 * 1024;
constexpr unsigned MaxDepth = 32;

struct Release {
    std::string tag;
    bool draft = false;
    bool prerelease = false;
    bool hasTag = false;
    bool hasDraft = false;
    bool hasPrerelease = false;
};

// Validate the whole response, including values we do not use. In particular,
// tag_name in a nested object or release notes must never identify a release.
class JsonReader {
public:
    explicit JsonReader(const std::string& text) : text_(text) {}

    bool release(Release& out) {
        space();
        if (!take('{')) return false;
        space();
        if (take('}')) return finish() && complete(out);
        do {
            std::string key;
            if (!string(&key)) return false;
            space();
            if (!take(':')) return false;
            space();
            if (key == "tag_name") {
                if (out.hasTag || !string(&out.tag)) return false;
                out.hasTag = true;
            } else if (key == "draft") {
                if (out.hasDraft || !boolean(out.draft)) return false;
                out.hasDraft = true;
            } else if (key == "prerelease") {
                if (out.hasPrerelease || !boolean(out.prerelease)) return false;
                out.hasPrerelease = true;
            } else if (!value(2)) {
                return false;
            }
            space();
            if (take('}')) return finish() && complete(out);
            if (!take(',')) return false;
            space();
        } while (true);
    }

private:
    static bool complete(const Release& out) {
        return out.hasTag && out.hasDraft && out.hasPrerelease;
    }

    void space() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
            ++pos_;
        }
    }

    bool take(char c) {
        if (pos_ == text_.size() || text_[pos_] != c) return false;
        ++pos_;
        return true;
    }

    bool finish() {
        space();
        return pos_ == text_.size();
    }

    bool literal(const char* token) {
        std::size_t next = pos_;
        for (; *token; ++token, ++next) {
            if (next == text_.size() || text_[next] != *token) return false;
        }
        pos_ = next;
        return true;
    }

    bool boolean(bool& out) {
        if (literal("true")) { out = true; return true; }
        if (literal("false")) { out = false; return true; }
        return false;
    }

    static int hex(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    bool codeUnit(unsigned& out) {
        out = 0;
        for (unsigned i = 0; i < 4; ++i) {
            if (pos_ == text_.size()) return false;
            const int digit = hex(text_[pos_++]);
            if (digit < 0) return false;
            out = out * 16 + static_cast<unsigned>(digit);
        }
        return true;
    }

    static void appendUtf8(std::string& out, unsigned code) {
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xc0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xe0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        } else {
            out.push_back(static_cast<char>(0xf0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        }
    }

    bool rawUtf8(unsigned char first, std::string* out) {
        unsigned code = 0;
        unsigned count = 0;
        unsigned minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            code = first & 0x1f; count = 1; minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            code = first & 0x0f; count = 2; minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            code = first & 0x07; count = 3; minimum = 0x10000;
        } else {
            return false;
        }
        for (unsigned i = 0; i < count; ++i) {
            if (pos_ == text_.size()) return false;
            const auto c = static_cast<unsigned char>(text_[pos_++]);
            if ((c & 0xc0) != 0x80) return false;
            code = (code << 6) | (c & 0x3f);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
        if (out) appendUtf8(*out, code);
        return true;
    }

    bool string(std::string* out) {
        if (!take('"')) return false;
        while (pos_ < text_.size()) {
            const auto c = static_cast<unsigned char>(text_[pos_++]);
            if (c == '"') return true;
            if (c < 0x20) return false;
            if (c >= 0x80) {
                if (!rawUtf8(c, out)) return false;
            } else if (c != '\\') {
                if (out) out->push_back(static_cast<char>(c));
            } else {
                if (pos_ == text_.size()) return false;
                const char escape = text_[pos_++];
                char decoded = 0;
                switch (escape) {
                case '"': decoded = '"'; break;
                case '\\': decoded = '\\'; break;
                case '/': decoded = '/'; break;
                case 'b': decoded = '\b'; break;
                case 'f': decoded = '\f'; break;
                case 'n': decoded = '\n'; break;
                case 'r': decoded = '\r'; break;
                case 't': decoded = '\t'; break;
                case 'u': {
                    unsigned code = 0;
                    if (!codeUnit(code)) return false;
                    if (code >= 0xd800 && code <= 0xdbff) {
                        unsigned low = 0;
                        if (!take('\\') || !take('u') || !codeUnit(low) || low < 0xdc00 || low > 0xdfff) return false;
                        code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
                    } else if (code >= 0xdc00 && code <= 0xdfff) {
                        return false;
                    }
                    if (out) appendUtf8(*out, code);
                    continue;
                }
                default: return false;
                }
                if (out) out->push_back(decoded);
            }
        }
        return false;
    }

    static bool digit(char c) { return c >= '0' && c <= '9'; }

    bool number() {
        take('-');
        if (!take('0')) {
            if (pos_ == text_.size() || text_[pos_] < '1' || text_[pos_] > '9') return false;
            do { ++pos_; } while (pos_ < text_.size() && digit(text_[pos_]));
        }
        if (take('.')) {
            if (pos_ == text_.size() || !digit(text_[pos_])) return false;
            do { ++pos_; } while (pos_ < text_.size() && digit(text_[pos_]));
        }
        if (take('e') || take('E')) {
            if (!take('+')) take('-');
            if (pos_ == text_.size() || !digit(text_[pos_])) return false;
            do { ++pos_; } while (pos_ < text_.size() && digit(text_[pos_]));
        }
        return true;
    }

    bool value(unsigned depth) {
        if (pos_ == text_.size()) return false;
        if ((text_[pos_] == '{' || text_[pos_] == '[') && depth > MaxDepth) return false;
        if (text_[pos_] == '"') return string(nullptr);
        if (take('{')) {
            space();
            if (take('}')) return true;
            do {
                if (!string(nullptr)) return false;
                space();
                if (!take(':')) return false;
                space();
                if (!value(depth + 1)) return false;
                space();
                if (take('}')) return true;
                if (!take(',')) return false;
                space();
            } while (true);
        }
        if (take('[')) {
            space();
            if (take(']')) return true;
            do {
                if (!value(depth + 1)) return false;
                space();
                if (take(']')) return true;
                if (!take(',')) return false;
                space();
            } while (true);
        }
        if (text_[pos_] == 't') return literal("true");
        if (text_[pos_] == 'f') return literal("false");
        if (text_[pos_] == 'n') return literal("null");
        return number();
    }

    const std::string& text_;
    std::size_t pos_ = 0;
};

bool timestamp(std::string& version) {
    if (version.size() == 13 && version[0] == 'v') version.erase(0, 1);
    if (version.size() != 12) return false;
    for (char c : version) if (c < '0' || c > '9') return false;
    const auto part = [&version](std::size_t start, std::size_t count) {
        unsigned value = 0;
        for (std::size_t i = 0; i < count; ++i) value = value * 10 + static_cast<unsigned>(version[start + i] - '0');
        return value;
    };
    const unsigned year = part(0, 4), month = part(4, 2), day = part(6, 2);
    if (!year || month < 1 || month > 12 || part(8, 2) > 23 || part(10, 2) > 59) return false;
    const unsigned days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    return day >= 1 && day <= days[month - 1] + (month == 2 && leap ? 1u : 0u);
}

View unavailable(std::string detail) { return {State::Unavailable, {}, std::move(detail)}; }
} // namespace

View evaluate(const std::string& currentVersion, const Response& response) {
    if (!response.error.empty()) return unavailable(response.error);
    if (response.status == 404) return {State::NoRelease, {}, "No published release is available."};
    if (response.status != 200) return unavailable("GitHub returned HTTP " + std::to_string(response.status) + ".");
    if (response.body.size() > MaxBody) return unavailable("The release response is too large.");
    Release release;
    if (!JsonReader(response.body).release(release)) return unavailable("The release response is invalid or incomplete.");
    if (release.draft || release.prerelease) return {State::NoRelease, {}, "No stable release is available."};
    std::string current = currentVersion;
    if (!timestamp(current)) return unavailable("The application version is not a valid build timestamp.");
    if (!timestamp(release.tag)) return unavailable("The release tag is not a valid build timestamp.");
    if (release.tag > current) return {State::Available, release.tag, "A newer release is available."};
    return {State::Current, release.tag, "This build is up to date."};
}

struct Checker::Impl {
    mutable std::mutex viewMutex;
    std::mutex lifecycleMutex;
    View view;
    std::atomic<bool> canceled{false};
    std::thread worker;
    bool started = false;
    bool stopped = false;
};

Checker::Checker() : impl_(std::make_unique<Impl>()) {}
Checker::~Checker() { stop(); }

void Checker::start(std::string currentVersion, Fetch fetch) {
    std::lock_guard<std::mutex> lifecycle(impl_->lifecycleMutex);
    if (impl_->started || impl_->stopped) return;
    impl_->started = true;
    {
        std::lock_guard<std::mutex> lock(impl_->viewMutex);
        impl_->view = {State::Checking, {}, {}};
    }
    try {
        impl_->worker = std::thread([state = impl_.get(), current = std::move(currentVersion), fetch = std::move(fetch)] {
            View result;
            try {
                const Response response = fetch(state->canceled);
                if (state->canceled.load()) return;
                result = evaluate(current, response);
            } catch (const std::exception&) {
                result = unavailable("The update check could not be completed.");
            } catch (...) {
                result = unavailable("The update check could not be completed.");
            }
            std::lock_guard<std::mutex> lock(state->viewMutex);
            if (!state->canceled.load()) state->view = std::move(result);
        });
    } catch (const std::exception&) {
        std::lock_guard<std::mutex> lock(impl_->viewMutex);
        impl_->view = unavailable("The update check could not be started.");
    }
}

void Checker::stop() {
    std::lock_guard<std::mutex> lifecycle(impl_->lifecycleMutex);
    impl_->stopped = true;
    impl_->canceled.store(true);
    if (impl_->worker.joinable()) impl_->worker.join();
    std::lock_guard<std::mutex> lock(impl_->viewMutex);
    if (impl_->view.state == State::Checking) impl_->view = {};
}

View Checker::snapshot() const {
    std::lock_guard<std::mutex> lock(impl_->viewMutex);
    return impl_->view;
}
} // namespace niceecatm::updates
