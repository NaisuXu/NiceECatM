// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#include "ui/internal.hpp"
#include <imgui_internal.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <vector>

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
std::string trendKey(const Field& field) {
    return std::to_string(field.device->position)+(field.input?"/in/":"/out/")+std::to_string(field.bit)+"/"+std::to_string(field.width);
}
struct Draft {char value[160]="";bool dirty=false,hex=false;uint64_t pending=0;};
struct PdoUi {
    uint64_t generation=~uint64_t{},descriptionRevision=~uint64_t{};
    char search[160]="";
    int direction=0;
    std::string selection;
    std::map<std::string,Draft> drafts;
    std::set<std::string> monitored;
    std::set<std::string> fixedTrendRange;
};
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
void updateTrendSelection(Controller& controller) {
    controller.setTrendSelection(std::vector<std::string>(page.monitored.begin(),page.monitored.end()));
}
void toggleTrend(Controller& controller,const std::string& key) {
    if(!page.monitored.insert(key).second)page.monitored.erase(key);
    updateTrendSelection(controller);
}
std::string trendNumber(float value) {
    char text[32]{};std::snprintf(text,sizeof text,"%.5g",static_cast<double>(value));return text;
}
struct TrendBounds {float minimum=0,maximum=1;bool valid=false;};
TrendBounds typeBounds(const Field& field) {
    if(field.type=="BOOL")return {0,1,true};
    if(field.type=="REAL32"||field.type=="REAL64")return {std::numeric_limits<float>::lowest(),std::numeric_limits<float>::max(),true};
    const bool unsignedType=field.type.compare(0,4,"UINT")==0;
    const bool signedType=field.type.compare(0,3,"INT")==0;
    if(!unsignedType&&!signedType)return {};
    const auto start=field.type.find_first_of("0123456789");
    if(start==std::string::npos)return {};
    const auto width=std::strtoul(field.type.c_str()+start,nullptr,10);
    if(width==0||width>64)return {};
    const long double scale=std::ldexp(1.0L,static_cast<int>(width-(signedType?1:0)));
    if(unsignedType)return {0,static_cast<float>(scale-1),true};
    return {static_cast<float>(-scale),static_cast<float>(scale-1),true};
}
void drawTrend(Controller& controller,const Field& field,const Snapshot& s,float width) {
    const auto key=trendKey(field);const bool monitored=page.monitored.count(key)!=0;
    const bool fixedRange=page.fixedTrendRange.count(key)!=0;
    constexpr float graphHeight=26, controlSize=26, gap=4, rangeWidth=26, rangeHeight=26;
    const auto found=s.trends.find(key);
    static const std::vector<float> emptyValues;
    const auto& values=found==s.trends.end()?emptyValues:found->second;
    TrendBounds bounds=fixedRange?typeBounds(field):TrendBounds{};
    const auto observedBounds=[&]() {
        if(values.empty())return TrendBounds{};
        TrendBounds observed{values.front(),values.front(),true};
        for(const auto value:values){observed.minimum=std::min(observed.minimum,value);observed.maximum=std::max(observed.maximum,value);}
        if(field.type=="BOOL"){observed.minimum=0;observed.maximum=1;}
        else if(observed.maximum-observed.minimum<.000001f){observed.minimum-=.5f;observed.maximum+=.5f;}
        return observed;
    };
    const float span=bounds.maximum-bounds.minimum;
    if(!bounds.valid||!std::isfinite(bounds.minimum)||!std::isfinite(bounds.maximum)||!std::isfinite(span)||span<=0)bounds=observedBounds();
    if(!bounds.valid||!std::isfinite(bounds.minimum)||!std::isfinite(bounds.maximum)||!std::isfinite(bounds.maximum-bounds.minimum)||bounds.maximum<=bounds.minimum)bounds={0,1,true};
    const float minimum=bounds.minimum,maximum=bounds.maximum;
    // During a live window resize the remaining table column can briefly be
    // narrower than the two controls. Keep the plot inside its cell instead
    // of forcing a minimum width that would draw into adjacent columns.
    float cellWidth=width;
    ImVec2 cellStart=ImGui::GetCursorScreenPos();
    float lineY=cellStart.y+4.f;
    if(const auto* table=ImGui::GetCurrentTable()) {
        const int column=ImGui::TableGetColumnIndex();
        if(column>=0&&column<table->ColumnsCount) {
            cellWidth=table->Columns[column].WorkMaxX-table->Columns[column].WorkMinX;
            cellStart.x=table->Columns[column].WorkMinX;
            const auto cellRect=ImGui::TableGetCellBgRect(table,column);
            lineY=cellRect.Min.y+(cellRect.Max.y-cellRect.Min.y-controlSize)*.5f;
        }
    }
    const float graphWidth=std::max(1.f,cellWidth-controlSize-gap-rangeWidth-gap);
    ImGui::SetCursorScreenPos({cellStart.x,lineY});
    if(monitored) {
        if(iconButton("trend-stop",Icon::Stop,tr("Stop capturing this signal","停止采集此信号"),{controlSize,controlSize}))toggleTrend(controller,key);
    } else if(iconButton("trend-monitor",Icon::Play,tr("Capture every valid I/O cycle","采集每个有效 I/O 周期"),{controlSize,controlSize}))toggleTrend(controller,key);
    ImGui::SetCursorScreenPos({cellStart.x+controlSize+gap,lineY+(controlSize-rangeHeight)*.5f});
    if(fixedRange)ImGui::PushStyleColor(ImGuiCol_Button,{teal.x,teal.y,teal.z,.18f});
    const bool rangeClicked=iconButton("trend-range",Icon::Range,fixedRange?tr("Use data min/max for the waveform","使用数据最小值/最大值作为波形范围"):tr("Use signal type limits for the waveform","使用信号类型边界作为波形范围"),{rangeWidth,rangeHeight});
    if(fixedRange)ImGui::PopStyleColor();
    if(rangeClicked){if(!page.fixedTrendRange.insert(key).second)page.fixedTrendRange.erase(key);}
    if(!monitored)return;
    ImGui::SetCursorScreenPos({cellStart.x+controlSize+gap+rangeWidth+gap,lineY+(controlSize-graphHeight)*.5f});
    const auto start=ImGui::GetCursorScreenPos();auto* draw=ImGui::GetWindowDrawList();
    ImGui::Dummy({graphWidth,graphHeight});
    if(!values.empty()) {
        const double lower=bounds.minimum,upper=bounds.maximum;
        const double span=upper-lower;
        const auto y=[&](float value) {
            double normalized=span>0.0&&std::isfinite(span)?(static_cast<double>(value)-lower)/span:0.5;
            if(!std::isfinite(normalized))normalized=normalized<0.0?0.0:1.0;
            normalized=std::clamp(normalized,0.0,1.0);
            return start.y+graphHeight-static_cast<float>(normalized*graphHeight);
        };
        const auto color=ImGui::GetColorU32(field.input?teal:amber);
        // The right edge is always the current end of a fixed 2000-sample
        // window. New captures occupy the rightmost samples, keeping the time
        // axis aligned when monitoring is started at different times.
        constexpr size_t capacity=2000;
        const size_t sampleCount=std::min(values.size(),capacity);
        const size_t firstIndex=values.size()-sampleCount;
        const float denominator=static_cast<float>(capacity-1);
        const float firstX=start.x+static_cast<float>(capacity-sampleCount)*graphWidth/denominator;
        for(size_t n=1;n<sampleCount;++n) {
            const float x1=firstX+(n-1)*graphWidth/denominator,x2=firstX+n*graphWidth/denominator;
            const float y1=y(values[firstIndex+n-1]),y2=y(values[firstIndex+n]);
            if(field.type=="BOOL") {
                draw->AddLine({x1,y1},{x2,y1},color,1.5f);
                draw->AddLine({x2,y1},{x2,y2},color,1.5f);
            } else draw->AddLine({x1,y1},{x2,y2},color,1.5f);
        }
        const auto minText=std::string(tr("MIN ","下限 "))+trendNumber(minimum);
        const auto maxText=std::string(tr("MAX ","上限 "))+trendNumber(maximum);
        const auto labelColor=ImGui::GetColorU32(ImVec4{muted.x,muted.y,muted.z,.9f});
        draw->AddText(fonts.small,10,{start.x+3,start.y+1},labelColor,maxText.c_str());
        draw->AddText(fonts.small,10,{start.x+3,start.y+11},labelColor,minText.c_str());
    }
    if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",field.input?tr("Actual valid input samples","实际有效输入采样"):tr("Sampled local output commands; not physical feedback","采样的本地输出命令；不代表物理反馈"));
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
        ImGui::TableSetupColumn(tr("CURRENT VALUE","当前值"),ImGuiTableColumnFlags_WidthFixed,width<630?119.f:137.f);
        ImGui::TableSetupColumn(tr("ACTION","操作"),ImGuiTableColumnFlags_WidthFixed,68);
        if(showTrend)ImGui::TableSetupColumn(tr("RECENT TREND","最近趋势"),ImGuiTableColumnFlags_WidthStretch,1.f);
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
            ImGui::TableSetColumnIndex(column++);ImGui::PushFont(fonts.mono,14);ImGui::TextUnformatted(f.type.c_str());ImGui::PopFont();
            ImGui::TableSetColumnIndex(column++);bool enter=false;
            if(f.type=="BOOL") {
                const bool on=readBits(fieldBytes(f),0,1)!=0;ImGui::TextColored(on?teal:muted,"%s",on?"1 · TRUE":"0 · FALSE");
                ImGui::PushFont(fonts.small,10);ImGui::TextDisabled("%s",f.padding?tr("Reserved padding","保留填充位"):(f.input?(liveInput(s)?tr("Valid input","有效输入"):tr("Cached input","缓存输入")):tr("Command image","命令映像")));ImGui::PopFont();
            } else if(f.input||f.padding) {
                const auto valueStart=ImGui::GetCursorScreenPos();
                const float formatX=valueStart.x+ImGui::GetContentRegionAvail().x-36.f;
                ImGui::PushFont(fonts.mono,14);ImGui::TextColored(ink,"%s",current.c_str());ImGui::PopFont();
                ImGui::SetCursorScreenPos({formatX,valueStart.y});formatToggle(f,draft);
                ImGui::SetCursorScreenPos({valueStart.x,valueStart.y+ImGui::GetFrameHeight()});
                ImGui::PushFont(fonts.small,10);if(!f.unit.empty())ImGui::TextDisabled("%s",f.unit.c_str());else ImGui::TextDisabled("%s",f.padding?tr("Reserved padding","保留填充位"):(liveInput(s)?tr("Valid input","有效输入"):tr("Cached input","缓存输入")));ImGui::PopFont();
            } else {
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,{5,3});ImGui::SetNextItemWidth(std::max(60.f,ImGui::GetContentRegionAvail().x-40));
                enter=ImGui::InputText("##value",draft.value,sizeof draft.value,ImGuiInputTextFlags_EnterReturnsTrue);
                if(ImGui::IsItemEdited())draft.dirty=true;
                if((ImGui::IsItemActive()||ImGui::IsItemDeactivated())&&ImGui::IsKeyPressed(ImGuiKey_Escape)){draft.dirty=false;std::snprintf(draft.value,sizeof draft.value,"%s",current.c_str());}
                ImGui::SameLine(0,4);formatToggle(f,draft);ImGui::PopStyleVar();
                ImGui::PushFont(fonts.small,10);ImGui::TextDisabled("%s %s%s%s",tr("Now","当前"),current.c_str(),f.unit.empty()?"":" ",f.unit.c_str());ImGui::PopFont();
            }
            ImGui::TableSetColumnIndex(column++);
            const auto actionStart=ImGui::GetCursorScreenPos();
            if(f.input||f.padding) {
                ImGui::SetCursorScreenPos({actionStart.x,actionStart.y+9.f});
                ImGui::TextDisabled("%s",f.padding?tr("Padding","填充位"):tr("Read only","只读"));
            } else {
                ImGui::SetCursorScreenPos({actionStart.x,actionStart.y+4.f});
                ImGui::BeginDisabled(!writable(s,*f.device));const bool on=f.type=="BOOL"&&readBits(fieldBytes(f),0,1)!=0;
                const bool click=ImGui::Button(f.type=="BOOL"?(on?tr("Set 0","置 0"):tr("Set 1","置 1")):tr("Write","写入"),{47,26});
                if(click||enter)sendField(controller,s,f,f.type=="BOOL"?(on?"0":"1"):draft.value,draft.hex,&draft);
                ImGui::EndDisabled();
            }
            if(showTrend){ImGui::TableSetColumnIndex(column++);drawTrend(controller,f,s,std::max(60.f,ImGui::GetContentRegionAvail().x));}
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
    if(page.generation!=s.generation||page.descriptionRevision!=s.descriptionRevision){
        const bool newGeneration=page.generation!=s.generation;
        page.generation=s.generation;page.descriptionRevision=s.descriptionRevision;page.selection.clear();page.drafts.clear();
        if(newGeneration&&!page.monitored.empty()){page.monitored.clear();updateTrendSelection(controller);}
        if(newGeneration)page.fixedTrendRange.clear();
    }
    for(auto& entry:page.drafts)if(entry.second.pending&&s.completed==entry.second.pending){if(s.result.ok)entry.second.dirty=false;entry.second.pending=0;}
    const auto fields=fieldsFor(s);
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
