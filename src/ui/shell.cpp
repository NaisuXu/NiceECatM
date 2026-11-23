// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#include "ui/internal.hpp"
#include "build_version.hpp"
#include "platform/update_check.hpp"
#include <SDL3/SDL_misc.h>
#include <algorithm>
#include <cstdio>
#include <string>

namespace niceecatm::workbench {
namespace {
const ImVec4 nav{.075f,.13f,.19f,1}, side{.09f,.17f,.24f,1};
const ImVec4 sideText{.66f,.75f,.82f,1}, sideMuted{.40f,.53f,.63f,1};
const ImVec4 sideLine{.17f,.27f,.35f,1}, sideCard{.11f,.19f,.26f,1};
ImFont* font(ImFont* preferred) { return preferred?preferred:ImGui::GetFont(); }
void textAt(float x,float y,const char* text,ImVec4 color=ink,ImFont* face=nullptr,float size=12.f) {
    const auto origin=ImGui::GetWindowPos();
    ImGui::GetWindowDrawList()->AddText(font(face),size,{origin.x+x,origin.y+y},ImGui::ColorConvertFloat4ToU32(color),text);
}
void rightText(float right,float y,const char* text,ImVec4 color=ink,ImFont* face=nullptr,float size=12.f) {
    textAt(right-font(face)->CalcTextSizeA(size,10000.f,0,text).x,y,text,color,face,size);
}
void smallLabel(float y,const char* en,const char* zh) { textAt(12,y,tr(en,zh),sideMuted,fonts.small,10); }
void tooltip(const char* en,const char* zh) {
    if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))ImGui::SetTooltip("%s",tr(en,zh));
}
void sourceChanged(Controller& c,const Snapshot& s,ConnectionMode mode) {
    if(s.connected||blocked(s)||scriptRunning()||mode==ui.config.mode)return;
    const auto previous=ui.config;
    if((mode==ConnectionMode::Simulation)!=(ui.config.mode==ConnectionMode::Simulation))ui.config.primary.clear();
    ui.config.mode=mode;ui.config.secondary.clear();
    // EEPROM is a programming-only workbench. Leave that page immediately
    // when the user switches back to Simulation or Hardware, including while
    // the asynchronous mode request is still being completed.
    if(mode!=ConnectionMode::Programming) {
        if(ui.activeTab=="eeprom")ui.activeTab="pdo";
        if(ui.requestedTab=="eeprom")ui.requestedTab.clear();
    }
    if(mode==ConnectionMode::Programming){ui.config.redundant=false;ui.config.overlap=false;ui.activeTab="eeprom";ui.requestedTab="eeprom";}
    Command request;request.kind=CommandKind::SetMode;request.connection=ui.config;request.generation=s.generation;
    const uint64_t id=c.submit(std::move(request));
    if(!id){ui.config=previous;ui.localError="Command queue is full.";return;}
    ui.previousModeConfig=previous;ui.pendingMode=mode;ui.modeRequest=id;ui.requests.insert(id);ui.awaiting=true;ui.localError.clear();
}
bool tintButton(const char* text,ImVec2 size,bool active,ImVec4 activeColor=teal) {
    ImGui::PushStyleColor(ImGuiCol_Button,active?ImVec4{activeColor.x,activeColor.y,activeColor.z,.12f}:ImGui::GetStyleColorVec4(ImGuiCol_Button));
    ImGui::PushStyleColor(ImGuiCol_Text,active?activeColor:ImGui::GetStyleColorVec4(ImGuiCol_Text));
    const bool pressed=ImGui::Button(text,size);ImGui::PopStyleColor(2);return pressed;
}
void versionTooltip(const updates::View& update) {
    if(!ImGui::IsItemHovered())return;
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImGui::BeginTooltip();
    ImGui::Text("%s: %s",tr("Current version","当前版本"),NICEECATM_VERSION);
    if(!update.latestVersion.empty())ImGui::Text("%s: %s",tr("Latest release","最新正式版本"),update.latestVersion.c_str());
    switch(update.state) {
    case updates::State::Checking: ImGui::TextUnformatted(tr("Checking for updates...","正在检查更新…"));break;
    case updates::State::Available: ImGui::TextUnformatted(tr("A new version is available.","有新版本可用。"));break;
    case updates::State::Current: ImGui::TextUnformatted(tr("No newer release is available.","没有更新的正式版本。"));break;
    case updates::State::NoRelease: ImGui::TextUnformatted(tr("No published release was found.","尚未找到正式发布的版本。"));break;
    case updates::State::Unavailable:
        ImGui::TextUnformatted(tr("Update check unavailable.","暂时无法检查更新。"));
        if(!update.detail.empty())ImGui::TextUnformatted(update.detail.c_str());
        break;
    default:break;
    }
    ImGui::TextUnformatted(tr("Click to open the Releases page.","点击打开 Releases 页面。"));
    ImGui::EndTooltip();
}
uint16_t scopeState(const Snapshot& s) {
    if(!s.connected||s.slaves.empty())return 0;
    if(!ui.allStates){const auto* slave=selected(s);return slave?uint16_t(slave->state&0x0f):0;}
    // AL error is carried in bit 4.  The toolbar represents the current
    // phase, so keep PRE-OP/SAFE-OP/OP selected even when a slave reports an
    // AL error alongside that phase.
    const uint16_t state=uint16_t(s.slaves.front().state&0x0f);
    for(const auto& slave:s.slaves)if((slave.state&0x0f)!=state)return 0;
    return state;
}
std::string deviceDescription(const Slave& slave,const Snapshot& s) {
    if(s.connection.mode==ConnectionMode::Programming)return slave.eepromIdentityKnown?tr("EEPROM programming target","EEPROM 编程目标"):tr("Blank / invalid SII","空白 / 无效 SII");
    if(s.simulation) {
        if(slave.name.find("EK1100")!=std::string::npos)return tr("EtherCAT bus coupler","EtherCAT 总线耦合器");
        if(slave.name.find("EL1008")!=std::string::npos)return tr("8-channel digital input","8 通道数字输入");
        if(slave.name.find("EL2008")!=std::string::npos)return tr("8-channel digital output","8 通道数字输出");
        if(slave.name.find("EL3062")!=std::string::npos)return tr("2-channel analog input","2 通道模拟输入");
    }
    return std::string("IN ")+std::to_string(slave.inputBits)+" b  /  OUT "+std::to_string(slave.outputBits)+" b";
}
}

void theme() {
    ImGui::StyleColorsLight();ImGuiStyle& s=ImGui::GetStyle();
    s.WindowPadding={14,12};s.FramePadding={7,5};s.ItemSpacing={8,6};s.ItemInnerSpacing={6,4};
    s.WindowRounding=0;s.ChildRounding=6;s.FrameRounding=4;s.PopupRounding=6;
    s.ScrollbarRounding=5;s.TabRounding=0;s.GrabRounding=3;s.ScrollbarSize=10;
    s.WindowBorderSize=0;s.ChildBorderSize=1;s.FrameBorderSize=1;
    s.Colors[ImGuiCol_Text]=ink;s.Colors[ImGuiCol_TextDisabled]=muted;
    s.Colors[ImGuiCol_WindowBg]=background;s.Colors[ImGuiCol_ChildBg]={1,1,1,1};
    s.Colors[ImGuiCol_PopupBg]={1,1,1,1};s.Colors[ImGuiCol_Border]=line;
    s.Colors[ImGuiCol_FrameBg]={1,1,1,1};s.Colors[ImGuiCol_FrameBgHovered]={.95f,.98f,.98f,1};
    s.Colors[ImGuiCol_FrameBgActive]={.91f,.96f,.97f,1};
    s.Colors[ImGuiCol_Button]={1,1,1,1};s.Colors[ImGuiCol_ButtonHovered]={.91f,.96f,.97f,1};
    s.Colors[ImGuiCol_ButtonActive]={.82f,.91f,.93f,1};
    s.Colors[ImGuiCol_Header]={.88f,.95f,.96f,1};s.Colors[ImGuiCol_HeaderHovered]={.91f,.96f,.97f,1};
    s.Colors[ImGuiCol_HeaderActive]={.82f,.92f,.94f,1};
    s.Colors[ImGuiCol_Tab]={0,0,0,0};s.Colors[ImGuiCol_TabHovered]={.88f,.95f,.96f,1};
    s.Colors[ImGuiCol_TabSelected]={.88f,.95f,.96f,1};
    s.Colors[ImGuiCol_TableHeaderBg]={.967f,.977f,.985f,1};
    s.Colors[ImGuiCol_TableBorderLight]={.92f,.94f,.96f,1};s.Colors[ImGuiCol_TableBorderStrong]=line;
    s.Colors[ImGuiCol_TableRowBgAlt]={.98f,.987f,.992f,.7f};
    s.Colors[ImGuiCol_CheckMark]=teal;s.Colors[ImGuiCol_SliderGrab]=teal;
    s.Colors[ImGuiCol_Separator]=line;s.Colors[ImGuiCol_ResizeGrip]={0,0,0,0};
    s.Colors[ImGuiCol_ScrollbarBg]={.97f,.98f,.99f,1};s.Colors[ImGuiCol_ScrollbarGrab]={.76f,.82f,.86f,1};
    s.Colors[ImGuiCol_ScrollbarGrabHovered]={.64f,.73f,.79f,1};
}

void topBar(const Snapshot&) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{0,0});ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding,0);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,0);
    ImGui::PushStyleColor(ImGuiCol_ChildBg,nav);ImGui::PushStyleColor(ImGuiCol_Text,{.64f,.72f,.80f,1});
    ImGui::PushStyleColor(ImGuiCol_Button,{0,0,0,0});ImGui::PushStyleColor(ImGuiCol_ButtonHovered,{.13f,.23f,.31f,1});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,{.16f,.29f,.36f,1});
    ImGui::BeginChild("top",{0,52},ImGuiChildFlags_None,ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
    const float width=ImGui::GetWindowWidth();const auto origin=ImGui::GetWindowPos();auto* draw=ImGui::GetWindowDrawList();
    draw->AddRectFilled({origin.x+22,origin.y+11},{origin.x+52,origin.y+41},IM_COL32(12,143,153,255),6);
    ImGui::SetCursorPos({29,18});icon(Icon::Network,16,IM_COL32(255,255,255,255));
    textAt(64,16,"Nice",{.94f,.97f,1,1},fonts.title,20);
    textAt(64+font(fonts.title)->CalcTextSizeA(20,10000,0,"Nice").x,16,"ECatM",{.33f,.79f,.81f,1},fonts.title,20);
    ImGui::PushFont(font(fonts.small),12);
    const char* version=NICEECATM_VERSION;
    const float languageX=width-99;
    const float versionSize=13.f;
    const float versionWidth=font(fonts.small)->CalcTextSizeA(versionSize,10000,0,version).x;
    const float versionX=languageX-16-versionWidth;
    const auto update=ui.updater?ui.updater->snapshot():updates::View{};
    ImGui::SetCursorPos({versionX-4,12});
    if(ImGui::InvisibleButton("version-releases",{versionWidth+8,28}))SDL_OpenURL(updates::ReleasesUrl);
    const bool versionHovered=ImGui::IsItemHovered();
    textAt(versionX,18,version,versionHovered?ImVec4{.72f,.91f,.93f,1}:ImVec4{.62f,.72f,.80f,1},fonts.small,versionSize);
    versionTooltip(update);
    if(update.state==updates::State::Available) {
        const char* text=tr("New version","有新版本");
        const float badgeWidth=ImGui::CalcTextSize(text).x+18;
        ImGui::SetCursorPos({versionX-badgeWidth-16,14});
        if(tintButton(text,{badgeWidth,24},true,{.33f,.79f,.81f,1}))SDL_OpenURL(updates::ReleasesUrl);
        versionTooltip(update);
    }
    ImGui::SetCursorPos({languageX,14});
    if(tintButton("EN###lang-en",{35,24},!ui.chinese,{.41f,.81f,.83f,1})){ui.chinese=false;savePreferences();}
    ImGui::SetCursorPos({languageX+39,14});
    if(tintButton("中文###lang-zh",{42,24},ui.chinese,{.41f,.81f,.83f,1})){ui.chinese=true;savePreferences();}
    draw->AddLine({origin.x,origin.y+51},{origin.x+width,origin.y+51},IM_COL32(37,55,71,255));
    ImGui::PopFont();ImGui::EndChild();ImGui::PopStyleColor(5);ImGui::PopStyleVar(3);
}

void sidebar(Controller& c,const Snapshot& s) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{15,16});ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,{6,5});
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding,0);
    ImGui::PushStyleColor(ImGuiCol_ChildBg,side);ImGui::PushStyleColor(ImGuiCol_Text,sideText);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled,sideMuted);ImGui::PushStyleColor(ImGuiCol_Border,sideLine);
    // Combo popups inherit the sidebar's light text, so give them its dark
    // palette as well. The main workbench retains the global light palette.
    ImGui::PushStyleColor(ImGuiCol_PopupBg,sideCard);
    ImGui::PushStyleColor(ImGuiCol_FrameBg,{.075f,.145f,.205f,1});
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,{.12f,.24f,.31f,1});ImGui::PushStyleColor(ImGuiCol_FrameBgActive,{.14f,.27f,.34f,1});
    ImGui::PushStyleColor(ImGuiCol_Button,{.12f,.23f,.30f,1});ImGui::PushStyleColor(ImGuiCol_ButtonHovered,{.16f,.31f,.39f,1});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,{.19f,.36f,.43f,1});
    ImGui::PushStyleColor(ImGuiCol_Header,{.13f,.24f,.31f,1});ImGui::PushStyleColor(ImGuiCol_HeaderHovered,{.13f,.24f,.31f,1});
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,{.15f,.29f,.36f,1});ImGui::PushStyleColor(ImGuiCol_Separator,sideLine);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg,side);ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab,sideLine);
    ImGui::BeginChild("sidebar",{246,0},ImGuiChildFlags_AlwaysUseWindowPadding,ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushFont(font(fonts.small),11);
    ImGui::TextDisabled("%s",tr("CONNECTION","连接配置"));ImGui::Dummy({0,2});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{12,10});ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding,6);
    ImGui::PushStyleColor(ImGuiCol_ChildBg,sideCard);
    ImGui::BeginChild("adapter-card",{0,ui.config.redundant?366.f:318.f},ImGuiChildFlags_Borders|ImGuiChildFlags_AlwaysUseWindowPadding,ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::SetCursorPos({12,12});icon(Icon::Network,15,IM_COL32(129,201,208,255));
    textAt(36,11,tr("EtherCAT master","EtherCAT 主站"),{.86f,.92f,.96f,1},fonts.regular,13);
    textAt(12,34,tr("One master · raw Ethernet access","单主机 · 原始以太网通信"),sideMuted,fonts.small,10);
    const bool programming=ui.config.mode==ConnectionMode::Programming,simulation=ui.config.mode==ConnectionMode::Simulation;
    ImGui::PushFont(font(fonts.small),10);ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,{3,5});
    ImGui::BeginDisabled(blocked(s)||s.connected||scriptRunning());ImGui::SetCursorPos({12,54});
    if(tintButton(label("Simulation","模拟","simulation").c_str(),{58,24},simulation,{.40f,.80f,.82f,1}))sourceChanged(c,s,ConnectionMode::Simulation);
    if(s.connected)tooltip("Disconnect before changing mode.","请先断开连接再切换模式。");else if(scriptRunning())tooltip("Stop the script before changing mode.","请先停止脚本再切换模式。");
    ImGui::SameLine(75);
    if(tintButton(label("Hardware","硬件","hardware").c_str(),{54,24},ui.config.mode==ConnectionMode::Hardware,{.40f,.80f,.82f,1}))sourceChanged(c,s,ConnectionMode::Hardware);
    if(s.connected)tooltip("Disconnect before changing mode.","请先断开连接再切换模式。");else if(scriptRunning())tooltip("Stop the script before changing mode.","请先停止脚本再切换模式。");
    ImGui::SameLine(134);
    if(tintButton(label("Programming","编程","programming").c_str(),{70,24},programming,{.40f,.80f,.82f,1}))sourceChanged(c,s,ConnectionMode::Programming);
    if(s.connected)tooltip("Disconnect before changing mode.","请先断开连接再切换模式。");else if(scriptRunning())tooltip("Stop the script before changing mode.","请先停止脚本再切换模式。");
    ImGui::EndDisabled();ImGui::PopStyleVar();ImGui::PopFont();ImGui::BeginDisabled(blocked(s)||s.connected);
    smallLabel(87,"Connection mode","连接模式");ImGui::SetCursorPos({12,102});ImGui::SetNextItemWidth(-1);
    const char* modes[]={tr("Single port","单网口"),tr("Dual-port redundancy","同总线双口冗余")};int mode=ui.config.redundant?1:0;
    ImGui::BeginDisabled(programming);
    if(ImGui::Combo("##connection-mode",&mode,modes,2))ui.config.redundant=mode==1;
    ImGui::EndDisabled();
    smallLabel(135,"Primary interface · A","主网口 · A");
    rightText(ImGui::GetWindowWidth()-13,135,simulation?"SIM":s.connected?"OPEN":"CLOSED",simulation?sideMuted:ImVec4{.42f,.75f,.62f,1},fonts.mono,9);
    ImGui::SetCursorPos({12,150});adapterCombo(s,"##primary",ui.config.primary);float offset=0;
    if(ui.config.redundant) {
        smallLabel(183,"Redundant interface · B","冗余网口 · B");
        rightText(ImGui::GetWindowWidth()-13,183,simulation?"SIM":s.connected?"OPEN":"CLOSED",sideMuted,fonts.mono,9);
        ImGui::SetCursorPos({12,198});adapterCombo(s,"##secondary",ui.config.secondary);offset=48;
    }
    smallLabel(183+offset,"Process-data mapping","过程数据映射");ImGui::SetCursorPos({12,198+offset});ImGui::SetNextItemWidth(-1);
    const char* mapping[]={programming?tr("Not used for programming","编程模式不使用映射"):tr("Normal mapping","普通映射"),tr("Overlapped mapping","重叠映射")};int overlap=ui.config.overlap?1:0;
    ImGui::BeginDisabled(programming);if(ImGui::Combo("##mapping-mode",&overlap,mapping,2))ui.config.overlap=overlap==1;ImGui::EndDisabled();
    ImGui::EndDisabled();
    const bool duplicate=ui.config.redundant&&!ui.config.primary.empty()&&ui.config.primary==ui.config.secondary;
    textAt(12,235+offset,programming?tr("Single port · INIT · no PDO mapping","单网口 · INIT · 不进行 PDO 映射"):duplicate?tr("Choose two different interfaces","选择两个不同的网卡"):ui.config.redundant?tr("A -> same slave chain -> B","A -> 同一从站链 -> B"):tr("Single interface - no redundant path","单网口 - 无冗余路径"),duplicate?ImVec4{.94f,.55f,.55f,1}:sideMuted,fonts.small,9);
    ImGui::SetCursorPos({12,255+offset});
    ImGui::PushStyleColor(ImGuiCol_Text,{.57f,.84f,.86f,1});ImGui::PushStyleColor(ImGuiCol_Button,{.125f,.24f,.31f,1});
    ImGui::BeginDisabled(blocked(s)||s.running||(programming&&scriptRunning()));
    if(ImGui::Button(label(programming?"Scan for programming":"Scan devices",programming?"扫描可编程从站":"扫描设备","scan").c_str(),{-1,28}))command(c,s,CommandKind::Scan);
    ImGui::EndDisabled();ImGui::PopStyleColor(2);ImGui::SetCursorPos({12,290+offset});
    ImGui::BeginDisabled(blocked(s)||s.connected);
    if(iconButton("refresh-adapters",Icon::Scan,tr("Refresh interfaces","刷新网卡"),{25,20}))command(c,s,CommandKind::RefreshAdapters);
    ImGui::EndDisabled();ImGui::SameLine(43);ImGui::BeginDisabled(!s.connected);
    if(ImGui::Button(label("Disconnect","断开连接","disconnect").c_str(),{-1,20})) {
        if(scriptRunning())stopScript(c);
        command(c,s,CommandKind::Disconnect);
    }
    ImGui::EndDisabled();tooltip("Disconnect to edit connection settings.","断开连接后才能编辑连接设置");
    ImGui::EndChild();ImGui::PopStyleColor();ImGui::PopStyleVar(2);
    ImGui::Dummy({0,9});ImGui::TextDisabled("%s",tr("DEVICE TOPOLOGY","设备拓扑"));
    char count[20];std::snprintf(count,sizeof(count),"%02zu",s.slaves.size());ImGui::SameLine(199);ImGui::TextDisabled("%s",count);ImGui::Dummy({0,2});
    const float treeHeight=std::max(95.f,ImGui::GetContentRegionAvail().y-8.f);
    ImGui::BeginChild("device-tree",{0,treeHeight},ImGuiChildFlags_None);ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,{0,4});
    const auto treeOrigin=ImGui::GetWindowPos();auto* draw=ImGui::GetWindowDrawList();const ImVec2 allPos=ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos({allPos.x+8,allPos.y+8});icon(Icon::Box,14,IM_COL32(127,201,205,255));
    draw->AddText(font(fonts.regular),12,{allPos.x+32,allPos.y+8},ImGui::ColorConvertFloat4ToU32(sideMuted),tr("All devices","全部设备"));
    ImGui::SetCursorScreenPos({allPos.x,allPos.y+36});
    if(s.slaves.empty()) {ImGui::PushStyleColor(ImGuiCol_Text,sideMuted);ImGui::TextWrapped("%s",tr("Scan an interface to discover devices.","扫描网卡以发现设备"));ImGui::PopStyleColor();}
    float previousY=allPos.y+35;
    for(const auto& slave:s.slaves) {
        ImGui::PushID(slave.position);const ImVec2 row=ImGui::GetCursorScreenPos();ImGui::SetCursorScreenPos({row.x+26,row.y});
        ImGui::BeginDisabled((programming&&blocked(s))||foeRunning());
        if(ImGui::Selectable("##device",ui.selected==slave.position,ImGuiSelectableFlags_None,{std::max(1.f,ImGui::GetContentRegionAvail().x),51}))ui.selected=slave.position;
        ImGui::EndDisabled();
        if(ImGui::IsItemHovered())ImGui::SetTooltip("%d · %s\n%s\n%s: %s (%s)\n%s: %s",slave.position,slave.name.c_str(),deviceDescription(slave,s).c_str(),tr("Actual AL","实际 AL"),stateLabel(slave.state).c_str(),hexNumber(slave.state,4).c_str(),tr("AL status code","AL 状态码"),hexNumber(slave.alCode,4).c_str());
        draw->AddLine({row.x+13,previousY},{row.x+13,row.y+25},ImGui::ColorConvertFloat4ToU32(sideLine));
        draw->AddLine({row.x+13,row.y+25},{row.x+25,row.y+25},ImGui::ColorConvertFloat4ToU32(sideLine));previousY=row.y+25;
        char number[16];std::snprintf(number,sizeof(number),"%02d",slave.position);
        draw->AddText(font(fonts.mono),10,{row.x+33,row.y+12},ImGui::ColorConvertFloat4ToU32(sideMuted),number);
        // AL state is shown in the workbench state toolbar and diagnostics.
        // Keep the topology row focused on device identity and process image.
        draw->PushClipRect({row.x+54,row.y},{treeOrigin.x+ImGui::GetWindowWidth()-10,row.y+30},true);
        draw->AddText(font(fonts.regular),12,{row.x+54,row.y+10},IM_COL32(203,220,233,255),slave.name.c_str());draw->PopClipRect();
        draw->PushClipRect({row.x+54,row.y+29},{treeOrigin.x+ImGui::GetWindowWidth()-10,row.y+49},true);
        draw->AddText(font(fonts.small),9,{row.x+54,row.y+30},ImGui::ColorConvertFloat4ToU32(sideMuted),deviceDescription(slave,s).c_str());draw->PopClipRect();
        ImGui::SetCursorScreenPos({row.x,row.y+55});ImGui::PopID();
    }
    ImGui::Dummy({0,0});ImGui::PopStyleVar();ImGui::EndChild();
    ImGui::PopFont();ImGui::EndChild();ImGui::PopStyleColor(17);ImGui::PopStyleVar(3);
}

void stateToolbar(Controller& c,const Snapshot& s) {
    // Keep the connection controls and live exchange metrics in one compact
    // strip. The former two-row card consumed almost 100 px vertically.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{10,5});ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,{4,2});
    ImGui::PushStyleColor(ImGuiCol_ChildBg,{1,1,1,1});
    ImGui::BeginChild("bus-status",{0,58},ImGuiChildFlags_Borders|ImGuiChildFlags_AlwaysUseWindowPadding,ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
    const float width=ImGui::GetWindowWidth();const auto origin=ImGui::GetWindowPos();auto* draw=ImGui::GetWindowDrawList();
    const uint16_t actual=scopeState(s);
    const bool alError=s.connected&&std::any_of(s.slaves.begin(),s.slaves.end(),[](const Slave& slave){return (ui.allStates||slave.position==ui.selected)&&(slave.state&0x10);});
    std::string phase=!s.connected?tr("Not connected","未连接"):actual?stateName(actual):tr("MIXED","状态混合");
    if(alError)phase+=tr(" + ERR"," + 错误");
    draw->AddRectFilled({origin.x+10,origin.y+13},{origin.x+34,origin.y+37},IM_COL32(233,245,246,255),5);
    ImGui::SetCursorPos({16,19});icon(Icon::Pulse,13,ImGui::ColorConvertFloat4ToU32(teal));
    textAt(42,11,phase.c_str(),alError?red:s.connected?stateColor(actual):muted,fonts.regular,12);
    const char* exchange=blocked(s)?tr("Working...","处理中"):s.running?tr("Cyclic I/O running","周期 I/O 运行中"):tr("Cyclic I/O stopped","周期 I/O 已停止");
    textAt(42,29,exchange,blocked(s)?amber:s.running?green:muted,fonts.small,8.f);

    // Cycle, target scope and AL-state buttons stay interactive in the same row.
    const float phaseWidth=font(fonts.regular)->CalcTextSizeA(12,10000,0,phase.c_str()).x;
    const bool compact=width<940.f;
    float x=std::max(compact?126.f:140.f,42.f+phaseWidth+16.f);
    const float rowY=15.f;
    ImGui::PushFont(font(fonts.small),11);ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,{5,9});
    const bool cycleEditable=!s.connected||(!s.running&&std::all_of(s.slaves.begin(),s.slaves.end(),[](const Slave& slave){const auto state=slave.state&0xf;return state==1||state==2;}));
    const bool programming=ui.config.mode==ConnectionMode::Programming;
    ImGui::SetCursorPos({x,rowY});ImGui::SetNextItemWidth(compact?72.f:80.f);ImGui::BeginDisabled(programming||blocked(s)||!cycleEditable);
    const int cycles[]={500,1000,2000,5000,10000,20000,50000,100000};const char* names[]={"500 μs","1 ms","2 ms","5 ms","10 ms","20 ms","50 ms","100 ms"};
    int cycle=4;for(int i=0;i<8;++i)if(ui.config.cycleUs==cycles[i])cycle=i;
    if(ImGui::Combo("##cycle",&cycle,names,8)){ui.config.cycleUs=cycles[cycle];if(s.connected){Command cmd;cmd.kind=CommandKind::SetCycle;cmd.connection.cycleUs=ui.config.cycleUs;send(c,s,std::move(cmd));}}
    ImGui::EndDisabled();tooltip("Target cycle. Change while disconnected or all slaves are INIT/PRE-OP.","断开连接或所有从站处于 INIT/PRE-OP 时可修改。");
    x+=compact?80.f:88.f;ImGui::SetCursorPos({x,rowY});ImGui::SetNextItemWidth(compact?82.f:86.f);
    const char* scopes[]={tr("All slaves","全部从站"),tr("Selected","当前从站")};int scope=ui.allStates?0:1;
    ImGui::BeginDisabled(programming||blocked(s));if(ImGui::Combo("##state-scope",&scope,scopes,2))ui.allStates=scope==0;ImGui::EndDisabled();
    tooltip("AL-state request target. Cyclic I/O always exchanges the whole bus.","AL 状态请求目标。周期 I/O 始终交换整条总线。");x+=compact?90.f:94.f;
    ImGui::BeginDisabled(programming||!s.connected||blocked(s));
    const AlState states[]={AlState::Boot,AlState::Init,AlState::PreOp,AlState::SafeOp,AlState::Op};
    const float stateWidths[]={compact?44.f:52.f,compact?40.f:48.f,compact?57.f:66.f,compact?65.f:72.f,compact?36.f:44.f};
    for(int i=0;i<5;++i) {
        ImGui::SetCursorPos({x,rowY});const auto state=states[i];const auto raw=static_cast<uint16_t>(state);
        if(tintButton(stateName(raw),{stateWidths[i],28},actual==raw,stateColor(raw))) {
            Command cmd;cmd.kind=CommandKind::State;cmd.slave=ui.allStates?0:ui.selected;cmd.state=state;send(c,s,std::move(cmd));
        }
        if(state==AlState::Boot)tooltip("Enter BOOT through INIT using the slave's bootstrap mailbox. FoE file transfer is available in BOOT.","通过 INIT 配置从站的 Bootstrap Mailbox 后进入 BOOT，可用于 FoE 文件传输。");
        x+=stateWidths[i]+3;
    }
    ImGui::EndDisabled();ImGui::PopStyleVar();ImGui::PopFont();

    // Four compact metric cells share the remaining width. Details such as
    // target cycle and missed deadlines are available through the controls and
    // diagnostics page, while the live values remain visible at a glance.
    const float metricsStart=x+10;const float column=std::max(1.f,(width-metricsStart-8)/4.f);
    char wkc[40],actualCycle[48],jitter[48],errors[40];
    if(s.running)std::snprintf(wkc,sizeof(wkc),"%d / %d",s.wkc,s.expectedWkc);else std::snprintf(wkc,sizeof(wkc),"- / %d",s.expectedWkc);
    if(s.cycles)std::snprintf(actualCycle,sizeof(actualCycle),"%.0f",s.actualCycleUs);else std::snprintf(actualCycle,sizeof(actualCycle),"-");
    if(s.cycles>1)std::snprintf(jitter,sizeof(jitter),"%.1f",s.maxJitterUs);else std::snprintf(jitter,sizeof(jitter),"-");
    std::snprintf(errors,sizeof(errors),"%llu",static_cast<unsigned long long>(s.errors));
    const char* titles[]={tr("WKC","工作计数"),tr("ACTUAL CYCLE","实际周期"),tr("MAX. DEVIATION","最大周期偏差"),tr("COMM. ERRORS","通讯异常")};
    const char* values[]={wkc,actualCycle,jitter,errors};
    for(int i=0;i<4;++i) {
        const float cellLeft=metricsStart+column*i;const float left=cellLeft+6,right=metricsStart+column*(i+1)-7;
        if(i)draw->AddLine({origin.x+cellLeft,origin.y+9},{origin.x+cellLeft,origin.y+49},ImGui::ColorConvertFloat4ToU32(line));
        draw->PushClipRect({origin.x+cellLeft+1,origin.y+1},{origin.x+metricsStart+column*(i+1)-1,origin.y+55},true);
        textAt(left,11,titles[i],muted,fonts.small,width<940?7.f:8.f);
        const ImVec4 color=i==0?(s.running?(s.wkc==s.expectedWkc?green:red):teal):i==3&&s.errors?red:ink;
        const float unitWidth=(i==1||i==2)?15.f:0.f;
        const float preferredSize=compact?16.f:18.f;
        const float measured=font(fonts.value)->CalcTextSizeA(preferredSize,10000.f,0,values[i]).x;
        const float available=std::max(1.f,right-unitWidth-left);
        const float valueSize=measured>available?std::max(10.f,preferredSize*available/measured):preferredSize;
        rightText(right-unitWidth,27+(preferredSize-valueSize)*.5f,values[i],color,fonts.value,valueSize);
        if(unitWidth)textAt(right-13,33,"μs",muted,fonts.small,8);
        draw->PopClipRect();
    }
    ImGui::Dummy({0,0});ImGui::EndChild();ImGui::PopStyleColor();ImGui::PopStyleVar(2);
}
void eventLog(Controller& controller,const Snapshot& s,float height) {
    // The log remains a compact table so severity and timestamps stay easy to
    // scan.  Selecting a row and pressing Ctrl+C copies the complete displayed
    // line, including the translated message, to the system clipboard.
    static uint64_t selectedSequence=0;
    static std::string selectedText;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{14,8});
    ImGui::PushStyleColor(ImGuiCol_ChildBg,{1,1,1,1});
    ImGui::BeginChild("event-log",{0,height},ImGuiChildFlags_Borders|ImGuiChildFlags_AlwaysUseWindowPadding,ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushFont(font(fonts.small),11);ImGui::SetCursorPos({14,11});icon(Icon::Logs,13,ImGui::ColorConvertFloat4ToU32(teal));
    const char* logTitle=tr("Log","\u65e5\u5fd7");
    textAt(35,10,logTitle,ink,fonts.regular,12);
    textAt(45+font(fonts.regular)->CalcTextSizeA(12,10000,0,logTitle).x,12,std::to_string(s.logs.size()).c_str(),muted,fonts.mono,9);
    const float width=ImGui::GetWindowWidth();ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,{3,3});
    const std::string clearLabel=label("Clear","清空","clear-log");
    const float clearWidth=58.0f;
    const float followTextWidth=font(fonts.small)->CalcTextSizeA(11,10000,0,tr("Follow","跟随")).x;
    const float followWidth=ImGui::GetFrameHeight()+ImGui::GetStyle().ItemInnerSpacing.x+followTextWidth;
    ImGui::SetCursorPos({width-ImGui::GetStyle().WindowPadding.x-clearWidth,6});
    if(ImGui::Button(clearLabel.c_str(),{clearWidth,24})) { controller.clearLogs();selectedSequence=0;selectedText.clear(); }
    const float followY=6.0f+(24.0f-ImGui::GetFrameHeight())*.5f;
    ImGui::SetCursorPos({width-ImGui::GetStyle().WindowPadding.x-clearWidth-ImGui::GetStyle().ItemSpacing.x-followWidth,followY});
    ImGui::Checkbox(label("Follow","跟随","follow-log").c_str(),&ui.autoScroll);ImGui::PopStyleVar();
    ImGui::SetCursorPos({0,33});ImGui::Separator();ImGui::SetCursorPos({14,40});ImGui::BeginChild("log-scroll",{0,0},ImGuiChildFlags_None);
    if(ImGui::BeginTable("log-lines",3,ImGuiTableFlags_BordersInnerH|ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("time",ImGuiTableColumnFlags_WidthFixed,78);ImGui::TableSetupColumn("level",ImGuiTableColumnFlags_WidthFixed,44);ImGui::TableSetupColumn("message",ImGuiTableColumnFlags_WidthStretch);
        for(const auto& entry:s.logs) {
            ImGui::TableNextRow(ImGuiTableRowFlags_None,23);
            ImGui::TableSetColumnIndex(0);ImGui::TextColored(muted,"%s",entry.time.c_str());
            ImGui::TableSetColumnIndex(1);ImGui::TextColored(entry.error?red:teal,"%s",entry.error?"ERROR":"INFO");
            ImGui::TableSetColumnIndex(2);
            ImGui::PushID(static_cast<int>(entry.sequence));
            const auto message=trMessage(entry.message);
            ImGui::PushStyleColor(ImGuiCol_Text,entry.error?red:ink);
            if(ImGui::Selectable(message.c_str(),selectedSequence==entry.sequence,ImGuiSelectableFlags_AllowDoubleClick,{0,23})) {
                selectedSequence=entry.sequence;
                selectedText=entry.time+"  "+(entry.error?"ERROR":"INFO")+"  "+message;
            }
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if(selectedSequence&&ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)&&ImGui::GetIO().KeyCtrl&&ImGui::IsKeyPressed(ImGuiKey_C,false))
        ImGui::SetClipboardText(selectedText.c_str());
    if(s.logs.empty())ImGui::TextDisabled("%s",tr("No events yet.","暂无事件。"));
    static uint64_t lastSequence=0;if(ui.autoScroll&&!s.logs.empty()&&lastSequence!=s.logs.back().sequence)ImGui::SetScrollHereY(1.f);
    if(!s.logs.empty())lastSequence=s.logs.back().sequence;
    ImGui::EndChild();ImGui::PopFont();ImGui::EndChild();ImGui::PopStyleColor();ImGui::PopStyleVar();
}
} // namespace niceecatm::workbench
