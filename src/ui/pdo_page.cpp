// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#include "ui/internal.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>

namespace niceecatm::workbench {
namespace {
struct Field {
    const Slave* device=nullptr;
    bool input=false;
    unsigned bit=0,width=8;
    std::string type="UINT8",english,chinese,index,unit,source;
    bool demo=false,raw=false,padding=false;
    std::string name()const {return ui.chinese?chinese:english;}
    std::string key()const {return std::to_string(device->position)+(input?"/in/":"/out/")+std::to_string(bit)+"/"+std::to_string(width)+"/"+type+"/"+index+(padding?"/padding":raw?"/raw":"/field");}
};
struct Draft {char value[160]="";bool dirty=false,hex=false;uint64_t pending=0;};
struct PdoUi {
    uint64_t generation=~uint64_t{},descriptionRevision=~uint64_t{},sampleCycle=~uint64_t{};
    char search[160]="";
    int direction=0;
    std::string selection;
    std::map<std::string,Draft> drafts;
    std::map<std::string,std::deque<float>> history;
};
constexpr size_t kTrendHistorySamples=200;
PdoUi page;
std::string lower(std::string value) {
    for(char& ch:value)if(static_cast<unsigned char>(ch)<128)ch=static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return value;
}
bool liveInput(const Snapshot& s) {return s.running&&s.cycles>0&&s.expectedWkc>0&&s.wkc>=s.expectedWkc;}
bool writable(const Snapshot& s,const Slave& d) {
    const auto state=d.state&0xf;
    return !blocked(s)&&s.connected&&s.mapped&&!d.outputs.empty()&&(state==2||state==4||state==8);
}
bool fits(const Field& f) {
    const auto& data=f.input?f.device->inputs:f.device->outputs;
    return f.width>0&&f.width<=64&&uint64_t(f.bit)+f.width<=data.size()*8&&uint64_t(f.bit)+f.width<=(f.input?f.device->inputBits:f.device->outputBits);
}
std::vector<uint8_t> fieldBytes(const Field& f) {
    if(!fits(f))return {};
    auto raw=readBits(f.input?f.device->inputs:f.device->outputs,f.bit,f.width);
    std::vector<uint8_t> data((f.width+7)/8);
    for(size_t n=0;n<data.size();++n)data[n]=static_cast<uint8_t>(raw>>(n*8));
    return data;
}
std::string valueOf(const Field& f,bool hex=false) {return fits(f)?decodeValue(fieldBytes(f),f.type,hex):"—";}
std::vector<Field> fieldsFor(const Snapshot& s) {
    std::vector<Field> result;
    for(const auto& d:s.slaves) {
        const auto add=[&](bool input,unsigned bit,const char* type,const std::string& en,const std::string& zh,const std::string& index="",const std::string& unit="") {
            Field f;f.device=&d;f.input=input;f.bit=bit;f.type=type;f.width=widthFor(type);f.english=en;f.chinese=zh;f.index=index;f.unit=unit;f.demo=s.simulation;
            if(fits(f))result.push_back(std::move(f));
        };
        if(s.simulation&&d.position==2&&d.inputBits==8) {
            const char* en[]={"Start button","Stop button","Position sensor","Part detection","Door feedback","Air pressure","Spare input 7","Spare input 8"};
            const char* zh[]={"启动按钮","停止按钮","到位传感器","工件检测","门禁反馈","气压反馈","备用输入 7","备用输入 8"};
            for(unsigned bit=0;bit<8;++bit)add(true,bit,"BOOL",en[bit],zh[bit],hexNumber(0x6000+bit*0x10,4)+":01");
        } else if(s.simulation&&d.position==3&&d.outputBits==8) {
            const char* en[]={"Run indicator","Alarm indicator","Solenoid A","Solenoid B","Spare output 5","Spare output 6","Spare output 7","Spare output 8"};
            const char* zh[]={"运行指示灯","报警指示灯","电磁阀 A","电磁阀 B","备用输出 5","备用输出 6","备用输出 7","备用输出 8"};
            for(unsigned bit=0;bit<8;++bit)add(false,bit,"BOOL",en[bit],zh[bit],hexNumber(0x7000+bit*0x10,4)+":01");
        } else if(s.simulation&&d.position==4&&d.inputBits==40) {
            add(true,0,"UINT8","Input status","输入状态");
            add(true,8,"INT16","Analog input / Channel 1","模拟输入 / 通道 1","0x6000:11","count");
            add(true,24,"INT16","Analog input / Channel 2","模拟输入 / 通道 2","0x6010:11","count");
        } else if(s.simulation&&d.position==5&&d.outputBits==152) {
            const char* en[]={"Speed setpoint","Current limit","Target position","Cycle counter","Voltage setpoint","Operating mode","Control bits","12-bit setpoint","Enable flag"};
            const char* zh[]={"速度设定","电流限值","目标位置","循环计数","电压设定","运行模式","控制位段","12 位设定","使能标志"};
            const char* type[]={"INT16","UINT16","INT32","UINT32","REAL32","UINT8","UINT3","UINT12","BOOL"};
            const char* unit[]={"rpm","mA","count","","V","","","",""};
            const unsigned bits[]={0,16,32,64,96,128,136,139,151};
            for(unsigned n=0;n<9;++n)add(false,bits[n],type[n],en[n],zh[n],"0x7100:"+hexNumber(n+1,2).substr(2),unit[n]);
        } else for(bool input:{true,false}) {
            const auto bits=std::min<unsigned>(input?d.inputBits:d.outputBits,static_cast<unsigned>((input?d.inputs:d.outputs).size()*8));
            const auto addRaw=[&](unsigned bit,unsigned count,const PdoEntry* entry=nullptr) {
                const auto end=bit+count;
                while(bit<end) {
                    const unsigned width=std::min(8u-bit%8,end-bit);
                    const auto type=width==1?std::string("BOOL"):"UINT"+std::to_string(width);
                    const auto number=std::to_string(width==8?bit/8:bit);
                    const auto suffix=width>1&&width<8?"–"+std::to_string(bit+width-1):"";
                    auto en=std::string(input?(width==8?"Input byte ":"Input bit "):(width==8?"Output byte ":"Output bit "))+number+suffix;
                    auto zh=std::string(input?(width==8?"输入字节 ":"输入位 "):(width==8?"输出字节 ":"输出位 "))+number+suffix;
                    if(entry&&entry->padding){en="Padding bit "+std::to_string(bit)+(width>1?"–"+std::to_string(bit+width-1):"");zh="填充位 "+std::to_string(bit)+(width>1?"–"+std::to_string(bit+width-1):"");}
                    if(entry&&!entry->padding) {
                        const auto nameEn=pdoEntryName(*entry,false),nameZh=pdoEntryName(*entry,true);
                        if(!nameEn.empty())en=nameEn+" · "+en;
                        if(!nameZh.empty())zh=nameZh+" · "+zh;
                    }
                    const auto address=entry&&entry->index?hexNumber(entry->index,4)+":"+hexNumber(entry->subindex,2).substr(2):"";
                    const auto previous=result.size();add(input,bit,type.c_str(),en,zh,address);
                    if(result.size()!=previous) {auto& f=result.back();f.raw=true;if(entry){f.source=entry->source;f.padding=entry->padding;}}
                    bit+=width;
                }
            };
            std::vector<const PdoEntry*> entries;
            bool verified=s.mapped&&(input?d.inputLayoutKnown:d.outputLayoutKnown);
            if(verified)for(const auto& entry:d.pdoEntries)if(entry.input==input)entries.push_back(&entry);
            std::sort(entries.begin(),entries.end(),[](const auto* a,const auto* b){return a->bitOffset<b->bitOffset;});
            uint64_t end=0;
            for(const auto* entry:entries) {
                if(!entry->bitLength||entry->bitOffset<end||uint64_t(entry->bitOffset)+entry->bitLength>bits){verified=false;break;}
                end=uint64_t(entry->bitOffset)+entry->bitLength;
            }
            if(!verified){addRaw(0,bits);continue;}
            unsigned bit=0;
            for(const auto* entry:entries) {
                if(bit<entry->bitOffset)addRaw(bit,entry->bitOffset-bit);
                const auto type=pdoType(entry->type,entry->bitLength);
                if(!entry->padding&&!type.empty()&&entry->bitLength<=64&&widthFor(type)==entry->bitLength) {
                    const auto address=entry->index?hexNumber(entry->index,4)+":"+hexNumber(entry->subindex,2).substr(2):"";
                    auto en=pdoEntryName(*entry,false),zh=pdoEntryName(*entry,true);
                    if(en.empty())en=address.empty()?"Unnamed signal":address;
                    if(zh.empty())zh=address.empty()?"未命名信号":address;
                    const auto previous=result.size();add(input,entry->bitOffset,type.c_str(),en,zh,address);
                    if(result.size()!=previous)result.back().source=entry->source;
                } else addRaw(entry->bitOffset,entry->bitLength,entry);
                bit=entry->bitOffset+entry->bitLength;
            }
            if(bit<bits)addRaw(bit,bits-bit);
        }
    }
    return result;
}
void sendField(Controller& controller,const Snapshot& s,const Field& field,const char* value,bool hex,Draft* draft=nullptr) {
    if(field.input||field.padding||!writable(s,*field.device))return;
    std::vector<uint8_t> encoded;std::string error;auto candidate=field.device->outputs;
    if(!fits(field))ui.localError=tr("The field is outside this process image.","字段超出过程映像范围。");
    else if(!encodeValue(value,field.type,hex,encoded,error)||!writeBits(candidate,field.bit,field.width,readBits(encoded,0,field.width),error))ui.localError=error;
    else {
        Command cmd;cmd.kind=CommandKind::WriteOutput;cmd.slave=field.device->position;cmd.offset=0;cmd.data=std::move(candidate);
        send(controller,s,std::move(cmd));
        if(draft&&ui.awaiting&&!ui.requests.empty())draft->pending=*ui.requests.rbegin();
    }
}
void small(const char* text,ImVec4 color=muted) {
    ImGui::PushFont(fonts.small,11);ImGui::TextColored(color,"%s",text);ImGui::PopFont();
}
void detailLine(const char* en,const char* zh,const std::string& value) {
    ImGui::PushFont(fonts.small,11);ImGui::TextColored(muted,"%s",tr(en,zh));ImGui::PopFont();
    ImGui::TextWrapped("%s",value.c_str());ImGui::Spacing();
}
void drawTrend(const std::string& key,bool input,const Snapshot& s,float width) {
    const auto found=page.history.find(key);
    // Keep the last valid trace visible while the bus is stopped or a WKC
    // failure makes the current snapshot stale. History is only appended
    // from live snapshots below, so rendering it here cannot invent data or
    // make the waveform blink during a communication fault.
    if(found==page.history.end()||found->second.size()<2){ImGui::TextDisabled("—");return;}
    const bool live=liveInput(s);
    const auto& values=found->second;const auto bounds=std::minmax_element(values.begin(),values.end());float minimum=*bounds.first,maximum=*bounds.second;
    if(maximum-minimum<.001f){minimum-=.5f;maximum+=.5f;}
    const auto start=ImGui::GetCursorScreenPos();const float height=25;auto* draw=ImGui::GetWindowDrawList();
    for(size_t n=1;n<values.size();++n) {
        const float x1=start.x+(n-1)*width/static_cast<float>(values.size()-1),x2=start.x+n*width/static_cast<float>(values.size()-1);
        const float y1=start.y+height-(values[n-1]-minimum)/(maximum-minimum)*height,y2=start.y+height-(values[n]-minimum)/(maximum-minimum)*height;
        draw->AddLine({x1,y1},{x2,y2},ImGui::GetColorU32(input?teal:amber),1.5f);
    }
    ImGui::Dummy({width,height});if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",live?(input?tr("Actual valid input snapshots","实际有效输入快照"):tr("Sampled local output commands; not physical feedback","采样的本地输出命令；不代表物理反馈")):(input?tr("Last valid input snapshots; bus stopped or stale","最后有效输入快照；总线已停止或数据已过期"):tr("Last sampled local output commands; bus stopped or stale","最后采样的本地输出命令；总线已停止或数据已过期")));
}
void formatToggle(const Field& f,Draft& draft) {
    ImGui::BeginDisabled(f.type.compare(0,4,"REAL")==0);
    if(ImGui::SmallButton(draft.hex?"HEX##format":"DEC##format")) {
        if(draft.dirty) {
            std::vector<uint8_t> data;std::string error;
            if(encodeValue(draft.value,f.type,draft.hex,data,error)){draft.hex=!draft.hex;std::snprintf(draft.value,sizeof draft.value,"%s",decodeValue(data,f.type,draft.hex).c_str());}
            else ui.localError=error;
        } else {draft.hex=!draft.hex;std::snprintf(draft.value,sizeof draft.value,"%s",valueOf(f,draft.hex).c_str());}
    }
    ImGui::EndDisabled();
}
void signalTable(Controller& controller,const Snapshot& s,const std::vector<Field>& fields,const std::vector<size_t>& visible) {
    const float width=ImGui::GetContentRegionAvail().x;const bool showTrend=width>=660,showDevice=width>=550;
    // Keep the signal label at the default-window width.  Extra room at
    // larger window sizes is assigned to the trend plot instead.
    constexpr float signalWidth=155.f;
    const int count=4+(showTrend?1:0)+(showDevice?1:0);const float height=std::max(80.f,ImGui::GetContentRegionAvail().y);
    const ImVec2 tableOrigin=ImGui::GetCursorPos();
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding,{10,9});
    if(ImGui::BeginTable("signal-table",count,ImGuiTableFlags_ScrollY|ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerH|ImGuiTableFlags_SizingStretchProp,{0,height})) {
        ImGui::TableSetupColumn(tr("SIGNAL NAME","信号名称"),ImGuiTableColumnFlags_WidthFixed,signalWidth);
        if(showDevice)ImGui::TableSetupColumn(tr("DEVICE","设备"),ImGuiTableColumnFlags_WidthFixed,110);
        ImGui::TableSetupColumn(tr("TYPE","类型"),ImGuiTableColumnFlags_WidthFixed,57);
        ImGui::TableSetupColumn(tr("CURRENT VALUE","当前值"),ImGuiTableColumnFlags_WidthFixed,width<630?137.f:155.f);
        if(showTrend)ImGui::TableSetupColumn(tr("RECENT TREND","最近趋势"),ImGuiTableColumnFlags_WidthStretch,1.f);
        ImGui::TableSetupColumn(tr("ACTION","操作"),ImGuiTableColumnFlags_WidthFixed,68);
        ImGui::TableSetupScrollFreeze(0,1);ImGui::PushFont(fonts.small,10);ImGui::TableHeadersRow();ImGui::PopFont();
        ImGuiListClipper clipper;clipper.Begin(static_cast<int>(visible.size()),53);
        while(clipper.Step())for(int n=clipper.DisplayStart;n<clipper.DisplayEnd;++n) {
            const auto& f=fields[visible[static_cast<size_t>(n)]];const auto key=f.key();auto& draft=page.drafts[key];const auto current=valueOf(f,draft.hex);
            if(!draft.dirty)std::snprintf(draft.value,sizeof draft.value,"%s",current.c_str());
            ImGui::PushID(key.c_str());ImGui::TableNextRow(ImGuiTableRowFlags_None,53);
            if(page.selection==key)ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,ImGui::GetColorU32({.925f,.974f,.973f,1}));
            int column=0;ImGui::TableSetColumnIndex(column++);const auto start=ImGui::GetCursorScreenPos();
            if(ImGui::Selectable("##select",page.selection==key,ImGuiSelectableFlags_SpanAllColumns|ImGuiSelectableFlags_AllowOverlap,{0,35})){page.selection=key;ui.selected=f.device->position;}
            ImGui::SetCursorScreenPos(start);ImGui::TextUnformatted(f.name().c_str());
            if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",f.name().c_str());
            ImGui::PushFont(fonts.small,10);ImGui::TextColored(f.input?teal:amber,"%s",f.input?"IN · TxPDO":"OUT · RxPDO");
            if(!f.index.empty()){ImGui::SameLine(0,7);ImGui::TextDisabled("%s",f.index.c_str());}ImGui::PopFont();
            if(showDevice){ImGui::TableSetColumnIndex(column++);ImGui::TextUnformatted(f.device->name.c_str());ImGui::PushFont(fonts.small,10);ImGui::TextDisabled("%s %02d",tr("Slave","从站"),f.device->position);ImGui::PopFont();}
            ImGui::TableSetColumnIndex(column++);ImGui::PushFont(fonts.mono,11);ImGui::TextUnformatted(f.type.c_str());ImGui::PopFont();
            ImGui::TableSetColumnIndex(column++);bool enter=false;
            if(f.type=="BOOL") {
                const bool on=readBits(fieldBytes(f),0,1)!=0;ImGui::TextColored(on?teal:muted,"%s",on?"1 · TRUE":"0 · FALSE");
                ImGui::PushFont(fonts.small,10);ImGui::TextDisabled("%s",f.padding?tr("Reserved padding","保留填充位"):(f.input?(liveInput(s)?tr("Valid input","有效输入"):tr("Cached input","缓存输入")):tr("Command image","命令映像")));ImGui::PopFont();
            } else if(f.input||f.padding) {
                ImGui::PushFont(fonts.mono,14);ImGui::TextColored(ink,"%s",current.c_str());ImGui::PopFont();
                ImGui::SameLine(0,6);formatToggle(f,draft);
                ImGui::PushFont(fonts.small,10);if(!f.unit.empty())ImGui::TextDisabled("%s",f.unit.c_str());else ImGui::TextDisabled("%s",f.padding?tr("Reserved padding","保留填充位"):(liveInput(s)?tr("Valid input","有效输入"):tr("Cached input","缓存输入")));ImGui::PopFont();
            } else {
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,{5,3});ImGui::SetNextItemWidth(std::max(60.f,ImGui::GetContentRegionAvail().x-40));
                enter=ImGui::InputText("##value",draft.value,sizeof draft.value,ImGuiInputTextFlags_EnterReturnsTrue);
                if(ImGui::IsItemEdited())draft.dirty=true;
                if((ImGui::IsItemActive()||ImGui::IsItemDeactivated())&&ImGui::IsKeyPressed(ImGuiKey_Escape)){draft.dirty=false;std::snprintf(draft.value,sizeof draft.value,"%s",current.c_str());}
                ImGui::SameLine(0,4);formatToggle(f,draft);ImGui::PopStyleVar();
                ImGui::PushFont(fonts.small,10);ImGui::TextDisabled("%s %s%s%s",tr("Now","当前"),current.c_str(),f.unit.empty()?"":" ",f.unit.c_str());ImGui::PopFont();
            }
            if(showTrend){ImGui::TableSetColumnIndex(column++);drawTrend(key,f.input,s,std::max(60.f,ImGui::GetContentRegionAvail().x));}
            ImGui::TableSetColumnIndex(column++);
            if(f.input||f.padding)ImGui::TextDisabled("%s",f.padding?tr("Padding","填充位"):tr("Read only","只读"));
            else {
                ImGui::BeginDisabled(!writable(s,*f.device));const bool on=f.type=="BOOL"&&readBits(fieldBytes(f),0,1)!=0;
                const bool click=ImGui::Button(f.type=="BOOL"?(on?tr("Set 0","置 0"):tr("Set 1","置 1")):tr("Write","写入"),{47,26});
                if(click||enter)sendField(controller,s,f,f.type=="BOOL"?(on?"0":"1"):draft.value,draft.hex,&draft);
                ImGui::EndDisabled();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    if(visible.empty()){
        // Keep the empty-state message at the top-left of the table body.
        ImGui::SetCursorPos({tableOrigin.x,tableOrigin.y+34.f});
        ImGui::TextDisabled("%s",tr("No matching signals","没有匹配的信号"));
    }
}
void inspector(const Snapshot& s,const Field* field) {
    cardHeader(tr("Signal details","信号详情"),Icon::Pulse);
    if(!field){note("Select a signal to inspect its value and process-image location.","选择信号，查看数值与过程映像位置。");return;}
    const auto& f=*field;const auto& device=*f.device;auto& draft=page.drafts[f.key()];char deviceText[256];std::snprintf(deviceText,sizeof deviceText,"%02d · %s",device.position,device.name.c_str());small(deviceText);
    ImGui::PushFont(fonts.title,17);ImGui::TextWrapped("%s",f.name().c_str());ImGui::PopFont();ImGui::Spacing();
    pill(f.input?"IN · TxPDO":"OUT · RxPDO",f.input?teal:amber);ImGui::SameLine(0,7);pill(f.type.c_str(),muted);ImGui::Spacing();
    ImGui::PushFont(fonts.value,30);ImGui::TextColored(teal,"%s",valueOf(f,draft.hex).c_str());ImGui::PopFont();if(!f.unit.empty()){ImGui::SameLine(0,7);small(f.unit.c_str());}
    small(f.padding?tr("Reserved padding · read only","保留填充位 · 只读"):(f.input?(liveInput(s)?tr("Valid input snapshot","有效输入快照"):tr("Initial / last valid input","初始 / 最后有效输入")):tr("Local output command · not feedback","本地输出命令 · 非反馈")));
    ImGui::Spacing();ImGui::TextDisabled("%s",tr("ACTUAL AL STATE","实际 AL 状态"));ImGui::SameLine();stateBadge(device.state,device.alCode);ImGui::Spacing();ImGui::Separator();ImGui::Spacing();
    if(!f.index.empty())detailLine("Object index / subindex","对象索引 / 子索引",f.index);
    detailLine("Device-local buffer offset","设备 Buffer 内偏移",std::to_string(f.bit/8)+" B + "+std::to_string(f.bit%8)+" bit");
    const int mapOffset=f.input?device.inputOffset:device.outputOffset;const unsigned startBit=f.input?device.inputStartBit:device.outputStartBit;
    detailLine("SOEM IOmap offset","SOEM IOmap 偏移",mapOffset<0?tr("Not mapped","未映射"):std::to_string(mapOffset+(startBit+f.bit)/8)+" B + "+std::to_string((startBit+f.bit)%8)+" bit");
    detailLine("Width / byte order","位宽 / 字节序",std::to_string(f.width)+tr(" bit · Little-endian"," 位 · 小端序"));detailLine("Raw field bytes","字段原始字节",hexBytes(fieldBytes(f)));
    if(!f.source.empty())detailLine("Signal description source","信号描述来源",f.source);
    if(!device.esiFile.empty())detailLine("ESI file","ESI 文件",std::filesystem::u8path(device.esiFile).filename().u8string());
    ImGui::PushFont(fonts.small,11);ImGui::TextWrapped("%s",f.demo?tr("Source: process image snapshot. Object indices are optional metadata.","来源：过程映像快照。对象索引为可选元数据。"):(f.raw?tr("Raw process-image field. No signal type is inferred for these bits.","原始过程映像字段，未推测这些位的信号类型。"):tr("Signal description matched to the current PDO layout; values come from the process image.","信号描述已匹配当前 PDO 布局，数值来自过程映像。")));ImGui::PopFont();
}
} // namespace
void bufferPage(Controller& controller,const Snapshot& s) {
    if(page.generation!=s.generation||page.descriptionRevision!=s.descriptionRevision){page.generation=s.generation;page.descriptionRevision=s.descriptionRevision;page.sampleCycle=~uint64_t{};page.selection.clear();page.drafts.clear();page.history.clear();}
    for(auto& entry:page.drafts)if(entry.second.pending&&s.completed==entry.second.pending){if(s.result.ok)entry.second.dirty=false;entry.second.pending=0;}
    const auto fields=fieldsFor(s);
    if(liveInput(s)&&page.sampleCycle!=s.cycles) {
        page.sampleCycle=s.cycles;
        for(const auto& field:fields) {
            const auto decoded=valueOf(field);char* end=nullptr;double value=0;
            if(field.type=="BOOL")value=readBits(fieldBytes(field),0,1);
            else {value=std::strtod(decoded.c_str(),&end);if(end==decoded.c_str()||*end||!std::isfinite(value))continue;}
            const float sample=static_cast<float>(value);if(!std::isfinite(sample))continue;
            auto& history=page.history[field.key()];history.push_back(sample);if(history.size()>kTrendHistorySamples)history.pop_front();
        }
    }
    std::vector<size_t> visible;const auto query=lower(page.search);
    for(size_t n=0;n<fields.size();++n) {
        const auto& field=fields[n];if(field.device->position!=ui.selected)continue;if(page.direction&&(field.input!=(page.direction==1)))continue;
        const auto text=lower(field.name()+" "+field.device->name+" "+std::to_string(field.device->position)+" "+field.type+" "+field.index);
        if(!query.empty()&&text.find(query)==std::string::npos)continue;
        visible.push_back(n);
    }
    const Field* selectedField=nullptr;for(const auto& field:fields)if(field.key()==page.selection)selectedField=&field;
    if(!selectedField||selectedField->device->position!=ui.selected) {
        selectedField=nullptr;for(size_t n:visible)if(fields[n].device->position==ui.selected){selectedField=&fields[n];break;}
        if(!selectedField&&!visible.empty())selectedField=&fields[visible.front()];
        page.selection=selectedField?selectedField->key():"";
    }
    const ImVec2 available=ImGui::GetContentRegionAvail();const float rightWidth=260,gap=14;
    beginCard("signal-card",{std::max(300.f,available.x-rightWidth-gap),available.y},{12,12});const float filterWidth=ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(std::max(130.f,filterWidth-155));ImGui::InputTextWithHint("##signal-search",tr("Search signals, devices or index…","搜索信号、设备或索引…"),page.search,sizeof page.search);
    ImGui::SameLine(0,8);ImGui::SetNextItemWidth(145);ImGui::Combo("##signal-direction",&page.direction,tr("All directions\0Input · TxPDO\0Output · RxPDO\0","全部方向\0输入 · TxPDO\0输出 · RxPDO\0"));
    ImGui::Spacing();signalTable(controller,s,fields,visible);endCard();ImGui::SameLine(0,gap);
    selectedField=nullptr;for(const auto& field:fields)if(field.key()==page.selection)selectedField=&field;
    beginCard("signal-inspector",{0,available.y},{15,13});inspector(s,selectedField);endCard();
}
} // namespace niceecatm::workbench
