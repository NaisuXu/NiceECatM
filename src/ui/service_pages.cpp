// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#include "ui/internal.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <map>
#include <set>
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
constexpr const char* sdoTypes[]={"BOOL","UINT3","UINT8","INT8","UINT12","UINT16","INT16","UINT32","INT32","UINT64","INT64","REAL32","REAL64","STRING","BYTES"};
constexpr int sdoBytesType=14;
struct SdoRead {
    struct Value {std::vector<uint8_t> bytes;std::string type;};
    struct Pending {uint64_t ticket=0;int slave=0;uint32_t address=0;std::string type;std::chrono::steady_clock::time_point issued;};
    uint64_t generation=~uint64_t(0);
    Controller* controller=nullptr;
    std::map<std::pair<int,uint32_t>,Value> values;
    std::vector<Pending> pending;
    void observe(const Snapshot& s) {
        if(generation!=s.generation){generation=s.generation;values.clear();pending.clear();}
        for(auto it=pending.begin();it!=pending.end();) {
            const bool ongoing=controller&&controller->pending(it->ticket);
            Result result;
            if(!controller||!controller->result(it->ticket,result)) {
                // Result history is bounded; a suspended GUI must not retain
                // an unresolvable request forever after heavy script traffic.
                if(controller&&!ongoing&&std::chrono::steady_clock::now()-it->issued>std::chrono::seconds(30))it=pending.erase(it);else ++it;
                continue;
            }
            if(result.ok) {
                const auto key=std::make_pair(it->slave,it->address);
                if(values.size()>=256&&!values.count(key))values.erase(values.begin());
                values[key]={std::move(result.data),it->type};
            }
            it=pending.erase(it);
        }
    }
    const Value* find(int slave,uint16_t index,uint8_t sub) const {
        const auto found=values.find({slave,(uint32_t(index)<<8)|sub});
        return found==values.end()?nullptr:&found->second;
    }
};
SdoRead sdoRead;
std::string objectAddress(uint16_t index,uint8_t sub) {return hexNumber(index,4)+":"+hexNumber(sub,2).substr(2);}
std::string sdoValueText(const SdoRead::Value& value,bool hex=false) {
    return value.type=="BYTES"?hexBytes(value.bytes):decodeValue(value.bytes,value.type,hex);
}
std::string sdoValuePreview(const SdoRead::Value& value) {
    if((value.type=="BYTES"||value.type=="STRING")&&value.bytes.size()>96) {
        SdoRead::Value preview;preview.type=value.type;preview.bytes.assign(value.bytes.begin(),value.bytes.begin()+96);
        if(value.type=="STRING") {
            // Keep an entire UTF-8 code point at the preview boundary.
            while(!preview.bytes.empty()&&(value.bytes[preview.bytes.size()]&0xc0)==0x80)preview.bytes.pop_back();
        }
        return sdoValueText(preview)+"…";
    }
    return sdoValueText(value);
}
std::string lowerSdoText(std::string text) {
    std::transform(text.begin(),text.end(),text.begin(),[](unsigned char ch){return char(std::tolower(ch));});return text;
}
struct SdoDictionaryView {
    struct Row {const SdoObject* object;const SdoEntry* entry;std::string name;bool child=false;};
    struct Pending {uint64_t ticket;std::shared_ptr<const SdoDictionary> previous;std::chrono::steady_clock::time_point issued;};
    uint64_t generation=~uint64_t(0);
    int slave=-1;
    bool chinese=false,dirty=true;
    char filter[160]="";
    std::shared_ptr<const SdoDictionary> online,esi;
    std::vector<SdoObject> objects;
    std::vector<Row> rows;
    std::set<int> attempted;
    Controller* controller=nullptr;
    std::map<int,Pending> pending;
    std::map<int,std::string> errors;
    std::map<int,std::shared_ptr<const SdoDictionary>> observedOnline;
    void observeResults(const Snapshot& s) {
        if(generation!=s.generation){auto* host=controller;*this={};controller=host;generation=s.generation;}
        // A result can become available before the GUI receives its metadata
        // snapshot. A later new dictionary supersedes that transient error.
        for(const auto& device:s.slaves) {
            auto& previous=observedOnline[device.position];
            if(previous!=device.sdoDictionary){previous=device.sdoDictionary;errors.erase(device.position);}
        }
        for(auto it=pending.begin();it!=pending.end();) {
            const auto device=std::find_if(s.slaves.begin(),s.slaves.end(),[&](const Slave& slave){return slave.position==it->first;});
            const bool updated=device!=s.slaves.end()&&device->sdoDictionary&&device->sdoDictionary!=it->second.previous;
            const bool ongoing=controller&&controller->pending(it->second.ticket);
            Result result;
            if(controller&&controller->result(it->second.ticket,result)) {
                if(result.ok||updated)errors.erase(it->first);else errors[it->first]=result.message;
            } else if(controller&&!ongoing&&updated)errors.erase(it->first);
            else if(controller&&!ongoing&&std::chrono::steady_clock::now()-it->second.issued>std::chrono::seconds(30))errors[it->first]="Online dictionary result expired; refresh to try again.";
            else {++it;continue;}
            it=pending.erase(it);
        }
    }
    void observe(Controller& host,const Snapshot& s,const Slave* device) {
        controller=&host;observeResults(s);
        if(slave!=(device?device->position:-1)||online!=(device?device->sdoDictionary:nullptr)||esi!=(device?device->esiSdoDictionary:nullptr)) {
            slave=device?device->position:-1;online=device?device->sdoDictionary:nullptr;esi=device?device->esiSdoDictionary:nullptr;
            objects=device?mergedSdoObjects(*device):std::vector<SdoObject>{};
            dirty=true;
        }
        if(chinese!=ui.chinese){chinese=ui.chinese;dirty=true;}
        if(!dirty)return;
        rows.clear();const auto query=lowerSdoText(filter);
        for(const auto& object:objects) {
            const auto objectName=sdoName(object.names,chinese);
            const bool objectMatch=query.empty()||lowerSdoText(hexNumber(object.index,4)+" "+objectName).find(query)!=std::string::npos;
            const bool scalar=object.objectCode!=8&&object.objectCode!=9&&object.entries.size()==1&&object.entries.front().subindex==0;
            if(scalar) {
                const auto& entry=object.entries.front();auto name=sdoName(entry.names,chinese);if(name.empty())name=objectName;
                if(objectMatch||lowerSdoText(objectAddress(object.index,0)+" "+name).find(query)!=std::string::npos)rows.push_back({&object,&entry,std::move(name)});
                continue;
            }
            std::vector<Row> children;
            for(const auto& entry:object.entries) {
                auto name=sdoName(entry.names,chinese);
                if(objectMatch||lowerSdoText(objectAddress(object.index,entry.subindex)+" "+name).find(query)!=std::string::npos)children.push_back({&object,&entry,std::move(name),true});
            }
            if(!objectMatch&&children.empty())continue;
            rows.push_back({&object,nullptr,objectName});
            rows.insert(rows.end(),children.begin(),children.end());
        }
        dirty=false;
    }
    void request(Controller& controller,const Snapshot& s,int position) {
        attempted.insert(position);Command command;command.kind=CommandKind::ReadSdoDictionary;command.slave=position;
        const auto previous=latestTicket();send(controller,s,std::move(command));
        if(latestTicket()!=previous){pending[position]={latestTicket(),online,std::chrono::steady_clock::now()};errors.erase(position);}
    }
};
SdoDictionaryView sdoDictionaryView;
void selectSdoEntry(uint16_t index,const SdoEntry& entry) {
    std::snprintf(ui.sdoIndex,sizeof(ui.sdoIndex),"0x%04X",index);
    std::snprintf(ui.sdoSub,sizeof(ui.sdoSub),"0x%02X",entry.subindex);
    ui.sdoType=sdoBytesType;
    for(int n=0;n<sdoBytesType;++n)if(entry.type==sdoTypes[n]){ui.sdoType=n;break;}
    // OCTET_STRING and UNICODE_STRING need their own codecs; preserve their
    // bytes rather than interpreting binary or UTF-16 as a visible string.
    if(entry.dataType==0x000a||entry.dataType==0x000b)ui.sdoType=sdoBytesType;
    ui.sdoCapacity=entry.bitLength?int(std::min<uint64_t>(65536,(uint64_t(entry.bitLength)+7)/8)):256;
    ui.localError.clear();
}
const char* sdoSource(bool online,bool esi) {return online?(esi?tr("Online + ESI","在线 + ESI"):tr("Online","在线")):(esi?"ESI":"—");}
const char* sdoAccess(const SdoEntry& entry) {
    if(!entry.accessKnown)return "—";
    const bool read=entry.access&7,write=entry.access&56;
    return read?(write?"R/W":"RO"):(write?"WO":"—");
}
void sdoAccessTooltip(const SdoEntry& entry) {
    if(!ImGui::IsItemHovered())return;
    ImGui::BeginTooltip();
    if(!entry.accessKnown)ImGui::TextUnformatted(tr("Access rights are not described; the slave validates access.","未提供访问权限描述，以从站实际响应为准。"));
    else {
        const char* states[]={"PRE-OP","SAFE-OP","OP"};
        for(unsigned n=0;n<3;++n)ImGui::Text("%s: %s / %s",states[n],(entry.access&(1u<<n))?tr("read","可读"):tr("no read","不可读"),(entry.access&(8u<<n))?tr("write","可写"):tr("no write","不可写"));
    }
    ImGui::EndTooltip();
}
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
    registerRead.observe(s);sdoRead.observe(s);sdoDictionaryView.observeResults(s);
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
    sdoRead.controller=&c;sdoRead.observe(s);const auto* d=selected(s);
    const auto state=d?d->state&0xf:0;const bool coe=d&&(d->mailboxProtocols&4);
    sdoDictionaryView.observe(c,s,d);
    const bool dictionaryEnabled=coe&&d->sdoInfo&&s.connected&&!blocked(s)&&!s.running&&d->state==2;
    if(dictionaryEnabled&&!scriptRunning()&&!d->sdoDictionary&&!sdoDictionaryView.attempted.count(d->position))sdoDictionaryView.request(c,s,d->position);
    const bool enabled=coe&&s.connected&&!blocked(s)&&!s.running&&(state==2||state==4||state==8);
    ImGui::BeginChild("sdo-page",{0,0},ImGuiChildFlags_None,ImGuiWindowFlags_NoScrollbar);
    const float height=std::max(1.0f,ImGui::GetContentRegionAvail().y-2);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding,ImVec2(0,0));
    if(ImGui::BeginTable("sdo-columns",2,ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("dictionary",ImGuiTableColumnFlags_WidthStretch);ImGui::TableSetupColumn("editor",ImGuiTableColumnFlags_WidthFixed,280);ImGui::TableNextColumn();
        beginCard("sdo-object-dictionary",{ImGui::GetContentRegionAvail().x-gutter,height});
        cardHeader(tr("Object dictionary","对象字典"),Icon::Book);
        const float refreshWidth=ImGui::CalcTextSize(tr("Refresh dictionary","刷新字典")).x+ImGui::GetStyle().FramePadding.x*2;
        ImGui::SetNextItemWidth(std::max(50.0f,ImGui::GetContentRegionAvail().x-refreshWidth-ImGui::GetStyle().ItemSpacing.x));
        if(ImGui::InputTextWithHint("##sdo-search",tr("Search name / index","搜索名称 / 索引"),sdoDictionaryView.filter,sizeof(sdoDictionaryView.filter)))sdoDictionaryView.dirty=true;
        ImGui::SameLine();ImGui::BeginDisabled(!dictionaryEnabled||blocked(s));
        if(ImGui::Button(label("Refresh dictionary","刷新字典","sdo-refresh-dictionary").c_str(),{refreshWidth,0}))sdoDictionaryView.request(c,s,d->position);
        ImGui::EndDisabled();
        if(d&&sdoDictionaryView.pending.count(d->position))smallNote("Reading online descriptions…","正在读取在线对象描述…");
        else if(d&&sdoDictionaryView.errors.count(d->position)) {
            ImGui::PushFont(fonts.small);ImGui::TextColored(amber,"%s",tr("Online dictionary unavailable; cached descriptions retained.","在线字典未能读取，保留已获取的描述。"));
            if(ImGui::IsItemHovered()){ImGui::BeginTooltip();ImGui::PushTextWrapPos(360);ImGui::TextUnformatted(trMessage(sdoDictionaryView.errors.at(d->position)).c_str());ImGui::PopTextWrapPos();ImGui::EndTooltip();}ImGui::PopFont();
        } else if(d&&d->sdoDictionary) {
            ImGui::PushFont(fonts.small);
            if(!d->sdoDictionary->complete&&d->sdoDictionary->objects.empty())ImGui::TextColored(amber,"%s",tr("Online dictionary unavailable","在线字典不可用"));
            else ImGui::TextColored(d->sdoDictionary->complete?muted:amber,"%s · %zu %s",d->sdoDictionary->complete?tr("Online dictionary","在线字典"):tr("Partial online dictionary","部分在线字典"),d->sdoDictionary->objects.size(),tr("objects","个对象"));
            if(ImGui::IsItemHovered()&&!d->sdoDictionary->status.empty()){ImGui::BeginTooltip();ImGui::PushTextWrapPos(360);ImGui::TextUnformatted(trMessage(d->sdoDictionary->status).c_str());ImGui::PopTextWrapPos();ImGui::EndTooltip();}ImGui::PopFont();
        } else if(d&&d->esiSdoDictionary)smallNote("ESI descriptions · current values are read from the slave","ESI 对象描述 · 当前值需从从站读取");
        else if(d&&!coe)smallNote("The selected slave does not declare CoE support.","所选从站未声明支持 CoE。");
        else if(d&&!d->sdoInfo)smallNote("No SDO Information support or ESI dictionary is available.","从站未声明支持 SDO Information，且没有 ESI 字典。");
        else if(d&&d->state!=2)smallNote("Enter PRE-OP to read the online dictionary.","切换到 PRE-OP 可读取在线字典。");
        else if(scriptRunning())smallNote("Use Refresh dictionary when the bus is available.","总线可用时，可点击刷新字典。");
        else smallNote("Select a slave in the topology tree.","请在左侧拓扑树中选择从站。");
        ImGui::Spacing();
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding,ImVec2(5,5));
        const auto dictionaryFlags=ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerH|ImGuiTableFlags_Resizable|ImGuiTableFlags_ScrollY|ImGuiTableFlags_ScrollX;
        // Preserve a readable name column in narrow windows. Reserve the
        // vertical scrollbar so it cannot cause horizontal scrolling at the
        // normal window size; only the table scrolls when space is limited.
        const float dictionaryWidth=std::max(650.0f,ImGui::GetContentRegionAvail().x-ImGui::GetStyle().ScrollbarSize);
        if(ImGui::BeginTable("sdo-dictionary-table",7,dictionaryFlags,{0,std::max(1.0f,ImGui::GetContentRegionAvail().y)},dictionaryWidth)) {
            ImGui::TableSetupColumn("Index : Sub",ImGuiTableColumnFlags_WidthFixed,98);
            ImGui::TableSetupColumn(tr("Name","名称"),ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn(tr("Type","类型"),ImGuiTableColumnFlags_WidthFixed,62);
            ImGui::TableSetupColumn(tr("Bits","位长"),ImGuiTableColumnFlags_WidthFixed,32);
            ImGui::TableSetupColumn(tr("Access","权限"),ImGuiTableColumnFlags_WidthFixed,46);
            ImGui::TableSetupColumn(tr("Read cache","读取缓存"),ImGuiTableColumnFlags_WidthFixed,90);
            ImGui::TableSetupColumn(tr("Source","来源"),ImGuiTableColumnFlags_WidthFixed,72);
            ImGui::TableSetupScrollFreeze(0,1);ImGui::TableHeadersRow();
            uint64_t currentIndex=0,currentSub=0;parseUnsigned(ui.sdoIndex,65535,currentIndex);parseUnsigned(ui.sdoSub,255,currentSub);
            ImGuiListClipper clipper;clipper.Begin(int(sdoDictionaryView.rows.size()));
            while(clipper.Step())for(int rowNumber=clipper.DisplayStart;rowNumber<clipper.DisplayEnd;++rowNumber) {
                const auto& row=sdoDictionaryView.rows[size_t(rowNumber)];const auto& object=*row.object;const auto* entry=row.entry;
                ImGui::PushID(object.index);ImGui::PushID(entry?entry->subindex:-1);ImGui::TableNextRow();ImGui::TableNextColumn();
                if(!entry)ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,ImGui::GetColorU32(ImVec4(.92f,.96f,.97f,1)));
                if(row.child)ImGui::SetCursorPosX(ImGui::GetCursorPosX()+12.0f);
                if(entry) {
                    const bool current=currentIndex==object.index&&currentSub==entry->subindex;
                    if(ImGui::Selectable(objectAddress(object.index,entry->subindex).c_str(),current,ImGuiSelectableFlags_SpanAllColumns))selectSdoEntry(object.index,*entry);
                } else {
                    ImGui::TextColored(teal,"%s",hexNumber(object.index,4).c_str());
                }
                ImGui::TableNextColumn();
                if(row.child)ImGui::SetCursorPosX(ImGui::GetCursorPosX()+12.0f);
                ImGui::TextColored(entry?ink:teal,"%s",row.name.empty()?"—":row.name.c_str());
                if(ImGui::IsItemHovered()&&!row.name.empty()){ImGui::BeginTooltip();ImGui::PushTextWrapPos(400);ImGui::TextUnformatted(row.name.c_str());ImGui::PopTextWrapPos();ImGui::EndTooltip();}
                ImGui::TableNextColumn();ImGui::TextUnformatted(entry?(entry->type.empty()?"BYTES":entry->type.c_str()):(object.objectCode==8?"ARRAY":object.objectCode==9?"RECORD":"—"));
                ImGui::TableNextColumn();if(entry&&entry->bitLength)ImGui::Text("%u",entry->bitLength);else ImGui::TextDisabled("—");
                ImGui::TableNextColumn();ImGui::TextUnformatted(entry?sdoAccess(*entry):"—");if(entry)sdoAccessTooltip(*entry);
                ImGui::TableNextColumn();
                if(entry) {
                    if(const auto* value=sdoRead.find(ui.selected,object.index,entry->subindex)) {
                        const auto text=sdoValuePreview(*value);ImGui::TextUnformatted(text.c_str());
                        if(ImGui::IsItemHovered()){ImGui::BeginTooltip();ImGui::PushTextWrapPos(400);ImGui::TextUnformatted(text.c_str());ImGui::PopTextWrapPos();ImGui::EndTooltip();}
                    } else ImGui::TextDisabled("%s",tr("Not read","尚未读取"));
                }
                ImGui::TableNextColumn();const bool online=entry?entry->online:object.online,esi=entry?entry->esi:object.esi;
                ImGui::TextDisabled("%s",sdoSource(online,esi));
                if(!online&&esi&&ImGui::IsItemHovered()){ImGui::BeginTooltip();ImGui::TextUnformatted(tr("ESI description; existence and access are not confirmed online.","ESI 离线描述，对象是否存在及访问权限尚未在线确认。"));ImGui::EndTooltip();}
                ImGui::PopID();ImGui::PopID();
            }
            if(sdoDictionaryView.rows.empty()) {
                ImGui::TableNextRow();ImGui::TableNextColumn();ImGui::TableSetColumnIndex(1);
                ImGui::TextDisabled("%s",tr(sdoDictionaryView.objects.empty()?"No object descriptions available":"No matching objects",sdoDictionaryView.objects.empty()?"没有可用的对象描述":"没有匹配的对象"));
            }
            ImGui::EndTable();
        }
        ImGui::PopStyleVar();endCard();ImGui::TableNextColumn();
        beginCard("sdo-parameter-editor",{0,height});cardHeader(tr("SDO parameter access","SDO 参数访问"),Icon::Book);
        ImGui::BeginChild("sdo-parameter-body",{0,0});
        if(d){ImGui::Text("%02d  %s",d->position,d->name.c_str());}else smallNote("Select a slave in the topology tree.","请在左侧拓扑树中选择从站。");
        ImGui::Spacing();
        if(ImGui::BeginTable("sdo-index-fields",2,ImGuiTableFlags_SizingStretchSame)){ImGui::TableNextColumn();fieldLabel("Index","Index");ImGui::InputText("##sdo-index",ui.sdoIndex,sizeof(ui.sdoIndex));ImGui::TableNextColumn();fieldLabel("SubIndex","SubIndex");ImGui::InputText("##sdo-sub",ui.sdoSub,sizeof(ui.sdoSub));ImGui::EndTable();}
        ImGui::Spacing();fieldLabel("Data type","数据类型");ImGui::SetNextItemWidth(-1);ImGui::Combo("##sdo-type",&ui.sdoType,sdoTypes,15);ImGui::Spacing();
        if(ImGui::BeginTable("sdo-capacity-fields",2,ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("capacity",ImGuiTableColumnFlags_WidthStretch);ImGui::TableSetupColumn("format",ImGuiTableColumnFlags_WidthFixed,68);ImGui::TableNextColumn();fieldLabel("Read capacity (B)","读取容量（B）");ImGui::InputInt("##sdo-capacity",&ui.sdoCapacity,0,0);
            ImGui::TableNextColumn();fieldLabel("Format","格式");
            if(ui.sdoType==sdoBytesType){bool hex=true;ImGui::BeginDisabled();ImGui::Checkbox("HEX##sdo-hex",&hex);ImGui::EndDisabled();}else ImGui::Checkbox("HEX##sdo-hex",&ui.sdoHex);
            ImGui::EndTable();
        }
        ImGui::Spacing();fieldLabel("Write value","写入值");ImGui::InputText("##sdo-value",ui.sdoValue,sizeof(ui.sdoValue));ImGui::Spacing();const float buttonWidth=(ImGui::GetContentRegionAvail().x-ImGui::GetStyle().ItemSpacing.x)*.5f;
        ImGui::BeginDisabled(!enabled);ImGui::PushStyleColor(ImGuiCol_Button,teal);ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(1,1,1,1));const bool read=ImGui::Button(label("Read","读取","sdo-read").c_str(),{buttonWidth,30});ImGui::PopStyleColor(2);ImGui::SameLine();const bool write=ImGui::Button(label("Write","写入","sdo-write").c_str(),{buttonWidth,30});
        if(read||write) {
            uint64_t index=0,sub=0;std::vector<uint8_t> bytes;std::string error;
            if(!parseUnsigned(ui.sdoIndex,65535,index)||!parseUnsigned(ui.sdoSub,255,sub))ui.localError="Invalid Index / SubIndex.";
            else if(read&&(ui.sdoCapacity<1||ui.sdoCapacity>65536))ui.localError="SDO receive capacity must be 1..65536 bytes";
            else if(write&&!(ui.sdoType==sdoBytesType?parseHex(ui.sdoValue,bytes,error):encodeValue(ui.sdoValue,sdoTypes[ui.sdoType],ui.sdoHex,bytes,error)))ui.localError=error;
            else {
                Command cmd;cmd.kind=read?CommandKind::ReadSdo:CommandKind::WriteSdo;cmd.slave=ui.selected;cmd.index=static_cast<uint16_t>(index);cmd.subindex=static_cast<uint8_t>(sub);cmd.length=ui.sdoCapacity;cmd.data=std::move(bytes);
                const auto previous=latestTicket();send(c,s,std::move(cmd));
                if(read&&latestTicket()!=previous)sdoRead.pending.push_back({latestTicket(),ui.selected,(uint32_t(index)<<8)|uint32_t(sub),sdoTypes[ui.sdoType],std::chrono::steady_clock::now()});
                // Values are only observations of acknowledged reads; a write
                // invalidates the previous observation rather than predicting it.
                if(write&&latestTicket()!=previous)sdoRead.values.erase({ui.selected,(uint32_t(index)<<8)|uint32_t(sub)});
            }
        }
        ImGui::EndDisabled();ImGui::Spacing();resultBox("sdo-read-result",95);smallText(tr("READ CACHE · ORIGINAL BYTES","读取缓存 · 原始字节"));
        uint64_t index=0,sub=0;
        const auto* value=parseUnsigned(ui.sdoIndex,65535,index)&&parseUnsigned(ui.sdoSub,255,sub)?sdoRead.find(ui.selected,uint16_t(index),uint8_t(sub)):nullptr;
        if(value) {
            smallText(objectAddress(uint16_t(index),uint8_t(sub)).c_str());monoText(hexBytes(value->bytes));
            if(value->type!="BYTES"){const auto decoded=sdoValueText(*value,ui.sdoHex);ImGui::TextWrapped("%s",value->type=="STRING"?decoded.c_str():trMessage(decoded).c_str());}
        }else smallNote("No read result","尚无读取结果");endResultBox();ImGui::Spacing();
        if(s.running)smallNote("Stop cyclic exchange before synchronous SDO access.","同步访问 SDO 前请先停止周期交换。");
        else if(!coe)smallNote("The selected slave does not declare CoE support.","所选从站未声明支持 CoE。");
        else if(state==1||state==3)smallNote("Mailbox SDO access requires PRE-OP, SAFE-OP or OP.","Mailbox SDO 访问需要 PRE-OP、SAFE-OP 或 OP。");
        else smallNote("Select an object or enter an address. The slave validates read / write access.","选择对象或手动填写地址，读写权限以从站实际响应为准。");
        ImGui::EndChild();endCard();ImGui::EndTable();
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
