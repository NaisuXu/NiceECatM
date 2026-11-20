// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#include "esi/sii_image.hpp"
#include "tinyxml2.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace niceecatm::sii {
namespace {
using Element=tinyxml2::XMLElement;
using Bytes=std::vector<uint8_t>;
constexpr size_t MaxXml=32*1024*1024,MaxImage=128*1024;
[[noreturn]] void fail(const std::string& error){throw std::runtime_error(error);}
std::string trim(std::string value){const auto n=value.find_first_not_of(" \t\r\n");if(n==std::string::npos)return {};return value.substr(n,value.find_last_not_of(" \t\r\n")-n+1);}
std::string local(const char* name){if(!name)return {};const auto* colon=std::strchr(name,':');return colon?colon+1:name;}
bool named(const Element* e,const char* name){return e&&local(e->Name())==name;}
const Element* child(const Element* e,const char* name){if(e)for(auto* n=e->FirstChildElement();n;n=n->NextSiblingElement())if(named(n,name))return n;return nullptr;}
std::string text(const Element* e){return e&&e->GetText()?trim(e->GetText()):std::string{};}
std::string text(const Element* e,const char* name){return text(child(e,name));}
std::string attr(const Element* e,const char* name){const auto* v=e?e->Attribute(name):nullptr;return v?trim(v):std::string{};}
uint64_t number(std::string value,uint64_t maximum,const std::string& name){
    value=trim(value);size_t n=0;unsigned base=10;
    if(value.size()>2&&((value[0]=='#'&&(value[1]=='x'||value[1]=='X'))||(value[0]=='0'&&(value[1]=='x'||value[1]=='X')))){n=2;base=16;}
    if(n==value.size())fail("Missing or invalid ESI number: "+name);
    uint64_t result=0;
    for(;n<value.size();++n){const auto c=value[n];unsigned digit=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:99;
        if(digit>=base||digit>maximum||result>(maximum-digit)/base)fail("ESI number is out of range: "+name);
        result=result*base+digit;}
    return result;
}
uint32_t num(const Element* e,const char* name,uint32_t maximum,uint32_t fallback=0){const auto* c=child(e,name);return c?static_cast<uint32_t>(number(text(c),maximum,name)):fallback;}
uint32_t anum(const Element* e,const char* name,uint32_t maximum,uint32_t fallback=0){auto s=attr(e,name);return s.empty()?fallback:static_cast<uint32_t>(number(s,maximum,name));}
int32_t signedNumber(std::string s,int64_t minimum,int64_t maximum,const char* label){
    s=trim(s);bool negative=!s.empty()&&s[0]=='-';if(negative)s.erase(0,1);
    const uint64_t limit=negative?static_cast<uint64_t>(-minimum):static_cast<uint64_t>(maximum);
    auto v=static_cast<int64_t>(number(s,limit,label));if(negative)v=-v;
    if(v<minimum||v>maximum)fail(std::string("ESI signed value out of range: ")+label);
    return static_cast<int32_t>(v);
}
bool boolean(const Element* e,const char* name,bool fallback=false){if(!e||!e->Attribute(name))return fallback;auto s=attr(e,name);if(s=="1"||s=="true")return true;if(s=="0"||s=="false")return false;fail(std::string("Invalid ESI boolean: ")+name);}
bool booleanText(const Element* e,const char* label){
    const auto s=text(e);if(s=="1"||s=="true")return true;if(s=="0"||s=="false")return false;
    fail(std::string("Invalid ESI boolean: ")+label);
}
void attributes(const Element* e,std::initializer_list<const char*> allowed){
    for(auto* a=e->FirstAttribute();a;a=a->Next()){auto name=local(a->Name());if(name=="LcId")continue;
        if(std::none_of(allowed.begin(),allowed.end(),[&](const char* item){return name==item;}))fail("Unsupported ESI attribute: "+local(e->Name())+"@"+name);}
}
void children(const Element* e,std::initializer_list<const char*> allowed){
    const std::set<std::string> repeated{"Name","Desc","Fmmu","Sm","RxPdo","TxPdo","Entry","OpMode","Port"};
    std::set<std::string> seen;
    if(e)for(auto* n=e->FirstChildElement();n;n=n->NextSiblingElement()){
        if(std::none_of(allowed.begin(),allowed.end(),[&](const char* item){return named(n,item);}))fail("Unsupported ESI image feature: "+local(e->Name())+"/"+local(n->Name()));
        const auto field=local(n->Name());if(!seen.insert(field).second&&!repeated.count(field))fail("Duplicate ESI image field: "+field);
    }
}
void put16(Bytes& b,size_t n,uint16_t v){b.at(n)=static_cast<uint8_t>(v);b.at(n+1)=static_cast<uint8_t>(v>>8);}
void put32(Bytes& b,size_t n,uint32_t v){put16(b,n,static_cast<uint16_t>(v));put16(b,n+2,static_cast<uint16_t>(v>>16));}
uint16_t get16(const Bytes& b,size_t n){return uint16_t(b.at(n))|uint16_t(b.at(n+1))<<8;}
uint32_t get32(const Bytes& b,size_t n){return uint32_t(get16(b,n))|uint32_t(get16(b,n+2))<<16;}
Bytes read(const std::filesystem::path& path,size_t maximum){
    std::error_code ec;auto length=std::filesystem::file_size(path,ec);if(ec||!length||length>maximum)fail("File is empty, inaccessible or exceeds its size limit: "+path.filename().u8string());
    std::ifstream f(path,std::ios::binary);if(!f)fail("Cannot open file: "+path.filename().u8string());Bytes result(static_cast<size_t>(length));
    f.read(reinterpret_cast<char*>(result.data()),static_cast<std::streamsize>(length));if(!f||f.peek()!=std::char_traits<char>::eof())fail("File changed or could not be completely read: "+path.filename().u8string());return result;
}
std::string xmlEncoding(const Bytes& raw){
    std::string s(raw.begin(),raw.end()),declaration=s.substr(0,256);auto end=declaration.find("?>");if(end!=std::string::npos)declaration.resize(end);
    std::transform(declaration.begin(),declaration.end(),declaration.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    auto enc=declaration.find("encoding");std::string encoding="utf-8";
    if(enc!=std::string::npos){auto q=declaration.find_first_of("\"'",enc);if(q!=std::string::npos){auto z=declaration.find(declaration[q],q+1);if(z!=std::string::npos)encoding=declaration.substr(q+1,z-q-1);}}
    if(encoding=="iso-8859-1"||encoding=="iso8859-1"){
        s.clear();for(uint8_t c:raw){if(c<128)s+=static_cast<char>(c);else{s+=static_cast<char>(0xc0|(c>>6));s+=static_cast<char>(0x80|(c&63));}}
    }else if(encoding!="utf-8"&&encoding!="utf8"&&encoding!="ascii"&&encoding!="us-ascii")fail("Unsupported ESI encoding: "+encoding);
    if(s.find('\0')!=std::string::npos||s.find("<!DOCTYPE")!=std::string::npos||s.find("<!ENTITY")!=std::string::npos)fail("NUL bytes, DTDs and external entities are not supported in ESI images");
    for(size_t n=0;n<s.size();){
        const auto c=static_cast<unsigned char>(s[n++]);if(c<0x80)continue;
        unsigned remaining=0;uint32_t code=0,minimum=0;
        if(c>=0xc2&&c<=0xdf){remaining=1;code=c&31;minimum=0x80;}
        else if(c>=0xe0&&c<=0xef){remaining=2;code=c&15;minimum=0x800;}
        else if(c>=0xf0&&c<=0xf4){remaining=3;code=c&7;minimum=0x10000;}
        else fail("Invalid UTF-8 in ESI file");
        if(remaining>s.size()-n)fail("Truncated UTF-8 in ESI file");
        while(remaining--){const auto next=static_cast<unsigned char>(s[n++]);if((next&0xc0)!=0x80)fail("Invalid UTF-8 continuation in ESI file");code=(code<<6)|(next&63);}
        if(code<minimum||code>0x10ffff||(code>=0xd800&&code<=0xdfff))fail("Invalid UTF-8 code point in ESI file");
    }
    return s;
}
struct Document {
    tinyxml2::XMLDocument xml;
    const Element *root=nullptr,*devices=nullptr;
    explicit Document(const std::filesystem::path& path){auto bytes=xmlEncoding(read(path,MaxXml));
        if(xml.Parse(bytes.c_str(),bytes.size())!=tinyxml2::XML_SUCCESS)fail("Invalid ESI XML: "+std::string(xml.ErrorStr()));
        root=xml.RootElement();if(!named(root,"EtherCATInfo"))fail("Expected an EtherCATInfo ESI root");
        size_t vendors=0,descriptions=0;
        for(auto* n=root->FirstChildElement();n;n=n->NextSiblingElement()){vendors+=named(n,"Vendor");descriptions+=named(n,"Descriptions");}
        if(vendors!=1||descriptions!=1)fail("ESI requires exactly one Vendor and Descriptions element");
        size_t ids=0;for(auto* n=child(root,"Vendor")->FirstChildElement();n;n=n->NextSiblingElement())ids+=named(n,"Id");
        if(ids!=1)fail("ESI requires exactly one Vendor ID");
        devices=child(child(root,"Descriptions"),"Devices");
        if(!devices)fail("ESI contains no device descriptions");}
    std::vector<const Element*> options()const{std::vector<const Element*> result;for(auto* d=devices->FirstChildElement();d;d=d->NextSiblingElement())if(named(d,"Device")){if(result.size()>=16384)fail("Too many ESI devices");result.push_back(d);}if(result.empty())fail("ESI contains no Device elements");return result;}
};
std::string name(const Element* e,const char* tag="Name"){
    const Element *first=nullptr,*neutral=nullptr;
    for(auto* n=e?e->FirstChildElement():nullptr;n;n=n->NextSiblingElement())if(named(n,tag)&&!text(n).empty()){
        if(!first)first=n;
        auto lc=attr(n,"LcId");if(lc.empty())neutral=n;else if((number(lc,65535,"LcId")&0x3ff)==9)return text(n);}
    return text(neutral?neutral:first);
}
struct Strings {
    std::vector<std::string> values;
    uint8_t add(const std::string& s){if(s.empty())return 0;if(s.size()>255)fail("SII strings are limited to 255 UTF-8 bytes");
        const auto found=std::find(values.begin(),values.end(),s);if(found!=values.end())return static_cast<uint8_t>(found-values.begin()+1);
        if(values.size()>=255)fail("SII has more than 255 unique strings");
        values.push_back(s);return static_cast<uint8_t>(values.size());}
    Bytes bytes()const{Bytes b{static_cast<uint8_t>(values.size())};for(const auto& s:values){b.push_back(static_cast<uint8_t>(s.size()));b.insert(b.end(),s.begin(),s.end());}return b;}
};
void append(Bytes& image,uint16_t category,Bytes data){if(data.size()%2)data.push_back(0);if(data.size()/2>65535)fail("SII category exceeds the word length limit");
    auto pos=image.size();image.resize(pos+4);put16(image,pos,category);put16(image,pos+2,static_cast<uint16_t>(data.size()/2));image.insert(image.end(),data.begin(),data.end());if(image.size()>MaxImage)fail("SII categories exceed the supported image limit");}
Bytes hex(std::string s){s.erase(std::remove_if(s.begin(),s.end(),[](unsigned char c){return std::isspace(c)!=0;}),s.end());if(s.size()%2)fail("ESI binary data requires complete hexadecimal byte pairs");Bytes b;
    for(size_t n=0;n<s.size();n+=2)b.push_back(static_cast<uint8_t>(number("#x"+s.substr(n,2),255,"binary data")));
    return b;}
struct Type {uint8_t code;unsigned bits;};
Type dataType(std::string type,unsigned bits){
    std::transform(type.begin(),type.end(),type.begin(),[](unsigned char c){return static_cast<char>(std::toupper(c));});
    static const std::map<std::string,Type> types={
        {"BOOL",{1,1}},{"BOOLEAN",{1,1}},{"SINT",{2,8}},{"INT8",{2,8}},{"INT",{3,16}},{"INT16",{3,16}},
        {"DINT",{4,32}},{"INT32",{4,32}},{"USINT",{5,8}},{"UINT8",{5,8}},{"BYTE",{5,8}},{"UINT",{6,16}},{"UINT16",{6,16}},{"WORD",{6,16}},
        {"UDINT",{7,32}},{"UINT32",{7,32}},{"DWORD",{7,32}},{"REAL",{8,32}},{"REAL32",{8,32}},
        {"STRING",{9,0}},{"VISIBLE_STRING",{9,0}},{"OCTET_STRING",{10,0}},{"UNICODE_STRING",{11,0}},
        {"INT24",{16,24}},{"LREAL",{17,64}},{"REAL64",{17,64}},{"INT40",{18,40}},{"INT48",{19,48}},{"INT56",{20,56}},{"LINT",{21,64}},{"INT64",{21,64}},
        {"UINT24",{22,24}},{"UINT40",{24,40}},{"UINT48",{25,48}},{"UINT56",{26,56}},{"ULINT",{27,64}},{"UINT64",{27,64}},{"LWORD",{27,64}},
        {"BITARR8",{45,8}},{"BITARR16",{46,16}},{"BITARR32",{47,32}},
        {"BIT",{48,1}},{"BIT1",{48,1}},{"BIT2",{49,2}},{"BIT3",{50,3}},{"BIT4",{51,4}},{"BIT5",{52,5}},{"BIT6",{53,6}},{"BIT7",{54,7}},{"BIT8",{55,8}}};
    auto f=types.find(type);if(f==types.end())fail("Unsupported SII PDO data type: "+type);
    if((f->second.bits&&bits!=f->second.bits)||(!f->second.bits&&(bits%8||!bits)))fail("SII PDO bit length does not match data type: "+type);
    return f->second;
}
uint16_t ports(const std::string& physics){if(physics.size()>4)fail("SII supports at most four physical ports");uint16_t result=0;
    for(size_t n=0;n<physics.size();++n){unsigned v=0;switch(physics[n]){case 'Y':v=1;break;case 'K':v=3;break;case ' ':case '-':break;default:fail("Unsupported ESI physical port type");}result|=uint16_t(v<<(n*4));}return result;}
void validate(const Bytes& raw,Image& image){
    if(raw.size()<130||raw.size()>MaxImage||raw.size()%2)fail("SII BIN must be word-aligned, at least 130 bytes and at most 128 KiB");
    if(raw[14]!=configCrc(raw)||raw[15]!=0)fail("SII configuration CRC is invalid");
    const size_t capacity=(size_t(get16(raw,124))+1)*128;
    if(capacity<256||capacity>MaxImage||raw.size()>capacity)fail("SII size declaration conflicts with the BIN length or supported capacity");
    if(get16(raw,126)!=1)fail("Unsupported SII version (expected version 1)");
    for(const size_t offset:{size_t(40),size_t(44),size_t(48),size_t(52)}){
        const auto address=get16(raw,offset),length=get16(raw,offset+2);
        if((!address)!=(!length)||uint32_t(address)+length>65536)fail("SII mailbox address/size is incomplete or exceeds ESC memory");
    }
    size_t n=128;std::vector<Category> categories;std::set<uint16_t> unique;
    bool terminated=false;unsigned stringCount=0;std::vector<std::pair<uint16_t,size_t>> stringsToCheck;
    while(n+2<=raw.size()){
        const auto type=get16(raw,n);if(type==65535){n+=2;terminated=true;break;}
        if(n+4>raw.size())fail("Truncated SII category header");
        const size_t size=size_t(get16(raw,n+2))*2;n+=4;
        if(size>raw.size()-n)fail("SII category length exceeds the input BIN");
        categories.push_back({type,n,size});
        if(type==10||type==30||type==40||type==41||type==42){if(!unique.insert(type).second)fail("Duplicate singleton SII category");}
        if(type==10){if(!size)fail("Empty SII strings category");stringCount=raw[n];size_t pos=n+1;for(unsigned i=0;i<stringCount;++i){if(pos>=n+size)fail("Truncated SII string length");auto len=raw[pos++];if(len>n+size-pos)fail("Truncated SII string");pos+=len;}if(n+size-pos>1)fail("Unexpected data at end of SII strings category");}
        if(type==30){if(size!=32)fail("SII General category must contain 32 bytes");for(size_t k=0;k<4;++k)stringsToCheck.push_back({raw[n+k],n+k});stringsToCheck.push_back({raw[n+14],n+14});}
        if(type==41){if(size%8||size/8>16)fail("Invalid SII SyncManager category length");for(size_t p=n;p<n+size;p+=8){if(raw[p+7]>4)fail("Unsupported SII SyncManager type");if(uint32_t(get16(raw,p))+get16(raw,p+2)>65536)fail("SII SyncManager address overflows ESC address space");}}
        if(type==42&&(!size||(size%6!=0&&size%6!=4)))fail("Invalid SII extended FMMU category length");
        if(type==42)for(size_t p=n;p<n+size;p+=3){if((p+2)>=n+size)break;if(raw[p]&0xf8)fail("SII extended FMMU flags contain reserved bits");}
        if(type==50||type==51){size_t pos=n;while(pos<n+size){if(n+size-pos<8)fail("Truncated SII PDO header");const size_t entries=raw[pos+2];if(raw[pos+3]!=255&&raw[pos+3]>=16)fail("SII PDO references an invalid SyncManager");stringsToCheck.push_back({raw[pos+5],pos+5});pos+=8;if(entries>(n+size-pos)/8)fail("Truncated SII PDO entries");for(size_t j=0;j<entries;++j,pos+=8){stringsToCheck.push_back({raw[pos+3],pos+3});if(!raw[pos+5])fail("SII PDO entry has zero bit length");}}}
        if(type==60){if(size%24)fail("Invalid SII DC category length");for(size_t p=n;p<n+size;p+=24){stringsToCheck.push_back({raw[p+18],p+18});stringsToCheck.push_back({raw[p+19],p+19});}}
        n+=size;
    }
    if(!terminated)fail("SII categories have no end marker");
    for(const auto& ref:stringsToCheck)if(ref.first>stringCount)fail("SII category references a missing string");
    // A complete BIN may contain vendor data after its category end marker.
    // A short BIN is padded explicitly; non-padding trailing data requires a
    // full-capacity BIN so its intended placement is unambiguous.
    if(raw.size()<capacity&&std::any_of(raw.begin()+static_cast<std::ptrdiff_t>(n),raw.end(),[](uint8_t v){return v!=0xff&&v!=0;}))fail("A short SII BIN has unexplained data after its end marker");
    image.bytes=raw;image.bytes.resize(capacity,0xff);image.capacity=capacity;image.sourceBytes=raw.size();image.usedBytes=n;image.categories=std::move(categories);
    image.alias=get16(raw,8);image.vendor=get32(raw,16);image.product=get32(raw,20);image.revision=get32(raw,24);image.serial=get32(raw,28);image.crc=raw[14];
}
Image build(const Document& xml,size_t ordinal){
    const auto options=xml.options();if(ordinal>=options.size())fail("Selected ESI device no longer exists");const auto* d=options[ordinal];
    attributes(d,{"Physics"});children(d,{"Type","Name","GroupType","Info","Profile","Fmmu","Sm","RxPdo","TxPdo","Dc","Eeprom","Mailbox","Image16x14","ImageData16x14","ImageFile16x14"});
    const auto* type=child(d,"Type");if(!type)fail("ESI device has no Type identity");attributes(type,{"ProductCode","RevisionNo","SerialNo"});
    const auto* eeprom=child(d,"Eeprom");if(!eeprom)fail("ESI device has no EEPROM configuration");attributes(eeprom,{});children(eeprom,{"ByteSize","ConfigData","BootStrap"});
    const size_t capacity=num(eeprom,"ByteSize",MaxImage);if(capacity<256||capacity%128)fail("ESI EEPROM capacity must be a multiple of 128 bytes, from 256 bytes to 128 KiB");
    Bytes result(128,0);auto config=hex(text(eeprom,"ConfigData"));if(config.size()<8||config.size()>14||config.size()%2)fail("ESI ConfigData must contain 8 to 14 word-aligned bytes");std::copy(config.begin(),config.end(),result.begin());
    result[14]=configCrc(result);put32(result,16,static_cast<uint32_t>(number(text(child(xml.root,"Vendor"),"Id"),UINT32_MAX,"Vendor ID")));
    put32(result,20,anum(type,"ProductCode",UINT32_MAX));if(attr(type,"ProductCode").empty())fail("ESI Type has no ProductCode");
    put32(result,24,anum(type,"RevisionNo",UINT32_MAX));put32(result,28,anum(type,"SerialNo",UINT32_MAX));
    put16(result,124,static_cast<uint16_t>(capacity/128-1));put16(result,126,1);
    auto boot=hex(text(eeprom,"BootStrap"));if(!boot.empty()){if(boot.size()!=8)fail("ESI BootStrap must contain exactly eight bytes");std::copy(boot.begin(),boot.end(),result.begin()+40);}
    Strings strings;auto display=name(d);if(display.empty())display=text(type);strings.add(display);
    Bytes general(32,0);general[0]=strings.add(text(d,"GroupType"));general[14]=general[0];general[2]=strings.add(text(type));general[3]=strings.add(display);put16(general,16,ports(attr(d,"Physics")));
    if(const auto* info=child(d,"Info")){
        attributes(info,{});children(info,{"EtherCATController","Port","IdentificationAdo","IdentificationReg134"});
        if(const auto* controller=child(info,"EtherCATController")){
            attributes(controller,{});children(controller,{"DpramSize","SmCount","FmmuCount"});
            // These controller capabilities do not have a standard SII field,
            // but validating them prevents malformed ESI metadata from being
            // silently accepted while retaining the standard image subset.
            for(const auto* field:{"DpramSize","SmCount","FmmuCount"})if(child(controller,field))num(controller,field,65535);
        }
        for(auto* port=info->FirstChildElement();port;port=port->NextSiblingElement())if(named(port,"Port")){
            attributes(port,{});children(port,{"Type","PhysicalPhyAddr"});
            if(child(port,"PhysicalPhyAddr"))num(port,"PhysicalPhyAddr",255);
            if(child(port,"Type")&&text(port,"Type").empty())fail("ESI Port Type cannot be empty");
        }
        if(const auto* reg=child(info,"IdentificationReg134")){attributes(reg,{});if(booleanText(reg,"IdentificationReg134"))general[11]|=8;}
        if(child(info,"IdentificationAdo")){put16(general,18,static_cast<uint16_t>(num(info,"IdentificationAdo",65535)));general[11]|=16;}
    }
    Bytes fmmus,fmmusEx,sms,rxPdos,txPdos,dcModes;bool hasFmmusEx=false;
    std::vector<unsigned> smKinds;
    for(auto* n=d->FirstChildElement();n;n=n->NextSiblingElement())if(named(n,"Fmmu")){
        attributes(n,{"OpOnly","Sm","Su"});children(n,{});auto s=text(n);uint8_t v=s=="Outputs"?1:s=="Inputs"?2:s=="MBoxState"?3:s=="Unused"?0:255;if(v==255)fail("Unsupported ESI FMMU function: "+s);if(fmmus.size()>=16)fail("Too many FMMU descriptions");fmmus.push_back(v);
        // ETG.2010 category 42 keeps three bytes per FMMU in category-40
        // order: flags (OpOnly is bit 0), optional SM, optional SyncUnit.
        // Keep entries without attributes too, so subsequent flags cannot
        // shift to another hardware FMMU. Category padding is added once.
        const bool hasSm=n->Attribute("Sm")!=nullptr,hasSu=n->Attribute("Su")!=nullptr;
        hasFmmusEx=hasFmmusEx||n->Attribute("OpOnly")||hasSm||hasSu;
        const auto sm=anum(n,"Sm",255),su=anum(n,"Su",255);
        fmmusEx.insert(fmmusEx.end(),{static_cast<uint8_t>((boolean(n,"OpOnly")?1:0)|(hasSm?2:0)|(hasSu?4:0)),static_cast<uint8_t>(sm),static_cast<uint8_t>(su)});
    }else if(named(n,"Sm")){
        attributes(n,{"StartAddress","DefaultSize","MinSize","MaxSize","ControlByte","Enable","OpOnly","Fixed","Virtual"});
        const auto kind=text(n);unsigned k=kind=="MBoxOut"?1:kind=="MBoxIn"?2:kind=="Outputs"?3:kind=="Inputs"?4:kind=="Unused"?0:255;
        if(k==255||smKinds.size()>=16)fail("Unsupported ESI SyncManager function or count");
        smKinds.push_back(k);
        const auto pos=sms.size();sms.resize(pos+8);const auto start=anum(n,"StartAddress",65535),length=anum(n,"DefaultSize",65535);
        if(k&&attr(n,"StartAddress").empty())fail("Active ESI SyncManager requires StartAddress");
        if(k&&attr(n,"ControlByte").empty())fail("Active ESI SyncManager requires ControlByte");
        if(start+length>65536)fail("ESI SyncManager address range exceeds ESC memory");
        if(anum(n,"MinSize",65535)>length||anum(n,"MaxSize",65535,65535)<length)fail("ESI SyncManager DefaultSize is outside MinSize/MaxSize");
        put16(sms,pos,static_cast<uint16_t>(start));put16(sms,pos+2,static_cast<uint16_t>(length));sms[pos+4]=static_cast<uint8_t>(anum(n,"ControlByte",255));
        sms[pos+6]=static_cast<uint8_t>((boolean(n,"Enable",true)?1:0)|(boolean(n,"Fixed")?2:0)|(boolean(n,"Virtual")?4:0)|(boolean(n,"OpOnly")?8:0));sms[pos+7]=static_cast<uint8_t>(k);
        if(k==1||k==2){if(!start||!length)fail("Mailbox SyncManager requires StartAddress and DefaultSize");put16(result,k==1?48:52,static_cast<uint16_t>(start));put16(result,k==1?50:54,static_cast<uint16_t>(length));}
    }
    if(const auto* mbx=child(d,"Mailbox")){
        attributes(mbx,{"DataLinkLayer"});children(mbx,{"AoE","EoE","CoE","FoE","SoE","VoE"});
        if(std::count(smKinds.begin(),smKinds.end(),1)!=1||std::count(smKinds.begin(),smKinds.end(),2)!=1)fail("Mailbox requires exactly one MBoxOut and MBoxIn SyncManager");
        uint16_t protocols=0;const std::array<const char*,6> names{{"AoE","EoE","CoE","FoE","SoE","VoE"}};
        for(unsigned i=0;i<names.size();++i)if(const auto* p=child(mbx,names[i])){protocols|=uint16_t(1<<i);
            if(i==2){attributes(p,{"SdoInfo","PdoAssign","PdoConfig","PdoUpload","CompleteAccess"});children(p,{"InitCmds"});general[5]=1;const std::array<const char*,5> flags{{"SdoInfo","PdoAssign","PdoConfig","PdoUpload","CompleteAccess"}};for(unsigned j=0;j<flags.size();++j)if(boolean(p,flags[j]))general[5]|=static_cast<uint8_t>(2<<j);}
            else {attributes(p,{});if(p->FirstChildElement())fail("Unsupported mailbox protocol image feature: "+std::string(names[i]));if(i==1)general[7]=1;else if(i==3)general[6]=1;else if(i==4)general[8]=1;}}
        put16(result,56,protocols);
    }
    std::set<std::pair<bool,uint16_t>> pdoIds;
    for(auto* n=d->FirstChildElement();n;n=n->NextSiblingElement())if(named(n,"RxPdo")||named(n,"TxPdo")){
        const bool rx=named(n,"RxPdo");attributes(n,{"Sm","Fixed","Mandatory"});children(n,{"Index","Name","Entry"});
        auto index=num(n,"Index",65535);if(!index||!pdoIds.insert({rx,static_cast<uint16_t>(index)}).second)fail("Invalid or duplicate ESI PDO index");
        const unsigned sm=anum(n,"Sm",255,255);if(sm!=255&&(sm>=smKinds.size()||smKinds[sm]!=(rx?3u:4u)))fail("ESI PDO references a missing or incompatible SyncManager");
        Bytes data(8,0);put16(data,0,static_cast<uint16_t>(index));data[3]=static_cast<uint8_t>(sm);data[5]=strings.add(name(n));put16(data,6,static_cast<uint16_t>((boolean(n,"Mandatory")?1:0)|(boolean(n,"Fixed")?16:0)));
        size_t count=0;for(auto* e=n->FirstChildElement();e;e=e->NextSiblingElement())if(named(e,"Entry")){
            attributes(e,{});children(e,{"Index","SubIndex","BitLen","Name","DataType"});if(++count>255)fail("SII PDO has more than 255 entries");
            if(!child(e,"Index"))fail("ESI PDO entry Index is missing");
            auto idx=num(e,"Index",65535),sub=num(e,"SubIndex",255),bits=num(e,"BitLen",255);if(!bits)fail("ESI PDO entry BitLen is missing or zero");
            auto typeName=text(e,"DataType");uint8_t code=0;if(idx||sub){if(typeName.empty())fail("ESI PDO entry has no data type");code=dataType(typeName,bits).code;}else if(!typeName.empty())code=dataType(typeName,bits).code;
            auto pos=data.size();data.resize(pos+8);put16(data,pos,static_cast<uint16_t>(idx));data[pos+2]=static_cast<uint8_t>(sub);data[pos+3]=strings.add(name(e));data[pos+4]=code;data[pos+5]=static_cast<uint8_t>(bits);
        }data[2]=static_cast<uint8_t>(count);if(!count)fail("ESI PDO has no entries");
        auto& category=rx?rxPdos:txPdos;category.insert(category.end(),data.begin(),data.end());
    }
    if(const auto* dc=child(d,"Dc")){
        attributes(dc,{});children(dc,{"OpMode"});for(auto* op=dc->FirstChildElement();op;op=op->NextSiblingElement()){
            attributes(op,{});children(op,{"Name","Desc","AssignActivate","CycleTimeSync0","ShiftTimeSync0","CycleTimeSync1","ShiftTimeSync1"});Bytes data(24,0);
            if(name(op).empty()||!child(op,"AssignActivate"))fail("ESI DC mode requires Name and AssignActivate");
            put32(data,0,num(op,"CycleTimeSync0",UINT32_MAX));
            for(const auto& field:std::array<std::pair<const char*,size_t>,2>{{{"ShiftTimeSync0",4},{"ShiftTimeSync1",8}}})if(const auto* value=child(op,field.first)){
                attributes(value,{});put32(data,field.second,static_cast<uint32_t>(signedNumber(text(value),INT32_MIN,INT32_MAX,field.first)));}
            if(num(op,"CycleTimeSync1",UINT32_MAX))fail("Fixed SYNC1 cycle time is not supported by the SII DC image subset");
            for(const auto& field:std::array<std::pair<const char*,size_t>,2>{{{"CycleTimeSync0",16},{"CycleTimeSync1",12}}})if(const auto* value=child(op,field.first)){
                attributes(value,{"Factor"});auto factor=attr(value,"Factor");if(!factor.empty())put16(data,field.second,static_cast<uint16_t>(signedNumber(factor,INT16_MIN,INT16_MAX,"DC factor")));}
            put16(data,14,static_cast<uint16_t>(num(op,"AssignActivate",65535)));data[18]=strings.add(name(op));data[19]=strings.add(name(op,"Desc"));dcModes.insert(dcModes.end(),data.begin(),data.end());
        }
    }
    append(result,10,strings.bytes());append(result,30,std::move(general));if(!fmmus.empty())append(result,40,std::move(fmmus));if(!sms.empty())append(result,41,std::move(sms));
    if(hasFmmusEx)append(result,42,std::move(fmmusEx));
    // SOEM's SII readers consume the first matching category. Keep every PDO
    // of one direction together instead of hiding later PDOs in duplicates.
    if(!txPdos.empty())append(result,50,std::move(txPdos));
    if(!rxPdos.empty())append(result,51,std::move(rxPdos));
    if(!dcModes.empty())append(result,60,std::move(dcModes));
    result.push_back(0xff);result.push_back(0xff);if(result.size()>capacity)fail("Generated SII categories exceed the declared EEPROM capacity");
    Image image;validate(result,image);return image;
}
std::string hexValue(uint32_t value,unsigned width=4){std::ostringstream stream;stream<<"#x"<<std::uppercase<<std::hex<<std::setfill('0')<<std::setw(width)<<value;return stream.str();}
std::string hexBytes(const Bytes& bytes,size_t offset,size_t count){std::ostringstream stream;stream<<std::uppercase<<std::hex<<std::setfill('0');for(size_t n=offset;n<offset+count;++n)stream<<std::setw(2)<<unsigned(bytes.at(n));return stream.str();}
// SII BIN import preserves arbitrary bytes. XML export must additionally reject
// invalid UTF-8 and characters forbidden in XML 1.0 instead of changing names.
void checkXmlString(const std::string& value){
    for(size_t n=0;n<value.size();){
        const auto c=static_cast<unsigned char>(value[n++]);uint32_t code=c,minimum=0;unsigned remaining=0;
        if(c>=0x80){if(c>=0xc2&&c<=0xdf){remaining=1;code=c&31;minimum=0x80;}else if(c>=0xe0&&c<=0xef){remaining=2;code=c&15;minimum=0x800;}else if(c>=0xf0&&c<=0xf4){remaining=3;code=c&7;minimum=0x10000;}else fail("SII string is not valid XML 1.0 UTF-8");}
        if(remaining>value.size()-n)fail("SII string is not valid XML 1.0 UTF-8");
        while(remaining--){const auto next=static_cast<unsigned char>(value[n++]);if((next&0xc0)!=0x80)fail("SII string is not valid XML 1.0 UTF-8");code=(code<<6)|(next&63);}
        if(code<minimum||code>0x10ffff||(code>=0xd800&&code<=0xdfff)||code==0xfffe||code==0xffff||(code<32&&code!=9&&code!=10&&code!=13))fail("SII string is not valid XML 1.0 UTF-8");
    }
}
const char* exportType(uint8_t code,unsigned bits){
    static const std::map<uint8_t,const char*> names={{1,"BOOL"},{2,"SINT"},{3,"INT"},{4,"DINT"},{5,"USINT"},{6,"UINT"},{7,"UDINT"},{8,"REAL"},{9,"STRING"},{10,"OCTET_STRING"},{11,"UNICODE_STRING"},{16,"INT24"},{17,"LREAL"},{18,"INT40"},{19,"INT48"},{20,"INT56"},{21,"LINT"},{22,"UINT24"},{24,"UINT40"},{25,"UINT48"},{26,"UINT56"},{27,"ULINT"},{45,"BITARR8"},{46,"BITARR16"},{47,"BITARR32"},{48,"BIT"},{49,"BIT2"},{50,"BIT3"},{51,"BIT4"},{52,"BIT5"},{53,"BIT6"},{54,"BIT7"},{55,"BIT8"}};
    const auto type=names.find(code);if(type==names.end())fail("Cannot export SII PDO data type: "+hexValue(code));
    dataType(type->second,bits);return type->second;
}
std::string exportEsi(const Image& source){
    Image image;validate(source.bytes,image);const auto& b=image.bytes;
    auto category=[&](uint16_t kind)->const Category*{for(const auto& c:image.categories)if(c.type==kind)return &c;return nullptr;};
    std::vector<std::string> strings;
    if(const auto* c=category(10)){size_t n=c->offset+1;for(unsigned i=0;i<b[c->offset];++i){const auto length=b[n++];strings.emplace_back(b.begin()+n,b.begin()+n+length);checkXmlString(strings.back());n+=length;}}
    auto string=[&](uint8_t ref)->std::string{return ref?strings.at(ref-1):std::string{};};
    const auto* general=category(30);const size_t gen=general?general->offset:0;
    auto typeName=general?string(b[gen+2]):std::string{};
    auto displayName=general?string(b[gen+3]):std::string{};
    auto groupName=general?string(b[gen]):std::string{};
    const bool generatedNames=typeName.empty()||groupName.empty();
    if(typeName.empty())typeName="SII_"+hexValue(image.product,8).substr(2)+"_"+hexValue(image.revision,8).substr(2);
    if(displayName.empty())displayName=typeName;
    if(groupName.empty())groupName="SII";
    tinyxml2::XMLDocument xml;
    xml.InsertEndChild(xml.NewDeclaration("xml version=\"1.0\" encoding=\"UTF-8\""));
    xml.InsertEndChild(xml.NewComment("Generated from EEPROM/SII; ESI export cannot recover information absent from SII, including the complete object dictionary and initialization commands."));
    if(generatedNames)xml.InsertEndChild(xml.NewComment("SII_ device identifiers and the SII group are generated placeholders where the EEPROM has no corresponding name."));
    xml.InsertEndChild(xml.NewComment("This basic ESI is not a byte-for-byte EEPROM backup; retain the original BIN for restoration."));
    auto add=[&](Element* parent,const char* name,const std::string& value=std::string{})->Element*{auto* e=xml.NewElement(name);parent->InsertEndChild(e);if(!value.empty())e->SetText(value.c_str());return e;};
    auto* root=xml.NewElement("EtherCATInfo");root->SetAttribute("Version","1.2");xml.InsertEndChild(root);
    auto* vendor=add(root,"Vendor");add(vendor,"Id",hexValue(image.vendor,8));
    auto* descriptions=add(root,"Descriptions");auto* group=add(add(descriptions,"Groups"),"Group");add(group,"Type",groupName);add(group,"Name",groupName);
    auto* device=add(add(descriptions,"Devices"),"Device");
    if(general){std::string physics;const auto ports=get16(b,gen+16);for(unsigned n=0;n<4;++n){const auto port=(ports>>(n*4))&15;switch(port){case 0:physics+='-';break;case 1:physics+='Y';break;case 3:physics+='K';break;default:fail("Cannot export SII physical port type: "+hexValue(port));}}while(!physics.empty()&&physics.back()=='-')physics.pop_back();if(!physics.empty())device->SetAttribute("Physics",physics.c_str());}
    auto* type=add(device,"Type",typeName);type->SetAttribute("ProductCode",hexValue(image.product,8).c_str());type->SetAttribute("RevisionNo",hexValue(image.revision,8).c_str());if(image.serial)type->SetAttribute("SerialNo",hexValue(image.serial,8).c_str());
    add(device,"Name",displayName);
    if(general&&(b[gen+11]&24)){auto* info=add(device,"Info");if(b[gen+11]&8)add(info,"IdentificationReg134","true");if(b[gen+11]&16)add(info,"IdentificationAdo",hexValue(get16(b,gen+18)));}
    add(device,"GroupType",groupName);
    const auto* fmmus=category(40);const auto* fmmusEx=category(42);size_t fmmuCount=fmmus?fmmus->bytes:0;
    if(fmmusEx){const auto count=fmmusEx->bytes/3;if(!fmmus||count>fmmuCount||(count+1<fmmuCount)||(count<fmmuCount&&b[fmmus->offset+count]!=0))fail("SII extended FMMU count does not match its FMMU category");fmmuCount=count;}
    if(fmmuCount>16)fail("Too many FMMU descriptions");
    for(size_t n=0;n<fmmuCount;++n){const auto kind=b[fmmus->offset+n];const char* name=kind==0?"Unused":kind==1?"Outputs":kind==2?"Inputs":kind==3?"MBoxState":nullptr;if(!name)fail("Cannot export SII FMMU function: "+hexValue(kind));auto* e=add(device,"Fmmu",name);if(fmmusEx){const auto pos=fmmusEx->offset+n*3;const auto flags=b[pos];if(flags&1)e->SetAttribute("OpOnly","1");if(flags&2)e->SetAttribute("Sm",unsigned(b[pos+1]));if(flags&4)e->SetAttribute("Su",unsigned(b[pos+2]));}}
    std::vector<uint8_t> smKinds;const auto* sms=category(41);
    if(sms)for(size_t pos=sms->offset;pos<sms->offset+sms->bytes;pos+=8){const auto kind=b[pos+7],flags=b[pos+6];const char* name=kind==0?"Unused":kind==1?"MBoxOut":kind==2?"MBoxIn":kind==3?"Outputs":"Inputs";auto* e=add(device,"Sm",name);e->SetAttribute("StartAddress",hexValue(get16(b,pos)).c_str());e->SetAttribute("DefaultSize",unsigned(get16(b,pos+2)));e->SetAttribute("ControlByte",hexValue(b[pos+4],2).c_str());e->SetAttribute("Enable",(flags&1)?"1":"0");if(flags&2)e->SetAttribute("Fixed","1");if(flags&4)e->SetAttribute("Virtual","1");if(flags&8)e->SetAttribute("OpOnly","1");smKinds.push_back(kind);}
    // Category 50 means TxPDO and 51 means RxPDO, regardless of the numeric
    // PDO index chosen by the device vendor.
    std::set<std::pair<uint16_t,uint16_t>> pdoIds;
    for(const auto kind:{uint16_t(51),uint16_t(50)})for(const auto& c:image.categories)if(c.type==kind){
        for(size_t pos=c.offset;pos<c.offset+c.bytes;){const auto index=get16(b,pos),flags=get16(b,pos+6);const auto count=b[pos+2],sm=b[pos+3];if(!index||!count)fail("Cannot export an empty or zero-index SII PDO");if(!pdoIds.insert({kind,index}).second)fail("Duplicate SII PDO index: "+hexValue(index));auto* pdo=add(device,kind==51?"RxPdo":"TxPdo");if(sm!=255){if(sm>=smKinds.size()||smKinds[sm]!=(kind==51?3:4))fail("SII PDO references a missing or incompatible SyncManager");pdo->SetAttribute("Sm",unsigned(sm));}if(flags&1)pdo->SetAttribute("Mandatory","1");if(flags&16)pdo->SetAttribute("Fixed","1");add(pdo,"Index",hexValue(index));if(b[pos+5])add(pdo,"Name",string(b[pos+5]));pos+=8;
            for(unsigned n=0;n<count;++n,pos+=8){auto* entry=add(pdo,"Entry");const auto index=get16(b,pos);add(entry,"Index",hexValue(index));add(entry,"SubIndex",std::to_string(b[pos+2]));add(entry,"BitLen",std::to_string(b[pos+5]));if(b[pos+3])add(entry,"Name",string(b[pos+3]));if(index||b[pos+2]||b[pos+4])add(entry,"DataType",exportType(b[pos+4],b[pos+5]));}
        }
    }
    const auto protocols=get16(b,56);if(protocols&~63)fail("Cannot export unsupported SII mailbox protocol flags");
    if(protocols){
        if(std::count(smKinds.begin(),smKinds.end(),1)!=1||std::count(smKinds.begin(),smKinds.end(),2)!=1)fail("SII mailbox configuration requires MBoxOut and MBoxIn SyncManagers for ESI export");
        for(size_t n=0;n<smKinds.size();++n)if(smKinds[n]==1||smKinds[n]==2){const auto header=smKinds[n]==1?48:52;const auto pos=sms->offset+n*8;if(get16(b,header)!=get16(b,pos)||get16(b,header+2)!=get16(b,pos+2))fail("SII mailbox header does not match its SyncManager category");if(!get16(b,pos)||!get16(b,pos+2))fail("SII mailbox SyncManagers require nonzero addresses and sizes for ESI export");}
        auto* mailbox=add(device,"Mailbox");const std::array<const char*,6> names{{"AoE","EoE","CoE","FoE","SoE","VoE"}};for(unsigned n=0;n<6;++n)if(protocols&(1<<n)){auto* protocol=add(mailbox,names[n]);if(n==2&&general){const std::array<const char*,5> flags{{"SdoInfo","PdoAssign","PdoConfig","PdoUpload","CompleteAccess"}};for(unsigned j=0;j<flags.size();++j)protocol->SetAttribute(flags[j],(b[gen+5]&(2<<j))?"1":"0");}}
    }
    Element* dc=nullptr;
    for(const auto& c:image.categories)if(c.type==60)for(size_t pos=c.offset;pos<c.offset+c.bytes;pos+=24){if(!dc)dc=add(device,"Dc");auto* op=add(dc,"OpMode");auto name=string(b[pos+18]);if(name.empty())name="SII_DC_"+std::to_string((pos-c.offset)/24+1);add(op,"Name",name);if(b[pos+19])add(op,"Desc",string(b[pos+19]));add(op,"AssignActivate",hexValue(get16(b,pos+14)));auto* sync0=add(op,"CycleTimeSync0",std::to_string(get32(b,pos)));const auto factor0=get16(b,pos+16),factor1=get16(b,pos+12);if(factor0)sync0->SetAttribute("Factor",std::to_string(factor0>32767?int(factor0)-65536:int(factor0)).c_str());const auto shift0=get32(b,pos+4),shift1=get32(b,pos+8);add(op,"ShiftTimeSync0",std::to_string(shift0>INT32_MAX?int64_t(shift0)-0x100000000ll:int64_t(shift0)));if(factor1){auto* sync1=add(op,"CycleTimeSync1","0");sync1->SetAttribute("Factor",std::to_string(factor1>32767?int(factor1)-65536:int(factor1)).c_str());}add(op,"ShiftTimeSync1",std::to_string(shift1>INT32_MAX?int64_t(shift1)-0x100000000ll:int64_t(shift1)));}
    auto* eeprom=add(device,"Eeprom");add(eeprom,"ByteSize",std::to_string(image.capacity));add(eeprom,"ConfigData",hexBytes(b,0,14));if(std::any_of(b.begin()+40,b.begin()+48,[](uint8_t v){return v!=0;}))add(eeprom,"BootStrap",hexBytes(b,40,8));
    tinyxml2::XMLPrinter printer;xml.Print(&printer);return printer.CStr();
}
} // namespace
uint8_t configCrc(const std::vector<uint8_t>& bytes){if(bytes.size()<14)return 0;uint8_t crc=0xff;for(size_t n=0;n<14;++n){crc^=bytes[n];for(unsigned bit=0;bit<8;++bit)crc=static_cast<uint8_t>((crc&0x80)?(crc<<1)^7:crc<<1);}return crc;}
bool listDevices(const std::filesystem::path& path,std::vector<Device>& devices,std::string& error){try{Document xml(path);auto options=xml.options();std::vector<Device> result;for(size_t n=0;n<options.size();++n){auto display=name(options[n]);if(display.empty())display=text(options[n],"Type");result.push_back({display,n});}devices=std::move(result);error.clear();return true;}catch(const std::exception& e){error=e.what();return false;}}
bool fromEsi(const std::filesystem::path& path,size_t ordinal,Image& image,std::string& error){try{Document xml(path);auto result=build(xml,ordinal);image=std::move(result);error.clear();return true;}catch(const std::exception& e){error=e.what();return false;}}
bool fromBin(const std::vector<uint8_t>& bytes,Image& image,std::string& error){try{Image result;validate(bytes,result);image=std::move(result);error.clear();return true;}catch(const std::exception& e){error=e.what();return false;}}
bool loadBin(const std::filesystem::path& path,Image& image,std::string& error){try{return fromBin(read(path,MaxImage),image,error);}catch(const std::exception& e){error=e.what();return false;}}
bool saveBin(const std::filesystem::path& path,const Image& image,std::string& error){try{Image validated;validate(image.bytes,validated);if(validated.sourceBytes!=validated.capacity)fail("Export requires a complete padded SII image");std::ofstream f(path,std::ios::binary|std::ios::trunc);if(!f)fail("Cannot create BIN file: "+path.filename().u8string());f.write(reinterpret_cast<const char*>(validated.bytes.data()),static_cast<std::streamsize>(validated.bytes.size()));f.flush();if(!f)fail("Failed to completely write BIN file: "+path.filename().u8string());error.clear();return true;}catch(const std::exception& e){error=e.what();return false;}}
bool toEsi(const Image& image,std::string& xml,std::string& error){try{auto result=exportEsi(image);xml=std::move(result);error.clear();return true;}catch(const std::exception& e){error=e.what();return false;}}
bool saveEsi(const std::filesystem::path& path,const Image& image,std::string& error){try{const auto xml=exportEsi(image);std::ofstream f(path,std::ios::binary|std::ios::trunc);if(!f)fail("Cannot create ESI file: "+path.filename().u8string());f.write(xml.data(),static_cast<std::streamsize>(xml.size()));f.flush();if(!f)fail("Failed to completely write ESI file: "+path.filename().u8string());error.clear();return true;}catch(const std::exception& e){error=e.what();return false;}}
} // namespace niceecatm::sii
