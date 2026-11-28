// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#pragma once
#include "ui/workbench.hpp"
#include <imgui.h>
#include <filesystem>
#include <set>
#include <string>
#include <vector>
namespace niceecatm::workbench {
inline const ImVec4 ink{.14f,.22f,.29f,1}, muted{.47f,.56f,.63f,1};
inline const ImVec4 teal{.02f,.49f,.54f,1}, green{.08f,.53f,.36f,1};
inline const ImVec4 amber{.72f,.46f,.12f,1}, red{.78f,.25f,.29f,1};
inline const ImVec4 background{.945f,.961f,.977f,1}, line{.86f,.90f,.93f,1};
struct Ui {
    bool initialized=false, chinese=false, awaiting=false, allStates=true, languageOverride=false;
    bool sdoHex=false, autoScroll=true;
    Connection config, previousModeConfig;
    ConnectionMode pendingMode=ConnectionMode::Hardware;
    updates::Checker* updater=nullptr;
    int selected=0, requestedSlave=0, regLength=2;
    int sdoType=13, sdoCapacity=256;
    uint64_t seenCompletion=0, modeRequest=0;
    std::set<uint64_t> requests;
    std::filesystem::path preferences;
    std::string localError, requestedTab, activeTab="pdo";
    char regAddress[24]="0x0130", regWrite[512]="00 00";
    char sdoIndex[24]="0x1008", sdoSub[24]="0x00", sdoValue[1024]="";
};
extern Ui ui;
extern const char* types[];
struct Fonts { ImFont* regular=nullptr;ImFont* small=nullptr;ImFont* title=nullptr;ImFont* value=nullptr;ImFont* mono=nullptr; };
extern Fonts fonts;
enum class Icon { Network, Box, Scan, Registers, Pulse, Table, Mail, Book, Clock, Logs, Play, Stop, Range };
void icon(Icon kind,float size=14,ImU32 color=0);
bool iconButton(const char* id,Icon kind,const char* tooltip,ImVec2 size={28,28});
void pill(const char* text,ImVec4 color=teal);
void beginCard(const char* id,ImVec2 size={0,0},ImVec2 padding={14,12});
void endCard();
void cardHeader(const char* title,Icon kind=Icon::Table);
const char* tr(const char*,const char*);
std::string label(const char*,const char*,const char*);
std::string trMessage(const std::string&);
void loadPreferences();void savePreferences();void theme();
const Slave* selected(const Snapshot&);
bool blocked(const Snapshot&);
void send(Controller&,const Snapshot&,Command);
void command(Controller&,const Snapshot&,CommandKind);
void note(const char*,const char*);
void keyValue(const char*,const char*,const std::string&);
std::string hexNumber(uint64_t,int width=8);
ImVec4 stateColor(uint16_t);
std::string stateLabel(uint16_t);
void statePill(uint16_t,int alCode=-1);void stateBadge(uint16_t,int alCode=-1);
bool parseUnsigned(const char*,uint64_t,uint64_t&);
void adapterCombo(const Snapshot&,const char*,std::string&);
void topBar(const Snapshot&);void sidebar(Controller&,const Snapshot&);
void stateToolbar(Controller&,const Snapshot&);
void devicePage(Controller&,const Snapshot&);void registersPage(Controller&,const Snapshot&);
void observeServiceResults(const Snapshot&);
unsigned widthFor(const std::string&);
void bufferPage(Controller&,const Snapshot&);
void sdoPage(Controller&,const Snapshot&);void diagnosticsPage(const Snapshot&);
void foePage(Controller&,const Snapshot&);void foeTick(Controller&,const Snapshot&);
bool foeRunning();
void eepromPage(Controller&,const Snapshot&);void eepromTick(Controller&,const Snapshot&);
void scriptPage(Controller&,const Snapshot&);
void scriptTick();
bool scriptRunning();
void stopScript();
void eventLog(Controller&,const Snapshot&,float height);void consume(const Snapshot&);
} // namespace niceecatm::workbench
