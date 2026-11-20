// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#include "ethercat/backend.hpp"
#include "ui/messages.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace niceecatm;
namespace {
int checks=0;
void check(bool value,const char* message) {++checks;if(!value)throw std::runtime_error(message);}
}
int main() {
    try {
        using detail::translateToolMessage;
        check(translateToolMessage("FoE error 0x0000800A: Invalid password or insufficient rights.")==
              "FoE 错误 0x0000800A：密码错误或权限不足。","FoE password error is translated with its original code");
        check(translateToolMessage("FoE error 0x00008001: File not found; check the device filename.")==
              "FoE 错误 0x00008001：文件不存在或文件名不受支持，请检查从站文件名。","FoE filename error has an actionable translated reason");
        check(translateToolMessage("FoE error 0x0000800A: Invalid password or insufficient rights. | Slave message: File already exists. | app.bin")==
              "FoE 错误 0x0000800A：密码错误或权限不足。 | 从站信息：File already exists. | app.bin","FoE slave text stays opaque after the translated reason");
        check(translateToolMessage("FoE error 0x12345678: Unknown or vendor-specific error; consult the slave documentation.")==
              "FoE 错误 0x12345678：未知或厂商自定义错误，请查阅从站文档。","FoE vendor error is not guessed");
        std::vector<uint8_t> bytes;std::string error;
        check(parseHex("0x01, FF 0A10",bytes,error)&&bytes==std::vector<uint8_t>({1,255,10,16}),"HEX parser supports byte groups");
        const auto original=bytes;
        for(const auto* bad:{"","F","GG","0x","12 3","-1","0x0Z"})check(!parseHex(bad,bytes,error)&&bytes==original,"Bad HEX must reject atomically");
        check(hexBytes({0,1,255})=="00 01 FF","HEX formatting");
        std::vector<uint8_t> bitImage={0xa5,0x81,0x7f,0xee};
        check(writeBits(bitImage,11,12,0xabc,error),"Cross-byte UINT12 write");
        check(readBits(bitImage,11,12)==0xabc,"Cross-byte UINT12 read");
        check((bitImage[0]==0xa5)&&(bitImage[1]&7)==1&&(bitImage[2]&0x80)==0&&(bitImage[3]==0xee),"Cross-byte write preserves all neighboring bits");
        const auto beforeInvalid=bitImage;
        check(!writeBits(bitImage,11,12,0x1000,error)&&bitImage==beforeInvalid,"Bit overflow rejected atomically");
        check(!writeBits(bitImage,25,12,1,error)&&bitImage==beforeInvalid,"Bit extent rejected atomically");
        check(!writeBits(bitImage,0,65,0,error),"Unsupported bit width rejected");
        check(encodeValue("-32768","INT16",false,bytes,error)&&bytes==std::vector<uint8_t>({0,128})&&decodeValue(bytes,"INT16")=="-32768","INT16 minimum");
        check(encodeValue("FFFF","INT16",true,bytes,error)&&decodeValue(bytes,"INT16")=="-1","Signed HEX uses bit-width two's complement");
        check(encodeValue("FFFFFFFF","INT32",true,bytes,error)&&decodeValue(bytes,"INT32")=="-1","INT32 signed HEX");
        check(encodeValue("4294967295","UINT32",false,bytes,error)&&decodeValue(bytes,"UINT32")=="4294967295","UINT32 retains full precision");
        check(encodeValue("18446744073709551615","UINT64",false,bytes,error)&&decodeValue(bytes,"UINT64")=="18446744073709551615","UINT64 maximum");
        check(encodeValue("-9223372036854775808","INT64",false,bytes,error)&&decodeValue(bytes,"INT64")=="-9223372036854775808","INT64 minimum");
        check(encodeValue("8000000000000000","INT64",true,bytes,error)&&decodeValue(bytes,"INT64")=="-9223372036854775808","INT64 signed HEX minimum");
        check(encodeValue("7","UINT3",false,bytes,error)&&bytes==std::vector<uint8_t>({7}),"UINT3 boundary");
        check(encodeValue("4095","UINT12",false,bytes,error)&&bytes==std::vector<uint8_t>({255,15}),"UINT12 boundary");
        for(unsigned width:{1u,2u,7u,24u,40u,48u,56u,63u}) {
            const auto name="INT"+std::to_string(width);
            const auto minimum=-(int64_t(1)<<(width-1));
            check(encodeValue(std::to_string(minimum),name,false,bytes,error)&&decodeValue(bytes,name)==std::to_string(minimum),"Signed PDO widths retain sign and bounds");
            const auto saved=bytes;
            check(!encodeValue(std::to_string(minimum-1),name,false,bytes,error)&&bytes==saved,"Nonstandard signed width rejects underflow atomically");
            const auto maximum=(uint64_t(1)<<width)-1;
            const auto unsignedName="UINT"+std::to_string(width);
            check(encodeValue(std::to_string(maximum),unsignedName,false,bytes,error)&&decodeValue(bytes,unsignedName)==std::to_string(maximum),"Nonstandard unsigned PDO widths round trip");
        }
        check(pdoType("INT",16)=="INT16"&&pdoType("UDINT",32)=="UINT32"&&pdoType("LREAL",64)=="REAL64","ESI IEC scalar types normalize");
        check(pdoType("BITARR8",8)=="UINT8"&&pdoType(uint16_t(0x002d),8)=="UINT8","Bit-array descriptions preserve packed bytes");
        check(pdoType("BIT3",3)=="UINT3"&&pdoType(uint16_t(0x0032),3)=="UINT3","ESI and SII bit fields normalize");
        check(pdoType("STRING",16).empty()&&pdoType("REAL",16).empty()&&pdoType("UINT32",16).empty(),"Unsupported or contradictory types remain raw");
        PdoEntry named;named.names={{0x0409,"Control word"}};
        check(pdoEntryName(named,true)=="Control word","English-only names stay English in Chinese UI");
        named.names.push_back({0x0804,"控制字"});
        check(pdoEntryName(named,false)=="Control word"&&pdoEntryName(named,true)=="控制字","Available ESI locales follow the interface language");
        check(encodeValue("true","BOOL",false,bytes,error)&&decodeValue(bytes,"BOOL")=="TRUE","BOOL true");
        check(encodeValue("false","BOOL",false,bytes,error)&&decodeValue(bytes,"BOOL")=="FALSE","BOOL false");
        for(const auto& item:std::vector<std::pair<std::string,std::string>>{{"32768","INT16"},{"-32769","INT16"},{"4294967296","UINT32"},{"18446744073709551616","UINT64"},{"-1","UINT8"},{"8","UINT3"},{"4096","UINT12"},{"2","BOOL"},{"1.5","INT32"},{"12junk","INT16"},{"+","INT64"},{"-","INT64"},{"0x10","UINT8"}}) {
            const auto saved=bytes;
            check(!encodeValue(item.first,item.second,false,bytes,error)&&bytes==saved,"Invalid integer rejects without changing output");
        }
        check(!encodeValue("10000","INT16",true,bytes,error),"HEX width overflow");
        check(!encodeValue("-1","INT16",true,bytes,error),"HEX input is an unsigned bit pattern");
        check(encodeValue("1.25","REAL32",false,bytes,error)&&bytes==std::vector<uint8_t>({0,0,160,63})&&decodeValue(bytes,"REAL32")=="1.25","REAL32 little-endian IEEE value");
        for(const auto* invalid:{"NaN","nan","Infinity","inf","1e100","3.5e38","1.2junk"}) {
            const auto saved=bytes;
            check(!encodeValue(invalid,"REAL32",false,bytes,error)&&bytes==saved,"Nonfinite / overflowing REAL rejected atomically");
        }
        check(encodeValue("-1.7976931348623157e308","REAL64",false,bytes,error)&&std::stod(decodeValue(bytes,"REAL64"))==-std::numeric_limits<double>::max(),"REAL64 finite minimum");
        check(decodeValue({0,0,128,127},"REAL32").find("Invalid REAL")!=std::string::npos,"Raw infinity is marked invalid");
        check(decodeValue({0},"UINT32")=="—","Truncated numeric bytes are not decoded");
        const std::string utf8="\xe8\xae\xbe\xe5\xa4\x87 actual value";
        check(encodeValue(utf8,"STRING",false,bytes,error)&&decodeValue(bytes,"STRING")==utf8,"STRING bytes remain unchanged");

        {
            Slave slave;auto esi=std::make_shared<SdoDictionary>();auto live=std::make_shared<SdoDictionary>();
            SdoObject offline;offline.index=0x2000;offline.names={{0x409,"Offline object"},{0x804,u8"参数"}};
            SdoEntry described;described.subindex=2;described.bitLength=32;described.type="UINT32";described.accessKnown=true;described.access=0x3f;
            described.names={{0x409,"Offline field"},{0x804,u8"字段"}};offline.entries={described};esi->objects.push_back(offline);
            SdoObject missing;missing.index=0x1008;esi->objects.push_back(missing);
            SdoObject online;online.index=0x2000;online.names={{0x409,"Live object"}};
            auto actual=described;actual.dataType=0x8;actual.type="REAL32";actual.access=7;actual.names={{0x409,"Live field"}};online.entries={actual};live->objects={online};
            slave.esiSdoDictionary=esi;slave.sdoDictionary=live;auto merged=mergedSdoObjects(slave);
            check(merged.size()==2&&merged[0].index==0x1008&&!merged[0].online&&merged[0].esi,"ESI-only objects stay visibly unconfirmed and sorted");
            const auto& field=merged[1].entries[0];
            check(field.type=="REAL32"&&field.access==7&&field.online&&field.esi,"Online type and access override ESI metadata");
            check(sdoName(field.names,false)=="Live field"&&sdoName(field.names,true)==u8"字段","ESI supplements localized descriptions without replacing live names");
            check(esi->objects[0].entries[0].type=="UINT32"&&live->objects[0].entries[0].names.size()==1,"Dictionary merge never mutates published metadata");
            live->objects[0].entries[0].type.clear();live->objects[0].entries[0].dataType=0xff;
            live->objects[0].entries[0].access=0;merged=mergedSdoObjects(slave);
            check(merged[1].entries[0].type.empty()&&merged[1].entries[0].access==0,"Unknown online encoding and explicit no-access cannot be replaced by ESI guesses");
            check(sdoName({{0x409,"English only"}},true)=="English only","SDO English-only names remain usable in Chinese");
            slave.esiSdoDictionary.reset();slave.sdoDictionary.reset();check(mergedSdoObjects(slave).empty(),"Missing dictionaries do not invent standard objects");
        }
        auto backend=makeSimulation();Connection connection;connection.primary="sim-primary";
        check(backend->scan(connection).ok,"Simulation scans");
        for(const int cycle: {5000,20000,50000,100000}) {
            auto extended=makeSimulation();Connection extendedConnection;extendedConnection.primary="sim-primary";extendedConnection.cycleUs=cycle;
            check(extended->scan(extendedConnection).ok,"Extended target cycle accepted by simulation");
        }
        auto view=backend->snapshot();check(view.slaves.size()==5&&view.expectedWkc==6,"Simulation device count and WKC");
        check(backend->setCycle(5000).ok&&backend->snapshot().connection.cycleUs==5000,"5 ms target cycle can be selected in PRE-OP");
        check(!backend->setCycle(4000).ok&&backend->snapshot().connection.cycleUs==5000,"Removed 4 ms target cycle is rejected without changing the current target");
        check(backend->setCycle(20000).ok&&backend->snapshot().connection.cycleUs==20000,"INIT/PRE-OP target cycle can be changed while stopped");
        check(view.slaves[3].inputs.size()==5&&view.slaves[4].outputs.size()==19,"Separate fixture process images");
        check(backend->writeOutput(5,0,{0xfa,0xff}).ok,"PRE-OP direct output preload");
        check(!backend->writeOutput(2,0,{1}).ok,"Input-only device cannot accept output writes");
        const auto out=backend->snapshot().slaves[4].outputs;
        check(!backend->writeOutput(5,18,{1,2}).ok&&backend->snapshot().slaves[4].outputs==out,"Out-of-range raw write is atomic");
        check(backend->readSdo(4,0x1000,0,4).ok,"CoE fixture supports SDO reads");
        check(!backend->readSdo(5,0x1000,0,4).ok,"Pure PDO fixture does not advertise CoE");
        check(!backend->readSdoDictionary(5).ok,"Pure PDO fixture cannot expose an SDO dictionary");
        check(backend->readSdoDictionary(4).ok,"PRE-OP CoE fixture discovers its dictionary");
        const auto dictionary=backend->snapshot().slaves[3].sdoDictionary;
        check(dictionary&&dictionary->complete&&dictionary->objects.size()==4,"Simulated dictionary describes implemented objects");
        for(const auto& object:dictionary->objects)for(const auto& entry:object.entries)
            check(backend->readSdo(4,object.index,entry.subindex,256).ok,"Every simulated dictionary entry has a readable device response");
        check(backend->snapshot().slaves[3].sdoDictionary==dictionary,"Service reads retain the same immutable dictionary");
        check(!backend->writeSdo(4,0x1008,0,{1}).ok,"Read-only SDO rejected");
        check(backend->writeSdo(4,0x8000,0x11,{0xe2,4}).ok&&backend->readSdo(4,0x8000,0x11,2).data==std::vector<uint8_t>({0xe2,4}),"SDO direct write / readback");
        check(backend->state(0,AlState::SafeOp).ok&&backend->snapshot().running,"SAFE-OP starts cyclic exchange automatically");
        check(backend->stop().ok&& !backend->snapshot().running,"Lowering from SAFE-OP can stop cyclic exchange");
        check(backend->state(0,AlState::PreOp).ok,"Return to PRE-OP before explicit start check");
        check(!backend->start().ok,"PRE-OP cannot start process exchange");
        check(backend->state(0,AlState::Op).ok&&backend->snapshot().running,"OP begins cyclic exchange");
        backend->cycle();check(backend->snapshot().wkc==6&&backend->snapshot().slaves[4].outputs==out,"Input receive does not change output bytes");
        check(!backend->readSdo(4,0x1000,0,4).ok,"Synchronous SDO blocked during cycles");
        check(!backend->readSdoDictionary(4).ok&&backend->snapshot().slaves[3].sdoDictionary==dictionary,"Running cycles reject discovery and retain cached descriptions");
        check(!backend->readRegister(1,0x130,2).ok,"Synchronous ESC read blocked during cycles");
        check(backend->stop().ok&&backend->snapshot().slaves[4].state==4,"Stop demotes OP to SAFE-OP");
        check(!backend->readSdoDictionary(4).ok,"Stopped SAFE-OP does not implicitly change state for discovery");
        check(backend->state(0,AlState::PreOp).ok,"Return to PRE-OP");
        check(backend->writeRegister(4,0x800,{1,2}).ok&&!backend->snapshot().mapped,"SM write invalidates mapping");
        check(!backend->state(0,AlState::Op).ok,"Invalid mapping prevents OP");
        backend->close();check(!backend->snapshot().connected&&backend->snapshot().slaves.empty(),"Disconnect clears device snapshots");
        connection.redundant=true;connection.secondary=connection.primary;
        check(!backend->scan(connection).ok,"Same adapter redundancy rejected");
        std::cout<<checks<<" core checks passed.\n";return 0;
    } catch(const std::exception& error) {std::cerr<<"Core check failed: "<<error.what()<<'\n';return 1;}
}
