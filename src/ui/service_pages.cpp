// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#include "ui/internal.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace niceecatm::workbench {
namespace {
constexpr float gutter=14.0f;
void smallText(const char* text) {ImGui::PushFont(fonts.small);ImGui::TextDisabled("%s",text);ImGui::PopFont();}
void smallNote(const char* en,const char* zh) {ImGui::PushFont(fonts.small);note(en,zh);ImGui::PopFont();}
void fieldLabel(const char* en,const char* zh) {ImGui::PushFont(fonts.small,11);ImGui::AlignTextToFramePadding();ImGui::TextDisabled("%s",tr(en,zh));ImGui::PopFont();ImGui::SetNextItemWidth(-1);}
void divider() {ImGui::Spacing();ImGui::Separator();ImGui::Spacing();}
void monoText(const std::string& value) {ImGui::PushFont(fonts.mono);ImGui::TextWrapped("%s",value.c_str());ImGui::PopFont();}
void selectedDeviceHeader(const Snapshot& s) {
    ImGui::AlignTextToFramePadding();
    if(const auto* d=selected(s)) {
        ImGui::TextDisabled("%s",tr("Selected slave","\u5df2\u9009\u62e9\u4ece\u7ad9"));ImGui::SameLine();
        ImGui::Text("%02d  %s",d->position,d->name.c_str());ImGui::SameLine(0,14);statePill(d->state,d->alCode);
    } else {
        ImGui::TextDisabled("%s",tr("Select a slave in the topology tree","\u8bf7\u5728\u5de6\u4fa7\u62d3\u6251\u6811\u4e2d\u9009\u62e9\u4ece\u7ad9"));
    }
    ImGui::SameLine(0,14);pill(tr(s.simulation?"Simulation":"Hardware",s.simulation?"\u6a21\u62df":"\u786c\u4ef6"),s.simulation?amber:teal);ImGui::Spacing();
}
bool keyTable(const char* id,float width=155.0f) {
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding,ImVec2(6,5));
    const bool opened=ImGui::BeginTable(id,2,ImGuiTableFlags_SizingStretchProp|ImGuiTableFlags_BordersInnerH);
    if(!opened){ImGui::PopStyleVar();return false;}
    ImGui::TableSetupColumn("key",ImGuiTableColumnFlags_WidthFixed,width);ImGui::TableSetupColumn("value",ImGuiTableColumnFlags_WidthStretch);return true;
}
void endKeyTable() {ImGui::EndTable();ImGui::PopStyleVar();}
std::string protocolNames(const Slave& d) {
    std::string value;const char* names[]={"AoE","EoE","CoE","FoE","SoE","VoE"};
    for(int i=0;i<6;++i)if(d.mailboxProtocols&(1u<<i)){if(!value.empty())value+=" · ";value+=names[i];}
    return value.empty()?tr("None declared","未声明"):value;
}
uint64_t latestTicket() {return ui.requests.empty()?0:*ui.requests.rbegin();}
struct RegisterRead {
    uint64_t generation=~uint64_t(0),ticket=0;int slave=-1;
    uint16_t pendingAddress=0,address=0;bool received=false;std::vector<uint8_t> bytes;
    void observe(const Snapshot& s) {
        if(generation!=s.generation||slave!=ui.selected){*this={};generation=s.generation;slave=ui.selected;}
        if(ticket&&s.completed==ticket){if(s.result.ok){bytes=s.result.data;address=pendingAddress;received=true;}ticket=0;}
    }
};
RegisterRead registerRead;
struct Reference {uint16_t index;uint8_t sub;const char* en;const char* zh;int type;};
constexpr Reference references[]={
    {0x1000,0,"Device type","设备类型",7},{0x1008,0,"Device name","设备名称",13},
    {0x1018,1,"Vendor ID","厂商 ID",7},{0x1018,2,"Product code","产品代码",7},
    {0x1018,3,"Revision","修订号",7},{0x1018,4,"Serial number","序列号",7}
};
struct SdoRead {
    uint64_t generation=~uint64_t(0),ticket=0;int slave=-1;
    uint16_t pendingIndex=0,index=0;uint8_t pendingSub=0,sub=0;
    bool received=false;std::vector<uint8_t> bytes;
    void observe(const Snapshot& s) {
        if(generation!=s.generation||slave!=ui.selected){*this={};generation=s.generation;slave=ui.selected;}
        if(ticket&&s.completed==ticket){if(s.result.ok){bytes=s.result.data;index=pendingIndex;sub=pendingSub;received=true;}ticket=0;}
    }
};
SdoRead sdoRead;
std::string objectAddress(uint16_t index,uint8_t sub) {return hexNumber(index,4)+":"+hexNumber(sub,2).substr(2);}
void resultBox(const char* id,float height) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4(.964f,.978f,.985f,1));ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(10,9));
    ImGui::BeginChild(id,{0,height},ImGuiChildFlags_Borders|ImGuiChildFlags_AlwaysUseWindowPadding);
}
void endResultBox() {ImGui::EndChild();ImGui::PopStyleVar();ImGui::PopStyleColor();}
struct RegisterPreset {uint16_t offset;int length;const char* en;const char* zh;bool configuration;};
constexpr RegisterPreset presets[]={
    {0x0000,4,"ESC information","ESC 信息",false},{0x0010,2,"Configured station address","配置的站地址",false},
    {0x0120,2,"AL Control","AL Control",false},{0x0130,2,"AL Status","AL Status",false},
    {0x0134,2,"AL Status Code","AL 状态码",false},{0x0400,2,"Watchdog divider","看门狗分频器",true},
    {0x0600,16,"FMMU configuration","FMMU 配置",true},{0x0800,8,"SM0 configuration","SM0 配置",true}
};
bool presetWriteSupported(const RegisterPreset& p,const Snapshot& s) {return p.offset==0x0120||(p.configuration&&(!s.simulation||p.offset>=0x0800));}
void registerDump() {
    if(!registerRead.received){smallNote("No register snapshot yet. Select an address and read it explicitly.","尚无寄存器快照。请选择地址并手动读取。");return;}
    ImGui::PushFont(fonts.mono);ImGui::TextDisabled("OFFSET  00 01 02 03 04 05 06 07  ASCII");
    for(size_t start=0;start<registerRead.bytes.size();start+=8) {
        std::string bytes,ascii;
        for(size_t i=0;i<8;++i) {
            if(start+i<registerRead.bytes.size()){const auto b=registerRead.bytes[start+i];char part[4];std::snprintf(part,sizeof(part),"%02X ",b);bytes+=part;ascii+=b>=32&&b<127?char(b):'.';}
            else bytes+="   ";
        }
        ImGui::Text("0x%04X  %s |%s|",unsigned(registerRead.address+start),bytes.c_str(),ascii.c_str());
    }
    ImGui::PopFont();
    if((registerRead.address==0x0130||registerRead.address==0x0120)&&registerRead.bytes.size()>=2) {
        const uint16_t value=uint16_t(registerRead.bytes[0])|(uint16_t(registerRead.bytes[1])<<8);
        const bool actual=registerRead.address==0x0130;
        const uint16_t state=actual?value:uint16_t(value&0xf);
        ImGui::Spacing();smallText(actual?tr("AL Status · decoded from returned bytes","AL Status · 由返回字节解码"):tr("AL Control · requested state","AL Control · 请求状态"));
        statePill(state);ImGui::SameLine();monoText(hexNumber(value,4));
        if(!actual&&(value&0x10))smallText(tr("Error acknowledgement bit set","已设置错误确认位"));
    }
}
} // namespace

// Keep a response bound to its issued address even if another tab is visible
// when the controller publishes completion. This performs no bus operation.
void observeServiceResults(const Snapshot& s) {
    registerRead.observe(s);sdoRead.observe(s);
}

void devicePage(Controller& controller,const Snapshot& s) {
    (void)controller;
    ImGui::BeginChild("device-page",{0,0});selectedDeviceHeader(s);const auto* d=selected(s);
    if(!d){beginCard("empty-device");cardHeader(tr("No device selected","尚未选择设备"),Icon::Box);note("Scan a connected bus or select a simulation device to inspect its capabilities.","请扫描已连接的总线，或选择模拟设备查看其能力。");endCard();ImGui::EndChild();return;}
    const float height=std::max(444.0f,ImGui::GetContentRegionAvail().y-2);ImGui::PushStyleVar(ImGuiStyleVar_CellPadding,ImVec2(0,0));
    if(ImGui::BeginTable("device-columns",2,ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("identity",ImGuiTableColumnFlags_WidthStretch,1.13f);ImGui::TableSetupColumn("capabilities",ImGuiTableColumnFlags_WidthStretch,1.0f);ImGui::TableNextColumn();
        beginCard("device-identity",{ImGui::GetContentRegionAvail().x-gutter,height});cardHeader(tr("Device capability report","设备能力报告"),Icon::Box);
        smallNote("Identity returned by the selected slave","所选从站返回的设备标识");ImGui::Spacing();
        if(keyTable("device-identity-table",142)) {
            keyValue("Device name","设备名称",d->name);keyValue("Vendor ID","厂商 ID",hexNumber(d->vendor));keyValue("Product code","产品代码",hexNumber(d->product));
            keyValue("Revision / Serial","修订 / 序列号",hexNumber(d->revision)+" / "+hexNumber(d->serial));keyValue("Station address","站地址",hexNumber(d->address,4));
            ImGui::TableNextRow();ImGui::TableNextColumn();ImGui::TextDisabled("%s",tr("Actual / Requested AL","实际 / 请求 AL"));ImGui::TableNextColumn();
            statePill(d->state,d->alCode);ImGui::SameLine();ImGui::TextDisabled("/");ImGui::SameLine();statePill(d->requested);
            keyValue("AL status code","AL 状态码",hexNumber(d->alCode,4));endKeyTable();
        }
        divider();smallText(tr("DECLARED MAILBOX PROTOCOLS","声明的 MAILBOX 协议"));ImGui::Spacing();ImGui::TextWrapped("%s",protocolNames(*d).c_str());
        divider();
        if(s.simulation)smallNote("Source: explicit simulation fixtures. This session does not access real hardware.","来源：明确标记的模拟数据。本次会话未访问真实硬件。");
        else smallNote("Source: SOEM scan and actual process mapping. Slave names, identity values and returned strings remain unchanged.","来源：SOEM 扫描及实际过程映射。从站名称、标识值和返回字符串保持原样。");
        endCard();ImGui::TableNextColumn();
        beginCard("device-mapping-capabilities",{0,height});cardHeader(tr("Process mapping & services","过程映射与服务"),Icon::Table);
        smallText(tr("ACTUAL MAPPING SUMMARY","实际映射摘要"));ImGui::Spacing();
        if(keyTable("device-process-mapping",154)) {
            keyValue("IN / OUT size","输入 / 输出大小",std::to_string(d->inputBits)+" / "+std::to_string(d->outputBits)+" bit");
            keyValue("IN / OUT start bit","输入 / 输出起始位",std::to_string(d->inputStartBit)+" / "+std::to_string(d->outputStartBit));
            keyValue("SOEM IN / OUT offset","SOEM 输入 / 输出偏移",std::to_string(d->inputOffset)+" / "+std::to_string(d->outputOffset));
            keyValue("Mapping mode","映射模式",s.connection.overlap?"Overlapped":"Normal");keyValue("Process map","过程映射",s.mapped?tr("Mapped","已映射"):tr("Not mapped","未映射"));endKeyTable();
        }
        divider();cardHeader(tr("DC / SYNC0 / SYNC1","DC / SYNC0 / SYNC1"),Icon::Clock);pill(d->dc?tr("DC declared","声明支持 DC"):tr("DC not declared","未声明 DC"),d->dc?teal:muted);
        ImGui::Spacing();smallNote("DC trigger configuration and readback are not connected to the native service yet.","原生服务尚未接入 DC 触发配置及实际读回。");
        divider();smallNote("Signal names and types use the verified PDO layout and matching ESI / online descriptions. Unresolved fields remain raw process data.","信号名称和类型来自已核对的 PDO 布局及匹配的 ESI / 在线描述，无法解释的字段保留原始过程数据。");endCard();ImGui::EndTable();
    }
    ImGui::PopStyleVar();ImGui::EndChild();
}

void registersPage(Controller& c,const Snapshot& s) {
    registerRead.observe(s);ImGui::BeginChild("registers-page",{0,0});const float height=std::max(480.0f,ImGui::GetContentRegionAvail().y-2);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding,ImVec2(0,0));
    if(ImGui::BeginTable("register-columns",2,ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("directory",ImGuiTableColumnFlags_WidthStretch);ImGui::TableSetupColumn("editor",ImGuiTableColumnFlags_WidthFixed,300);ImGui::TableNextColumn();
        beginCard("register-directory",{ImGui::GetContentRegionAvail().x-gutter,height});cardHeader(tr("Register snapshot","寄存器快照"),Icon::Registers);
        smallNote("ESC physical addresses · values are read explicitly","ESC 物理地址 · 当前值需手动读取");divider();
        ImGui::BeginChild("register-hex-snapshot",{0,110});registerDump();ImGui::EndChild();divider();ImGui::PushStyleVar(ImGuiStyleVar_CellPadding,ImVec2(8,8));
        if(ImGui::BeginTable("register-reference",4,ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerH|ImGuiTableFlags_ScrollY,{0,std::max(190.0f,ImGui::GetContentRegionAvail().y-65)})) {
            ImGui::TableSetupColumn(tr("ESC offset","ESC 偏移"),ImGuiTableColumnFlags_WidthFixed,76);ImGui::TableSetupColumn(tr("Common registers","常用寄存器"),ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn(tr("Access","访问"),ImGuiTableColumnFlags_WidthFixed,55);ImGui::TableSetupColumn(tr("Size / Limits","长度 / 限制"),ImGuiTableColumnFlags_WidthFixed,108);
            ImGui::TableSetupScrollFreeze(0,1);ImGui::TableHeadersRow();uint64_t current=0;parseUnsigned(ui.regAddress,65535,current);
            for(const auto& p:presets) {
                ImGui::PushID(p.offset);ImGui::TableNextRow();ImGui::TableNextColumn();
                if(ImGui::Selectable(hexNumber(p.offset,4).c_str(),current==p.offset,ImGuiSelectableFlags_SpanAllColumns)){std::snprintf(ui.regAddress,sizeof(ui.regAddress),"0x%04X",p.offset);ui.regLength=p.length;}
                ImGui::TableNextColumn();ImGui::TextWrapped("%s",tr(p.en,p.zh));ImGui::TableNextColumn();const bool writable=presetWriteSupported(p,s);pill(writable?"RW":"RO",writable?amber:teal);
                ImGui::TableNextColumn();smallText(p.configuration&&writable?"INIT / PRE-OP":(p.offset==0x0120?tr("AL request","AL 请求"):(std::to_string(p.length)+" B").c_str()));ImGui::PopID();
            }ImGui::EndTable();
        }
        ImGui::PopStyleVar();divider();smallNote("Selecting a row fills the address only. Read-only and unknown permissions are enforced by the backend; no write unlock is required.","选择行仅填入地址。只读及未知权限由后端强制检查，无需解锁即可写入允许的寄存器。");endCard();ImGui::TableNextColumn();
        beginCard("register-editor",{0,height});cardHeader(tr("ESC register access","ESC 寄存器访问"),Icon::Registers);
        if(const auto* d=selected(s)){fieldLabel("Selected slave","\u5df2\u9009\u62e9\u4ece\u7ad9");ImGui::Text("%02d  %s",d->position,d->name.c_str());}else smallNote("Select a slave in the topology tree.","\u8bf7\u5728\u5de6\u4fa7\u62d3\u6251\u6811\u4e2d\u9009\u62e9\u4ece\u7ad9");ImGui::Spacing();
        if(ImGui::BeginTable("register-address-fields",2,ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextColumn();fieldLabel("ESC physical offset","ESC 物理偏移");ImGui::InputText("##reg-offset",ui.regAddress,sizeof(ui.regAddress));
            ImGui::TableNextColumn();fieldLabel("Length (bytes)","长度（字节）");ImGui::InputInt("##reg-length",&ui.regLength,0,0);ImGui::EndTable();
        }
        ImGui::Spacing();fieldLabel("Write bytes (HEX, little-endian)","写入字节（HEX，小端序）");ImGui::InputTextMultiline("##reg-data",ui.regWrite,sizeof(ui.regWrite),{-1,74});ImGui::Spacing();
        const float buttonWidth=(ImGui::GetContentRegionAvail().x-ImGui::GetStyle().ItemSpacing.x)*.5f;ImGui::BeginDisabled(!selected(s)||!s.connected||blocked(s)||s.running);
        if(ImGui::Button(label("Read","读取","reg-read").c_str(),{buttonWidth,30})) {
            uint64_t address=0;
            if(!parseUnsigned(ui.regAddress,65535,address)||ui.regLength<1||ui.regLength>32||address+ui.regLength>65536)ui.localError="Use an offset in 0…65535 and a length in 1…32 bytes.";
            else{Command cmd;cmd.kind=CommandKind::ReadRegister;cmd.slave=ui.selected;cmd.address=static_cast<uint16_t>(address);cmd.length=ui.regLength;const auto previous=latestTicket();send(c,s,std::move(cmd));if(latestTicket()!=previous){registerRead.ticket=latestTicket();registerRead.pendingAddress=static_cast<uint16_t>(address);}}
        }
        ImGui::SameLine();ImGui::PushStyleColor(ImGuiCol_Button,teal);ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(1,1,1,1));
        if(ImGui::Button(label("Write directly","直接写入","reg-write").c_str(),{buttonWidth,30})) {
            uint64_t address=0;std::vector<uint8_t> bytes;std::string error;
            if(!parseUnsigned(ui.regAddress,65535,address))ui.localError="Invalid ESC offset.";else if(!parseHex(ui.regWrite,bytes,error))ui.localError=error;
            else if(bytes.empty()||bytes.size()>32||address+bytes.size()>65536)ui.localError="Write 1…32 bytes within the ESC address range.";
            else{Command cmd;cmd.kind=CommandKind::WriteRegister;cmd.slave=ui.selected;cmd.address=static_cast<uint16_t>(address);cmd.data=std::move(bytes);send(c,s,std::move(cmd));}
        }
        ImGui::PopStyleColor(2);ImGui::EndDisabled();ImGui::Spacing();resultBox("register-read-result",87);smallText(tr("LAST READ · ORIGINAL BYTES","最近读取 · 原始字节"));
        if(registerRead.received){smallText(hexNumber(registerRead.address,4).c_str());monoText(hexBytes(registerRead.bytes));}else smallNote("No read result","尚无读取结果");endResultBox();ImGui::Spacing();
        if(s.running)smallNote("Stop cyclic exchange before synchronous ESC access.","同步访问 ESC 前请先停止周期交换。");
        else smallNote("AL Control requests a state transition. Configuration writes require INIT / PRE-OP; changing SM or FMMU invalidates the map and requires a rescan.","AL Control 用于请求状态切换。配置写入需处于 INIT / PRE-OP；修改 SM 或 FMMU 后映射失效，需重新扫描。");
        smallNote("1–32 B per operation · no write unlock","每次操作 1–32 B · 无需写入解锁");endCard();ImGui::EndTable();
    }
    ImGui::PopStyleVar();ImGui::EndChild();
}

void mailboxPage(const Snapshot& s) {
    ImGui::BeginChild("mailbox-page",{0,0});selectedDeviceHeader(s);const auto* d=selected(s);const float height=std::max(405.0f,ImGui::GetContentRegionAvail().y-2);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding,ImVec2(0,0));
    if(ImGui::BeginTable("mailbox-columns",2,ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("protocols",ImGuiTableColumnFlags_WidthStretch,1.4f);ImGui::TableSetupColumn("availability",ImGuiTableColumnFlags_WidthStretch,1.0f);ImGui::TableNextColumn();
        beginCard("mailbox-protocols",{ImGui::GetContentRegionAvail().x-gutter,height});cardHeader(tr("Mailbox protocol capabilities","Mailbox 协议能力"),Icon::Mail);
        smallNote("Mailbox is a transport; CoE / SDO is one of its services.","Mailbox 是传输通道，CoE / SDO 是其中一种上层服务。");divider();ImGui::PushStyleVar(ImGuiStyleVar_CellPadding,ImVec2(8,10));
        if(ImGui::BeginTable("mailbox-support",3,ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerH)) {
            ImGui::TableSetupColumn(tr("Protocol","协议"),ImGuiTableColumnFlags_WidthFixed,88);ImGui::TableSetupColumn(tr("Slave declares","从站声明"),ImGuiTableColumnFlags_WidthFixed,100);ImGui::TableSetupColumn(tr("Tool service","工具服务"));ImGui::TableHeadersRow();
            const char* names[]={"CoE / SDO","FoE","EoE","SoE","VoE","AoE"};const unsigned masks[]={4,8,2,16,32,1};
            for(int i=0;i<6;++i){ImGui::TableNextRow();ImGui::TableNextColumn();ImGui::TextUnformatted(names[i]);ImGui::TableNextColumn();const bool yes=d&&(d->mailboxProtocols&masks[i]);pill(d?(yes?tr("Yes","支持"):tr("No","未声明")):"—",yes?teal:muted);ImGui::TableNextColumn();ImGui::TextWrapped("%s",i==0?tr("Manual SDO read / write","手动 SDO 读写"):tr("Not connected","尚未接入"));}ImGui::EndTable();
        }
        ImGui::PopStyleVar();divider();smallNote("The declaration belongs to the slave. Each protocol requires a separate service adapter in the tool.","能力声明属于从站，各协议需要工具接入独立的服务适配器。");endCard();ImGui::TableNextColumn();
        beginCard("mailbox-availability",{0,height});cardHeader(tr("Transport & availability","传输与可用性"),Icon::Network);
        if(d){if(keyTable("mailbox-summary",128)){keyValue("Protocol mask","协议掩码",hexNumber(d->mailboxProtocols,4));keyValue("Actual AL state","实际 AL 状态",stateLabel(d->state));const unsigned state=d->state&15;keyValue("Mailbox phase","Mailbox 阶段",state==1?tr("Unavailable in INIT","INIT 中不可用"):(state==2||state==4||state==8?tr("Mailbox phase available","状态允许 Mailbox"):tr("Unconfirmed state","未确认的状态")));keyValue("Synchronous access","同步访问",s.running?tr("Stop cyclic exchange first","需先停止周期交换"):tr("Cycle stopped","周期已停止"));endKeyTable();}}
        else smallNote("Select a slave to inspect its declaration.","请选择从站查看其能力声明。");
        divider();cardHeader(tr("CoE · SDO","CoE · SDO"),Icon::Book);smallNote("Use the CoE · SDO tab for manual parameter transactions when CoE is declared and the actual state is PRE-OP, SAFE-OP or OP.","从站声明支持 CoE 且实际状态为 PRE-OP、SAFE-OP 或 OP 时，可在 CoE · SDO 页手动访问参数。");
        divider();cardHeader(tr("Raw Mailbox / other protocols","原始 Mailbox / 其他协议"),Icon::Mail);smallNote("Raw Mailbox send / receive, FoE file transfer, EoE tunnelling, SoE and VoE are pending native services. No response is fabricated.","原始 Mailbox 收发、FoE 文件传输、EoE 隧道、SoE 和 VoE 原生服务待接入，不生成虚假的设备响应。");endCard();ImGui::EndTable();
    }
    ImGui::PopStyleVar();ImGui::EndChild();
}

void sdoPage(Controller& c,const Snapshot& s) {
    sdoRead.observe(s);const auto* d=selected(s);const auto state=d?d->state&0xf:0;const bool coe=d&&(d->mailboxProtocols&4);
    const bool enabled=coe&&s.connected&&!blocked(s)&&!s.running&&(state==2||state==4||state==8);
    ImGui::BeginChild("sdo-page",{0,0});const float height=std::max(490.0f,ImGui::GetContentRegionAvail().y-2);ImGui::PushStyleVar(ImGuiStyleVar_CellPadding,ImVec2(0,0));
    if(ImGui::BeginTable("sdo-columns",2,ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("reference",ImGuiTableColumnFlags_WidthStretch);ImGui::TableSetupColumn("editor",ImGuiTableColumnFlags_WidthFixed,300);ImGui::TableNextColumn();
        beginCard("sdo-address-reference",{ImGui::GetContentRegionAvail().x-gutter,height});cardHeader(tr("Object address reference","对象地址参考"),Icon::Book);smallNote("Manual address shortcuts · not a discovered object dictionary","手动地址快捷项 · 非已发现的对象字典");divider();
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding,ImVec2(8,11));
        if(ImGui::BeginTable("sdo-reference-table",4,ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerH|ImGuiTableFlags_ScrollY,{0,std::max(255.0f,ImGui::GetContentRegionAvail().y-120)})) {
            ImGui::TableSetupColumn("Index : Sub",ImGuiTableColumnFlags_WidthFixed,100);ImGui::TableSetupColumn(tr("Reference object","参考对象"),ImGuiTableColumnFlags_WidthStretch);ImGui::TableSetupColumn(tr("Data type","数据类型"),ImGuiTableColumnFlags_WidthFixed,72);ImGui::TableSetupColumn(tr("Read cache","读取缓存"),ImGuiTableColumnFlags_WidthFixed,108);ImGui::TableSetupScrollFreeze(0,1);ImGui::TableHeadersRow();
            uint64_t currentIndex=0,currentSub=0;parseUnsigned(ui.sdoIndex,65535,currentIndex);parseUnsigned(ui.sdoSub,255,currentSub);
            for(const auto& ref:references) {
                ImGui::PushID(ref.index*256+ref.sub);ImGui::TableNextRow();ImGui::TableNextColumn();
                if(ImGui::Selectable(objectAddress(ref.index,ref.sub).c_str(),currentIndex==ref.index&&currentSub==ref.sub,ImGuiSelectableFlags_SpanAllColumns)){std::snprintf(ui.sdoIndex,sizeof(ui.sdoIndex),"0x%04X",ref.index);std::snprintf(ui.sdoSub,sizeof(ui.sdoSub),"0x%02X",ref.sub);ui.sdoType=ref.type;ui.localError.clear();}
                ImGui::TableNextColumn();ImGui::TextWrapped("%s",tr(ref.en,ref.zh));ImGui::TableNextColumn();smallText(types[ref.type]);ImGui::TableNextColumn();
                if(sdoRead.received&&sdoRead.index==ref.index&&sdoRead.sub==ref.sub){const auto value=decodeValue(sdoRead.bytes,types[ref.type]);ImGui::TextWrapped("%s",ref.type==13?value.c_str():trMessage(value).c_str());}else smallText(tr("Not read","尚未读取"));ImGui::PopID();
            }ImGui::EndTable();
        }
        ImGui::PopStyleVar();divider();smallNote("These common CoE addresses are navigation aids. Their existence, access rights and current values are confirmed only by the selected device's response.","常见 CoE 地址仅用于快速填入。是否存在、访问权限及当前值，以所选设备实际响应为准。");
        ImGui::Spacing();smallNote("Dictionary discovery is pending. Manual Index / SubIndex access remains available; object permissions are enforced by the slave.","对象字典发现待接入。可手动输入 Index / SubIndex，具体访问权限由从站执行。");endCard();ImGui::TableNextColumn();
        beginCard("sdo-parameter-editor",{0,height});cardHeader(tr("SDO parameter access","SDO 参数访问"),Icon::Book);if(const auto* d=selected(s)){fieldLabel("Selected slave","\u5df2\u9009\u62e9\u4ece\u7ad9");ImGui::Text("%02d  %s",d->position,d->name.c_str());}else smallNote("Select a slave in the topology tree.","\u8bf7\u5728\u5de6\u4fa7\u62d3\u6251\u6811\u4e2d\u9009\u62e9\u4ece\u7ad9");ImGui::Spacing();
        if(ImGui::BeginTable("sdo-index-fields",2,ImGuiTableFlags_SizingStretchSame)){ImGui::TableNextColumn();fieldLabel("Index","Index");ImGui::InputText("##sdo-index",ui.sdoIndex,sizeof(ui.sdoIndex));ImGui::TableNextColumn();fieldLabel("SubIndex","SubIndex");ImGui::InputText("##sdo-sub",ui.sdoSub,sizeof(ui.sdoSub));ImGui::EndTable();}
        ImGui::Spacing();fieldLabel("Data type","数据类型");ImGui::SetNextItemWidth(-1);ImGui::Combo("##sdo-type",&ui.sdoType,types,14);ImGui::Spacing();
        if(ImGui::BeginTable("sdo-capacity-fields",2,ImGuiTableFlags_SizingStretchProp)){ImGui::TableSetupColumn("capacity",ImGuiTableColumnFlags_WidthStretch);ImGui::TableSetupColumn("format",ImGuiTableColumnFlags_WidthFixed,68);ImGui::TableNextColumn();fieldLabel("Read capacity (B)","读取容量（B）");ImGui::InputInt("##sdo-capacity",&ui.sdoCapacity,0,0);ImGui::TableNextColumn();fieldLabel("Format","格式");ImGui::Checkbox("HEX##sdo-hex",&ui.sdoHex);ImGui::EndTable();}
        ImGui::Spacing();fieldLabel("Write value","写入值");ImGui::InputText("##sdo-value",ui.sdoValue,sizeof(ui.sdoValue));ImGui::Spacing();const float buttonWidth=(ImGui::GetContentRegionAvail().x-ImGui::GetStyle().ItemSpacing.x)*.5f;
        ImGui::BeginDisabled(!enabled);ImGui::PushStyleColor(ImGuiCol_Button,teal);ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(1,1,1,1));const bool read=ImGui::Button(label("Read","读取","sdo-read").c_str(),{buttonWidth,30});ImGui::PopStyleColor(2);ImGui::SameLine();const bool write=ImGui::Button(label("Write","写入","sdo-write").c_str(),{buttonWidth,30});
        if(read||write) {
            uint64_t index=0,sub=0;std::vector<uint8_t> bytes;std::string error;
            if(!parseUnsigned(ui.sdoIndex,65535,index)||!parseUnsigned(ui.sdoSub,255,sub))ui.localError="Invalid Index / SubIndex.";else if(read&&(ui.sdoCapacity<1||ui.sdoCapacity>4096))ui.localError="Read capacity must be 1…4096 bytes.";else if(write&&!encodeValue(ui.sdoValue,types[ui.sdoType],ui.sdoHex,bytes,error))ui.localError=error;
            else{Command cmd;cmd.kind=read?CommandKind::ReadSdo:CommandKind::WriteSdo;cmd.slave=ui.selected;cmd.index=static_cast<uint16_t>(index);cmd.subindex=static_cast<uint8_t>(sub);cmd.length=ui.sdoCapacity;cmd.data=std::move(bytes);const auto previous=latestTicket();send(c,s,std::move(cmd));if(read&&latestTicket()!=previous){sdoRead.ticket=latestTicket();sdoRead.pendingIndex=static_cast<uint16_t>(index);sdoRead.pendingSub=static_cast<uint8_t>(sub);}}
        }
        ImGui::EndDisabled();ImGui::Spacing();resultBox("sdo-read-result",95);smallText(tr("LAST READ · ORIGINAL BYTES","最近读取 · 原始字节"));
        if(sdoRead.received){smallText(objectAddress(sdoRead.index,sdoRead.sub).c_str());monoText(hexBytes(sdoRead.bytes));const auto decoded=decodeValue(sdoRead.bytes,types[ui.sdoType],ui.sdoHex);ImGui::TextWrapped("%s",std::string(types[ui.sdoType])=="STRING"?decoded.c_str():trMessage(decoded).c_str());}else smallNote("No read result","尚无读取结果");endResultBox();ImGui::Spacing();
        if(s.running)smallNote("Stop cyclic exchange before synchronous SDO access.","同步访问 SDO 前请先停止周期交换。");else if(!coe)smallNote("The selected slave does not declare CoE support.","所选从站未声明支持 CoE。");else if(state==1)smallNote("Mailbox is unavailable in INIT. Request PRE-OP, SAFE-OP or OP first.","INIT 中 Mailbox 不可用，请先请求 PRE-OP、SAFE-OP 或 OP。");else smallNote("Write directly when the object permits. Device-returned strings and raw Abort codes are preserved.","对象允许时可直接写入，保留设备返回的字符串和原始 Abort 码。");endCard();ImGui::EndTable();
    }
    ImGui::PopStyleVar();ImGui::EndChild();
}

void diagnosticsPage(const Snapshot& s) {
    struct History{uint64_t generation=~uint64_t(0),cycle=0;std::vector<float> values;};static History history;
    if(history.generation!=s.generation){history={};history.generation=s.generation;}
    if(s.cycles!=history.cycle){history.cycle=s.cycles;if(s.cycles&&std::isfinite(s.actualCycleUs)&&s.actualCycleUs>0){history.values.push_back(float(s.actualCycleUs-s.connection.cycleUs));if(history.values.size()>200)history.values.erase(history.values.begin());}}
    ImGui::BeginChild("diagnostics-page",{0,0});
    const float summaryHeight=std::min(280.0f,std::max(200.0f,ImGui::GetContentRegionAvail().y-138.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding,ImVec2(0,0));
    if(ImGui::BeginTable("diagnostics-columns",2,ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();beginCard("diagnostics-slaves",{ImGui::GetContentRegionAvail().x-gutter,summaryHeight});cardHeader(tr("Slave states & timing","从站状态与周期"),Icon::Pulse);ImGui::PushStyleVar(ImGuiStyleVar_CellPadding,ImVec2(6,6));
        if(ImGui::BeginTable("diagnostics-slave-states",3,ImGuiTableFlags_BordersInnerH|ImGuiTableFlags_ScrollY,{0,125})) {
            ImGui::TableSetupColumn(tr("Slave","从站"),ImGuiTableColumnFlags_WidthStretch);ImGui::TableSetupColumn(tr("Actual AL","实际 AL"),ImGuiTableColumnFlags_WidthFixed,112);ImGui::TableSetupColumn(tr("Request / Code","请求 / 状态码"),ImGuiTableColumnFlags_WidthFixed,135);ImGui::TableSetupScrollFreeze(0,1);ImGui::TableHeadersRow();
            for(const auto& slave:s.slaves){ImGui::TableNextRow();ImGui::TableNextColumn();ImGui::Text("%02d  %s",slave.position,slave.name.c_str());ImGui::TableNextColumn();statePill(slave.state,slave.alCode);ImGui::TableNextColumn();ImGui::PushFont(fonts.small);ImGui::Text("%s / %s",stateName(slave.requested),hexNumber(slave.alCode,4).c_str());ImGui::PopFont();}
            if(s.slaves.empty()){ImGui::TableNextRow();ImGui::TableNextColumn();ImGui::TextDisabled("%s",tr("No scanned slaves","尚无已扫描的从站"));}ImGui::EndTable();
        }
        ImGui::PopStyleVar();divider();
        if(keyTable("diagnostics-timing",154)){keyValue("Target / Actual","目标 / 实际",std::to_string(s.connection.cycleUs)+" / "+(s.cycles?std::to_string(int(s.actualCycleUs)):"—")+" µs");keyValue("Cycles / Missed","周期数 / 错过周期",std::to_string(s.cycles)+" / "+std::to_string(s.missed));keyValue("Maximum deviation","最大周期偏差",s.cycles?std::to_string(int(s.maxJitterUs))+" µs":"—");endKeyTable();}
        endCard();ImGui::TableNextColumn();beginCard("diagnostics-transport",{0,summaryHeight});cardHeader(tr("Master & links","主站与链路"),Icon::Network);
        if(keyTable("diagnostics-master",145)){keyValue("Backend","后端",s.simulation?tr("Built-in simulation","内置模拟器"):"SOEM V2");keyValue("Driver / transport","驱动 / 传输",s.driver.empty()?"—":trMessage(s.driver));keyValue("Connection mode","连接模式",s.connection.redundant?tr("Same-bus dual-port redundancy","同一总线双口冗余"):tr("Single NIC","单网口"));keyValue("Mapping mode","映射模式",s.connection.overlap?"Overlapped":"Normal");keyValue("WKC actual / expected","实际 / 期望 WKC",std::to_string(s.wkc)+" / "+std::to_string(s.expectedWkc));keyValue("Errors / Generation","错误数 / 连接代次",std::to_string(s.errors)+" / "+std::to_string(s.generation));endKeyTable();}
        divider();smallNote("Per-port failure counters and DC synchronisation readback are not available yet. The connection mode does not establish individual link health.","逐网口故障计数和 DC 同步读回尚未接入。连接模式不代表逐条链路的健康状态。");endCard();ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    // Size the plot from the actual space left after its header and limits.
    // A fixed minimum plot height overflows this card at the default size.
    const float availableHistory=ImGui::GetContentRegionAvail().y-ImGui::GetStyle().ItemSpacing.y-2.0f;
    const float historyHeight=std::max(1.0f,std::min(152.0f,availableHistory));
    ImGui::Spacing();beginCard("diagnostics-history",{0,historyHeight},{14,8});cardHeader(tr("Cycle deviation history","周期偏差历史"),Icon::Pulse);
    if(history.values.size()>1) {
        float limit=1;for(float value:history.values)limit=std::max(limit,std::abs(value));
        std::ostringstream limits;limits<<std::fixed<<std::setprecision(1)<<-limit<<" … +"<<limit<<" µs";
        smallText((std::string(tr("Window limits: ","窗口上下限："))+limits.str()).c_str());
        const ImVec2 availablePlot=ImGui::GetContentRegionAvail();
        const float plotHeight=std::min(80.0f,availablePlot.y-ImGui::GetStyle().ItemSpacing.y-1.0f);
        if(plotHeight>0) {
            ImGui::PushStyleColor(ImGuiCol_PlotLines,teal);const float alpha=ImGui::GetStyle().Alpha;
            ImGui::BeginDisabled();ImGui::PushStyleVar(ImGuiStyleVar_Alpha,alpha);
            ImGui::PlotLines("##actual-cycle-deviation",history.values.data(),int(history.values.size()),0,nullptr,-limit,limit,{availablePlot.x,plotHeight});
            ImGui::PopStyleVar();ImGui::EndDisabled();ImGui::PopStyleColor();
        }
    }
    endCard();ImGui::EndChild();
}
} // namespace niceecatm::workbench
