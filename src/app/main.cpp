// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#include "app/cli.hpp"
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif
static int runApplication(int argc, char** argv) {
    bool cli=false;
    for(int i=1;i<argc;++i) {
        if(!std::strcmp(argv[i],"--help") || !std::strcmp(argv[i],"--adapters") || !std::strcmp(argv[i],"--scan") || !std::strcmp(argv[i],"--read-register") || !std::strcmp(argv[i],"--sdo-read") || !std::strcmp(argv[i],"--state") || !std::strcmp(argv[i],"--programming-scan") || !std::strcmp(argv[i],"--eeprom-read") || !std::strcmp(argv[i],"--sii-export")) cli=true;
    }
    try {
        if(cli) return runCli(argc,argv);
#ifdef NICEECATM_GUI
#ifdef _WIN32
        // Detach only a console allocated for this process; never hide the user's terminal.
        DWORD owners[2];
        if(GetConsoleProcessList(owners,2)==1) FreeConsole();
#endif
        return runDesktop(argc,argv);
#else
        std::cerr<<"This build has no desktop UI. Use --help.\n";return 2;
#endif
    } catch(const std::exception& e) { std::cerr<<"NiceECatM: "<<e.what()<<'\n';return 1; }
}
#ifdef _WIN32
int wmain(int argc,wchar_t** wideArgv) {
    std::vector<std::string> values;
    for(int i=0;i<argc;++i) {
        const int length=WideCharToMultiByte(CP_UTF8,0,wideArgv[i],-1,nullptr,0,nullptr,nullptr);
        std::string value(static_cast<size_t>(length), '\0');
        WideCharToMultiByte(CP_UTF8,0,wideArgv[i],-1,value.data(),length,nullptr,nullptr);
        if(!value.empty())value.pop_back();values.push_back(std::move(value));
    }
    std::vector<char*> argv;for(auto& value:values)argv.push_back(value.data());argv.push_back(nullptr);
    return runApplication(argc,argv.data());
}
#else
int main(int argc,char** argv) {return runApplication(argc,argv);}
#endif
