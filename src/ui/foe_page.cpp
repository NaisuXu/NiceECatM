// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#include "ui/internal.hpp"
#include "platform/paths.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <system_error>

namespace niceecatm::workbench {
namespace {
struct FileSelection {FoeDirection direction;std::string path,error;};
struct DialogMailbox {std::mutex mutex;std::deque<FileSelection> results;};
struct DialogRequest {std::shared_ptr<DialogMailbox> mailbox;FileSelection selection;std::string location;};
struct FoeUi {
    std::shared_ptr<DialogMailbox> mailbox=std::make_shared<DialogMailbox>();
    FoeDirection direction=FoeDirection::Download;
    char remoteName[1024]="firmware.bin",password[32]="0x00000000";
    std::filesystem::path source,destination;
    uintmax_t sourceSize=0;
    int uploadMiB=16,responseSeconds=3,noProgressSeconds=30,totalSeconds=300,retries=3;
    bool dialog=false,cancelRequested=false;
    uint64_t ticket=0,lastTicket=0,generation=0;
    int activeSlave=0;
    Result result;
    FoeProgress lastProgress;
};
FoeUi page;
const SDL_DialogFileFilter filters[]={{"All files","*"}};
constexpr size_t maxFileBytes=128u*1024u*1024u;

void reportError(Controller& controller,const std::string& message) {
    controller.appendLog("FoE: "+message,true);
}
void SDLCALL fileSelected(void* userdata,const char* const* files,int) {
    // Native dialog completion may run on a different thread or after this
    // page is hidden. Only the GUI thread consumes drafts and submits work.
    std::unique_ptr<DialogRequest> request(static_cast<DialogRequest*>(userdata));
    if(!files){request->selection.error=SDL_GetError();if(request->selection.error.empty())request->selection.error="Native file dialog failed.";}
    else if(files[0])request->selection.path=files[0];
    std::lock_guard<std::mutex> lock(request->mailbox->mutex);
    request->mailbox->results.push_back(std::move(request->selection));
}
void chooseFile() {
    if(page.dialog||page.ticket)return;
    auto* request=new DialogRequest{page.mailbox,{page.direction,{},{}},{}};
    page.dialog=true;
    auto* window=SDL_GetWindowFromID(static_cast<SDL_WindowID>(reinterpret_cast<uintptr_t>(ImGui::GetMainViewport()->PlatformHandle)));
    const auto& current=page.direction==FoeDirection::Download?page.source:page.destination;
    request->location=current.empty()?(executableDirectory()/(page.direction==FoeDirection::Upload?"FoE-readback.bin":"")).u8string():current.u8string();
    if(page.direction==FoeDirection::Download)SDL_ShowOpenFileDialog(fileSelected,request,window,filters,1,request->location.c_str(),false);
    else SDL_ShowSaveFileDialog(fileSelected,request,window,filters,1,request->location.c_str());
}
std::string bytes(double value) {
    char text[64];
    if(value>=1048576.)std::snprintf(text,sizeof(text),"%.1f MiB",value/1048576.);
    else if(value>=1024.)std::snprintf(text,sizeof(text),"%.1f KiB",value/1024.);
    else std::snprintf(text,sizeof(text),"%.0f B",value);
    return text;
}
bool passwordValue(uint32_t& value) {
    const char* first=page.password;const char* end=first+std::char_traits<char>::length(first);
    const bool hexadecimal=end-first>2&&first[0]=='0'&&(first[1]=='x'||first[1]=='X');
    if(hexadecimal)first+=2;
    const auto parsed=std::from_chars(first,end,value,hexadecimal?16:10);
    return first!=end&&parsed.ec==std::errc{}&&parsed.ptr==end;
}
const char* stageName(FoeStage stage) {
    switch(stage) {
    case FoeStage::Preparing:return tr("Preparing transfer","准备传输");
    case FoeStage::Sending:return tr("Sending","正在发送");
    case FoeStage::Receiving:return tr("Receiving","正在接收");
    case FoeStage::Busy:return tr("Device BUSY · waiting","从站 BUSY · 等待中");
    case FoeStage::Finalizing:return tr("Finalizing transfer","正在完成传输");
    case FoeStage::Complete:return tr("Transfer complete","传输完成");
    case FoeStage::Cancelled:return tr("Cancelled","已取消");
    case FoeStage::Failed:return tr("Failed","失败");
    default:return tr("Ready","就绪");
    }
}
void fieldLabel(const char* en,const char* zh) {
    ImGui::PushFont(fonts.small,11);ImGui::TextDisabled("%s",tr(en,zh));ImGui::PopFont();ImGui::SetNextItemWidth(-1);
}
void smallNote(const char* en,const char* zh) {
    ImGui::PushFont(fonts.small,11);note(en,zh);ImGui::PopFont();
}
bool transferAvailable(const Snapshot& s,const Slave* target) {
    if(!target||!s.connected||s.running||blocked(s)||scriptRunning()||page.dialog||s.connection.mode==ConnectionMode::Programming)return false;
    if((target->state&0x10)!=0||target->alCode)return false;
    const auto state=target->state&15;
    if(state!=2&&state!=3)return false;
    if(state==2&&!(target->mailboxProtocols&8))return false;
    return std::none_of(s.slaves.begin(),s.slaves.end(),[](const Slave& slave){return (slave.state&15)==8;});
}
void startTransfer(Controller& controller,const Snapshot& s) {
    const auto* target=selected(s);
    if(!transferAvailable(s,target))return;
    uint32_t password=0;
    if(!passwordValue(password)){reportError(controller,"Password must be an unsigned 32-bit decimal or hexadecimal number.");return;}
    if(!page.remoteName[0]){reportError(controller,"Enter a device filename.");return;}
    const auto& file=page.direction==FoeDirection::Download?page.source:page.destination;
    if(file.empty()){reportError(controller,"Choose a local file or destination before starting.");return;}
    if(page.responseSeconds<1||page.responseSeconds>3600||page.noProgressSeconds<1||page.noProgressSeconds>3600||page.totalSeconds<1||page.totalSeconds>3600||page.retries<0||page.retries>20) {
        reportError(controller,"FoE timeouts must be 1..3600 seconds and retries must be 0..20.");return;
    }
    Command request;request.kind=page.direction==FoeDirection::Download?CommandKind::FoeDownload:CommandKind::FoeUpload;
    request.generation=s.generation;request.connection=s.connection;request.slave=target->position;
    request.foe.remoteName=page.remoteName;request.foe.localPath=file.u8string();request.foe.password=password;
    request.foe.maxBytes=page.direction==FoeDirection::Upload?size_t(page.uploadMiB)*1048576u:maxFileBytes;
    request.foe.responseTimeoutMs=page.responseSeconds*1000;request.foe.noProgressTimeoutMs=page.noProgressSeconds*1000;
    request.foe.totalTimeoutMs=page.totalSeconds*1000;request.foe.retries=page.retries;
    const uint64_t ticket=controller.submit(std::move(request));
    if(!ticket){reportError(controller,"Command queue is full.");return;}
    page.ticket=page.lastTicket=ticket;page.generation=s.generation;page.activeSlave=target->position;
    page.cancelRequested=false;page.result={};page.lastProgress={};page.lastProgress.stage=FoeStage::Preparing;
    page.lastProgress.request=ticket;page.lastProgress.slave=target->position;page.lastProgress.direction=page.direction;
    ui.requests.insert(ticket);ui.awaiting=true;ui.localError.clear();
}
void fileCard(float width,float height) {
    beginCard("foe-file",{width,height});cardHeader(tr("FoE file transfer","FoE 文件传输"),Icon::Mail);
    ImGui::BeginChild("foe-file-body",{0,0},ImGuiChildFlags_None);
    ImGui::BeginDisabled(page.ticket!=0||page.dialog);
    const float buttonWidth=(ImGui::GetContentRegionAvail().x-ImGui::GetStyle().ItemSpacing.x)*.5f;
    for(const auto direction:{FoeDirection::Download,FoeDirection::Upload}) {
        ImGui::PushID(static_cast<int>(direction));
        const bool active=page.direction==direction;
        ImGui::PushStyleColor(ImGuiCol_Button,active?ImVec4(.86f,.95f,.96f,1):ImGui::GetStyleColorVec4(ImGuiCol_Button));
        ImGui::PushStyleColor(ImGuiCol_Text,active?teal:ink);
        if(ImGui::Button(direction==FoeDirection::Upload?tr("Upload · slave → master","上传 · 从站 → 主站"):tr("Download · master → slave","下载 · 主站 → 从站"),{buttonWidth,29}))page.direction=direction;
        ImGui::PopStyleColor(2);ImGui::PopID();if(direction==FoeDirection::Download)ImGui::SameLine();
    }
    ImGui::Spacing();fieldLabel("Device filename","从站文件名");
    ImGui::InputText("##foe-device-filename",page.remoteName,sizeof(page.remoteName));
    ImGui::Spacing();
    if(ImGui::BeginTable("foe-credentials",2,ImGuiTableFlags_SizingStretchSame)) {
        // Keep labels in their own row: a framed input's baseline must not
        // shift the next column's label down.
        ImGui::TableNextRow();ImGui::TableSetColumnIndex(0);fieldLabel("Password · uint32","密码 · uint32");
        ImGui::TableSetColumnIndex(1);
        fieldLabel(page.direction==FoeDirection::Upload?"Maximum upload size":"Local file size",page.direction==FoeDirection::Upload?"上传大小上限":"本地文件大小");
        ImGui::TableNextRow();ImGui::TableSetColumnIndex(0);ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##foe-password",page.password,sizeof(page.password));
        ImGui::TableSetColumnIndex(1);ImGui::SetNextItemWidth(-1);
        if(page.direction==FoeDirection::Upload) {
            const int limits[]={1,4,16,64,128};const char* names[]={"1 MiB","4 MiB","16 MiB","64 MiB","128 MiB"};
            int index=2;for(int i=0;i<5;++i)if(page.uploadMiB==limits[i])index=i;
            if(ImGui::Combo("##foe-upload-limit",&index,names,5))page.uploadMiB=limits[index];
        }else {
            const auto size=page.source.empty()?std::string("—"):bytes(double(page.sourceSize));
            char display[64];std::snprintf(display,sizeof(display),"%s",size.c_str());
            ImGui::InputText("##foe-local-size",display,sizeof(display),ImGuiInputTextFlags_ReadOnly);
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();fieldLabel(page.direction==FoeDirection::Upload?"Local destination":"Local file",page.direction==FoeDirection::Upload?"本地保存位置":"本地文件");
    const auto& file=page.direction==FoeDirection::Upload?page.destination:page.source;
    const auto name=file.filename().u8string();const char* choose=page.direction==FoeDirection::Upload?tr("Save as…","另存为…"):tr("Choose file…","选择文件…");
    const float chooseWidth=ImGui::CalcTextSize(choose).x+ImGui::GetStyle().FramePadding.x*2;
    ImGui::SetNextItemWidth(std::max(1.f,ImGui::GetContentRegionAvail().x-chooseWidth-ImGui::GetStyle().ItemSpacing.x));
    char display[2048];std::snprintf(display,sizeof(display),"%s",name.c_str());
    ImGui::InputTextWithHint("##foe-local-file",tr("Choose a file","选择文件"),display,sizeof(display),ImGuiInputTextFlags_ReadOnly);
    if(!file.empty()&&ImGui::IsItemHovered())ImGui::SetTooltip("%s",file.u8string().c_str());
    ImGui::SameLine();if(ImGui::Button(choose,{chooseWidth,0}))chooseFile();
    ImGui::Spacing();
    if(ImGui::CollapsingHeader(tr("Timeouts & retries","超时与重试"))) {
        if(ImGui::BeginTable("foe-timeouts",2,ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextRow();ImGui::TableSetColumnIndex(0);fieldLabel("Response timeout · s","响应超时 · 秒");
            ImGui::TableSetColumnIndex(1);fieldLabel("No-progress timeout · s","无进展超时 · 秒");
            ImGui::TableNextRow();ImGui::TableSetColumnIndex(0);ImGui::SetNextItemWidth(-1);ImGui::InputInt("##foe-response-timeout",&page.responseSeconds,0,0);
            ImGui::TableSetColumnIndex(1);ImGui::SetNextItemWidth(-1);ImGui::InputInt("##foe-progress-timeout",&page.noProgressSeconds,0,0);
            ImGui::TableNextRow();ImGui::TableSetColumnIndex(0);fieldLabel("Total timeout · s","总超时 · 秒");
            ImGui::TableSetColumnIndex(1);fieldLabel("Retry limit","重试次数上限");
            ImGui::TableNextRow();ImGui::TableSetColumnIndex(0);ImGui::SetNextItemWidth(-1);ImGui::InputInt("##foe-total-timeout",&page.totalSeconds,0,0);
            ImGui::TableSetColumnIndex(1);ImGui::SetNextItemWidth(-1);ImGui::InputInt("##foe-retry-limit",&page.retries,0,0);
            ImGui::EndTable();
        }
    }
    ImGui::EndDisabled();ImGui::EndChild();endCard();
}
void stateHint(const Snapshot& s,const Slave* target) {
    if(!s.connected)smallNote("Connect and scan before transferring.","请先连接并扫描设备。");
    else if(!target)smallNote("Select a slave in the topology tree.","请在左侧拓扑树中选择从站。");
    else if(scriptRunning())smallNote("Stop the script before a FoE transfer.","FoE 传输前请先停止脚本。");
    else if(s.running||std::any_of(s.slaves.begin(),s.slaves.end(),[](const Slave& slave){return (slave.state&15)==8;}))smallNote("Switch all slaves to PRE-OP before a FoE transfer.","FoE 传输前请先将全部从站切换到 PRE-OP。");
    else if((target->state&0x10)||target->alCode)smallNote("Resolve the slave AL error before transferring.","请先处理从站的 AL 错误。");
    else if((target->state&15)==3)smallNote("BOOT · uses the bootstrap mailbox.","BOOT · 使用 Bootstrap Mailbox。");
    else if((target->state&15)!=2)smallNote("Use the top state buttons to select PRE-OP or BOOT.","请使用顶部状态按钮切换到 PRE-OP 或 BOOT。");
    else if(!(target->mailboxProtocols&8))smallNote("FoE is not advertised in the normal mailbox. Check the slave's BOOT requirements.","普通 Mailbox 未声明 FoE，请核对从站的 BOOT 使用要求。");
    else smallNote("PRE-OP · uses the normal mailbox.","PRE-OP · 使用普通 Mailbox。");
}
void transferCard(Controller& controller,const Snapshot& s,float width,float height) {
    beginCard("foe-transfer",{width,height});cardHeader(tr("Transfer control","传输控制"),Icon::Scan);
    ImGui::BeginChild("foe-transfer-body",{0,0},ImGuiChildFlags_None);
    const auto* target=selected(s);stateHint(s,target);ImGui::Spacing();
    const float cancelWidth=74.f;const float startWidth=std::max(1.f,ImGui::GetContentRegionAvail().x-cancelWidth-ImGui::GetStyle().ItemSpacing.x);
    ImGui::BeginDisabled(!transferAvailable(s,target));ImGui::PushStyleColor(ImGuiCol_Button,teal);ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(1,1,1,1));
    if(ImGui::Button(page.direction==FoeDirection::Upload?tr("Start upload","开始上传"):tr("Start download","开始下载"),{startWidth,30}))startTransfer(controller,s);
    ImGui::PopStyleColor(2);ImGui::EndDisabled();ImGui::SameLine();
    ImGui::BeginDisabled(!page.ticket||page.cancelRequested);ImGui::PushStyleColor(ImGuiCol_Text,red);
    if(ImGui::Button(tr("Cancel","取消"),{cancelWidth,30})&&controller.cancel(page.ticket))page.cancelRequested=true;
    ImGui::PopStyleColor();ImGui::EndDisabled();ImGui::Spacing();
    const bool ownsProgress=target&&page.lastTicket&&page.generation==s.generation&&page.activeSlave==target->position;
    const FoeProgress* progress=ownsProgress?&page.lastProgress:nullptr;
    const auto stage=progress?progress->stage:FoeStage::Idle;
    ImGui::TextColored(stage==FoeStage::Failed?red:stage==FoeStage::Busy?amber:stage==FoeStage::Complete?green:teal,"%s",page.cancelRequested?tr("Cancelling…","正在取消…"):stageName(stage));
    const bool upload=progress?progress->direction==FoeDirection::Upload:page.direction==FoeDirection::Upload;
    const bool pending=page.ticket&&ownsProgress;
    const size_t completed=progress?progress->completed:0,total=progress?progress->total:0;
    const float fraction=pending&&upload?-float(ImGui::GetTime()+.001):stage==FoeStage::Complete?1.f:total?std::min(1.f,float(completed)/float(total)):0.f;
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram,stage==FoeStage::Failed?red:stage==FoeStage::Busy?amber:teal);
    ImGui::ProgressBar(fraction,{-1,7},"");ImGui::PopStyleColor();
    std::string amount=bytes(double(completed));
    if(total&&!upload)amount+=" / "+bytes(double(total));
    else if(pending&&upload)amount+=std::string(" · ")+tr("size unknown","总大小未知");
    ImGui::TextDisabled("%s",amount.c_str());ImGui::Spacing();
    if(ImGui::BeginTable("foe-progress-metrics",3,ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();fieldLabel("ACK blocks","已确认块");ImGui::Text("%zu",progress?progress->blocks:0);
        ImGui::TableNextColumn();fieldLabel("Average speed","平均速率");ImGui::Text("%s",progress?(bytes(progress->bytesPerSecond)+"/s").c_str():"—");
        ImGui::TableNextColumn();fieldLabel("Elapsed","已用时间");ImGui::Text("%.1f s",progress?progress->elapsedSeconds:0.);
        ImGui::EndTable();
    }
    ImGui::Spacing();
    if(ownsProgress&&progress->errorCode)ImGui::TextColored(red,"FoE %s",hexNumber(progress->errorCode,8).c_str());
    if(ownsProgress&&!page.ticket&&!page.result.message.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text,page.result.ok?green:stage==FoeStage::Cancelled?muted:red);
        ImGui::TextWrapped("%s",trMessage(page.result.message).c_str());ImGui::PopStyleColor();
    }else if(ownsProgress&&pending&&!progress->detail.empty())ImGui::TextWrapped("%s",trMessage(progress->detail).c_str());
    else if(!ownsProgress&&page.lastTicket)smallNote("Select the target used by the last FoE transfer to see its result.","选择上次 FoE 传输使用的目标可查看其结果。");
    else if(!page.ticket)smallNote("No transfer yet.","尚未执行传输。");
    ImGui::EndChild();endCard();
}
} // namespace

bool foeRunning() {return page.ticket!=0;}
void foeTick(Controller& controller,const Snapshot& s) {
    std::deque<FileSelection> selections;
    {std::lock_guard<std::mutex> lock(page.mailbox->mutex);selections.swap(page.mailbox->results);}
    for(const auto& selection:selections) {
        page.dialog=false;
        if(!selection.error.empty()){reportError(controller,selection.error);continue;}
        if(selection.path.empty())continue;
        try {
            const auto file=std::filesystem::u8path(selection.path);
            if(selection.direction==FoeDirection::Download) {
                std::error_code issue;const auto size=std::filesystem::file_size(file,issue);
                if(issue){reportError(controller,"Cannot read the selected local file.");continue;}
                if(size>maxFileBytes){reportError(controller,"FoE files must not exceed 128 MiB.");continue;}
                page.source=file;page.sourceSize=size;
            }else page.destination=file;
        }catch(const std::exception& exception){reportError(controller,exception.what());}
    }
    if(page.lastTicket&&s.foe.request==page.lastTicket&&s.foe.slave==page.activeSlave&&s.generation==page.generation)page.lastProgress=s.foe;
    if(page.ticket) {
        if(s.generation==page.generation&&std::any_of(s.slaves.begin(),s.slaves.end(),[](const Slave& slave){return slave.position==page.activeSlave;}))ui.selected=page.activeSlave;
        Result result;
        if(controller.result(page.ticket,result)) {
            const bool cancelled=page.cancelRequested;
            ui.requests.erase(page.ticket);page.ticket=0;page.cancelRequested=false;page.result=std::move(result);
            if(page.result.ok)page.lastProgress.stage=FoeStage::Complete;
            else if(cancelled||page.lastProgress.stage==FoeStage::Cancelled)page.lastProgress.stage=FoeStage::Cancelled;
            else page.lastProgress.stage=FoeStage::Failed;
            if(ui.requests.empty())ui.awaiting=false;
        }
    }
}
void foePage(Controller& controller,const Snapshot& s) {
    ImGui::PushFont(fonts.small,12);ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,{6,5});
    const auto* target=selected(s);
    if(target) {
        ImGui::AlignTextToFramePadding();ImGui::Text("%02d  %s",target->position,target->name.c_str());ImGui::SameLine(0,12);statePill(target->state,target->alCode);
        ImGui::SameLine(0,8);pill(tr(target->mailboxProtocols&8?"FoE advertised":"FoE not advertised",target->mailboxProtocols&8?"已声明 FoE":"未声明 FoE"),target->mailboxProtocols&8?teal:muted);
        ImGui::SameLine(0,8);pill(tr(target->bootstrapMailbox?"Bootstrap present":"Bootstrap unconfirmed",target->bootstrapMailbox?"存在 Bootstrap":"未确认 Bootstrap"),target->bootstrapMailbox?teal:muted);
    }else smallNote("Select a slave in the topology tree.","请在左侧拓扑树中选择从站。");
    ImGui::Spacing();
    const float width=ImGui::GetContentRegionAvail().x,height=std::max(1.f,ImGui::GetContentRegionAvail().y-1);
    const float left=(width-14.f)*.515f,right=width-left-14.f;
    fileCard(left,height);ImGui::SameLine(0,14);transferCard(controller,s,right,height);
    ImGui::PopStyleVar();ImGui::PopFont();
}
} // namespace niceecatm::workbench
