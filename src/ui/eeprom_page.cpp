// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#include "ui/internal.hpp"
#include "esi/sii_image.hpp"
#include "platform/paths.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>

namespace niceecatm::workbench {
namespace {
enum class FileAction { OpenEsi, OpenBin, ExportBin, ExportEsi, SaveRead };
struct FileSelection {
    FileAction action;
    std::string path,error;
    std::vector<uint8_t> bytes;
};
// SDL may return a native dialog result on another thread, including after the
// page was closed. The callback owns a shared mailbox and never touches ImGui,
// the Controller, or UI drafts. Only the GUI thread drains the mailbox.
struct DialogMailbox {std::mutex mutex;std::deque<FileSelection> results;};
struct DialogRequest {std::shared_ptr<DialogMailbox> mailbox;FileSelection result;std::string location;};
struct Confirmation {
    uint64_t generation=0;
    int slave=0;
    uint32_t vendor=0,product=0,revision=0;
    bool identityKnown=false,sizeKnown=false;
    std::string name,interface,file;
    sii::Image image;
    int capacity=2048;
    uint32_t capacityLimit=131072;
};
struct EepromUi {
    std::shared_ptr<DialogMailbox> mailbox=std::make_shared<DialogMailbox>();
    bool dialog=false,cancelRequested=false,reading=false,programConfirm=false,readConfirm=false;
    std::filesystem::path source;
    std::vector<sii::Device> devices;
    int device=0,readSize=2048;
    std::optional<sii::Image> image;
    std::string error;
    uint64_t ticket=0;
    int activeSlave=0;
    // Keep the last EEPROM operation attached to its physical target. The
    // controller publishes one progress object for the whole bus.
    uint64_t lastTicket=0;
    std::string startupImage;
    Confirmation confirmation;
    uint64_t readGeneration=0;
    int readSlave=0;
};
EepromUi page;
const SDL_DialogFileFilter esiFilter[]={{"EtherCAT slave information","xml"}};
const SDL_DialogFileFilter binFilter[]={{"EEPROM binary image","bin"},{"All files","*"}};

void SDLCALL fileSelected(void* userdata,const char* const* files,int) {
    std::unique_ptr<DialogRequest> request(static_cast<DialogRequest*>(userdata));
    if(!files){request->result.error=SDL_GetError();if(request->result.error.empty())request->result.error="Native file dialog failed.";}
    else if(files[0])request->result.path=files[0];
    std::lock_guard<std::mutex> lock(request->mailbox->mutex);
    request->mailbox->results.push_back(std::move(request->result));
}
void chooseFile(FileAction action,const char* defaultName=nullptr,std::vector<uint8_t> bytes={}) {
    if(page.dialog)return;
    auto* request=new DialogRequest{page.mailbox,{action,{},{},std::move(bytes)},{}};
    page.dialog=true;
    auto* window=SDL_GetWindowFromID(static_cast<SDL_WindowID>(reinterpret_cast<uintptr_t>(ImGui::GetMainViewport()->PlatformHandle)));
    request->location=defaultName?std::string(defaultName):(page.source.empty()?(executableDirectory()/"esi").u8string():page.source.u8string());
    if(action==FileAction::OpenEsi||action==FileAction::OpenBin)
        SDL_ShowOpenFileDialog(fileSelected,request,window,action==FileAction::OpenEsi?esiFilter:binFilter,action==FileAction::OpenEsi?1:2,request->location.c_str(),false);
    else SDL_ShowSaveFileDialog(fileSelected,request,window,action==FileAction::ExportEsi?esiFilter:binFilter,action==FileAction::ExportEsi?1:2,request->location.c_str());
}
void error(Controller& controller,const std::string& message) {
    page.error=message;controller.appendLog("EEPROM: "+message,true);
}
bool rawSave(const std::filesystem::path& path,const std::vector<uint8_t>& bytes,std::string& detail) {
    std::ofstream output(path,std::ios::binary|std::ios::trunc);
    if(!output){detail="Cannot open the selected output file.";return false;}
    output.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));output.close();
    if(!output){detail="The selected file could not be saved completely.";return false;}
    return true;
}
void loadEsiDevice(Controller& controller,int index) {
    sii::Image image;std::string detail;
    if(index<0||index>=static_cast<int>(page.devices.size()))return;
    page.device=index;
    if(!sii::fromEsi(page.source,page.devices[static_cast<size_t>(index)].ordinal,image,detail)) {page.image.reset();error(controller,detail);return;}
    page.image=std::move(image);page.error.clear();
    controller.appendLog("EEPROM image prepared: "+page.source.filename().u8string()+" ("+std::to_string(page.image->bytes.size())+" bytes)");
}
bool programming(const Snapshot& s) {return s.connection.mode==ConnectionMode::Programming;}
bool canAccess(const Snapshot& s,const Slave* slave) {
    return programming(s)&&s.connected&&!blocked(s)&&!scriptRunning()&&slave&&(slave->state&0xf)==1;
}
bool match(const Slave& target,const sii::Image& image) {
    return !target.eepromIdentityKnown||(target.vendor==image.vendor&&target.product==image.product);
}
void selectUniqueImageMatch(const Snapshot& s) {
    if(!page.image)return;
    // Loading a different image must not silently replace an explicitly
    // selected physical target, particularly when changing its SII identity.
    if(selected(s))return;
    int matched=0;
    for(const auto& slave:s.slaves) {
        if(slave.eepromIdentityKnown&&match(slave,*page.image)) {
            matched=matched?0:slave.position;
            if(matched==0)break; // More than one match: preserve the user selection.
        }
    }
    if(matched)ui.selected=matched;
}
bool submitOperation(Controller& controller,Command command) {
    if(page.ticket)return false;
    const int slave=command.slave;
    const uint64_t ticket=controller.submit(std::move(command));
    if(!ticket){error(controller,"Command queue is full.");return false;}
    page.ticket=ticket;page.activeSlave=slave;page.cancelRequested=false;page.error.clear();
    page.lastTicket=ticket;
    ui.requests.insert(ticket);ui.awaiting=true;
    return true;
}
void startRead(Controller& controller,const Snapshot& s,int slave,int bytes) {
    if(bytes<128||bytes>131072||(bytes&1)){error(controller,tr("Read size must be an even number from 128 to 131,072 bytes.","读取长度必须为 128 至 131,072 字节之间的偶数。"));return;}
    Command command;command.kind=CommandKind::ReadEeprom;command.generation=s.generation;command.connection=s.connection;command.slave=slave;command.length=bytes;
    page.reading=true;
    if(!submitOperation(controller,std::move(command)))page.reading=false;
}
void small(const char* text,ImVec4 color=muted) {ImGui::TextColored(color,"%s",text);}
std::string fitted(std::string text,float width) {
    if(ImGui::CalcTextSize(text.c_str()).x<=width)return text;
    while(!text.empty()&&ImGui::CalcTextSize((text+"…").c_str()).x>width){
        size_t end=text.size()-1;while(end>0&&(static_cast<unsigned char>(text[end])&0xc0)==0x80)--end;text.resize(end);
    }
    return text+"…";
}
void message(const std::string& text,ImVec4 color) {
    ImGui::PushStyleColor(ImGuiCol_Text,color);ImGui::PushTextWrapPos(0);ImGui::TextUnformatted(text.c_str());ImGui::PopTextWrapPos();ImGui::PopStyleColor();
}
bool metadata(const char* id,float labelWidth=112.f) {
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding,{0,3});
    if(!ImGui::BeginTable(id,2,ImGuiTableFlags_SizingStretchProp)){ImGui::PopStyleVar();return false;}
    ImGui::TableSetupColumn("label",ImGuiTableColumnFlags_WidthFixed,labelWidth);ImGui::TableSetupColumn("value",ImGuiTableColumnFlags_WidthStretch);return true;
}
void endMetadata(){ImGui::EndTable();ImGui::PopStyleVar();}
std::string identity(uint32_t value,bool known){return known?hexNumber(value):tr("Unknown","未知");}
void frame(const char* name,const char* en,const char* zh,Icon symbol,float width,float height,const char* suffix=nullptr) {
    beginCard(name,{width,height},{14,12});
    cardHeader(tr(en,zh),symbol);
    if(suffix){const auto previous=ImGui::GetCursorPos();const float x=ImGui::GetWindowWidth()-ImGui::GetStyle().WindowPadding.x-ImGui::CalcTextSize(suffix).x;ImGui::SetCursorPos({x,12});small(suffix);ImGui::SetCursorPos(previous);}
}
const char* stageName(const EepromProgress& state) {
    switch(state.stage){
    case EepromStage::Preparing:return tr("Checking target and EEPROM access…","正在检查目标与 EEPROM 访问…");
    case EepromStage::Reading:return tr("Reading EEPROM…","正在读取 EEPROM…");
    case EepromStage::Writing:return tr("Writing EEPROM…","正在写入 EEPROM…");
    case EepromStage::Verifying:return tr("Reading back and comparing…","正在回读并比较镜像…");
    case EepromStage::Complete:return state.written?tr("Verified · activation pending","回读一致 · 等待配置生效"):tr("EEPROM read complete","EEPROM 读取完成");
    case EepromStage::Cancelled:return state.written?tr("Cancelled · image incomplete","已取消 · 镜像未完成"):tr("Cancelled · EEPROM unchanged","已取消 · EEPROM 未改写");
    case EepromStage::Failed:return tr("EEPROM operation failed","EEPROM 操作失败");
    default:return tr("Ready to program","准备就绪");
    }
}
void imageCard(Controller& controller,const Snapshot& s,float width,float height) {
    frame("eeprom-image","EEPROM image","EEPROM 镜像",Icon::Book,width,height,tr("OFFLINE PREPARATION","离线准备"));
    ImGui::BeginChild("eeprom-image-scroll",{0,0},ImGuiChildFlags_None);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,{7,4});
    const bool frozen=page.dialog||page.ticket!=0||blocked(s);
    ImGui::BeginDisabled(frozen);
    if(ImGui::Button(tr("Open ESI…","打开 ESI…")))chooseFile(FileAction::OpenEsi);
    ImGui::SameLine();if(ImGui::Button(tr("Open BIN…","打开 BIN…")))chooseFile(FileAction::OpenBin);
    const float exportWidth=ImGui::CalcTextSize(tr("Export BIN…","导出 BIN…")).x+ImGui::CalcTextSize(tr("Export ESI…","导出 ESI…")).x+28+ImGui::GetStyle().ItemSpacing.x;
    const float exportX=ImGui::GetContentRegionMax().x-exportWidth;
    const float nextX=ImGui::GetItemRectMax().x-ImGui::GetWindowPos().x+ImGui::GetStyle().ItemSpacing.x;
    // Keep export actions together; on a narrow card put them on the next row.
    if(exportX>=nextX)ImGui::SameLine();
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorStartPos().x,exportX));
    ImGui::BeginDisabled(!page.image);
    if(ImGui::Button(tr("Export BIN…","导出 BIN…"))){const auto filename=page.source.stem().u8string()+".eeprom.bin";chooseFile(FileAction::ExportBin,filename.c_str(),page.image->bytes);}
    ImGui::SameLine();
    if(ImGui::Button(tr("Export ESI…","导出 ESI…"))){
        std::string xml,detail;
        if(!sii::toEsi(*page.image,xml,detail))error(controller,detail);
        else {
            const auto filename=page.source.stem().u8string()+".sii.xml";
            chooseFile(FileAction::ExportEsi,filename.c_str(),std::vector<uint8_t>(xml.begin(),xml.end()));
        }
    }
    ImGui::EndDisabled();ImGui::EndDisabled();
    ImGui::Spacing();
    if(!page.source.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text,ink);ImGui::TextUnformatted(fitted(page.source.filename().u8string(),ImGui::GetContentRegionAvail().x).c_str());ImGui::PopStyleColor();
        if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",page.source.u8string().c_str());
    } else small(tr("No ESI or BIN image loaded","尚未加载 ESI 或 BIN 镜像"));
    if(!page.devices.empty()){
        ImGui::Spacing();ImGui::AlignTextToFramePadding();small(tr("ESI device","ESI 内设备"));ImGui::SameLine();ImGui::SetNextItemWidth(-1);
        ImGui::BeginDisabled(frozen);
        const auto choice=std::min(static_cast<size_t>(std::max(0,page.device)),page.devices.size()-1);
        if(ImGui::BeginCombo("##esi-image-device",page.devices[choice].name.c_str())){
            for(size_t n=0;n<page.devices.size();++n){ImGui::PushID(static_cast<int>(n));if(ImGui::Selectable(page.devices[n].name.c_str(),choice==n)){loadEsiDevice(controller,static_cast<int>(n));selectUniqueImageMatch(s);}ImGui::PopID();}ImGui::EndCombo();
        }ImGui::EndDisabled();
    }
    ImGui::Spacing();
    if(page.image){
        const auto& image=*page.image;
        if(metadata("image-metadata")){
            keyValue("Vendor ID","厂商 ID",hexNumber(image.vendor));keyValue("Product code","产品代码",hexNumber(image.product));keyValue("Revision","修订号",hexNumber(image.revision));
            keyValue("Serial / Alias","序列号 / 别名",hexNumber(image.serial)+" / "+hexNumber(image.alias,4));
            keyValue("Image size","镜像大小",std::to_string(image.bytes.size())+" B");
            keyValue("Used / Source","有效 / 原始长度",std::to_string(image.usedBytes)+" / "+std::to_string(image.sourceBytes)+" B");
            keyValue("Config CRC","配置区 CRC",hexNumber(image.crc,2)+tr(" · Valid"," · 有效"));endMetadata();
        }
        ImGui::Spacing();message(tr("SII structure and configuration CRC are valid.","SII 结构与配置区 CRC 有效。"),green);
        if(image.sourceBytes<image.bytes.size())message(tr("Unused image bytes are explicitly padded with FF.","镜像未使用的字节已明确填充为 FF。"),muted);
        message(tr("ESI export includes only EEPROM/SII data and cannot recover the full object dictionary or initialization commands.","导出 ESI 仅包含 EEPROM/SII 中已有的信息，无法还原完整对象字典和初始化命令。"),muted);
        ImGui::Spacing();ImGui::Separator();
        if(ImGui::TreeNode(tr("Image details","镜像详情"))){
            for(const auto& category:image.categories)ImGui::Text("%s  @ %s  · %zu B",hexNumber(category.type,4).c_str(),hexNumber(category.offset,4).c_str(),category.bytes);
            ImGui::PushFont(fonts.mono,11);
            for(size_t offset=0;offset<std::min<size_t>(image.bytes.size(),256);offset+=16){
                std::ostringstream line;line<<hexNumber(offset,4)<<"  ";
                for(size_t n=offset;n<std::min(offset+16,image.bytes.size());++n){char byte[4];std::snprintf(byte,sizeof(byte),"%02X ",image.bytes[n]);line<<byte;}
                ImGui::TextUnformatted(line.str().c_str());
            }ImGui::PopFont();ImGui::TreePop();
        }
    } else message(tr("Load an ESI description to build an image, or import a validated SII BIN file. Image preparation does not access the bus.","加载 ESI 描述以生成镜像，或导入经过检查的 SII BIN 文件。准备镜像不会访问总线。"),muted);
    if(!page.error.empty()){ImGui::Spacing();message(trMessage(page.error),red);}
    ImGui::PopStyleVar();ImGui::EndChild();endCard();
}
void targetCard(Controller& controller,const Snapshot& s,float width,float height) {
    const auto* target=selected(s);
    frame("eeprom-target","Target & programming","目标与烧录",Icon::Registers,width,height);
    ImGui::BeginChild("eeprom-target-scroll",{0,0},ImGuiChildFlags_None);
    if(target){
        char position[16];std::snprintf(position,sizeof(position),"%02d  ",target->position);
        const float badgeWidth=ImGui::CalcTextSize(stateLabel(target->state).c_str()).x+24;
        const auto targetLabel=fitted(std::string(position)+target->name,std::max(40.f,ImGui::GetContentRegionAvail().x-badgeWidth));
        ImGui::AlignTextToFramePadding();ImGui::TextUnformatted(targetLabel.c_str());
        if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",target->name.c_str());
        ImGui::SameLine();statePill(target->state,target->alCode);
        small((std::string(tr("Physical position","物理位置"))+" "+std::to_string(target->position)).c_str());
        if(metadata("target-metadata")){
            const bool known=target->eepromIdentityKnown;
            keyValue("Vendor ID","厂商 ID",identity(target->vendor,known));keyValue("Product code","产品代码",identity(target->product,known));keyValue("Revision","修订号",identity(target->revision,known));
            keyValue("EEPROM capacity","EEPROM 容量",target->eepromSizeKnown?std::to_string(target->eepromCapacity)+tr(" B · SII declaration"," B · SII 声明值"):tr("Unknown · not measured","未知 · 未实测容量"));
            keyValue("Serial / Alias","序列号 / 别名",known?hexNumber(target->serial)+" / "+hexNumber(target->eepromAlias,4):tr("Unknown","未知"));endMetadata();
        }
    } else {small(tr("No slave selected","未选择从站"));ImGui::Spacing();}
    ImGui::Spacing();
    const bool identityMatches=target&&page.image&&match(*target,*page.image);
    const bool fits=target&&page.image&&(!target->eepromSizeKnown||page.image->bytes.size()<=target->eepromCapacity);
    if(!s.connected||!target)message(tr("Scan and select a slave first.","请先扫描并选择从站。"),muted);
    else if((target->state&0xf)!=1)message(tr("The selected slave must have a confirmed INIT state.","必须确认所选从站处于 INIT。"),red);
    else if(!page.image)message(tr("Load an image to review its identity and size.","加载镜像以查看标识与大小。"),muted);
    else if(!fits)message(tr("The image exceeds the target's declared EEPROM capacity.","镜像大小超过目标声明的 EEPROM 容量。"),red);
    else if(!identityMatches) {
        const auto actual=hexNumber(target->vendor)+" / "+hexNumber(target->product);
        const auto image=hexNumber(page.image->vendor)+" / "+hexNumber(page.image->product);
        message(std::string(tr("Identity will change: ","标识将改变："))+actual+" → "+image,amber);
    }
    else if(!target->eepromIdentityKnown)message(tr("Blank SII: identity and physical capacity cannot be confirmed.","空白 SII：无法核实设备标识与物理容量。"),amber);
    else if(target->revision!=page.image->revision)message(tr("Revision differs. Confirm the intended change before programming.","修订号不同，请在烧录前确认目标版本。"),amber);
    else message(tr("Vendor, product and revision match.","厂商、产品及修订号匹配。"),green);
    ImGui::Spacing();ImGui::Separator();ImGui::Spacing();ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,{7,5});
    if(page.ticket){
        ImGui::BeginDisabled(page.cancelRequested);
        if(ImGui::Button(tr("Cancel","取消"))){if(controller.cancel(page.ticket))page.cancelRequested=true;}
        ImGui::EndDisabled();
        if(page.cancelRequested){ImGui::SameLine();small(tr("Cancelling…","正在取消…"));}
    }else{
        ImGui::BeginDisabled(!canAccess(s,target)||!page.image||!fits||page.dialog);
        if(ImGui::Button(tr("Program EEPROM…","烧录 EEPROM…"))){
            auto& confirm=page.confirmation;confirm={};confirm.generation=s.generation;confirm.slave=target->position;confirm.name=target->name;confirm.interface=s.connection.primary;confirm.file=page.source.filename().u8string();
            confirm.vendor=target->vendor;confirm.product=target->product;confirm.revision=target->revision;confirm.identityKnown=target->eepromIdentityKnown;confirm.sizeKnown=target->eepromSizeKnown;confirm.image=*page.image;
            confirm.capacity=static_cast<int>(target->eepromSizeKnown?target->eepromCapacity:page.image->bytes.size());
            confirm.capacityLimit=target->eepromSizeKnown?target->eepromCapacity:131072;
            page.programConfirm=true;
        }ImGui::EndDisabled();
        ImGui::SameLine();ImGui::BeginDisabled(!canAccess(s,target)||page.dialog);
        if(ImGui::Button(tr("Read & save…","读取并保存…"))){
            if(target->eepromSizeKnown)startRead(controller,s,target->position,static_cast<int>(target->eepromCapacity));
            else {page.readGeneration=s.generation;page.readSlave=target->position;page.readSize=2048;page.readConfirm=true;}
        }ImGui::EndDisabled();
    }
    ImGui::PopStyleVar();ImGui::Spacing();
    const auto& progress=s.eeprom;
    const bool failed=progress.stage==EepromStage::Failed||progress.stage==EepromStage::Cancelled;
    const bool trackedProgress=page.lastTicket!=0&&progress.request==page.lastTicket;
    const bool ownsProgress=target&&trackedProgress&&progress.slave==target->position;
    const bool activeTarget=target&&page.ticket!=0&&page.activeSlave==target->position;
    if(!s.connected)small(tr("Prepare an EEPROM image offline","可离线准备 EEPROM 镜像"),teal);
    else if(!ownsProgress&&!activeTarget){
        if(trackedProgress)small(tr("Select the target used by the last EEPROM operation.","请选择上次 EEPROM 操作使用的目标。"),muted);
        else small(tr("Ready","就绪"),teal);
    }
    else if(!ownsProgress&&activeTarget)message(tr("Keep power and cable connected.","请保持从站供电与网线连接。"),muted);
    else {message(stageName(progress),failed?red:teal);if(progress.total){const float ratio=std::min(1.f,static_cast<float>(progress.completed)/static_cast<float>(progress.total));char value[48];std::snprintf(value,sizeof(value),"%zu / %zu B",progress.completed,progress.total);ImGui::ProgressBar(ratio,{-1,6},"");small(value);}}
    if(ownsProgress&&progress.stage==EepromStage::Complete&&progress.written)message(tr("Follow the slave's reload or power-cycle requirements, then disconnect and scan in Hardware mode. OP is not restored automatically.","请按设备要求重新加载或重新上电，再断开并在硬件模式下扫描，不会自动恢复 OP。"),muted);
    else if(ownsProgress&&failed&&progress.written)message(tr("EEPROM may be partially written. Previous contents were not restored; retry the complete image.","EEPROM 可能只完成部分写入，未恢复旧内容，请重新烧录完整镜像。"),red);
    else if(ownsProgress&&!progress.detail.empty())message(trMessage(progress.detail),failed?red:muted);
    else if(page.ticket)message(tr("Keep power and cable connected.","请保持从站供电与网线连接。"),muted);
    ImGui::EndChild();endCard();
}
void confirmations(Controller& controller,const Snapshot& s) {
    const auto programTitle=label("Program EEPROM","烧录 EEPROM","program-eeprom-confirm");
    const auto readTitle=label("Read EEPROM","读取 EEPROM","read-eeprom-size");
    if(page.programConfirm){ImGui::OpenPopup(programTitle.c_str());page.programConfirm=false;}
    if(page.readConfirm){ImGui::OpenPopup(readTitle.c_str());page.readConfirm=false;}
    ImGui::SetNextWindowSize({490,0},ImGuiCond_Always);
    if(ImGui::BeginPopupModal(programTitle.c_str(),nullptr,ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoSavedSettings)){
        const auto& confirm=page.confirmation;
        ImGui::TextUnformatted(tr("Program the selected EEPROM?","烧录所选从站的 EEPROM？"));ImGui::Separator();
        message(tr("This writes persistent configuration to the selected slave.","此操作将改写所选从站的持久化配置。"),muted);
        if(metadata("confirm-image",122)){
            keyValue("Interface","网卡",confirm.interface);keyValue("Physical target","物理目标",std::to_string(confirm.slave)+" · "+confirm.name);keyValue("Image file","镜像文件",confirm.file);
            keyValue("Current vendor ID","原厂商 ID",identity(confirm.vendor,confirm.identityKnown));
            keyValue("New vendor ID","新厂商 ID",hexNumber(confirm.image.vendor));
            keyValue("Current product code","原产品代码",identity(confirm.product,confirm.identityKnown));
            keyValue("New product code","新产品代码",hexNumber(confirm.image.product));
            keyValue("Image revision","镜像修订号",hexNumber(confirm.image.revision));
            keyValue("Serial / Alias","序列号 / 别名",hexNumber(confirm.image.serial)+" / "+hexNumber(confirm.image.alias,4));
            keyValue("Write size","写入大小",std::to_string(confirm.image.bytes.size())+" B");endMetadata();
        }
        if(!confirm.identityKnown)message(tr("The blank SII cannot identify this hardware. Confirm the physical target and its EEPROM capacity.","空白 SII 无法识别此硬件，请确认物理目标及其 EEPROM 容量。"),amber);
        else if(confirm.vendor!=confirm.image.vendor||confirm.product!=confirm.image.product)
            message(tr("Programming will replace the current Vendor ID and Product Code with the image values.","烧录后，当前厂商 ID 和产品代码将替换为镜像中的值。"),amber);
        if(confirm.identityKnown&&confirm.revision!=confirm.image.revision)message(std::string(tr("Revision change: ","修订号变化："))+hexNumber(confirm.revision)+" → "+hexNumber(confirm.image.revision),amber);
        ImGui::AlignTextToFramePadding();small(tr("Confirmed capacity (bytes)","确认的容量（字节）"));ImGui::SameLine();ImGui::SetNextItemWidth(140);
        ImGui::InputInt("##confirmed-capacity",&page.confirmation.capacity,0,0);
        if(confirm.sizeKnown)message(tr("The suggested capacity comes from SII, not a physical measurement.","建议容量来自 SII 声明，并非物理容量测量。"),muted);
        message(tr("Keep power and cable connected. Cancelling after writing starts leaves a partially programmed image.","请保持从站供电和网线连接。开始写入后取消操作，会留下未完成的镜像。"),amber);
        const bool capacityValid=confirm.capacity>=static_cast<int>(confirm.image.bytes.size())&&confirm.capacity<=static_cast<int>(confirm.capacityLimit)&&(confirm.capacity&1)==0;
        if(!capacityValid)message(confirm.sizeKnown?tr("Capacity must be even, at least the image size, and no larger than the valid SII declaration.","容量必须为偶数、不小于镜像大小，且不能超过有效 SII 声明值。"):tr("Capacity must be even, at least the image size, and no more than 131,072 bytes.","容量必须为偶数、不小于镜像大小且不超过 131,072 字节。"),red);
        const bool sameTarget=s.connected&&programming(s)&&s.generation==confirm.generation&&selected(s)&&selected(s)->position==confirm.slave&&(selected(s)->state&0xf)==1;
        if(!sameTarget)message(tr("The connection or selected target changed. Close this confirmation and try again.","连接或所选目标已变化，请关闭确认窗口后重试。"),red);
        if(ImGui::Button(tr("Back","返回")))ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        ImGui::BeginDisabled(!capacityValid||!sameTarget||blocked(s)||scriptRunning());
        const bool accepted=ImGui::Button(tr("Program this slave","烧录此从站"));ImGui::EndDisabled();
        if(accepted){
            Command request;request.kind=CommandKind::WriteEeprom;request.generation=confirm.generation;request.connection=s.connection;request.slave=confirm.slave;request.data=confirm.image.bytes;request.length=confirm.capacity;
            page.reading=false;submitOperation(controller,std::move(request));ImGui::CloseCurrentPopup();
        }ImGui::EndPopup();
    }
    ImGui::SetNextWindowSize({490,0},ImGuiCond_Always);
    if(ImGui::BeginPopupModal(readTitle.c_str(),nullptr,ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoSavedSettings)){
        ImGui::TextUnformatted(tr("Read EEPROM","读取 EEPROM"));
        message(tr("The SII capacity is unknown. Enter a bounded read length appropriate for this hardware.","SII 容量未知，请按此硬件的实际容量指定有界读取长度。"),muted);
        ImGui::SetNextItemWidth(150);ImGui::InputInt(label("Bytes","字节","read-eeprom-bytes").c_str(),&page.readSize,0,0);
        const bool lengthValid=page.readSize>=128&&page.readSize<=131072&&(page.readSize&1)==0;
        if(!lengthValid)message(tr("Enter an even byte count from 128 to 131,072.","请输入 128 至 131,072 之间的偶数字节数。"),red);
        const auto* readTarget=selected(s);
        const bool readTargetValid=readTarget&&readTarget->position==page.readSlave&&canAccess(s,readTarget);
        if(!readTargetValid)message(tr("The connection, target or INIT state changed. Close this confirmation and try again.","连接、目标或 INIT 状态已变化，请关闭确认窗口后重试。"),red);
        if(ImGui::Button(tr("Back","返回")))ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        ImGui::BeginDisabled(!lengthValid||!readTargetValid||s.generation!=page.readGeneration);
        const bool accepted=ImGui::Button(tr("Read & save…","读取并保存…"));ImGui::EndDisabled();
        if(accepted){startRead(controller,s,page.readSlave,page.readSize);ImGui::CloseCurrentPopup();}ImGui::EndPopup();
    }
}
} // namespace

void eepromTick(Controller& controller,const Snapshot& snapshot) {
    std::deque<FileSelection> selections;
    {std::lock_guard<std::mutex> lock(page.mailbox->mutex);selections.swap(page.mailbox->results);}
    if(!page.startupImage.empty()){
        auto extension=std::filesystem::u8path(page.startupImage).extension().u8string();
        std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
        selections.push_back({extension==".xml"?FileAction::OpenEsi:FileAction::OpenBin,std::move(page.startupImage),{}, {}});page.startupImage.clear();
    }
    for(auto& selection:selections){
        page.dialog=false;
        if(!selection.error.empty()){error(controller,selection.error);continue;}
        if(selection.path.empty())continue;
        try {
            const auto path=std::filesystem::u8path(selection.path);
            if(selection.action==FileAction::OpenEsi){
                // A newly selected file is a new preparation transaction. Do
                // not leave a previously validated image armed after this
                // file fails to parse or contains no usable device.
                page.source=path;page.devices.clear();page.device=0;page.image.reset();page.error.clear();
                std::vector<sii::Device> devices;std::string detail;
                if(!sii::listDevices(path,devices,detail)||devices.empty()){error(controller,detail.empty()?"The ESI contains no devices.":detail);continue;}
                page.devices=std::move(devices);page.device=0;loadEsiDevice(controller,0);selectUniqueImageMatch(snapshot);
            }else if(selection.action==FileAction::OpenBin){
                page.source=path;page.devices.clear();page.device=0;page.image.reset();page.error.clear();
                sii::Image image;std::string detail;
                if(!sii::loadBin(path,image,detail)){error(controller,detail);continue;}
                page.image=std::move(image);page.error.clear();controller.appendLog("EEPROM image loaded: "+path.filename().u8string());selectUniqueImageMatch(snapshot);
            }else{
                if(selection.action==FileAction::ExportEsi){
                    std::error_code sameFileError;
                    if(std::filesystem::equivalent(page.source,path,sameFileError)){
                        error(controller,"The exported ESI must not overwrite the source image.");continue;
                    }
                }
                std::string detail;
                if(!rawSave(path,selection.bytes,detail)){error(controller,detail);continue;}
                if(selection.action==FileAction::SaveRead)page.source=path;
                page.error.clear();controller.appendLog(std::string(selection.action==FileAction::SaveRead?"EEPROM read saved: ":selection.action==FileAction::ExportEsi?"ESI description exported: ":"EEPROM image exported: ")+path.filename().u8string()+" ("+std::to_string(selection.bytes.size())+" bytes)");
            }
        }catch(const std::exception& exception){error(controller,exception.what());}
    }
    if(page.ticket){
        if(std::any_of(snapshot.slaves.begin(),snapshot.slaves.end(),[](const Slave& slave){return slave.position==page.activeSlave;}))ui.selected=page.activeSlave;
        Result result;
        if(controller.result(page.ticket,result)){
            ui.requests.erase(page.ticket);page.ticket=0;page.cancelRequested=false;
            if(!result.ok)page.error=result.message;
            if(result.ok&&page.reading){
                // A read replaces the prepared image, even when its raw bytes
                // are not a valid SII image. Keep that raw read saveable.
                page.source=std::filesystem::path("EEPROM-read.bin");page.devices.clear();page.device=0;page.image.reset();
                sii::Image image;std::string detail;
                if(sii::fromBin(result.data,image,detail)){
                    page.image=std::move(image);page.error.clear();
                    controller.appendLog("EEPROM read loaded for image export.");
                }else error(controller,"EEPROM read is not a valid SII image; save the raw BIN to inspect it. | "+detail);
                chooseFile(FileAction::SaveRead,"EEPROM-read.bin",std::move(result.data));
            }
            page.reading=false;
        }
    }
}
void eepromPage(Controller& controller,const Snapshot& s) {
    ImGui::PushFont(fonts.small,12);ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,{6,5});
    const float width=ImGui::GetContentRegionAvail().x,height=ImGui::GetContentRegionAvail().y;
    const float left=(width-14.f)*.515f,right=width-left-14.f;
    imageCard(controller,s,left,height);ImGui::SameLine(0,14);targetCard(controller,s,right,height);
    confirmations(controller,s);
    ImGui::PopStyleVar();ImGui::PopFont();
}
} // namespace niceecatm::workbench

namespace niceecatm {
void configureWorkbenchEepromImage(const char* path) {if(path)workbench::page.startupImage=path;}
}
