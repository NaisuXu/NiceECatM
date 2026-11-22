// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#include "esi/catalog.hpp"
#include "tinyxml2.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace niceecatm {
namespace {
namespace fs=std::filesystem;
using Element=tinyxml2::XMLElement;
constexpr size_t MaxFileBytes=32*1024*1024,MaxFiles=1024,MaxDevices=16384,MaxEntries=131072;
constexpr unsigned MaxDirectionBits=8*65536;
struct DeviceDescription {
    uint32_t vendor=0,product=0,revision=0;
    std::string file;
    std::vector<PdoEntry> entries;
    std::shared_ptr<const SdoDictionary> sdo;
};
std::string trim(std::string text) {
    const auto start=text.find_first_not_of(" \t\r\n");
    if(start==std::string::npos)return {};
    return text.substr(start,text.find_last_not_of(" \t\r\n")-start+1);
}
std::string lower(std::string text) {
    for(auto& c:text)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}
const char* localName(const char* name) {
    if(!name)return "";
    const auto* colon=std::strchr(name,':');return colon?colon+1:name;
}
bool named(const Element* node,const char* name) {return node&&std::string(localName(node->Name()))==name;}
const Element* child(const Element* parent,const char* name) {
    if(parent)for(auto* node=parent->FirstChildElement();node;node=node->NextSiblingElement())if(named(node,name))return node;
    return nullptr;
}
std::string text(const Element* node) {return node&&node->GetText()?trim(node->GetText()):std::string{};}
std::string text(const Element* node,const char* name) {return text(child(node,name));}
uint32_t number(const std::string& value,uint32_t maximum,const char* label) {
    auto s=trim(value);unsigned base=10;size_t offset=0;
    if(s.size()>2&&((s[0]=='#'&&s[1]=='x')||(s[0]=='0'&&(s[1]=='x'||s[1]=='X')))){base=16;offset=2;}
    if(offset==s.size())throw std::runtime_error(std::string("Invalid ESI ")+label);
    uint64_t result=0;
    for(size_t n=offset;n<s.size();++n) {
        const unsigned c=static_cast<unsigned char>(s[n]);
        const unsigned digit=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:99;
        if(digit>=base||result>(maximum-digit)/base||digit>maximum)throw std::runtime_error(std::string("Invalid ESI ")+label);
        result=result*base+digit;
    }
    if(s.empty())throw std::runtime_error(std::string("Missing ESI ")+label);
    return static_cast<uint32_t>(result);
}
std::vector<LocalizedText> names(const Element* parent,const char* tag="Name") {
    std::vector<LocalizedText> result;
    if(parent)for(auto* n=parent->FirstChildElement();n;n=n->NextSiblingElement())if(named(n,tag)) {
        auto value=text(n);if(value.empty())continue;
        if(value.size()>4096)throw std::runtime_error("ESI name exceeds 4096 bytes");
        uint32_t lcid=0;if(const char* attr=n->Attribute("LcId"))lcid=number(attr,65535,"name language");
        const auto duplicate=std::find_if(result.begin(),result.end(),[&](const LocalizedText& t){return t.lcid==lcid;});
        if(duplicate!=result.end()) {
            if(duplicate->text!=value)throw std::runtime_error("Conflicting ESI names for one language");
        } else result.push_back({lcid,std::move(value)});
    }
    return result;
}
void appendUtf8(std::string& out,uint32_t value) {
    if(value<0x80)out+=static_cast<char>(value);
    else if(value<0x800){out+=static_cast<char>(0xc0|(value>>6));out+=static_cast<char>(0x80|(value&63));}
    else {out+=static_cast<char>(0xe0|(value>>12));out+=static_cast<char>(0x80|((value>>6)&63));out+=static_cast<char>(0x80|(value&63));}
}
bool validUtf8(const std::string& s) {
    for(size_t n=0;n<s.size();) {
        auto c=static_cast<unsigned char>(s[n++]);if(c<0x80){if(c==0)return false;continue;}
        unsigned count=0;uint32_t value=0,minimum=0;
        if(c>=0xc2&&c<=0xdf){count=1;value=c&31;minimum=0x80;}
        else if(c>=0xe0&&c<=0xef){count=2;value=c&15;minimum=0x800;}
        else if(c>=0xf0&&c<=0xf4){count=3;value=c&7;minimum=0x10000;}
        else return false;
        if(count>s.size()-n)return false;
        while(count--){c=static_cast<unsigned char>(s[n++]);if((c&0xc0)!=0x80)return false;value=(value<<6)|(c&63);}
        if(value<minimum||value>0x10ffff||(value>=0xd800&&value<=0xdfff))return false;
    }
    return true;
}
std::string xmlUtf8(const std::string& bytes) {
    auto declaration=lower(bytes.substr(0,std::min<size_t>(bytes.size(),256)));
    const auto declEnd=declaration.find("?>");if(declEnd!=std::string::npos)declaration.resize(declEnd);
    const auto enc=declaration.find("encoding");
    std::string encoding="utf-8";
    if(enc!=std::string::npos) {
        const auto quote=declaration.find_first_of("\"'",enc+8);
        if(quote!=std::string::npos){const auto end=declaration.find(declaration[quote],quote+1);if(end!=std::string::npos)encoding=declaration.substr(quote+1,end-quote-1);}
    }
    if(encoding=="utf-8"||encoding=="utf8"||encoding=="us-ascii"||encoding=="ascii") {
        if(!validUtf8(bytes))throw std::runtime_error("Invalid UTF-8 in ESI file");
        return bytes;
    }
    if(encoding!="iso-8859-1"&&encoding!="iso8859-1"&&encoding!="windows-1252")throw std::runtime_error("Unsupported ESI encoding: "+encoding);
    constexpr std::array<uint16_t,32> cp1252{{0x20ac,0x81,0x201a,0x192,0x201e,0x2026,0x2020,0x2021,0x2c6,0x2030,0x160,0x2039,0x152,0x8d,0x17d,0x8f,0x90,0x2018,0x2019,0x201c,0x201d,0x2022,0x2013,0x2014,0x2dc,0x2122,0x161,0x203a,0x153,0x9d,0x17e,0x178}};
    std::string result;result.reserve(bytes.size());
    for(unsigned char c:bytes){if(c==0)throw std::runtime_error("NUL byte in ESI file");appendUtf8(result,encoding=="windows-1252"&&c>=0x80&&c<0xa0?cp1252[c-0x80]:c);}
    return result;
}
std::string readFile(const fs::path& path) {
    std::error_code ec;const auto size=fs::file_size(path,ec);
    if(ec)throw std::runtime_error("Cannot read ESI file: "+path.filename().u8string());
    if(size==0||size>MaxFileBytes)throw std::runtime_error("ESI file must contain 1 byte to 32 MiB: "+path.filename().u8string());
    std::ifstream stream(path,std::ios::binary);if(!stream)throw std::runtime_error("Cannot read ESI file: "+path.filename().u8string());
    std::string data(static_cast<size_t>(size),'\0');stream.read(data.data(),static_cast<std::streamsize>(size));
    if(!stream||stream.peek()!=std::char_traits<char>::eof())throw std::runtime_error("ESI file changed while reading: "+path.filename().u8string());
    return data;
}
using TypeMap=std::map<std::string,const Element*>;
const Element* dictionary(const Element* device) {return child(child(device,"Profile"),"Dictionary");}
TypeMap dictionaryTypes(const Element* device) {
    TypeMap result;const auto* parent=child(dictionary(device),"DataTypes");
    if(parent)for(auto* node=parent->FirstChildElement();node;node=node->NextSiblingElement())if(named(node,"DataType")) {
        const auto name=text(node,"Name");
        if(!name.empty()&&!result.emplace(name,node).second)throw std::runtime_error("Duplicate ESI dictionary data type");
    }
    return result;
}
std::string resolveType(const std::string& type,unsigned bits,const TypeMap& types,unsigned depth=0) {
    if(auto direct=pdoType(type,bits);!direct.empty())return direct;
    if(depth>=16)return {};
    const auto found=types.find(type);if(found==types.end())return {};
    const auto* node=found->second;
    if(const auto size=text(node,"BitSize");!size.empty()&&number(size,MaxDirectionBits,"type bit size")!=bits)return {};
    const auto base=text(node,"BaseType");return base.empty()?std::string{}:resolveType(base,bits,types,depth+1);
}
const Element* objectDescription(const Element* device,uint16_t index) {
    const auto* parent=child(dictionary(device),"Objects");
    if(parent)for(auto* node=parent->FirstChildElement();node;node=node->NextSiblingElement())if(named(node,"Object")) {
        if(number(text(node,"Index"),65535,"object index")==index)return node;
    }
    return nullptr;
}
void dictionaryEntry(const Element* device,const TypeMap& types,PdoEntry& entry) {
    const auto* object=objectDescription(device,entry.index);if(!object)return;
    const auto type=text(object,"Type");const auto found=types.find(type);const Element* description=entry.subindex==0?object:nullptr;
    if(found!=types.end())for(auto* sub=found->second->FirstChildElement();sub;sub=sub->NextSiblingElement())if(named(sub,"SubItem")) {
        const auto subIndex=text(sub,"SubIdx");if(!subIndex.empty()&&number(subIndex,255,"dictionary subindex")==entry.subindex){description=sub;break;}
    }
    if(!description)return;
    if(entry.names.empty())entry.names=names(description);
    if(entry.type.empty())entry.type=resolveType(text(description,"Type"),entry.bitLength,types);
}
std::vector<LocalizedText> descriptionNames(const Element* node) {
    auto value=names(node,"DisplayName");return value.empty()?names(node):value;
}
const Element* compositeType(const std::string& type,const TypeMap& types,unsigned depth=0) {
    const auto found=types.find(type);if(found==types.end())return nullptr;
    if(depth>=16)throw std::runtime_error("Cyclic or excessively nested ESI dictionary type");
    const auto* node=found->second;
    if(child(node,"SubItem")||child(node,"ArrayInfo"))return node;
    const auto base=text(node,"BaseType");return base.empty()?nullptr:compositeType(base,types,depth+1);
}
unsigned typeBits(const std::string& type,const TypeMap& types,unsigned depth=0) {
    if(depth>=16)throw std::runtime_error("Cyclic or excessively nested ESI dictionary type");
    const auto found=types.find(type);
    if(found!=types.end()) {
        const auto bits=text(found->second,"BitSize");
        if(!bits.empty())return number(bits,MaxDirectionBits,"dictionary type bit size");
        const auto base=text(found->second,"BaseType");if(!base.empty())return typeBits(base,types,depth+1);
    }
    const auto value=lower(type);
    if(value=="bool"||value=="bit")return 1;
    if(value=="sint"||value=="usint"||value=="byte")return 8;
    if(value=="int"||value=="uint"||value=="word")return 16;
    if(value=="dint"||value=="udint"||value=="dword"||value=="real")return 32;
    if(value=="lint"||value=="ulint"||value=="lword"||value=="lreal")return 64;
    for(const auto* prefix:{"uint","int","bitarr","bit","real"}) {
        const std::string tag=prefix;
        if(value.compare(0,tag.size(),tag)==0) {
            const auto width=value.substr(tag.size());
            if(!width.empty()&&width.size()<=2&&std::all_of(width.begin(),width.end(),[](char c){return c>='0'&&c<='9';}))return number(width,64,"dictionary scalar width");
        }
    }
    if(value.compare(0,7,"string(")==0&&value.back()==')')return number(value.substr(7,value.size()-8),MaxDirectionBits/8,"dictionary string length")*8;
    return 0;
}
std::string sdoScalar(const std::string& type,unsigned bits,const TypeMap& types,unsigned depth=0) {
    if(auto value=pdoType(type,bits);!value.empty())return value;
    const auto value=lower(type);
    if(value=="string"||value=="visible_string"||value.compare(0,7,"string(")==0)return "STRING";
    const auto found=types.find(type);
    if(found!=types.end()&&!child(found->second,"SubItem")&&!child(found->second,"ArrayInfo")) {
        if(depth>=16)throw std::runtime_error("Cyclic or excessively nested ESI dictionary type");
        const auto base=text(found->second,"BaseType");
        if(!base.empty())return sdoScalar(base,bits,types,depth+1);
    }
    return type; // Unsupported scalar or complex entries remain explicitly raw.
}
uint16_t scalarCode(const std::string& type,unsigned bits) {
    if(type=="STRING")return 0x0009;
    for(uint16_t code=1;code<=0x37;++code)if(!type.empty()&&pdoType(code,bits)==type)return code;
    return 0;
}
unsigned accessStates(const char* attribute,bool& valid) {
    if(!attribute)return 7;
    const auto value=lower(attribute);
    if(value=="preop")return 1;
    if(value=="preop_safeop")return 3;
    if(value=="preop_op")return 5;
    if(value=="safeop")return 2;
    if(value=="safeop_op")return 6;
    if(value=="op")return 4;
    valid=false;return 0;
}
void sdoAccess(const Element* description,const Element* object,SdoEntry& entry) {
    auto* access=child(child(description,"Flags"),"Access");
    if(!access)access=child(child(object,"Flags"),"Access");
    if(!access)return;
    const auto value=lower(text(access));if(value!="ro"&&value!="rw"&&value!="wo")return;
    bool valid=true;
    const auto read=accessStates(access->Attribute("ReadRestrictions"),valid);
    const auto write=accessStates(access->Attribute("WriteRestrictions"),valid);
    if(!valid)return;
    entry.accessKnown=true;entry.access=uint16_t((value!="wo"?read:0)|(value!="ro"?write<<3:0));
}
std::shared_ptr<const SdoDictionary> sdoDictionary(const Element* device,const TypeMap& types,size_t& total) {
    const auto* parent=child(dictionary(device),"Objects");if(!parent)return {};
    auto result=std::make_shared<SdoDictionary>();result->complete=true;
    std::map<uint16_t,SdoObject> objects;
    for(auto* node=parent->FirstChildElement();node;node=node->NextSiblingElement())if(named(node,"Object")) {
        SdoObject object;object.index=uint16_t(number(text(node,"Index"),65535,"dictionary index"));object.esi=true;object.names=descriptionNames(node);
        if(objects.count(object.index))throw std::runtime_error("Duplicate ESI dictionary object index");
        const auto add=[&](const Element* description,unsigned sub,const std::string& type,unsigned bits,bool array) {
            if(++total>MaxEntries)throw std::runtime_error("Too many ESI dictionary entries");
            SdoEntry entry;entry.subindex=uint8_t(sub);entry.esi=true;entry.bitLength=bits;entry.type=sdoScalar(type,bits,types);entry.dataType=scalarCode(entry.type,bits);
            entry.names=descriptionNames(description);sdoAccess(description,node,entry);
            if(array)for(auto& name:entry.names)name.text+=" ["+std::to_string(sub)+"]";
            if(std::any_of(object.entries.begin(),object.entries.end(),[&](const SdoEntry& prior){return prior.subindex==entry.subindex;}))throw std::runtime_error("Duplicate ESI dictionary subindex");
            object.maxSubindex=std::max(object.maxSubindex,entry.subindex);object.entries.push_back(std::move(entry));
        };
        const auto array=[&](const Element* definition,const Element* description) {
            const auto* info=child(definition,"ArrayInfo");
            if(!info)return false;
            unsigned dimensions=0;for(auto* item=definition->FirstChildElement();item;item=item->NextSiblingElement())if(named(item,"ArrayInfo"))++dimensions;
            const auto bound=text(info,"LBound"),count=text(info,"Elements"),base=text(definition,"BaseType");
            if(dimensions!=1||bound.empty()||bound[0]=='-'||base.empty())return false;
            const auto first=number(bound,255,"dictionary array bound"),length=number(count,256,"dictionary array length");
            if(!length||length>256-first)return false;
            const auto declared=text(definition,"BitSize");
            const auto totalBits=declared.empty()?0:number(declared,MaxDirectionBits,"dictionary array bit size");
            if(totalBits&&totalBits%length)return false;
            const auto bits=totalBits?totalBits/length:typeBits(base,types);
            if(compositeType(base,types))return false;
            for(unsigned offset=0;offset<length;++offset)add(description,first+offset,base,bits,true);
            return true;
        };
        const auto type=text(node,"Type");const auto* definition=compositeType(type,types);
        if(definition&&child(definition,"SubItem")) {
            object.objectCode=9;
            for(auto* sub=definition->FirstChildElement();sub;sub=sub->NextSiblingElement())if(named(sub,"SubItem")) {
                const auto subType=text(sub,"Type"),subIndex=text(sub,"SubIdx");const auto* shape=compositeType(subType,types);
                if(shape&&child(shape,"ArrayInfo")) {
                    if(!array(shape,sub))result->complete=false;
                } else if(!subIndex.empty()) {
                    const auto bits=text(sub,"BitSize");
                    add(sub,number(subIndex,255,"dictionary subindex"),subType,bits.empty()?typeBits(subType,types):number(bits,MaxDirectionBits,"dictionary entry bit size"),false);
                } else result->complete=false; // Padding/unnumbered fields are not SDO addresses.
            }
        } else if(definition&&child(definition,"ArrayInfo")) {
            object.objectCode=8;if(!array(definition,node))result->complete=false;
        } else {
            object.objectCode=7;const auto bits=text(node,"BitSize");
            add(node,0,type,bits.empty()?typeBits(type,types):number(bits,MaxDirectionBits,"dictionary entry bit size"),false);
        }
        std::sort(object.entries.begin(),object.entries.end(),[](const SdoEntry& a,const SdoEntry& b){return a.subindex<b.subindex;});
        objects.emplace(object.index,std::move(object));
        if(objects.size()>65536)throw std::runtime_error("Too many ESI dictionary objects");
    }
    for(auto& item:objects)result->objects.push_back(std::move(item.second));
    result->status=result->complete?"ESI object dictionary":"ESI object dictionary (partial)";
    return result;
}
std::vector<DeviceDescription> parseFile(const std::string& data,const fs::path& file) {
    auto utf8=xmlUtf8(data);tinyxml2::XMLDocument xml;
    if(xml.Parse(utf8.data(),utf8.size())!=tinyxml2::XML_SUCCESS)throw std::runtime_error("Malformed ESI XML: "+file.filename().u8string()+" (line "+std::to_string(xml.ErrorLineNum())+")");
    for(auto* node=xml.FirstChild();node;node=node->NextSibling())if(node->ToUnknown())throw std::runtime_error("ESI DTD and external declarations are unsupported");
    const auto* root=xml.RootElement();if(!named(root,"EtherCATInfo")||root->NextSiblingElement())throw std::runtime_error("Not an EtherCAT ESI file: "+file.filename().u8string());
    const auto vendor=number(text(child(root,"Vendor"),"Id"),std::numeric_limits<uint32_t>::max(),"Vendor ID");
    const auto* devices=child(child(root,"Descriptions"),"Devices");if(!devices)throw std::runtime_error("Missing ESI device descriptions");
    std::vector<DeviceDescription> result;size_t entryCount=0,dictionaryCount=0;
    for(auto* node=devices->FirstChildElement();node;node=node->NextSiblingElement())if(named(node,"Device")) {
        if(result.size()>=MaxDevices)throw std::runtime_error("Too many ESI device descriptions");
        DeviceDescription device;device.vendor=vendor;device.file=file.u8string();const auto* identity=child(node,"Type");
        if(!identity||!identity->Attribute("ProductCode"))throw std::runtime_error("Missing ESI ProductCode");
        device.product=number(identity->Attribute("ProductCode"),std::numeric_limits<uint32_t>::max(),"ProductCode");
        device.revision=identity->Attribute("RevisionNo")?number(identity->Attribute("RevisionNo"),std::numeric_limits<uint32_t>::max(),"RevisionNo"):0;
        const auto types=dictionaryTypes(node);unsigned offsets[2]={0,0};
        device.sdo=sdoDictionary(node,types,dictionaryCount);
        for(auto* pdo=node->FirstChildElement();pdo;pdo=pdo->NextSiblingElement()) {
            const bool input=named(pdo,"TxPdo");if(!input&&!named(pdo,"RxPdo"))continue;
            const auto pdoIndex=number(text(pdo,"Index"),65535,"PDO index");
            int sm=-1;if(const auto* value=pdo->Attribute("Sm"))sm=static_cast<int>(number(value,255,"PDO SyncManager"));
            for(auto* field=pdo->FirstChildElement();field;field=field->NextSiblingElement())if(named(field,"Entry")) {
                if(++entryCount>MaxEntries)throw std::runtime_error("Too many ESI PDO entries");
                PdoEntry entry;entry.input=input;entry.pdoIndex=static_cast<uint16_t>(pdoIndex);entry.syncManager=sm;
                const auto idx=text(field,"Index"),sub=text(field,"SubIndex");
                entry.index=idx.empty()?0:static_cast<uint16_t>(number(idx,65535,"entry index"));
                entry.subindex=sub.empty()?0:static_cast<uint8_t>(number(sub,255,"entry subindex"));
                entry.padding=entry.index==0&&entry.subindex==0;
                entry.bitLength=number(text(field,"BitLen"),MaxDirectionBits,"entry bit length");
                if(!entry.bitLength||entry.bitLength>MaxDirectionBits-offsets[input])throw std::runtime_error("ESI PDO direction exceeds 64 KiB or contains an empty entry");
                entry.bitOffset=offsets[input];offsets[input]+=entry.bitLength;
                entry.names=names(field);entry.type=resolveType(text(field,"DataType"),entry.bitLength,types);entry.source="ESI";
                if(!entry.padding)dictionaryEntry(node,types,entry);
                device.entries.push_back(std::move(entry));
            }
        }
        result.push_back(std::move(device));
    }
    if(result.empty())throw std::runtime_error("No devices in ESI file");
    return result;
}
bool sameNames(const std::vector<LocalizedText>& a,const std::vector<LocalizedText>& b) {
    if(a.size()!=b.size())return false;
    for(const auto& x:a)if(std::none_of(b.begin(),b.end(),[&](const LocalizedText& y){return x.lcid==y.lcid&&x.text==y.text;}))return false;
    return true;
}
bool sameEntry(const PdoEntry& a,const PdoEntry& b) {
    return a.input==b.input&&a.padding==b.padding&&a.pdoIndex==b.pdoIndex&&a.index==b.index&&a.subindex==b.subindex&&a.bitLength==b.bitLength&&a.syncManager==b.syncManager&&a.type==b.type&&sameNames(a.names,b.names);
}
bool sameSdo(const std::shared_ptr<const SdoDictionary>& a,const std::shared_ptr<const SdoDictionary>& b) {
    if(!a||!b)return !a&&!b;
    if(a->complete!=b->complete||a->objects.size()!=b->objects.size())return false;
    for(size_t n=0;n<a->objects.size();++n) {
        const auto& x=a->objects[n];const auto& y=b->objects[n];
        if(x.index!=y.index||x.objectCode!=y.objectCode||x.maxSubindex!=y.maxSubindex||!sameNames(x.names,y.names)||x.entries.size()!=y.entries.size())return false;
        for(size_t i=0;i<x.entries.size();++i) {
            const auto& p=x.entries[i];const auto& q=y.entries[i];
            if(p.subindex!=q.subindex||p.dataType!=q.dataType||p.access!=q.access||p.accessKnown!=q.accessKnown||p.bitLength!=q.bitLength||p.type!=q.type||!sameNames(p.names,q.names))return false;
        }
    }
    return true;
}
bool sameDevice(const DeviceDescription& a,const DeviceDescription& b) {
    if(a.entries.size()!=b.entries.size()||!sameSdo(a.sdo,b.sdo))return false;
    for(size_t n=0;n<a.entries.size();++n)if(!sameEntry(a.entries[n],b.entries[n]))return false;
    return true;
}
bool verifiedDirection(const Slave& slave,bool input) {
    if(!(input?slave.inputLayoutKnown:slave.outputLayoutKnown))return false;
    unsigned offset=0;const auto bits=input?slave.inputBits:slave.outputBits;
    for(const auto& entry:slave.pdoEntries)if(entry.input==input) {
        if(!entry.bitLength||entry.bitOffset!=offset||entry.bitLength>bits-offset)return false;
        offset+=entry.bitLength;
    }
    return offset==bits;
}
bool sameBinding(const PdoEntry& online,const PdoEntry& esi) {
    return online.input==esi.input&&online.padding==esi.padding&&online.index==esi.index&&online.subindex==esi.subindex&&online.bitLength==esi.bitLength&&
        (online.pdoIndex==0||online.pdoIndex==esi.pdoIndex)&&
        (online.syncManager<0||esi.syncManager<0||online.syncManager==esi.syncManager);
}
std::string hexId(uint32_t value) {
    std::ostringstream out;
    out<<"0x"<<std::uppercase<<std::hex<<std::setw(8)<<std::setfill('0')<<value;
    return out.str();
}
}

struct EsiCatalog::Impl {
    fs::path directory;
    std::vector<DeviceDescription> devices;
    size_t files=0;
    uint64_t revision=0;
    std::string status;
    std::vector<std::string> loadIssues;
    bool directoryExists=false;
    explicit Impl(fs::path path):directory(std::move(path)){}
};
EsiCatalog::EsiCatalog(fs::path directory):impl_(std::make_shared<Impl>(std::move(directory))){reload();}
Result EsiCatalog::reload() {
    ++impl_->revision;impl_->devices.clear();impl_->files=0;
    auto& failures=impl_->loadIssues;failures.clear();
    std::vector<fs::path> paths;
    try {
        std::error_code ec;
        impl_->directoryExists=fs::exists(impl_->directory,ec);
        if(impl_->directoryExists) {
            if(!fs::is_directory(impl_->directory))throw std::runtime_error("ESI library path is not a directory");
            for(const auto& item:fs::directory_iterator(impl_->directory))if(item.is_regular_file()&&lower(item.path().extension().u8string())==".xml") {
                if(paths.size()>=MaxFiles)throw std::runtime_error("ESI library exceeds 1024 XML files");
                paths.push_back(item.path());
            }
        } else if(ec)throw std::runtime_error("Cannot access ESI library directory");
        std::sort(paths.begin(),paths.end());
        for(const auto& path:paths)try {
            auto parsed=parseFile(readFile(path),path);
            if(impl_->devices.size()+parsed.size()>MaxDevices)throw std::runtime_error("ESI library exceeds 16384 device descriptions");
            for(auto& device:parsed) {
                bool duplicate=false;
                for(const auto& prior:impl_->devices)if(std::tie(prior.vendor,prior.product,prior.revision)==std::tie(device.vendor,device.product,device.revision)&&sameDevice(prior,device)){duplicate=true;break;}
                if(!duplicate)impl_->devices.push_back(std::move(device));
            }
            ++impl_->files;
        }catch(const std::exception& e){if(failures.size()<8)failures.push_back(path.filename().u8string()+": "+e.what());}
    }catch(const fs::filesystem_error&){failures.push_back("Cannot access ESI library directory");}
    catch(const std::exception& e){failures.push_back(e.what());}
    impl_->status="ESI library: "+std::to_string(impl_->files)+" file(s), "+std::to_string(impl_->devices.size())+" device description(s)";
    for(const auto& failure:failures)impl_->status+="; "+failure;
    return {failures.empty(),impl_->status,{}};
}
void EsiCatalog::decorate(Snapshot& view) const {
    view.esiDirectory=impl_->directory.u8string();view.esiFiles=impl_->files;view.esiDevices=impl_->devices.size();view.esiStatus=impl_->status;view.descriptionRevision=impl_->revision;
    for(auto& slave:view.slaves) {
        slave.esiFile.clear();slave.esiSdoDictionary.reset();slave.esiStatus="No matching ESI description";
        if(view.simulation){slave.esiStatus="Simulation fixture";continue;}
        const DeviceDescription* exact=nullptr;bool family=false,ambiguous=false;
        for(const auto& candidate:impl_->devices)if(candidate.vendor==slave.vendor&&candidate.product==slave.product) {
            family=true;if(candidate.revision!=slave.revision)continue;
            if(exact){ambiguous=true;break;}exact=&candidate;
        }
        if(ambiguous){slave.esiStatus="Ambiguous ESI device identity";continue;}
        if(!exact){if(family)slave.esiStatus="ESI revision mismatch";continue;}
        slave.esiFile=exact->file;
        slave.esiSdoDictionary=exact->sdo;
        const bool verified[2]={verifiedDirection(slave,false),verifiedDirection(slave,true)};
        size_t matched=0,missing=0;bool unavailable=false;
        for(bool input:{false,true})if((input?slave.inputBits:slave.outputBits)&&!verified[input])unavailable=true;
        for(auto& online:slave.pdoEntries) {
            if(!verified[online.input]||online.padding)continue;
            const PdoEntry* descriptor=nullptr;bool conflict=false;
            for(const auto& entry:exact->entries)if(sameBinding(online,entry)) {
                if(descriptor&&(descriptor->type!=entry.type||!sameNames(descriptor->names,entry.names))){conflict=true;break;}
                descriptor=&entry;
            }
            if(!descriptor||conflict||(!online.type.empty()&&!descriptor->type.empty()&&online.type!=descriptor->type)){++missing;continue;}
            ++matched;
            if(online.type.empty())online.type=descriptor->type;
            // ESI supplies multilingual labels. Preserve additional online
            // languages but prefer the exact-matched ESI text for its LCID.
            auto merged=descriptor->names;
            for(const auto& name:online.names) {
                // Online/SII strings have no declared language. A generic
                // neutral string must not hide an English-only ESI label in
                // the Chinese UI; retain only explicit extra languages here.
                if(!descriptor->names.empty()&&name.lcid==0)continue;
                if(std::none_of(merged.begin(),merged.end(),[&](const LocalizedText& n){return n.lcid==name.lcid;}))merged.push_back(name);
            }
            online.names=std::move(merged);
            if((!descriptor->names.empty()||!descriptor->type.empty())&&online.source.find("ESI")==std::string::npos)online.source+=(online.source.empty()?"":" + ")+std::string("ESI");
        }
        if(unavailable)slave.esiStatus="ESI matched; online layout unavailable";
        else if(missing)slave.esiStatus=matched?"ESI matched; partial PDO description":"ESI matched; PDO entries differ";
        else slave.esiStatus="ESI matched";
    }
}
std::vector<std::string> EsiCatalog::scanDiagnostics(const Snapshot& decorated) const {
    if(decorated.simulation||decorated.slaves.empty())return {};
    std::vector<std::string> messages;
    size_t affected=0;
    for(const auto& slave:decorated.slaves) {
        if(slave.esiStatus.empty()||slave.esiStatus=="ESI matched")continue;
        if(++affected>8)continue;
        auto line=slave.esiStatus+" | #"+std::to_string(slave.position)+" "+slave.name+
            "  VendorId="+hexId(slave.vendor)+"  ProductCode="+hexId(slave.product)+"  RevisionNo="+hexId(slave.revision);
        if(!slave.esiFile.empty())line+="  XML: "+fs::u8path(slave.esiFile).filename().u8string();
        if(slave.esiStatus=="ESI revision mismatch"||slave.esiStatus=="Ambiguous ESI device identity") {
            size_t candidates=0;
            for(const auto& device:impl_->devices) {
                if(device.vendor!=slave.vendor||device.product!=slave.product)continue;
                if(slave.esiStatus=="Ambiguous ESI device identity"&&device.revision!=slave.revision)continue;
                if(candidates==3){line+=" …";break;}
                line+=(candidates++?", ":"  XML: ")+fs::u8path(device.file).filename().u8string()+
                    " (RevisionNo="+hexId(device.revision)+")";
            }
        }
        messages.push_back(std::move(line));
    }
    if(affected>8)messages.push_back("Additional slaves with ESI diagnostics | "+std::to_string(affected-8));
    if(messages.empty()&&impl_->loadIssues.empty())return {};
    std::string directoryReason="ESI XML directory";
    if(impl_->files==0&&impl_->loadIssues.empty())
        directoryReason=impl_->directoryExists?"No XML files in the ESI directory":"ESI directory does not exist";
    messages.insert(messages.begin(),directoryReason+" | "+impl_->directory.u8string());
    messages.insert(messages.end(),impl_->loadIssues.begin(),impl_->loadIssues.end());
    return messages;
}
}
