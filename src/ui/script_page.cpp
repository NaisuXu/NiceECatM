// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#include "ui/internal.hpp"
#include "platform/paths.hpp"
#include "script/bridge.hpp"
#include "script/runtime.hpp"
#include "TextEditor.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <memory>
#include <string>
#include <vector>

namespace niceecatm::workbench {
namespace {

// The editor deliberately owns no SOEM or backend object.  Runtime operations
// cross the ScriptRuntime/Bridge boundary and enter the existing controller
// queue. Keeping this state local means opening the Scripts page does not
// change a bus state until the user presses Run.
constexpr size_t sourceCapacity=64*1024;
const char* starterSource=R"LUA(-- NiceECatM script
-- Attach to the workbench connection first. Scan only when no bus is connected.
local bus = ecat.current()
local status = bus:status()
if not status.connected then
    local adapters = ecat.adapters()
    assert(#adapters > 0, "no physical adapters")
    bus = ecat.open({ interface = adapters[1].name, cycle_us = 10000 })
    local devices = bus:scan()
    bus:state("all", "PRE-OP")
    log.info("scan complete: " .. #devices .. " device(s)")
else
    log.info("attached to the current bus; state=" .. status.status)
end
-- Signal names are device-specific. Use PDO identity from bus:signals() to
-- avoid ambiguous display names when an ESI exposes duplicates.
-- local signals = bus:signals(1)
-- local value = bus:signal(1, signals[1].index, signals[1].subindex):read()
-- timer.every(100, function() log.info("poll") end)
-- cycle.every(1, function(frame) end) -- every received cycle snapshot
)LUA";

struct ScriptFile {
    std::filesystem::path path;
    std::string name;
    bool draft=false;
    bool dirty=false, loaded=false;
    std::string source;
};
struct ScriptUi {
    bool initialized=false, dirty=false, running=false, renameFocus=false;
    bool editorSync=true;
    int selected=-1, renameIndex=-1;
    std::filesystem::path directory;
    std::vector<ScriptFile> files;
    std::vector<uint64_t> pending;
    uint64_t lastRuntimeLog=0;
    std::unique_ptr<script::Bridge> bridge;
    std::unique_ptr<script::Runtime> runtime;
    std::unique_ptr<TextEditor> editor;
    Controller* controller=nullptr;
    std::mutex runtimeMutex;
    std::string error;
    char name[192]="untitled.lua";
    char rename[192]="";
    char source[sourceCapacity]{};
};
ScriptUi script;

uint64_t hostSubmit(const script::ScriptRequest& request);

std::string pathName(const std::filesystem::path& path) {
    try { return path.filename().u8string(); } catch(...) { return path.filename().string(); }
}

void setText(char* target,size_t capacity,const std::string& value) {
    if(!capacity)return;
    const auto count=std::min(capacity-1,value.size());
    std::memcpy(target,value.data(),count);target[count]=0;
}

void rememberCurrentFile() {
    if(script.selected<0||script.selected>=static_cast<int>(script.files.size()))return;
    auto& file=script.files[static_cast<size_t>(script.selected)];
    file.source=script.source;file.loaded=true;file.dirty=script.dirty||file.draft;
}

void syncEditorFromSource() {
    if(!script.editor)return;
    script.editor->SetLanguageDefinition(TextEditor::LanguageDefinition::Lua());
    script.editor->SetPalette(TextEditor::GetLightPalette());
    script.editor->SetTabSize(4);
    script.editor->SetShowWhitespaces(false);
    script.editor->SetText(std::string(script.source));
    script.editorSync=false;
}

// TextEditor::GetText() serializes its internal sentinel line as a trailing
// newline.  Rebuild from the line array so a file's original final-newline
// state is preserved and loading an unchanged file does not become dirty.
std::string editorSource() {
    if(!script.editor)return {};
    const auto lines=script.editor->GetTextLines();
    std::string text;
    for(size_t n=0;n<lines.size();++n) {
        if(n)text.push_back('\n');
        text+=lines[n];
    }
    return text;
}

// The action row is rendered before the editor card.  Pull the editor text
// once at the beginning of the frame so Run/Save always see the latest
// in-memory text, including edits made immediately before clicking a button.
bool pullEditorToSource() {
    if(!script.editor||script.editorSync){rememberCurrentFile();return true;}
    const std::string text=editorSource();
    if(text.size()>=sourceCapacity) {
        script.dirty=true;rememberCurrentFile();
        script.error="Script exceeds the 64 KiB source limit.";
        return false;
    }
    if(text!=script.source) {
        setText(script.source,sizeof(script.source),text);
        script.dirty=true;
        script.error.clear();
    }
    rememberCurrentFile();
    return true;
}

void addLog(const std::string& line,bool error=false) {
    if(script.controller)script.controller->appendLog("Script: "+line,error);
}

uint64_t immediateResult(script::HostResult result) {
    static std::atomic<uint64_t> next{1};
    const auto id=UINT64_C(0x8000000000000000)|next.fetch_add(1);
    if(script.runtime)script.runtime->complete(id,std::move(result));
    return id;
}

uint64_t hostSubmit(const script::ScriptRequest& request) {
    if(!script.bridge)return 0;
    if(request.kind==script::RequestKind::ReadSignal) {
        const auto info=request.signal.empty()?script.bridge->readSignal(request.slave,request.index,request.subindex):script.bridge->readSignal(request.slave,request.signal);
        if(!info.valid)return immediateResult({false,info.error.empty()?"Signal is unavailable.":info.error,{}});
        return immediateResult({true,info.value,{}});
    }
    std::string error;const auto id=script.bridge->submit(request,&error);
    if(!id)return immediateResult({false,error.empty()?"Host rejected the script request.":error,{}});
    {std::lock_guard<std::mutex> lock(script.runtimeMutex);script.pending.push_back(id);}
    return id;
}

void refreshFiles() {
    rememberCurrentFile();
    const auto selectedPath=(script.selected>=0&&script.selected<static_cast<int>(script.files.size()))?script.files[static_cast<size_t>(script.selected)].path:std::filesystem::path{};
    try {
        std::vector<ScriptFile> refreshed;
        std::filesystem::create_directories(script.directory);
        for(const auto& entry:std::filesystem::directory_iterator(script.directory)) {
            if(!entry.is_regular_file())continue;
            const auto extension=entry.path().extension().u8string();
            if(extension!=".lua"&&extension!=".LUA")continue;
            const auto previous=std::find_if(script.files.begin(),script.files.end(),[&](const ScriptFile& file){return file.path==entry.path();});
            if(previous!=script.files.end())refreshed.push_back(*previous);
            else refreshed.push_back({entry.path(),pathName(entry.path())});
        }
        // Refreshing the disk list must not discard another file's edits.
        // Keep virtual drafts and dirty buffers even if their disk file was
        // removed externally; Save can write that buffer back to its path.
        for(const auto& file:script.files)if(file.draft||file.dirty) {
            const bool listed=std::any_of(refreshed.begin(),refreshed.end(),[&](const ScriptFile& other){return other.path==file.path;});
            if(!listed)refreshed.push_back(file);
        }
        script.files=std::move(refreshed);
        std::sort(script.files.begin(),script.files.end(),[](const ScriptFile& a,const ScriptFile& b){return a.name<b.name;});
        script.selected=-1;
        if(!selectedPath.empty())for(size_t n=0;n<script.files.size();++n)if(script.files[n].path==selectedPath){script.selected=static_cast<int>(n);break;}
        if(script.selected<0&&!script.files.empty())script.selected=0;
    } catch(const std::exception& exception) { script.error=exception.what();addLog(script.error,true); }
}

bool loadFile(ScriptFile& file,bool reload=false) {
    if(!file.draft&&(!file.loaded||reload)) {
        std::ifstream input(file.path,std::ios::binary);
        if(!input) { script.error="Unable to open script: "+file.name;addLog(script.error,true);return false; }
        input.seekg(0,std::ios::end);const auto length=input.tellg();input.seekg(0,std::ios::beg);
        if(length<0||static_cast<size_t>(length)>=sourceCapacity) { script.error="Script exceeds the 64 KiB source limit.";addLog(script.error,true);return false; }
        std::string text(static_cast<size_t>(length),'\0');input.read(text.data(),length);
        if(!input) { script.error="Unable to read script: "+file.name;addLog(script.error,true);return false; }
        file.source=std::move(text);file.loaded=true;file.dirty=false;
    }
    setText(script.source,sizeof(script.source),file.source);
    setText(script.name,sizeof(script.name),file.name);
    script.editorSync=true;script.dirty=file.dirty||file.draft;script.error.clear();
    addLog("Loaded "+file.name);
    return true;
}

bool selectFile(int index) {
    if(index==script.selected)return true;
    if(index<0||index>=static_cast<int>(script.files.size())||!pullEditorToSource())return false;
    if(!loadFile(script.files[static_cast<size_t>(index)]))return false;
    script.selected=index;return true;
}

std::filesystem::path newDraftPath() {
    const auto occupied=[&](const std::filesystem::path& path) {
        if(std::filesystem::exists(path))return true;
        return std::any_of(script.files.begin(),script.files.end(),[&](const ScriptFile& file){return file.path==path;});
    };
    auto candidate=script.directory/"untitled.lua";
    if(!occupied(candidate))return candidate;
    for(unsigned int suffix=1;;++suffix) {
        candidate=script.directory/("untitled-"+std::to_string(suffix)+".lua");
        if(!occupied(candidate))return candidate;
    }
}

bool saveFile() {
    if(!pullEditorToSource()) {
        if(script.error.empty())script.error="The script could not be saved.";
        addLog(script.error,true);
        return false;
    }
    std::string fileName=script.name;
    if(fileName.empty()) { script.error="Enter a script name.";addLog(script.error,true);return false; }
    if(fileName.find(".lua")==std::string::npos)fileName+=".lua";
    // The name field is intentionally restricted to one filename.  This
    // prevents a script from escaping the portable scripts directory.
    const auto leaf=std::filesystem::path(std::filesystem::u8path(fileName)).filename();
    if(leaf.empty()||leaf=="."||leaf=="..") { script.error="Invalid script name.";addLog(script.error,true);return false; }
    auto path=script.directory/leaf;
    const bool savingSelected=script.selected>=0&&script.selected<static_cast<int>(script.files.size())&&script.files[static_cast<size_t>(script.selected)].path==path;
    const bool savingDraft=savingSelected&&script.files[static_cast<size_t>(script.selected)].draft;
    if(std::filesystem::exists(path)&&(!savingSelected||savingDraft)) {
        // A newly created script starts as untitled.lua. Never truncate an
        // existing file when that draft is saved; choose the next free name.
        if(savingDraft&&leaf.filename().u8string().rfind("untitled",0)==0) {
            const auto stem=leaf.stem().u8string();const auto extension=leaf.extension().u8string();
            for(unsigned int suffix=1;;++suffix) {
                const auto candidate=script.directory/(stem+"-"+std::to_string(suffix)+extension);
                bool draftPath=false;for(const auto& file:script.files)if(file.draft&&file.path==candidate){draftPath=true;break;}
                if(!std::filesystem::exists(candidate)&&!draftPath){path=candidate;break;}
            }
        } else {
            script.error="A script with that name already exists.";addLog(script.error,true);return false;
        }
    }
    std::ofstream output(path,std::ios::binary|std::ios::trunc);
    if(!output) { script.error="Unable to save script: "+pathName(path);addLog(script.error,true);return false; }
    output.write(script.source,static_cast<std::streamsize>(std::strlen(script.source)));
    if(!output) { script.error="The script could not be written.";addLog(script.error,true);return false; }
    setText(script.name,sizeof(script.name),pathName(path));script.dirty=false;script.error.clear();
    if(script.selected>=0&&script.selected<static_cast<int>(script.files.size())) {
        auto& file=script.files[static_cast<size_t>(script.selected)];
        file.path=path;file.name=pathName(path);file.draft=false;file.dirty=false;file.loaded=true;file.source=script.source;
    }
    refreshFiles();
    for(size_t n=0;n<script.files.size();++n)if(script.files[n].path==path)script.selected=static_cast<int>(n);
    addLog("Saved "+pathName(path));return true;
}

bool renameFile(int index,const std::string& requested) {
    if(index<0||index>=static_cast<int>(script.files.size()))return false;
    if(!pullEditorToSource())return false;
    std::string fileName=requested;
    if(fileName.empty()) { script.error="Enter a script filename.";addLog(script.error,true);return false; }
    if(fileName.find_first_of("/\\")!=std::string::npos) {
        script.error="Script names cannot contain path separators.";addLog(script.error,true);return false;
    }
    auto leaf=std::filesystem::path(std::filesystem::u8path(fileName));
    if(leaf.empty()||leaf=="."||leaf=="..") {
        script.error="Invalid script filename.";addLog(script.error,true);return false;
    }
    const auto extension=leaf.extension().u8string();
    if(extension.empty())leaf+=".lua";
    else if(extension!=".lua"&&extension!=".LUA") {
        script.error="Script filenames must use the .lua extension.";addLog(script.error,true);return false;
    }
    auto& file=script.files[static_cast<size_t>(index)];
    const auto oldPath=file.path;
    const auto newPath=script.directory/leaf.filename();
    if(newPath==oldPath) {script.renameIndex=-1;return true;}
    if(std::filesystem::exists(newPath)||std::any_of(script.files.begin(),script.files.end(),[&](const ScriptFile& other){return other.path==newPath;})) {
        script.error="A script with that name already exists.";addLog(script.error,true);return false;
    }
    if(file.draft) {
        file.path=newPath;file.name=pathName(newPath);script.renameIndex=-1;
        if(script.selected==index)setText(script.name,sizeof(script.name),file.name);
        script.error.clear();addLog("Renamed draft to "+file.name);return true;
    }
    std::error_code error;
    std::filesystem::rename(oldPath,newPath,error);
    if(error) {
        script.error="Unable to rename script: "+error.message();addLog(script.error,true);return false;
    }
    const auto oldName=pathName(oldPath),newName=pathName(newPath);
    file.path=newPath;file.name=newName;
    refreshFiles();
    for(size_t n=0;n<script.files.size();++n)if(script.files[n].path==newPath)script.selected=static_cast<int>(n);
    if(script.selected>=0&&script.files[static_cast<size_t>(script.selected)].path==newPath)setText(script.name,sizeof(script.name),newName);
    script.renameIndex=-1;script.error.clear();addLog("Renamed "+oldName+" to "+newName);return true;
}

void ensureInitialized(Controller* controller) {
    if(controller)script.controller=controller;
    if(!script.initialized) {
        script.initialized=true;
        try { script.directory=executableDirectory()/"scripts";std::filesystem::create_directories(script.directory); }
        catch(const std::exception& exception) { script.error=exception.what();addLog(script.error,true); }
        refreshFiles();
        if(script.files.empty()) {
            const auto draftPath=newDraftPath();
            script.files.push_back({draftPath,pathName(draftPath),true,true,true,starterSource});
            script.selected=0;
        }
        if(script.selected>=0)loadFile(script.files[script.selected]);
    }
    if(!script.editor)script.editor=std::make_unique<TextEditor>();
    if(script.editorSync)syncEditorFromSource();
    if(controller&&!script.runtime) {
        script.bridge=std::make_unique<script::Bridge>(*controller);
        script.runtime=std::make_unique<script::Runtime>(script::HostCallbacks{
            [](const script::ScriptRequest& request){return hostSubmit(request);},
            [](const script::ScriptRequest& request,script::HostResult& result){return script.bridge&&script.bridge->execute(request,result);}
        });
    }
}

void cancelPendingRequests() {
    if(!script.bridge)return;
    std::vector<uint64_t> pending;
    { std::lock_guard<std::mutex> lock(script.runtimeMutex);pending=script.pending;script.pending.clear(); }
    for(const auto id:pending)script.bridge->cancel(id);
}

void pumpRuntime(const Snapshot& snapshot) {
    if(!script.runtime||!script.bridge)return;
    script.runtime->updateSnapshot(snapshot);
    std::vector<uint64_t> pending;
    { std::lock_guard<std::mutex> lock(script.runtimeMutex);pending=script.pending; }
    std::vector<uint64_t> completed;
    for(const auto id:pending) {
        Result result;
        if(!script.bridge->result(id,result))continue;
        script.runtime->complete(id,{result.ok,result.message,result.data,result.adapters});completed.push_back(id);
    }
    if(!completed.empty()) {
        std::lock_guard<std::mutex> lock(script.runtimeMutex);
        script.pending.erase(std::remove_if(script.pending.begin(),script.pending.end(),[&](uint64_t id){return std::find(completed.begin(),completed.end(),id)!=completed.end();}),script.pending.end());
    }
    const auto view=script.runtime->snapshot();
    script.running=view.status==script::Status::Running||view.status==script::Status::Waiting;
    // Runtime errors are retained in snapshots after execution stops. Only
    // forward new log events: editor actions clear script.error, and using
    // that mutable UI text to deduplicate snapshots replays old failures.
    for(const auto& event:view.logs)if(event.sequence>script.lastRuntimeLog){
        const bool error=event.level==script::LogLevel::Error;
        addLog((event.time.empty()?std::string():event.time+" ")+event.message,error);
        script.lastRuntimeLog=std::max(script.lastRuntimeLog,event.sequence);
    }
    if(view.status==script::Status::Error&&!view.error.empty()) {
        script.error=view.error;
    }
}

void insertSnippet(const char* snippet) {
    size_t current=std::strlen(script.source);const auto extra=std::strlen(snippet);
    if(current+extra+2>=sizeof(script.source)) { script.error="The source buffer is full.";addLog(script.error,true);return; }
    if(current&&script.source[current-1]!='\n')script.source[current++]='\n';
    std::memcpy(script.source+current,snippet,extra);script.source[current+extra]=0;script.dirty=true;script.error.clear();script.editorSync=true;rememberCurrentFile();
}

void snippetButton(const char* en,const char* zh,const char* snippet) {
    if(ImGui::Button(tr(en,zh),{-1,0}))insertSnippet(snippet);
}

// A small dependency-free formatter is enough for commissioning scripts. It
// only normalizes indentation. Tokens inside strings and comments are ignored,
// so words such as "end" in a log message cannot change the block structure.
std::vector<std::string> luaFormatTokens(const std::string& line) {
    std::vector<std::string> tokens;
    for(size_t n=0;n<line.size();) {
        const char c=line[n];
        if(c=='-'&&n+1<line.size()&&line[n+1]=='-')break;
        if(c=='\''||c=='"') {
            const char quote=c;bool escaped=false;++n;
            while(n<line.size()) {if(!escaped&&line[n]==quote){++n;break;}if(!escaped&&line[n]=='\\')escaped=true;else escaped=false;++n;}
            continue;
        }
        if(c=='['&&n+1<line.size()&&line[n+1]=='[') {
            const auto close=line.find("]]",n+2);n=close==std::string::npos?line.size():close+2;continue;
        }
        if(std::isalpha(static_cast<unsigned char>(c))||c=='_') {
            const size_t begin=n++;while(n<line.size()&&(std::isalnum(static_cast<unsigned char>(line[n]))||line[n]=='_'))++n;
            tokens.push_back(line.substr(begin,n-begin));continue;
        }
        ++n;
    }
    return tokens;
}

bool luaHasToken(const std::vector<std::string>& tokens,const char* token) {
    return std::find(tokens.begin(),tokens.end(),token)!=tokens.end();
}

bool formatLuaSource(char* source,size_t capacity,std::string& error) {
    if(!source||capacity<2){error="The source buffer is too small.";return false;}
    const std::string input(source);const bool trailingNewline=!input.empty()&&input.back()=='\n';
    std::vector<std::string> lines;size_t start=0;
    while(start<input.size()||(!input.empty()&&start==input.size())) {
        const size_t end=input.find('\n',start);
        lines.push_back(input.substr(start,end==std::string::npos?std::string::npos:end-start));
        if(end==std::string::npos)break;
        start=end+1;if(start==input.size())break;
    }
    auto trim=[](const std::string& value) {
        const auto first=value.find_first_not_of(" \t\r");
        if(first==std::string::npos)return std::string{};
        const auto last=value.find_last_not_of(" \t\r");return value.substr(first,last-first+1);
    };
    std::string output;int indent=0;
    for(const auto& original:lines) {
        const std::string line=trim(original);
        if(line.empty()){output.push_back('\n');continue;}
        const auto tokens=luaFormatTokens(line);
        const bool leadingElse=!tokens.empty()&&(tokens.front()=="end"||tokens.front()=="else"||tokens.front()=="elseif"||tokens.front()=="until");
        const int current=std::max(0,indent-(leadingElse?1:0));
        output.append(static_cast<size_t>(current*4),' ');output+=line;output.push_back('\n');

        int opens=0,closes=0;
        const bool hasThen=luaHasToken(tokens,"then"),hasDo=luaHasToken(tokens,"do");
        const bool branchLine=!tokens.empty()&&(tokens.front()=="else"||tokens.front()=="elseif");
        for(size_t n=0;n<tokens.size();++n) {
            const auto& token=tokens[n];
            if(token=="end"||token=="until")++closes;
            else if(token=="function"||token=="repeat")++opens;
            else if(token=="if"&&!branchLine&&hasThen)++opens;
            else if((token=="for"||token=="while")&&hasDo)++opens;
            else if(token=="do"&&!luaHasToken(tokens,"for")&&!luaHasToken(tokens,"while"))++opens;
        }
        // else/elseif closes the previous branch and opens its replacement;
        // the leading dedent above already accounts for the close.
        if(!tokens.empty()&&(tokens.front()=="else"||tokens.front()=="elseif"))++opens;
        const int consumedClose=leadingElse?1:0;
        const int remainingCloses=std::max(0,closes-consumedClose);
        indent=std::max(0,current+opens-remainingCloses);
    }
    if(!trailingNewline&&!output.empty())output.pop_back();
    if(output.size()>=capacity){error="Formatted source exceeds the 64 KiB limit.";return false;}
    std::memcpy(source,output.data(),output.size());source[output.size()]=0;return true;
}

void editorHeader() {
    // Use explicit positions so the icon/title, limit hint and action button
    // share one vertical center instead of relying on each widget's baseline.
    const ImVec2 origin=ImGui::GetCursorPos();
    const float rowY=origin.y;
    const float contentRight=origin.x+ImGui::GetContentRegionAvail().x;
    const char* limit=tr("UTF-8 · 64 KiB limit","UTF-8 · 上限 64 KiB");
    const float buttonWidth=72.f;
    const float limitWidth=ImGui::CalcTextSize(limit).x;
    const float buttonX=contentRight-buttonWidth;
    const float limitX=buttonX-10.f-limitWidth;
    ImGui::SetCursorPos({origin.x,rowY+5.f});icon(Icon::Logs,14,ImGui::GetColorU32(teal));
    ImGui::SetCursorPos({origin.x+22.f,rowY+4.f});ImGui::TextColored(ink,"%s",tr("Lua editor","Lua 编辑器"));
    ImGui::SetCursorPos({limitX,rowY+5.f});ImGui::TextDisabled("%s",limit);
    ImGui::SetCursorPos({buttonX,rowY});
    if(ImGui::Button(tr("Format","格式化"),{buttonWidth,24})) {
        std::string error;
        if(formatLuaSource(script.source,sizeof(script.source),error)){script.dirty=true;script.error.clear();script.editorSync=true;rememberCurrentFile();addLog("Formatted script");}
        else {script.error=error.empty()?"Unable to format Lua source.":error;addLog(script.error,true);}
    }
    ImGui::SetCursorPos({origin.x,rowY+28.f});ImGui::Separator();ImGui::SetCursorPosY(rowY+36.f);
}

void scriptList() {
    cardHeader(tr("Scripts","脚本"),Icon::Book);
    // Keep the panel title fixed; only the script list body scrolls.
    ImGui::BeginChild("script-list-scroll",{0,0},ImGuiChildFlags_None);
    ImGui::Spacing();
    if(ImGui::Button(tr("New","新建"),{-1,0})) {
        if(!pullEditorToSource()){addLog(script.error,true);ImGui::EndChild();return;}
        const auto draftPath=newDraftPath();
        const std::string draftName=pathName(draftPath);
        const std::string draftSource="-- New NiceECatM script\n";
        script.files.push_back({draftPath,draftName,true,true,true,draftSource});
        std::sort(script.files.begin(),script.files.end(),[](const ScriptFile& a,const ScriptFile& b){return a.name<b.name;});
        script.selected=-1;for(size_t n=0;n<script.files.size();++n)if(script.files[n].path==draftPath){script.selected=static_cast<int>(n);break;}
        script.renameIndex=-1;script.renameFocus=false;setText(script.name,sizeof(script.name),draftName);setText(script.source,sizeof(script.source),draftSource);script.editorSync=true;
        script.dirty=true;script.error.clear();addLog("New script draft: "+draftName);
    }
    ImGui::Spacing();
    int renameRequest=-1;std::string renameText;bool renameCancel=false;
    for(size_t n=0;n<script.files.size();++n) {
        ImGui::PushID(static_cast<int>(n));
        if(script.renameIndex==static_cast<int>(n)) {
            ImGui::SetNextItemWidth(-1);
            if(script.renameFocus) { ImGui::SetKeyboardFocusHere();script.renameFocus=false; }
            const bool submitted=ImGui::InputText("##script-rename",script.rename,sizeof(script.rename),ImGuiInputTextFlags_EnterReturnsTrue|ImGuiInputTextFlags_AutoSelectAll);
            const bool cancel=ImGui::IsItemFocused()&&ImGui::IsKeyPressed(ImGuiKey_Escape);
            if(cancel)renameCancel=true;
            else if(submitted||ImGui::IsItemDeactivatedAfterEdit()) {renameRequest=static_cast<int>(n);renameText=script.rename;}
        } else {
            std::string label=script.files[n].name;
            if(script.files[n].dirty||script.files[n].draft)label="* "+label;
            if(ImGui::Selectable(label.c_str(),script.selected==static_cast<int>(n))) {
                selectFile(static_cast<int>(n));
            }
            if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",tr("Double-click to rename","双击重命名"));
            if(ImGui::IsItemHovered()&&ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if(selectFile(static_cast<int>(n))){setText(script.rename,sizeof(script.rename),script.files[n].name);script.renameIndex=static_cast<int>(n);script.renameFocus=true;}
            }
        }
        ImGui::PopID();
    }
    if(renameCancel)script.renameIndex=-1;
    else if(renameRequest>=0)renameFile(renameRequest,renameText);
    if(script.files.empty())ImGui::TextDisabled("%s",tr("No .lua files yet","尚无 .lua 文件"));
    ImGui::EndChild();
}

void editorCard(float height) {
    (void)height;
    editorHeader();
    if(!script.editor)script.editor=std::make_unique<TextEditor>();
    if(script.editorSync)syncEditorFromSource();
    ImGui::PushFont(fonts.mono?fonts.mono:ImGui::GetFont());
    const ImVec2 editorSize={-1,std::max(180.f,ImGui::GetContentRegionAvail().y)};
    script.editor->Render("##lua-text-editor",editorSize,false);
    ImGui::PopFont();
    if(script.editor->IsTextChanged()) {
        const std::string text=editorSource();
        if(text.size()>=sourceCapacity) {
            script.error="Script exceeds the 64 KiB source limit.";addLog(script.error,true);
            // Keep the draft marked as unsaved so Run cannot fall back to the
            // previous source after the editor is reset to its last valid text.
            script.dirty=true;
            script.editorSync=true;
            rememberCurrentFile();
        } else {
            setText(script.source,sizeof(script.source),text);script.dirty=true;script.error.clear();
            rememberCurrentFile();
        }
    }
}

void apiCard() {
    cardHeader(tr("API snippets","API 片段"),Icon::Book);
    // Keep the API heading fixed; only the snippet body scrolls.
    ImGui::BeginChild("api-snippets-scroll",{0,0},ImGuiChildFlags_None);
    ImGui::TextDisabled("%s",tr("Insert a focused operation into the editor.","插入聚焦操作到编辑器。"));
    ImGui::Spacing();
    // Keep each insertion focused on a single operation. The card scrolls
    // independently so the larger reference does not shrink the editor.
    snippetButton("List physical adapters","列出有线网卡","for index, adapter in ipairs(ecat.adapters()) do\n    log.info(index .. \": \" .. adapter.description .. \" | \" .. adapter.name)\nend\n");
        snippetButton("Open hardware bus","打开硬件总线","local adapters = ecat.adapters()\nassert(#adapters > 0, \"no wired Ethernet adapters\")\nlocal bus = ecat.open({ interface = adapters[1].name, simulation = false, cycle_us = 10000 })\nlocal devices = bus:scan()\n");
    snippetButton("Attach current bus","附加当前总线","local bus = ecat.current()\n");
    snippetButton("Scan devices","扫描设备","local devices = ecat.current():scan()\nlog.info(\"devices: \" .. #devices)\n");
    snippetButton("Disconnect","断开连接","ecat.current():disconnect()\n");
    snippetButton("Bus status / WKC","总线状态 / WKC","local status = ecat.current():status()\nlog.info(string.format(\"%s | running=%s | WKC=%d/%d\", status.state, tostring(status.running), status.wkc, status.expected_wkc))\n");
    snippetButton("Set INIT","切换 INIT","ecat.current():state(\"all\", \"INIT\")\n");
    snippetButton("Set PRE-OP","切换 PRE-OP","ecat.current():state(\"all\", \"PRE-OP\")\n");
    snippetButton("Set SAFE-OP","切换 SAFE-OP","ecat.current():state(\"all\", \"SAFE-OP\")\n");
    snippetButton("Set OP","切换 OP","ecat.current():state(\"all\", \"OP\")\n");
    snippetButton("Set one slave PRE-OP","单个从站切换 PRE-OP","ecat.current():state(1, \"PRE-OP\")\n");
    snippetButton("Target cycle (10 ms)","目标周期（10 ms）","-- Change while stopped and all slaves are INIT / PRE-OP.\necat.current():cycle(10000) -- microseconds\n");
    snippetButton("Stop cyclic I/O","停止周期 I/O","ecat.current():stop()\n");
    snippetButton("Resume cyclic I/O","恢复周期 I/O","-- Requires SAFE-OP / OP.\necat.current():start()\n");
    snippetButton("List mapped signals","列出映射信号","for _, item in ipairs(ecat.current():signals(1)) do\n    log.info(string.format(\"%s 0x%04X:%02X %s (%s, %d bit)\", item.input and \"IN\" or \"OUT\", item.index, item.subindex, item.name, item.type, item.bit_length))\nend\n");
    snippetButton("Read signal","读取信号","local value = ecat.current():signal(1, \"Input\"):read()\nlog.info(value)\n");
    snippetButton("Write signal = 0","写入信号 = 0","ecat.current():signal(1, \"Output\"):write(0)\n");
    snippetButton("Read PDO by index","按索引读取 PDO","-- Use the Index / SubIndex from bus:signals().\nlocal value = ecat.current():signal(1, 0x3001, 1):read()\nlog.info(value)\n");
    snippetButton("Write PDO by index","按索引写入 PDO","-- Replace with a mapped output Index / SubIndex.\necat.current():signal(1, 0x3101, 1):write(0)\n");
    snippetButton("Read ESC AL status","读取 ESC AL 状态","-- Stop cyclic I/O before synchronous ESC access.\nlocal bytes = ecat.current():register(1, 0x0130):read(2)\nlog.info(bytes)\n");
    snippetButton("Write ESC AL control","写入 ESC AL 控制","-- Stop cyclic I/O first; 02 00 requests PRE-OP.\necat.current():register(1, 0x0120):write(\"02 00\")\n");
    snippetButton("Read SDO device name","读取 SDO 设备名称","-- Requires CoE in PRE-OP / SAFE-OP / OP with cyclic I/O stopped.\nlocal bytes = ecat.current():sdo(1, 0x1008, 0):read(128)\nlog.info(bytes) -- original hex bytes\n");
    snippetButton("Write SDO bytes","写入 SDO 字节","-- Stop cyclic I/O; replace with a writable CoE object and its bytes.\necat.current():sdo(1, 0x8000, 0x11):write(\"00 00\")\n");
    snippetButton("One-shot timer (1 s)","一次性定时器（1 s）","timer.after(1000, function()\n    log.info(\"one second elapsed\")\nend)\n");
    snippetButton("Timer (100 ms)","定时器（100 ms）","timer.every(100, function()\n    log.info(\"poll\")\nend)\n");
    snippetButton("Cancel timer","取消定时器","local timerId = timer.every(100, function()\n    log.info(\"poll\")\nend)\ntimer.after(1000, function()\n    timer.cancel(timerId)\nend)\n");
    snippetButton("Cycle callback (1 cycle)","周期回调（每 1 个周期）","cycle.every(1, function(frame)\n    -- runs once per received cycle snapshot\nend)\n");
    snippetButton("Cancel cycle callback","取消周期回调","local seen = 0\nlocal cycleId\ncycleId = cycle.every(1, function(frame)\n    seen = seen + 1\n    log.info(\"cycle \" .. frame.cycles)\n    if seen >= 10 then cycle.cancel(cycleId) end\nend)\n");
    snippetButton("Periodic status (1 s)","定期状态检查（1 s）","local bus = ecat.current()\ntimer.every(1000, function()\n    local status = bus:status()\n    log.info(string.format(\"%s | WKC=%d/%d\", status.state, status.wkc, status.expected_wkc))\nend)\n");
    snippetButton("Handle API errors","处理 API 错误","local ok, value = pcall(function()\n    return ecat.current():signal(1, 0x3001, 1):read()\nend)\nif ok then log.info(value) else log.error(value) end\n");
    ImGui::EndChild();
}

void actions(Controller& controller,const Snapshot& snapshot) {
    if(ImGui::Button(tr("Open / Reload","打开 / 重新加载"))) {
        if(script.selected>=0&&script.selected<static_cast<int>(script.files.size()))loadFile(script.files[script.selected],true);
        else { script.error="Select a script from the list first.";addLog(script.error,true); }
    }
    ImGui::SameLine();
    if(ImGui::Button(tr("Save","保存")))saveFile();
    ImGui::SameLine();
    ImGui::BeginDisabled(snapshot.connection.mode==ConnectionMode::Programming||ui.modeRequest!=0);
    const bool run=ImGui::Button(tr("Run","运行"));
    ImGui::EndDisabled();
    if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)&&snapshot.connection.mode==ConnectionMode::Programming)ImGui::SetTooltip("%s",tr("Scripts are unavailable in Programming mode.","编程模式下不能运行脚本。"));
    if(run) {
        if(!pullEditorToSource()) {
            addLog(script.error.empty()?"The script contains too much text.":script.error,true);
            return;
        }
        if(script.dirty) {
            script.error=tr("Save the script before running.","运行前请先保存脚本。");
            addLog(script.error,true);
            return;
        }
        if(!script.runtime)ensureInitialized(&controller);
        cancelPendingRequests();
        if(script.runtime&&script.runtime->load(script.name,std::string(script.source))) {
            controller.setScriptRunning(true);
            if(controller.snapshot().connection.mode==ConnectionMode::Programming){controller.setScriptRunning(false);addLog(tr("Scripts are unavailable in Programming mode.","编程模式下不能运行脚本。"),true);return;}
            if(script.runtime->start()){script.running=true;script.error.clear();addLog("Script started");}
            else {controller.setScriptRunning(false);script.error="The script runtime could not start.";addLog(script.error,true);}
        } else {
            const auto view=script.runtime?script.runtime->snapshot():script::View{};
            script.error=view.error.empty()?"The script could not be loaded.":view.error;addLog(script.error,true);
        }
    }
    ImGui::SameLine();
    if(ImGui::Button(tr("Stop","停止"))) {
        cancelPendingRequests();
        if(script.runtime)script.runtime->stop();
        script.running=false;controller.setScriptRunning(false);addLog("Script stopped");
    }
    ImGui::SameLine(0,8);
    const float statusY=ImGui::GetCursorPosY();ImGui::SetCursorPosY(statusY+2.0f);
    pill(script.running?tr("Running","运行中"):tr("Stopped","已停止"),script.running?green:muted);
}

} // namespace

void scriptTick(Controller& controller,const Snapshot& snapshot) {
    script.controller=&controller;
    controller.setScriptRunning(script.running);
    if(script.initialized) {
        controller.enableScriptSnapshots(script.running);
        pumpRuntime(snapshot);
        controller.enableScriptSnapshots(script.running);
    }
    controller.setScriptRunning(script.running);
}

bool scriptRunning() {return script.running;}

void scriptPage(Controller& controller,const Snapshot& snapshot) {
    ensureInitialized(&controller);
    pullEditorToSource();
    const float available=ImGui::GetContentRegionAvail().y;
    // Script failures are already appended to the shared log window.  Keep
    // the action row compact and avoid repeating the same error beside it.
    ImGui::BeginGroup();actions(controller,snapshot);ImGui::EndGroup();
    ImGui::Spacing();
    const float editorHeight=std::max(260.f,available-42.f);
    if(ImGui::BeginTable("script-layout",3,ImGuiTableFlags_SizingStretchProp|ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("script-list",ImGuiTableColumnFlags_WidthFixed,225.f);ImGui::TableSetupColumn("editor",ImGuiTableColumnFlags_WidthStretch,2.f);ImGui::TableSetupColumn("api",ImGuiTableColumnFlags_WidthFixed,245.f);
        ImGui::TableNextColumn();beginCard("script-list-card",{0,editorHeight});scriptList();endCard();
        ImGui::TableNextColumn();beginCard("script-editor-card",{0,editorHeight});
        editorCard(editorHeight);endCard();
        ImGui::TableNextColumn();beginCard("script-api-card",{0,editorHeight});apiCard();endCard();
        ImGui::EndTable();
    }
}

void shutdownScriptPage() {
    cancelPendingRequests();
    if(script.runtime)script.runtime->stop();
    if(script.controller)script.controller->setScriptRunning(false);
    script.runtime.reset();
    script.bridge.reset();
}
} // namespace niceecatm::workbench

namespace niceecatm {
void shutdownScripts() { workbench::shutdownScriptPage(); }
}
