// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#include "esi/sii_image.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

#ifndef NICEECATM_SOURCE_DIR
#define NICEECATM_SOURCE_DIR "."
#endif

namespace {
namespace fs=std::filesystem;
using niceecatm::sii::Image;
using Bytes=std::vector<uint8_t>;
int checks=0;
void check(bool condition,const char* message){++checks;if(!condition)throw std::runtime_error(message);}
struct Temporary {
    fs::path directory=fs::temp_directory_path()/fs::u8path("NiceECatM-sii-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Temporary(){fs::create_directories(directory);}
    ~Temporary(){std::error_code ec;fs::remove_all(directory,ec);}
};
void write(const fs::path& path,const std::string& xml){std::ofstream stream(path,std::ios::binary);stream<<xml;if(!stream)throw std::runtime_error("Cannot write SII fixture");}
uint16_t u16(const Bytes& b,size_t n){return b.at(n)|(uint16_t(b.at(n+1))<<8);}
uint32_t u32(const Bytes& b,size_t n){return u16(b,n)|(uint32_t(u16(b,n+2))<<16);}
void p16(Bytes& b,size_t n,uint16_t value){b.at(n)=uint8_t(value);b.at(n+1)=uint8_t(value>>8);}
const niceecatm::sii::Category& category(const Image& image,uint16_t kind,size_t ordinal=0){for(const auto& c:image.categories)if(c.type==kind){if(!ordinal--)return c;}throw std::runtime_error("Expected SII category missing");}
size_t count(const Image& image,uint16_t kind){return std::count_if(image.categories.begin(),image.categories.end(),[&](const auto& c){return c.type==kind;});}
std::string device(const std::string& extra="",const std::string& config="040F00441027F0FF",const std::string& eepromExtra=""){
    return "<Device Physics=\"YY\"><Type ProductCode=\"#xD\" RevisionNo=\"1\" SerialNo=\"#x12345678\">Board</Type><Name LcId=\"2052\">测试</Name><Name LcId=\"1033\">English name</Name>"+extra+"<Eeprom><ByteSize>2048</ByteSize><ConfigData>"+config+"</ConfigData>"+eepromExtra+"</Eeprom></Device>";
}
std::string document(const std::string& devices){return "<?xml version=\"1.0\" encoding=\"UTF-8\"?><EtherCATInfo><Vendor><Id>#xE00004D8</Id></Vendor><Descriptions><Devices>"+devices+"</Devices></Descriptions></EtherCATInfo>";}
std::string entry(const std::string& type="UINT",int bits=16){return "<Entry><Index>#x7000</Index><SubIndex>1</SubIndex><BitLen>"+std::to_string(bits)+"</BitLen><DataType>"+type+"</DataType><Name>Command</Name></Entry>";}
std::string pdo(const std::string& type="UINT",int bits=16){return "<Sm StartAddress=\"#x1000\" DefaultSize=\"2\" ControlByte=\"#x64\" Enable=\"1\" OpOnly=\"1\">Outputs</Sm><RxPdo Sm=\"0\" Fixed=\"1\" Mandatory=\"1\"><Index>#x1600</Index><Name>Output PDO</Name>"+entry(type,bits)+"</RxPdo>";}
}
int main(){try{
    using namespace niceecatm::sii;
    Temporary temp;std::string error;Image image;
    const auto library=fs::path(NICEECATM_SOURCE_DIR)/"resources"/"esi";
    const auto ax=library/"AX58100_EVB_DIO_ESI_00010200_00000001_20220323.xml";
    const auto lan=library/"Microchip EVB-LAN9252-DIGIO-8IN-8OUT.xml";
    std::vector<Device> options;
    check(listDevices(ax,options,error)&&options.size()==1,"AX58100 Device can be selected independently of UI language");
    check(fromEsi(ax,0,image,error),"Supplied AX58100 ESI converts successfully");
    check(image.bytes.size()==2048&&image.capacity==2048&&image.usedBytes<image.capacity,"AX image is padded to the declared 2 KiB capacity");
    check(image.vendor==0xB95&&image.product==0x10200&&image.revision==1&&image.serial==0&&image.alias==0,"AX fixed identity and alias are serialized");
    check(image.bytes[14]==0xc1&&image.bytes[15]==0,"Known AX configuration vector has CRC-8 0xC1 and reserved high byte zero");
    check(u16(image.bytes,124)==15&&u16(image.bytes,126)==1,"SII EEPROM size is encoded in 128-byte units and version is one");
    check(std::all_of(image.bytes.begin()+image.usedBytes,image.bytes.end(),[](uint8_t b){return b==255;}),"No implicit zero fill in the unused EEPROM image");
    check(count(image,50)==1&&count(image,51)==1&&category(image,50).bytes==32&&category(image,51).bytes==32&&count(image,60)==1&&category(image,60).bytes==48,"AX groups every PDO and DC mode into single categories visible to SOEM");
    const auto sm=category(image,41).offset;
    check(category(image,41).bytes==24&&u16(image.bytes,sm)==0x0f02&&u16(image.bytes,sm+8)==0x0f03&&u16(image.bytes,sm+16)==0x1000,"AX has three original physical SyncManagers");
    check(image.bytes[sm+6]==9&&image.bytes[sm+14]==9&&image.bytes[sm+22]==1,"SM OpOnly flag is preserved separately from Enable");
    check(image.bytes[category(image,50).offset+12]==0x2d&&image.bytes[category(image,51).offset+12]==0x2d,"BITARR8 uses its EtherCAT type ID rather than UNDEF");
    const auto dc=category(image,60).offset+24;
    check(u16(image.bytes,dc+14)==0x0100&&u16(image.bytes,dc+16)==1,"DC hexadecimal activation and Sync0 factor are serialized at their SII offsets");
    const Image axImage=image;
    check(fromEsi(lan,0,image,error),"Supplied LAN9252 ESI converts successfully");
    check(image.vendor==0xE00004D8&&image.product==13&&image.capacity==2048,"LAN9252 metadata uses the file's own identity");
    check(count(image,41)==1&&category(image,41).bytes==16&&count(image,50)==1&&count(image,51)==1&&count(image,60)==0,"LAN9252 fixed PDO/SM image has no invented DC category");
    Image roundtrip;check(fromBin(image.bytes,roundtrip,error)&&roundtrip.bytes==image.bytes,"Full BIN import retains exact image bytes");
    auto shortBytes=image.bytes;shortBytes.resize(image.usedBytes);
    check(fromBin(shortBytes,roundtrip,error)&&roundtrip.sourceBytes==shortBytes.size()&&roundtrip.bytes==image.bytes,"A BIN ending at the SII terminator is explicitly FF-padded");
    auto bin=temp.directory/fs::u8path(u8"烧录镜像.bin");
    check(saveBin(bin,image,error)&&loadBin(bin,roundtrip,error)&&roundtrip.bytes==image.bytes,"Unicode filename BIN export and import round-trip");
    auto fixture=temp.directory/fs::u8path(u8"设备.xml");
    write(fixture,document(device(pdo())+device()));
    check(listDevices(fixture,options,error)&&options.size()==2&&options[0].name=="English name"&&options[1].ordinal==1,"Multiple Device options select deterministic English names");
    check(fromEsi(fixture,0,image,error)&&image.serial==0x12345678&&u32(image.bytes,28)==0x12345678,"Type SerialNo is not silently replaced with the current slave's serial");
    check((u16(image.bytes,category(image,51).offset+6)&17)==17,"Fixed and Mandatory PDO flags are distinct");
    const auto general=category(image,30).offset;
    check(u16(image.bytes,general+16)==0x0011,"Two MII ports use two physical-port nibbles");
    check(axImage.bytes[category(axImage,30).offset]==axImage.bytes[category(axImage,30).offset+14],"General category repeats GroupIdx at its compatibility offset");
    write(fixture,document(device("<Info><EtherCATController><DpramSize>4096</DpramSize><SmCount>4</SmCount><FmmuCount>3</FmmuCount></EtherCATController><Port><Type>MII</Type><PhysicalPhyAddr>0</PhysicalPhyAddr></Port><IdentificationReg134>true</IdentificationReg134></Info>")));
    check(fromEsi(fixture,0,image,error)&&((image.bytes[category(image,30).offset+11]&8)!=0),"Nested controller and port metadata is validated and AL-status identification is serialized");
    write(fixture,document(device(pdo())+device()));
    check(fromEsi(fixture,1,image,error)&&count(image,51)==0,"Selecting another ESI Device rebuilds its own complete image");
    check(!fromEsi(fixture,2,image,error)&&!error.empty(),"Out-of-range ESI Device selection is rejected");
    write(fixture,document(device("<Fmmu OpOnly=\"1\" Sm=\"0\" Su=\"2\">Outputs</Fmmu>"+pdo())));
    check(fromEsi(fixture,0,image,error)&&count(image,40)==1&&count(image,42)==1&&category(image,42).bytes==4,"FMMU OpOnly/assignment attributes are represented by the extended FMMU category");
    const auto fmmuEx=category(image,42).offset;
    check(image.bytes[fmmuEx]==7&&image.bytes[fmmuEx+1]==0&&image.bytes[fmmuEx+2]==2,"Extended FMMU flags and assignments preserve ESI values");
    auto rejected=[&](std::string extra,const char* reason){write(fixture,document(device(extra)));Image unchanged=axImage;check(!fromEsi(fixture,0,unchanged,error)&&!error.empty(),reason);check(unchanged.bytes==axImage.bytes,"Failed conversion keeps the previously prepared image intact");};
    rejected(pdo("UNKNOWN_VENDOR_TYPE"),"Unknown PDO data type cannot become type zero");
    rejected(pdo("BITARR8",16),"Conflicting data type and bit width is rejected");
    rejected("<Slots><Slot/></Slots>","Modular layouts are explicitly unsupported");
    rejected("<Eeprom><ByteSize>2048</ByteSize><ConfigData>040F00441027F0FF</ConfigData></Eeprom>","Duplicate critical EEPROM fields are rejected");
    rejected("<RxPdo Sm=\"0\"><Index>#x1600</Index>"+entry()+"</RxPdo>","PDO cannot refer to an absent SyncManager");
    rejected("<Sm StartAddress=\"65535\" DefaultSize=\"2\">Outputs</Sm>","ESC physical memory range cannot wrap");
    rejected("<Sm StartAddress=\"#x1000\" VendorExtension=\"1\">Outputs</Sm>","Unknown image-affecting SyncManager attributes are rejected");
    rejected("<Info><EtherCATController><Unsupported>1</Unsupported></EtherCATController></Info>","Unknown nested controller metadata is rejected");
    rejected("<Dc><OpMode><Name>DC</Name><AssignActivate>65536</AssignActivate></OpMode></Dc>","DC activation cannot overflow its word");
    rejected("<Dc><OpMode><Name>DC</Name><AssignActivate>0</AssignActivate><CycleTimeSync1>500000</CycleTimeSync1></OpMode></Dc>","Unsupported fixed SYNC1 timing is not silently discarded");
    rejected("<Dc><OpMode><Name>DC</Name><AssignActivate>0</AssignActivate><ShiftTimeSync0 Input=\"1\">0</ShiftTimeSync0></OpMode></Dc>","Unsupported DC shift mode is not silently discarded");
    rejected("<Mailbox><CoE/></Mailbox>","Mailbox cannot omit its physical SyncManagers");
    const std::string mailbox="<Sm StartAddress=\"#x1000\" DefaultSize=\"128\" ControlByte=\"#x26\">MBoxOut</Sm><Sm StartAddress=\"#x1080\" DefaultSize=\"128\" ControlByte=\"#x22\">MBoxIn</Sm><Mailbox DataLinkLayer=\"1\"><CoE SdoInfo=\"1\" CompleteAccess=\"1\"/><FoE/></Mailbox>";
    write(fixture,document(device(mailbox)));
    check(fromEsi(fixture,0,image,error),"Mailbox ESI fixture converts successfully");
    check(u16(image.bytes,48)==0x1000&&u16(image.bytes,50)==128&&u16(image.bytes,52)==0x1080&&u16(image.bytes,54)==128&&u16(image.bytes,56)==12,"Mailbox fixed header preserves Rx/Tx physical addresses, sizes and protocol mask");
    check(image.bytes[category(image,30).offset+5]==35&&image.bytes[category(image,30).offset+6]==1,"General category reflects CoE SDO-info/CA and FoE capability");
    write(fixture,document(device("","040F00441027F0FF341200000000","<BootStrap>0010800080108000</BootStrap>")));
    check(fromEsi(fixture,0,image,error)&&image.alias==0x1234,"Alias from configuration bytes is included in the image and its CRC");
    check(u16(image.bytes,40)==0x1000&&u16(image.bytes,42)==128&&u16(image.bytes,44)==0x1080&&u16(image.bytes,46)==128,"Bootstrap mailbox bytes occupy fixed SII words 0x14 through 0x17");
    write(fixture,document(device("","040F00441027F0FF","<BootStrap>0010000080108000</BootStrap>")));
    check(!fromEsi(fixture,0,image,error),"Incomplete bootstrap mailbox address/size is rejected");
    write(fixture,document(device("<Dc><OpMode><Name>Oversampling</Name><AssignActivate>#x300</AssignActivate><CycleTimeSync0 Factor=\"-4\">0</CycleTimeSync0><ShiftTimeSync0>-200</ShiftTimeSync0><CycleTimeSync1 Factor=\"-1\">0</CycleTimeSync1><ShiftTimeSync1>40</ShiftTimeSync1></OpMode></Dc>")));
    check(fromEsi(fixture,0,image,error),"Signed DC timing ESI fixture converts successfully");
    const auto signedDc=category(image,60).offset;
    check(u32(image.bytes,signedDc+4)==0xffffff38&&u32(image.bytes,signedDc+8)==40&&u16(image.bytes,signedDc+12)==0xffff&&u16(image.bytes,signedDc+16)==0xfffc,"DC shifts/factors retain signed little-endian values without conversion overflow");
    write(fixture,document(device("","040F00441027F0FF","<Category><CatNo>2048</CatNo><Data>0000</Data></Category>")));
    check(!fromEsi(fixture,0,image,error),"Custom ESI EEPROM categories require BIN fallback instead of being silently dropped");
    auto invalid=[&](Bytes bytes,const char* reason){Image unchanged=axImage;check(!fromBin(bytes,unchanged,error)&&!error.empty(),reason);check(unchanged.bytes==axImage.bytes,"Invalid BIN cannot replace the prepared image");};
    auto bad=axImage.bytes;bad[14]^=1;invalid(bad,"Corrupt configuration CRC blocks import");
    bad=axImage.bytes;p16(bad,124,0);invalid(bad,"Impossible declared size blocks import");
    bad=axImage.bytes;p16(bad,126,2);invalid(bad,"Unsupported SII version blocks import");
    bad=axImage.bytes;p16(bad,130,65535);invalid(bad,"Category lengths are checked before accessing payload");
    bad=axImage.bytes;bad[category(axImage,30).offset+3]=255;invalid(bad,"Category string references are checked");
    bad=axImage.bytes;bad[category(axImage,50).offset+2]=255;invalid(bad,"PDO entry count cannot overrun its category");
    bad=axImage.bytes;bad.resize(129);invalid(bad,"Truncated terminator/header and odd lengths are rejected");
    bad=axImage.bytes;bad.resize(axImage.usedBytes-2);invalid(bad,"Missing category end marker is rejected");
    bad=axImage.bytes;bad.resize(axImage.usedBytes+2);bad.back()=0x5a;invalid(bad,"Short BIN unexplained trailing bytes are not silently padded");
    bad.assign(axImage.bytes.begin(),axImage.bytes.begin()+128);bad.resize(140);p16(bad,128,0x8123);p16(bad,130,3);bad[132]=0x12;bad[133]=0x34;bad[134]=0x56;bad[135]=0x78;bad[136]=0x9a;bad[137]=0xbc;p16(bad,138,65535);
    check(fromBin(bad,roundtrip,error)&&roundtrip.categories.size()==1&&roundtrip.categories[0].type==0x8123&&roundtrip.bytes[137]==0xbc,"Unknown vendor categories are bounded and preserved for BIN import");
    write(fixture,"<!DOCTYPE EtherCATInfo [<!ENTITY x SYSTEM \"file:///secret\">]>"+document(device()));
    check(!fromEsi(fixture,0,image,error),"DTD and external entities cannot enter an image source");
    write(fixture,document(device("","040F00441027F")));
    check(!fromEsi(fixture,0,image,error),"Incomplete ConfigData byte pairs are rejected");
    write(fixture,document(device("","040F")));
    check(!fromEsi(fixture,0,image,error),"Incomplete critical ESC ConfigData is rejected");
    std::cout<<checks<<" SII image checks passed\n";return 0;
}catch(const std::exception& e){std::cerr<<"SII image test failed: "<<e.what()<<'\n';return 1;}}
