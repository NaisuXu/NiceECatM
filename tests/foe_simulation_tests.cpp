// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#include "ethercat/backend.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace niceecatm;
namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct TempDirectory {
    std::filesystem::path path=std::filesystem::temp_directory_path()/
        ("niceecatm-foe-simulation-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TempDirectory(){std::filesystem::create_directory(path);}
    ~TempDirectory(){std::error_code ignored;std::filesystem::remove_all(path,ignored);}
};
void save(const std::filesystem::path& path,const std::vector<uint8_t>& bytes){
    std::ofstream file(path,std::ios::binary|std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));
    require(bool(file),"test input file write failed");
}
std::vector<uint8_t> load(const std::filesystem::path& path){
    std::ifstream file(path,std::ios::binary);
    require(bool(file),"expected local file was not created");
    return {std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
}
std::unique_ptr<Backend> scanned(){
    auto backend=makeSimulation();Connection connection;connection.mode=ConnectionMode::Simulation;
    require(backend->scan(connection).ok,"simulation scan failed");return backend;
}
void statesAndScope(){
    auto backend=scanned();
    require(backend->snapshot().slaves[3].bootstrapMailbox,"FoE simulator bootstrap capability is missing");
    require(!backend->state(0,AlState::Boot).ok,"unsupported all-target BOOT was accepted");
    for(const auto& slave:backend->snapshot().slaves)require(slave.state==2,"BOOT preflight partially changed a slave");
    require(backend->state(4,AlState::Boot).ok,"supported target could not enter BOOT");
    auto view=backend->snapshot();
    require(view.slaves[3].state==3&&!view.mapped&&!view.running,"BOOT failed to invalidate mapping");
    require(!backend->readSdo(4,0x1008,0,64).ok,"SDO read was allowed in BOOT");
    require(!backend->writeSdo(4,0x8000,0x11,{0,0}).ok,"SDO write was allowed in BOOT");
    require(!backend->writeOutput(3,0,{1}).ok,"PDO write was allowed with BOOT-invalidated mapping");
    require(backend->state(4,AlState::PreOp).ok,"BOOT exit to PRE-OP failed");
    require(!backend->state(0,AlState::Op).ok,"cyclic I/O restarted with invalid mapping");
    require(backend->readSdo(4,0x1008,0,64).ok,"normal mailbox did not work after BOOT exit");
    Connection connection;connection.mode=ConnectionMode::Simulation;
    require(backend->scan(connection).ok&&backend->state(0,AlState::Op).ok,"rescan did not restore normal I/O");
    require(!backend->state(4,AlState::Boot).ok,"BOOT was entered while cyclic I/O ran");
}
void virtualFileRoundTrips(){
    TempDirectory temp;auto backend=scanned();
    for(const size_t size:{size_t(0),size_t(512),size_t(1024),size_t(1025),size_t(64000)}){
        std::vector<uint8_t> expected(size);
        for(size_t i=0;i<size;++i)expected[i]=uint8_t(i*23+19);
        const auto source=temp.path/std::filesystem::u8path(u8"source-\u6d4b\u8bd5.bin");
        const auto destination=temp.path/std::filesystem::u8path(u8"readback-\u6d4b\u8bd5.bin");
        save(source,expected);save(destination,{0xFF,0xFE});
        FoeOptions options;options.remoteName="roundtrip.bin";options.localPath=source.u8string();
        std::vector<FoeProgress> events;
        FoeControl callbacks;callbacks.progress=[&](const FoeProgress& progress){events.push_back(progress);};
        const auto written=backend->foeTransfer(4,FoeDirection::Download,options,callbacks);
        require(written.ok,written.message.c_str());require(written.data.empty(),"download result retained file bytes");
        require(!events.empty()&&events.back().stage==FoeStage::Complete,"download did not reach complete");
        require(events.back().completed==size&&events.back().slave==4,"download progress target or length incorrect");
        options.localPath=destination.u8string();events.clear();
        const auto read=backend->foeTransfer(4,FoeDirection::Upload,options,callbacks);
        require(read.ok,read.message.c_str());require(read.data.empty(),"upload result retained file bytes");
        require(load(destination)==expected,"FoE readback differs from downloaded bytes");
        require(!events.empty()&&events.back().stage==FoeStage::Complete,"upload did not reach complete");
        require(events.back().completed==size,"upload did not report its exact received size");
        require(std::distance(std::filesystem::directory_iterator(temp.path),std::filesystem::directory_iterator{})==2,
            "successful transfer leaked a temporary file");
    }
    require(backend->state(4,AlState::Boot).ok,"BOOT preparation failed");
    FoeOptions options;options.remoteName="roundtrip.bin";options.localPath=(temp.path/"boot-read.bin").u8string();
    require(backend->foeTransfer(4,FoeDirection::Upload,options,{}).ok,"virtual file could not be read in BOOT");
    require(load(std::filesystem::u8path(options.localPath)).size()==64000,"BOOT upload lost the virtual file");
}
void failedAndCancelledUploadPreserveDestination(){
    TempDirectory temp;auto backend=scanned();const auto destination=temp.path/"existing.bin";
    const std::vector<uint8_t> original={0x91,0x82,0x73};save(destination,original);
    FoeOptions options;options.localPath=destination.u8string();options.remoteName="not-present.bin";
    auto result=backend->foeTransfer(4,FoeDirection::Upload,options,{});
    require(!result.ok&&load(destination)==original,"remote error replaced an existing destination");
    require(backend->snapshot().foe.errorCode==0x8001,"remote FoE error code was lost");
    options.remoteName="firmware.bin";options.maxBytes=100;
    result=backend->foeTransfer(4,FoeDirection::Upload,options,{});
    require(!result.ok&&load(destination)==original,"size limit failure replaced the destination");
    options.maxBytes=1024*1024;
    bool cancel=false;FoeControl callbacks;callbacks.cancelled=[&]{return cancel;};
    callbacks.progress=[&](const FoeProgress& progress){if(progress.completed>=512)cancel=true;};
    result=backend->foeTransfer(4,FoeDirection::Upload,options,callbacks);
    require(!result.ok&&backend->snapshot().foe.stage==FoeStage::Cancelled,"active upload cancellation was ignored");
    require(load(destination)==original,"cancelled upload replaced its destination");
    require(std::distance(std::filesystem::directory_iterator(temp.path),std::filesystem::directory_iterator{})==1,
        "failed or cancelled upload leaked a temporary file");
}
void cancelledDownloadDoesNotCommitAndGuards(){
    TempDirectory temp;auto backend=scanned();
    FoeOptions options;options.remoteName="firmware.bin";options.localPath=(temp.path/"original.bin").u8string();
    require(backend->foeTransfer(4,FoeDirection::Upload,options,{}).ok,"default virtual firmware is missing");
    const auto original=load(std::filesystem::u8path(options.localPath));
    const auto source=temp.path/"changed.bin";save(source,std::vector<uint8_t>(4096,0xAB));options.localPath=source.u8string();
    bool cancel=false;FoeControl callbacks;callbacks.cancelled=[&]{return cancel;};
    callbacks.progress=[&](const FoeProgress& progress){if(progress.completed>=512)cancel=true;};
    require(!backend->foeTransfer(4,FoeDirection::Download,options,callbacks).ok,"active download cancellation was ignored");
    options.localPath=(temp.path/"after.bin").u8string();
    require(backend->foeTransfer(4,FoeDirection::Upload,options,{}).ok,"virtual firmware vanished after cancellation");
    require(load(std::filesystem::u8path(options.localPath))==original,"partial virtual download was committed");
    require(!backend->foeTransfer(1,FoeDirection::Upload,options,{}).ok,"unsupported target accepted FoE");
    require(backend->state(4,AlState::Init).ok&&!backend->foeTransfer(4,FoeDirection::Upload,options,{}).ok,"INIT accepted FoE");
    require(backend->state(0,AlState::PreOp).ok&&backend->state(0,AlState::Op).ok,"cyclic preparation failed");
    require(!backend->foeTransfer(4,FoeDirection::Upload,options,{}).ok,"running cyclic bus accepted FoE");
}
}
int main(){
    try{
        statesAndScope();virtualFileRoundTrips();failedAndCancelledUploadPreserveDestination();cancelledDownloadDoesNotCommitAndGuards();
        std::cout<<"FoE simulation integration tests passed.\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
