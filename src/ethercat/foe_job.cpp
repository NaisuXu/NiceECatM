// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#include "ethercat/foe_job.hpp"
#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace niceecatm::foe {
bool validBootstrapMailbox(uint16_t txOffset,uint16_t txLength,uint16_t rxOffset,uint16_t rxLength,size_t capacity) {
    const auto valid=[capacity](uint16_t offset,uint16_t length){return offset>=0x1000&&length>12&&length<=capacity&&uint32_t(offset)+length<=65536;};
    return valid(txOffset,txLength)&&valid(rxOffset,rxLength)&&
           (uint32_t(txOffset)+txLength<=rxOffset||uint32_t(rxOffset)+rxLength<=txOffset);
}
namespace {
// Unknown/vendor codes retain their raw value; do not infer their meaning
// from the request that failed.
const char* errorDescription(uint32_t code) {
    switch(code) {
    case 0x8000:return "Unspecified slave error.";
    case 0x8001:case 0x800f:return "File not found; check the device filename.";
    case 0x8002:return "File access denied.";
    case 0x8003:return "Slave storage or memory is full.";
    case 0x8004:return "Illegal FoE operation.";
    case 0x8005:return "Invalid FoE packet number.";
    case 0x8006:return "File already exists.";
    case 0x8007:return "No user is available.";
    case 0x8008:return "FoE requires BOOT state.";
    case 0x8009:return "File is not permitted in BOOT; check the device filename.";
    case 0x800a:return "Invalid password or insufficient rights.";
    case 0x800b:return "Slave programming failed.";
    case 0x800c:return "File checksum is invalid.";
    case 0x800d:return "Firmware is incompatible with the slave hardware.";
    case 0x8010:return "File header is missing or invalid.";
    case 0x8011:return "Slave flash access failed.";
    case 0x8012:return "File format is incompatible.";
    default:return "Unknown or vendor-specific error; consult the slave documentation.";
    }
}
std::FILE* temporarySource() {
#ifdef _WIN32
    // The Windows CRT tmpfile() uses the drive root, which may require admin.
    // Open an exclusive private file in the user's temp directory instead.
    std::error_code issue;const auto directory=std::filesystem::temp_directory_path(issue);
    if(issue)return nullptr;
    static std::atomic<uint64_t> sequence{0};
    for(unsigned attempt=0;attempt<16;++attempt) {
        const auto path=directory/(L"NiceECatM-FoE-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L"-"+std::to_wstring(++sequence)+L".tmp");
        HANDLE handle=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,nullptr);
        if(handle==INVALID_HANDLE_VALUE)continue;
        const int fd=_open_osfhandle(reinterpret_cast<intptr_t>(handle),_O_RDWR|_O_BINARY);
        if(fd<0){CloseHandle(handle);return nullptr;}
        std::FILE* file=_fdopen(fd,"w+b");if(!file)_close(fd);
        return file;
    }
    return nullptr;
#else
    return std::tmpfile();
#endif
}
class Job {
    Wire wire_;
    const FoeOptions& options_;
    const FoeControl& control_;
    FoeProgress progress_;
    uint64_t started_=0,progressAt_=0,responseAt_=0,reportedAt_=0;
    std::ifstream input_;
    std::FILE* source_=nullptr;
    std::FILE* output_=nullptr;
    std::filesystem::path destination_,temporary_;
    bool temporaryOwned_=false;
    std::string failure_;
    Packet pending_;
    unsigned retries_=0;

    uint64_t now() const {return wire_.nowMs();}
    void report(FoeStage stage,const std::string& detail,bool force=false) {
        const auto time=now();
        force=force||stage!=progress_.stage;
        progress_.stage=stage;progress_.detail=detail;
        progress_.elapsedSeconds=double(time-started_)/1000;
        progress_.bytesPerSecond=progress_.elapsedSeconds>0?double(progress_.completed)/progress_.elapsedSeconds:0;
        if(control_.progress&&(force||time-reportedAt_>=100)) {reportedAt_=time;control_.progress(progress_);}
    }
    bool active() {
        if(!failure_.empty())return false;
        if(control_.cancelled&&control_.cancelled())failure_="FoE cancelled locally; accepted slave data is not rolled back.";
        else if(now()-started_>=uint64_t(options_.totalTimeoutMs))failure_="FoE total timeout exceeded.";
        else if(now()-progressAt_>=uint64_t(options_.noProgressTimeoutMs))failure_="FoE no-progress timeout exceeded.";
        return failure_.empty();
    }
    bool send() {
        for(unsigned attempt=0;attempt<=unsigned(options_.retries)&&active();++attempt) {
            if(wire_.send(pending_,20)) {responseAt_=now();return active();}
            wire_.waitMs(1);
        }
        if(failure_.empty())failure_="FoE mailbox send failed; the last packet may have reached the slave.";
        return false;
    }
    bool receive(Packet& packet) {
        while(active()) {
            const int received=wire_.receive(packet,20);
            if(!active())return false;
            if(received<0) {failure_="FoE link or mailbox transport is unavailable.";return false;}
            if(received>0) {
                if(packet.data.size()>wire_.rxPayload) {failure_="FoE response exceeds the slave mailbox payload capacity.";return false;}
                if(packet.opcode==Opcode::Error) {
                    progress_.errorCode=packet.number;
                    std::ostringstream message;message<<"FoE error 0x"<<std::uppercase<<std::hex<<std::setw(8)<<std::setfill('0')<<packet.number
                        <<": "<<errorDescription(packet.number);
                    std::string text;for(uint8_t byte:packet.data) {if(!byte)break;if(byte>=32&&byte!=127)text+=char(byte);}
                    if(!text.empty())message<<" | Slave message: "<<text;
                    failure_=message.str();return false;
                }
                if(packet.opcode!=Opcode::Busy)return true;
                report(FoeStage::Busy,"Slave reports BUSY; waiting within the no-progress deadline.");
                // BUSY is not a successful ACK and never advances the file or
                // resets either the no-progress or total-transfer deadline.
                for(unsigned delay=0;delay<100&&active();delay+=10)wire_.waitMs(10);
                if(!active()||!send())return false;
                continue;
            }
            if(now()-responseAt_>=uint64_t(options_.responseTimeoutMs)) {
                if(retries_++>=unsigned(options_.retries)) {failure_="FoE response timeout; retry limit reached.";return false;}
                if(!send())return false;
            }
            // A nonblocking fake/transport must not turn an offline link into
            // an unbounded CPU spin. Real receives already wait up to 20 ms.
            wire_.waitMs(1);
        }
        return false;
    }
    Result finish(bool success) {
        if(success) {
            progress_.total=progress_.completed;
            report(FoeStage::Complete,"FoE transfer completed.",true);
            return {true,progress_.detail,{}};
        }
        const bool cancelled=control_.cancelled&&control_.cancelled();
        report(cancelled?FoeStage::Cancelled:FoeStage::Failed,failure_.empty()?"FoE transfer failed.":failure_,true);
        return {false,progress_.detail,{}};
    }
    bool prepareFiles(FoeDirection direction) {
        destination_=std::filesystem::u8path(options_.localPath);
        std::error_code issue;
        if(direction==FoeDirection::Download) {
            if(!std::filesystem::is_regular_file(destination_,issue)||issue) {failure_="Select a readable regular file for FoE download.";return false;}
            const auto bytes=std::filesystem::file_size(destination_,issue);
            if(issue||bytes>MaxFileBytes) {failure_="FoE download exceeds the 128 MiB file limit or its size cannot be read.";return false;}
            progress_.total=size_t(bytes);input_.open(destination_,std::ios::binary);
            if(!input_) {failure_="Cannot open the local FoE download file.";return false;}
            source_=temporarySource();
            if(!source_) {failure_="Cannot prepare an immutable temporary FoE source file.";return false;}
            std::array<char,65536> chunk{};
            for(size_t copied=0;copied<progress_.total;) {
                if(!active())return false;
                const auto size=std::min(chunk.size(),progress_.total-copied);
                input_.read(chunk.data(),std::streamsize(size));
                if(size_t(input_.gcount())!=size||std::fwrite(chunk.data(),1,size,source_)!=size) {
                    failure_="Cannot prepare the complete FoE source file.";return false;
                }
                copied+=size;report(FoeStage::Preparing,"Preparing immutable FoE source file.");
            }
            if(input_.peek()!=std::char_traits<char>::eof()||std::fflush(source_)!=0||std::fseek(source_,0,SEEK_SET)!=0) {
                failure_="FoE source changed in size during preparation, or temporary source I/O failed.";return false;
            }
            input_.close();
            return true;
        }
        if(destination_.filename().empty()) {failure_="Choose a local filename for FoE upload.";return false;}
        static std::atomic<uint64_t> sequence{0};
        // A sibling temporary file keeps an existing destination intact until
        // the complete file and its final ACK have been accepted.
        for(unsigned attempt=0;attempt<16;++attempt) {
            temporary_=destination_;
            temporary_+=std::filesystem::u8path(".foe-part-"+std::to_string(started_)+"-"+std::to_string(++sequence));
#ifdef _WIN32
            const int fd=_wopen(temporary_.c_str(),_O_WRONLY|_O_CREAT|_O_EXCL|_O_BINARY,_S_IREAD|_S_IWRITE);
            if(fd<0)continue;
            output_=_fdopen(fd,"wb");
            if(!output_)_close(fd);
#else
            const int fd=::open(temporary_.c_str(),O_WRONLY|O_CREAT|O_EXCL,0600);
            if(fd<0)continue;
            output_=fdopen(fd,"wb");
            if(!output_)::close(fd);
#endif
            temporaryOwned_=true;
            if(!output_) {failure_="Cannot open the temporary FoE upload stream.";return false;}
            return true;
        }
        failure_="Cannot allocate a temporary FoE upload filename.";return false;
    }
    bool commitUpload() {
        if(std::fflush(output_)!=0) {failure_="Cannot flush the received FoE file to disk.";return false;}
        const bool closed=std::fclose(output_)==0;output_=nullptr;
        if(!closed) {failure_="Cannot close the received FoE file.";return false;}
#ifdef _WIN32
        if(!MoveFileExW(temporary_.c_str(),destination_.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) {
            failure_="Cannot replace the destination with the completed FoE upload.";return false;
        }
#else
        std::error_code issue;std::filesystem::rename(temporary_,destination_,issue);
        if(issue) {failure_="Cannot replace the destination with the completed FoE upload: "+issue.message();return false;}
#endif
        temporaryOwned_=false;return true;
    }
    bool download() {
        uint32_t expected=0;
        if(!send())return false;
        while(active()) {
            Packet packet;if(!receive(packet))return false;
            if(packet.opcode!=Opcode::Ack||!packet.data.empty()) {failure_="Expected a FoE ACK without data.";return false;}
            if(packet.number<expected)continue; // Delayed duplicate ACK.
            if(packet.number!=expected) {failure_="FoE ACK packet number is out of sequence.";return false;}
            progressAt_=now();retries_=0;
            if(expected) {
                progress_.completed+=pending_.data.size();++progress_.blocks;
                if(pending_.data.size()<wire_.txPayload) {
                    // Detect appending/truncation while streaming; a changed
                    // source is never reported as a successful original file.
                    if(std::fgetc(source_)!=EOF) {failure_="Prepared FoE source has unexpected trailing data.";return false;}
                    return true;
                }
            }
            pending_={Opcode::Data,++expected,{}};
            const size_t count=std::min(wire_.txPayload,progress_.total-progress_.completed);
            pending_.data.resize(count);
            if(count) {
                if(std::fread(pending_.data.data(),1,count,source_)!=count) {failure_="Cannot read the expected bytes from the prepared FoE source file.";return false;}
            }
            report(count<wire_.txPayload?FoeStage::Finalizing:FoeStage::Sending,
                   count<wire_.txPayload?"Waiting for final FoE ACK.":"Sending FoE data.");
            if(!send())return false;
        }
        return false;
    }
    bool upload() {
        uint32_t expected=1;
        if(!send())return false;
        while(active()) {
            Packet packet;if(!receive(packet))return false;
            if(packet.opcode!=Opcode::Data) {failure_="Expected a FoE DATA response.";return false;}
            if(expected>1&&packet.number==expected-1) {
                // ACK again without appending the same bytes twice.
                if(!send())return false;
                continue;
            }
            if(packet.number!=expected) {failure_="FoE DATA packet number is out of sequence.";return false;}
            if(packet.data.size()>options_.maxBytes-progress_.completed) {failure_="FoE upload exceeds the configured maximum file size.";return false;}
            if(!packet.data.empty()&&std::fwrite(packet.data.data(),1,packet.data.size(),output_)!=packet.data.size()) {failure_="Cannot write the received FoE data to the local file.";return false;}
            progress_.completed+=packet.data.size();++progress_.blocks;progressAt_=now();retries_=0;
            const bool final=packet.data.size()<wire_.rxPayload;
            report(final?FoeStage::Finalizing:FoeStage::Receiving,final?"Acknowledging the final FoE data block.":"Receiving FoE data.");
            pending_={Opcode::Ack,expected++,{}};
            if(!send())return false;
            if(final)return commitUpload();
        }
        return false;
    }
public:
    Job(Wire wire,int slave,FoeDirection direction,const FoeOptions& options,const FoeControl& control)
        :wire_(std::move(wire)),options_(options),control_(control) {
        if(!wire_.nowMs)wire_.nowMs=[] {return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());};
        if(!wire_.waitMs)wire_.waitMs=[](unsigned ms){std::this_thread::sleep_for(std::chrono::milliseconds(ms));};
        progress_.slave=slave;progress_.direction=direction;started_=progressAt_=reportedAt_=now();
    }
    ~Job() {
        if(source_)std::fclose(source_);
        if(output_)std::fclose(output_);
        if(temporaryOwned_) {std::error_code ignored;std::filesystem::remove(temporary_,ignored);}
    }
    Result run(FoeDirection direction) {
        report(FoeStage::Preparing,"Preparing FoE transfer.",true);
        if(!wire_.send||!wire_.receive||!wire_.txPayload||!wire_.rxPayload||wire_.txPayload>65523||wire_.rxPayload>65523)
            failure_="FoE requires valid send and receive mailbox payload capacities.";
        else if(options_.remoteName.empty()||options_.remoteName.find('\0')!=std::string::npos||options_.remoteName.size()>wire_.txPayload)
            failure_="Device filename is empty, contains a null byte, or exceeds the mailbox payload capacity.";
        else if(options_.localPath.empty())failure_="Choose a local FoE file.";
        else if(options_.responseTimeoutMs<1||options_.noProgressTimeoutMs<1||options_.totalTimeoutMs<1||
                options_.responseTimeoutMs>3600000||options_.noProgressTimeoutMs>3600000||options_.totalTimeoutMs>3600000||options_.retries<0||options_.retries>20)
            failure_="FoE timeouts must be 1..3600000 ms and retry limit 0..20.";
        else if(direction==FoeDirection::Upload&&(!options_.maxBytes||options_.maxBytes>MaxFileBytes))
            failure_="FoE upload limit must be 1 byte through 128 MiB.";
        if(!active()||!prepareFiles(direction))return finish(false);
        pending_={direction==FoeDirection::Upload?Opcode::Read:Opcode::Write,options_.password,
                  std::vector<uint8_t>(options_.remoteName.begin(),options_.remoteName.end())};
        return finish(direction==FoeDirection::Upload?upload():download());
    }
};
} // namespace
Result run(Wire wire,int slave,FoeDirection direction,const FoeOptions& options,const FoeControl& control) {
    Job job(std::move(wire),slave,direction,options,control);
    return job.run(direction);
}
} // namespace niceecatm::foe
