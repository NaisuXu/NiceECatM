// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#include "ui/internal.hpp"
#include "ui/messages.hpp"
#include "platform/paths.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace niceecatm::workbench {
Ui ui;
Fonts fonts;
const char* types[]={"BOOL","UINT3","UINT8","INT8","UINT12","UINT16","INT16","UINT32","INT32","UINT64","INT64","REAL32","REAL64","STRING"};
const char* tr(const char* en,const char* zh) { return ui.chinese?zh:en; }
std::string label(const char* en,const char* zh,const char* id) { return std::string(tr(en,zh))+"###"+id; }
std::string trMessage(const std::string& message) {
    return ui.chinese?detail::translateToolMessage(message):message;
}
void loadPreferences() {
    try {
        // Keep the small preference file beside the executable so a portable
        // folder carries its language choice with the binary.
        const auto directory=executableDirectory();
        if(!directory.empty())ui.preferences=directory/"preferences.ini";
        std::ifstream file(ui.preferences);
        std::string line;
        while(std::getline(file,line)) if(line=="language=zh-CN") ui.chinese=true;
    } catch(...) { /* A storage failure does not affect the master or language switch. */ }
}
void savePreferences() {
    try {
        if(ui.preferences.empty()) return;
        std::filesystem::create_directories(ui.preferences.parent_path());
        std::ofstream file(ui.preferences,std::ios::trunc);
        file<<"language="<<(ui.chinese?"zh-CN":"en")<<'\n';
    } catch(...) {}
}
const Slave* selected(const Snapshot& s) {
    for(const auto& slave:s.slaves) if(slave.position==ui.selected) return &slave;
    return nullptr;
}
bool blocked(const Snapshot& s) { return s.busy||ui.awaiting||ui.modeRequest!=0||foeRunning(); }
void send(Controller& controller,const Snapshot& s,Command cmd) {
    cmd.generation=s.generation;
    cmd.connection=ui.config;
    const uint64_t id=controller.submit(std::move(cmd));
    if(!id) {ui.localError="Command queue is full.";return;}
    ui.requests.insert(id);
    while(ui.requests.size()>128)ui.requests.erase(ui.requests.begin());
    ui.awaiting=true;ui.localError.clear();
}
void command(Controller& controller,const Snapshot& s,CommandKind kind) {
    Command cmd;cmd.kind=kind;send(controller,s,std::move(cmd));
}
void note(const char* en,const char* zh) { ImGui::PushStyleColor(ImGuiCol_Text,muted);ImGui::TextWrapped("%s",tr(en,zh));ImGui::PopStyleColor(); }
void keyValue(const char* en,const char* zh,const std::string& value) {
    ImGui::TableNextRow();ImGui::TableSetColumnIndex(0);ImGui::TextDisabled("%s",tr(en,zh));
    ImGui::TableSetColumnIndex(1);ImGui::TextWrapped("%s",value.c_str());
}
std::string hexNumber(uint64_t value,int width) {std::ostringstream o;o<<"0x"<<std::hex<<std::uppercase<<std::setw(width)<<std::setfill('0')<<value;return o.str();}
ImVec4 stateColor(uint16_t state) {if(state&0x10)return red;switch(state&0xf){case 8:return green;case 4:return teal;case 2:case 3:return amber;default:return muted;}}
std::string stateLabel(uint16_t state) {
    return std::string(stateName(state))+((state&0x10)?tr(" + ERR"," + 错误"):"");
}
namespace {
void stateTooltip(uint16_t state,int alCode) {
    if(!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))return;
    ImGui::BeginTooltip();ImGui::PushTextWrapPos(ImGui::GetFontSize()*28);
    ImGui::Text("%s: %s",tr("Raw AL status","原始 AL 状态"),hexNumber(state,4).c_str());
    if(alCode>=0)ImGui::Text("%s: %s",tr("AL status code","AL 状态码"),hexNumber(static_cast<uint16_t>(alCode),4).c_str());
    if(state&0x10) {
        ImGui::TextUnformatted(tr("The slave reports the AL error flag (0x0010) alongside its current phase.","从站在当前阶段之外还报告了 AL 错误标志（0x0010）。"));
        if(alCode==0)ImGui::TextUnformatted(tr("AL status code is zero; no specific error cause was reported.","AL 状态码为零，从站未报告具体错误原因。"));
    }
    ImGui::PopTextWrapPos();ImGui::EndTooltip();
}
}
void statePill(uint16_t state,int alCode) {
    pill(stateLabel(state).c_str(),stateColor(state));stateTooltip(state,alCode);
}
void stateBadge(uint16_t state,int alCode) {
    ImGui::TextColored(stateColor(state),"%s",stateLabel(state).c_str());stateTooltip(state,alCode);
}
bool parseUnsigned(const char* text,uint64_t max,uint64_t& out) {
    if(!text||!*text||*text=='-'||*text=='+')return false;
    errno=0;char* end=nullptr;auto value=std::strtoull(text,&end,0);
    if(errno==ERANGE||end==text||*end||value>max)return false;
    out=value;return true;
}
void adapterCombo(const Snapshot& s,const char* id,std::string& choice) {
    const Adapter* found=nullptr;
    for(const auto& a:s.adapters)if(a.name==choice)found=&a;
    const auto preview=found?trMessage(found->description):std::string(tr("Select interface","选择网卡"));
    ImGui::SetNextItemWidth(-1);
    if(ImGui::BeginCombo(id,preview.c_str())) {
        for(const auto& a:s.adapters) {
            ImGui::PushID(a.name.c_str());
            if(ImGui::Selectable(trMessage(a.description).c_str(),a.name==choice))choice=a.name;
            if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",a.name.c_str());
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
}
unsigned widthFor(const std::string& type) {
    if(type=="BOOL")return 1;
    if(type=="STRING")return 0;
    const auto pos=type.find_first_of("0123456789");return pos==std::string::npos?0:static_cast<unsigned>(std::strtoul(type.c_str()+pos,nullptr,10));
}
void consume(const Snapshot& s) {
    static uint64_t selectionGeneration=~uint64_t{};
    if(s.busy||s.completed!=ui.seenCompletion)ui.awaiting=false;
    if(s.completed!=ui.seenCompletion){
        ui.seenCompletion=s.completed;
        ui.requests.erase(s.completed);
    }
    if(!s.slaves.empty()) {
        // A fresh scan starts from the first discovered slave. Subsequent
        // frames preserve an explicit user selection within that generation.
        if(selectionGeneration!=s.generation) {
            selectionGeneration=s.generation;
            ui.selected=s.slaves.front().position;
            if(ui.requestedSlave>0) {
                for(const auto& slave:s.slaves)if(slave.position==ui.requestedSlave){ui.selected=slave.position;break;}
                ui.requestedSlave=0;
            }
        } else if(!selected(s))ui.selected=s.slaves.front().position;
    }
    const auto validAdapter=[&](const std::string& name){return std::any_of(s.adapters.begin(),s.adapters.end(),[&](const Adapter& a){return a.name==name;});};
    if(s.connection.mode==ui.config.mode&&!s.adapters.empty()&&!validAdapter(ui.config.primary)) {
        const Adapter* first=&s.adapters.front();
        if(ui.config.mode!=ConnectionMode::Simulation)for(const auto& a:s.adapters)if(a.description.find("Intel")!=std::string::npos){first=&a;break;}
        ui.config.primary=first->name;
    }
    if(s.connection.mode==ui.config.mode&&!validAdapter(ui.config.secondary)) {
        ui.config.secondary.clear();for(const auto& a:s.adapters)if(a.name!=ui.config.primary){ui.config.secondary=a.name;break;}
    }
}
}
namespace niceecatm {
using namespace workbench;
void configureWorkbenchFonts(ImFont* regular,ImFont* small,ImFont* title,ImFont* value,ImFont* mono) {
    fonts={regular,small,title,value,mono};
}
namespace {
void pageTabs() {
    struct Tab { const char* id;const char* en;const char* zh;Icon icon; };
    static const Tab tabs[]={
      {"device","Device / DC","设备 / DC",Icon::Box},
      {"registers","ESC registers","ESC 寄存器",Icon::Registers},
      {"pdo","Buffer · PDO","Buffer · PDO",Icon::Table},
      {"sdo","CoE · SDO","CoE · SDO",Icon::Book},
      {"foe","FoE","FoE",Icon::Mail},
      {"scripts","Scripts","脚本",Icon::Logs},
      {"eeprom","EEPROM","EEPROM",Icon::Registers},
      {"diagnostics","Diagnostics","诊断",Icon::Pulse}};
    const bool programming=ui.config.mode==ConnectionMode::Programming;
    if(programming)ui.activeTab="eeprom";
    else if(ui.activeTab=="eeprom")ui.activeTab="pdo";
    const bool revealTab=!ui.requestedTab.empty();
    if(revealTab) {
        if(!ui.modeRequest)for(const auto& tab:tabs)
            if(ui.requestedTab==tab.id&&((std::strcmp(tab.id,"eeprom")==0)==programming))ui.activeTab=tab.id;
        ui.requestedTab.clear();
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{0,0});
    ImGui::BeginChild("page-tabs",{0,46},ImGuiChildFlags_None,ImGuiWindowFlags_HorizontalScrollbar|ImGuiWindowFlags_NoBackground);
    const auto start=ImGui::GetCursorScreenPos();const float width=ImGui::GetContentRegionAvail().x;
    const float gap=6.f;
    ImGui::PushFont(fonts.small,12);float tabX=start.x;
    for(const auto& tab:tabs) {
        ImGui::SetCursorScreenPos({tabX,start.y});ImGui::PushID(tab.id);
        const auto p=ImGui::GetCursorScreenPos();const bool active=ui.activeTab==tab.id;
        const char* text=tr(tab.en,tab.zh);const float w=ImGui::CalcTextSize(text).x+38;
        const bool available=!ui.modeRequest&&((std::strcmp(tab.id,"eeprom")==0)==programming);
        ImGui::BeginDisabled(!available);
        if(ImGui::InvisibleButton("tab",{w,34}))ui.activeTab=tab.id;
        const bool hover=ImGui::IsItemHovered();auto* draw=ImGui::GetWindowDrawList();
        if(hover)draw->AddRectFilled(p,{p.x+w,p.y+33},ImGui::GetColorU32({.90f,.94f,.96f,1}),4);
        auto after=ImGui::GetCursorScreenPos();ImGui::SetCursorScreenPos({p.x+9,p.y+9});
        const auto color=ImGui::GetColorU32(active?teal:muted);
        icon(tab.icon,14,color);ImGui::SetCursorScreenPos(after);
        draw->AddText({p.x+30,p.y+8},color,text);
        if(active)draw->AddRectFilled({p.x,p.y+33},{p.x+w,p.y+35},ImGui::GetColorU32(teal),1);
        if(active&&revealTab)ImGui::SetScrollHereX();
        ImGui::EndDisabled();
        ImGui::PopID();tabX+=w+gap;
    }
    ImGui::PopFont();
    ImGui::GetWindowDrawList()->AddLine({start.x,start.y+35},{start.x+width,start.y+35},ImGui::GetColorU32(line));
    ImGui::SetCursorScreenPos({tabX,start.y+35});ImGui::Dummy({0,0});
    ImGui::EndChild();ImGui::PopStyleVar();
}
} // namespace
void drawWorkbench(Controller& controller) {
    if(!ui.initialized){if(!ui.languageOverride)loadPreferences();theme();ui.initialized=true;}
    Snapshot snapshot=controller.snapshot();
    if(ui.modeRequest) {
        Result result;
        // Another command can complete between two GUI frames. Resolve this
        // request from its own result instead of the snapshot's last ID.
        if(controller.result(ui.modeRequest,result)) {
            ui.requests.erase(ui.modeRequest);ui.modeRequest=0;
            if(result.ok) {
                ui.config.mode=ui.pendingMode;
                if(ui.config.mode==ConnectionMode::Programming)ui.requestedTab="eeprom";
            } else {ui.config=ui.previousModeConfig;ui.localError=result.message;}
            if(ui.requests.empty())ui.awaiting=false;
        }
    }
    if(snapshot.connected&&!ui.modeRequest)ui.config=snapshot.connection;
    consume(snapshot);scriptTick();eepromTick(controller,snapshot);foeTick(controller,snapshot);
    const auto* viewport=ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{0,0});
    ImGui::Begin("NiceECatM###niceecatm-root",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoBringToFrontOnFocus|ImGuiWindowFlags_NoScrollbar);
    const float height=ImGui::GetWindowHeight();
    topBar(snapshot);
    ImGui::SetCursorPos({0,52});
    ImGui::PushStyleColor(ImGuiCol_ChildBg,background);
    ImGui::BeginChild("application",{0,height-52},ImGuiChildFlags_None,ImGuiWindowFlags_NoScrollbar);
    sidebar(controller,snapshot);ImGui::SameLine(0,0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{24,12});
    ImGui::BeginChild("work-area",{0,0},ImGuiChildFlags_AlwaysUseWindowPadding,ImGuiWindowFlags_NoScrollbar);
    stateToolbar(controller,snapshot);ImGui::Dummy({0,2});
    observeServiceResults(snapshot);pageTabs();
    // Reserve space for controller messages and multi-line script errors.
    const float logHeight=height<840?150.f:180.f;
    const float contentHeight=std::max(180.f,ImGui::GetContentRegionAvail().y-logHeight-6.f);
    ImGui::BeginChild("workbench-pages",{0,contentHeight},ImGuiChildFlags_None,ImGuiWindowFlags_NoScrollbar);
    if(ui.modeRequest)note("Changing operating mode…","正在切换运行模式…");
    else if(ui.config.mode==ConnectionMode::Programming)eepromPage(controller,snapshot);
    else if(ui.activeTab=="device")devicePage(controller,snapshot);
    else if(ui.activeTab=="registers")registersPage(controller,snapshot);
    else if(ui.activeTab=="sdo")sdoPage(controller,snapshot);
    else if(ui.activeTab=="foe")foePage(controller,snapshot);
    else if(ui.activeTab=="diagnostics")diagnosticsPage(snapshot);
    else if(ui.activeTab=="scripts")scriptPage(controller,snapshot);
    else bufferPage(controller,snapshot);
    ImGui::EndChild();
    eventLog(controller,snapshot,logHeight);
    ImGui::EndChild();ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::End();ImGui::PopStyleVar();
}
void configureWorkbench(const char* language,const char* tab,int slave) {
    if(language&&(std::strcmp(language,"en")==0||std::strcmp(language,"zh-CN")==0)){
        ui.chinese=std::strcmp(language,"zh-CN")==0;ui.languageOverride=true;
    }
    if(tab)ui.requestedTab=tab;
    if(slave>0){ui.selected=slave;ui.requestedSlave=slave;}
}
void configureWorkbenchMode(ConnectionMode mode) { ui.config.mode=mode; }
void configureWorkbenchUpdates(updates::Checker* checker) { ui.updater=checker; }
}
