// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#include "platform/paths.hpp"
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace niceecatm {
std::filesystem::path executableDirectory() {
#ifdef _WIN32
    std::vector<wchar_t> buffer(512);
    while(buffer.size()<=32768) {
        const DWORD length=GetModuleFileNameW(nullptr,buffer.data(),static_cast<DWORD>(buffer.size()));
        if(!length)break;
        if(length<buffer.size())return std::filesystem::path(std::wstring(buffer.data(),length)).parent_path();
        buffer.resize(buffer.size()*2);
    }
#elif defined(__linux__)
    std::vector<char> buffer(4096);
    while(buffer.size()<=1024*1024) {
        const auto length=::readlink("/proc/self/exe",buffer.data(),buffer.size());
        if(length<0)break;
        if(static_cast<size_t>(length)<buffer.size())return std::filesystem::path(std::string(buffer.data(),static_cast<size_t>(length))).parent_path();
        buffer.resize(buffer.size()*2);
    }
#endif
    std::error_code error;
    return std::filesystem::current_path(error);
}
}
