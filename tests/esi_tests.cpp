// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#include "esi/catalog.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace niceecatm;
namespace {
namespace fs=std::filesystem;
int checks=0;
void check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
struct Temporary {
    fs::path directory=fs::temp_directory_path()/fs::u8path("NiceECatM-esi-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Temporary(){fs::create_directories(directory);}
    ~Temporary(){std::error_code ec;fs::remove_all(directory,ec);}
};
void write(const fs::path& path,const std::string& value) {
    fs::create_directories(path.parent_path());std::ofstream stream(path,std::ios::binary);stream<<value;
    if(!stream)throw std::runtime_error("Cannot write test fixture");
}
std::string entry(const std::string& index,int sub,int bits,const std::string& name,const std::string& type="UINT") {
    return "<Entry><Index>"+index+"</Index><SubIndex>"+std::to_string(sub)+"</SubIndex><BitLen>"+std::to_string(bits)+"</BitLen>"+name+"<DataType>"+type+"</DataType></Entry>";
}
std::string document(const std::string& pdos,const std::string& revision="#x1",const std::string& extra="") {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?><EtherCATInfo><Vendor><Id>#xB95</Id></Vendor><Descriptions><Devices><Device><Type ProductCode=\"#x10200\" RevisionNo=\""+revision+"\">Test</Type>"+extra+pdos+"</Device></Devices></Descriptions></EtherCATInfo>";
}
PdoEntry online(bool input,unsigned offset,unsigned bits,uint16_t pdo,uint16_t index,uint8_t sub,int sm=-1) {
    PdoEntry e;e.input=input;e.bitOffset=offset;e.bitLength=bits;e.pdoIndex=pdo;e.index=index;e.subindex=sub;e.padding=!index&&!sub;e.syncManager=sm;e.source="CoE";return e;
}
Snapshot snapshot(std::vector<PdoEntry> entries,unsigned inputs,unsigned outputs) {
    Snapshot s;s.simulation=false;s.connected=s.mapped=true;Slave d;d.position=1;d.vendor=0xb95;d.product=0x10200;d.revision=1;
    d.inputLayoutKnown=d.outputLayoutKnown=true;d.inputBits=inputs;d.outputBits=outputs;d.inputs.resize((inputs+7)/8);d.outputs.resize((outputs+7)/8);d.pdoEntries=std::move(entries);s.slaves.push_back(std::move(d));return s;
}
std::string pdo(const std::string& tag,const std::string& index,const std::string& fields,int sm=2) {
    return "<"+tag+" Sm=\""+std::to_string(sm)+"\"><Index>"+index+"</Index>"+fields+"</"+tag+">";
}
}
int main() {
    try {
        Temporary temp;const auto library=temp.directory/fs::u8path(u8"描述库");EsiCatalog catalog(library);Snapshot view;
        catalog.decorate(view);check(view.esiFiles==0&&view.esiDevices==0&&!fs::exists(library),"Empty library does not create a folder");
        const auto source=library/fs::u8path(u8"信号.xml");
        const auto labels="<Name LcId=\"1033\">Setpoint &amp; limit</Name><Name LcId=\"2052\">速度设定</Name>";
        const auto fields=entry("#x7000",1,3,labels,"UINT3")+entry("0",0,1,"","")+entry("#x7000",2,12,"<Name>Position</Name>","UINT12");
        write(source,document(pdo("RxPdo","#x1600",fields)));
        check(catalog.reload().ok,"ESI loads from a Unicode library and filename");
        auto s=snapshot({online(false,0,3,0x1600,0x7000,1,2),online(false,3,1,0x1600,0,0,2),online(false,4,12,0x1600,0x7000,2,2)},0,16);
        catalog.decorate(s);
        check(s.esiFiles==1&&s.esiDevices==1&&s.slaves[0].esiStatus=="ESI matched","Exact identity and online layout matched");
        check(s.slaves[0].esiFile==source.u8string(),"Matched metadata retains the actual source file path");
        check(pdoEntryName(s.slaves[0].pdoEntries[0],false)=="Setpoint & limit"&&pdoEntryName(s.slaves[0].pdoEntries[0],true)==u8"速度设定","ESI entities and localized names decoded");
        check(s.slaves[0].pdoEntries[0].type=="UINT3"&&s.slaves[0].pdoEntries[2].type=="UINT12","Nonbyte integer types resolved from ESI");
        check(s.slaves[0].pdoEntries[1].padding&&s.slaves[0].pdoEntries[2].bitOffset==4,"Padding preserved in actual offsets");
        check(pdoEntryName(s.slaves[0].pdoEntries[2],true)=="Position","English-only neutral name retained in Chinese");
        const auto revision=s.descriptionRevision;check(catalog.reload().ok,"An unchanged library can be loaded again");
        catalog.decorate(s);check(s.esiFiles==1&&s.descriptionRevision>revision,"Reload refreshes metadata revision without duplicating files");
        EsiCatalog restarted(library);auto fresh=snapshot({online(false,0,3,0x1600,0x7000,1),online(false,3,1,0x1600,0,0),online(false,4,12,0x1600,0x7000,2)},0,16);
        restarted.decorate(fresh);check(pdoEntryName(fresh.slaves[0].pdoEntries[0],false)=="Setpoint & limit","Library persists between instances");
        auto mismatch=snapshot({online(false,0,16,0x1600,0x7000,1)},0,16);catalog.decorate(mismatch);
        check(mismatch.slaves[0].pdoEntries[0].names.empty()&&mismatch.slaves[0].esiStatus=="ESI matched; PDO entries differ","Matching byte length cannot bind different entries");
        auto unknown=snapshot({},0,16);unknown.slaves[0].outputLayoutKnown=false;catalog.decorate(unknown);
        check(unknown.slaves[0].pdoEntries.empty()&&unknown.slaves[0].esiStatus=="ESI matched; online layout unavailable","ESI never invents unverified offsets");
        auto wrongRevision=fresh;wrongRevision.slaves[0].revision=2;catalog.decorate(wrongRevision);
        check(wrongRevision.slaves[0].esiFile.empty()&&wrongRevision.slaves[0].esiStatus=="ESI revision mismatch","Revision mismatch never chooses newest description");
        auto gap=snapshot({online(false,1,3,0x1600,0x7000,1),online(false,4,12,0x1600,0x7000,2)},0,16);catalog.decorate(gap);
        check(gap.slaves[0].pdoEntries[0].names.empty(),"Incomplete actual layout cannot bind ESI");
        auto overrun=snapshot({online(false,0,17,0x1600,0x7000,1)},0,16);catalog.decorate(overrun);
        check(overrun.slaves[0].pdoEntries[0].names.empty(),"Out of bounds actual layout cannot bind ESI");
        auto noEsi=snapshot({online(false,0,16,0x1600,0x7000,1)},0,16);noEsi.slaves[0].vendor=0x1234;
        noEsi.slaves[0].pdoEntries[0].type="INT16";noEsi.slaves[0].pdoEntries[0].names={{0,"Trusted online name"}};catalog.decorate(noEsi);
        check(noEsi.slaves[0].pdoEntries[0].type=="INT16"&&pdoEntryName(noEsi.slaves[0].pdoEntries[0],true)=="Trusted online name","Missing ESI preserves trusted online type and name");

        const auto dynamicLib=temp.directory/"dynamic";EsiCatalog dynamic(dynamicLib);
        const auto dynamicSource=dynamicLib/"dynamic.xml";
        write(dynamicSource,document(pdo("TxPdo","#x1A00",entry("#x6000",1,16,"<Name LcId=\"1033\">First</Name>","INT"),3)+pdo("TxPdo","#x1A01",entry("#x6010",1,32,"<Name LcId=\"1033\">Second</Name>","REAL"),3)));
        check(dynamic.reload().ok,"Dynamic-layout description loaded from the library");
        auto ordered=snapshot({online(true,0,32,0x1a01,0x6010,1,3),online(true,32,16,0x1a00,0x6000,1,3)},48,0);dynamic.decorate(ordered);
        check(pdoEntryName(ordered.slaves[0].pdoEntries[0],false)=="Second"&&ordered.slaves[0].pdoEntries[0].type=="REAL32"&&ordered.slaves[0].pdoEntries[1].bitOffset==32,"Actual PDO assignment order overrides ESI default order");
        check(pdoEntryName(ordered.slaves[0].pdoEntries[0],true)=="Second","English LCID-only description retains English in Chinese");
        auto onlineNeutral=snapshot({online(true,0,32,0x1a01,0x6010,1,3),online(true,32,16,0x1a00,0x6000,1,3)},48,0);
        onlineNeutral.slaves[0].pdoEntries[0].names={{0,"Generic input"}};dynamic.decorate(onlineNeutral);
        check(pdoEntryName(onlineNeutral.slaves[0].pdoEntries[0],true)=="Second","Neutral SII label does not hide English-only ESI label in Chinese");
        auto conflicting=snapshot({online(true,0,32,0x1a01,0x6010,1,3),online(true,32,16,0x1a00,0x6000,1,3)},48,0);
        conflicting.slaves[0].pdoEntries[0].type="UINT32";conflicting.slaves[0].pdoEntries[0].names={{0,"Online value"}};dynamic.decorate(conflicting);
        check(conflicting.slaves[0].pdoEntries[0].type=="UINT32"&&pdoEntryName(conflicting.slaves[0].pdoEntries[0],false)=="Online value","Conflicting ESI type never overrides online metadata");
        const auto duplicateEntries=temp.directory/"entry-conflict"/"entry-conflict.xml";EsiCatalog entryConflict(duplicateEntries.parent_path());
        write(duplicateEntries,document(pdo("RxPdo","#x1600",entry("#x7000",1,16,"<Name>Choice A</Name>")+entry("#x7000",1,16,"<Name>Choice B</Name>"))));
        check(entryConflict.reload().ok,"Ambiguous entry description can still be inspected as a library file");
        auto repeated=snapshot({online(false,0,16,0x1600,0x7000,1)},0,16);entryConflict.decorate(repeated);
        check(repeated.slaves[0].pdoEntries[0].names.empty()&&repeated.slaves[0].esiStatus=="ESI matched; PDO entries differ","Conflicting descriptions for one online entry are never selected arbitrarily");

        const auto dictionarySource=temp.directory/"dictionary"/"dictionary.xml";EsiCatalog dictionaryCatalog(dictionarySource.parent_path());
        const std::string dictionary="<Profile><Dictionary><DataTypes><DataType><Name>SignedAlias</Name><BitSize>16</BitSize><BaseType>INT</BaseType></DataType><DataType><Name>Record</Name><SubItem><SubIdx>1</SubIdx><Name>Dictionary label</Name><Type>SignedAlias</Type><BitSize>16</BitSize></SubItem></DataType></DataTypes><Objects><Object><Index>#x7000</Index><Type>Record</Type></Object></Objects></Dictionary></Profile>";
        write(dictionarySource,document(pdo("RxPdo","#x1600",entry("#x7000",1,16,"","")),"1",dictionary));
        check(dictionaryCatalog.reload().ok,"Dictionary aliases parsed");
        auto aliased=snapshot({online(false,0,16,0x1600,0x7000,1)},0,16);dictionaryCatalog.decorate(aliased);
        check(aliased.slaves[0].pdoEntries[0].type=="INT16"&&pdoEntryName(aliased.slaves[0].pdoEntries[0],false)=="Dictionary label","Dictionary supplies missing entry name and type");

        const auto invalidLib=temp.directory/"invalid";
        const auto invalid=invalidLib/"invalid.xml";const auto valid=invalidLib/"valid.xml";
        write(invalid,"<EtherCATInfo>");write(valid,document(""));
        EsiCatalog invalidCatalog(invalidLib);check(!invalidCatalog.reload().ok,"Malformed library file reports a load failure");
        Snapshot partiallyValid;invalidCatalog.decorate(partiallyValid);
        check(partiallyValid.esiFiles==1&&partiallyValid.esiDevices==1,"A malformed file does not hide valid descriptions in the same library");
        for(const auto& data:std::vector<std::string>{document(pdo("RxPdo","#x1600",entry("#x10000",1,16,""))),document(pdo("RxPdo","#x1600",entry("#x7000",256,16,""))),document(pdo("RxPdo","#x1600",entry("#x7000",1,0,""))),document(pdo("RxPdo","#x1600",entry("#x7000",1,524289,""))),document("","4294967296"),"<!DOCTYPE EtherCATInfo SYSTEM 'file:///never-read'>"+document(""),document("")+"<extra/>"}) {
            write(invalid,data);check(!invalidCatalog.reload().ok,"Malformed identity, unsafe widths and external declarations rejected");
        }
        write(invalid,document(pdo("RxPdo","#x1600",entry("#x7000",1,16,std::string("<Name>")+char(0xff)+"</Name>"))));
        check(!invalidCatalog.reload().ok,"Invalid UTF-8 is rejected when the library loads");
        const auto conflictSource=library/"conflict.xml";write(conflictSource,document(pdo("RxPdo","#x1600",entry("#x7000",1,16,"<Name>Different</Name>"))));
        check(catalog.reload().ok,"Conflicting identity is retained for explicit ambiguity reporting");
        auto ambiguous=fresh;catalog.decorate(ambiguous);
        check(ambiguous.slaves[0].esiStatus=="Ambiguous ESI device identity"&&ambiguous.esiFiles==2,"Conflicting identities do not silently override earlier description");
        const auto duplicate=library/"duplicate.xml";write(duplicate,document(pdo("RxPdo","#x1600",fields)));
        check(restarted.reload().ok,"Equivalent description under another filename loads");
        auto dedup=fresh;restarted.decorate(dedup);check(dedup.esiDevices==2,"Equivalent metadata is deduplicated while conflicting identity remains distinct");

        const auto latin=temp.directory/"latin"/"latin.xml";auto latinData=document(pdo("RxPdo","#x1600",entry("#x7000",1,8,"<Name>Temp\xe9rature</Name>","USINT")));
        latinData.replace(latinData.find("UTF-8"),5,"ISO-8859-1");write(latin,latinData);
        EsiCatalog latinCatalog(latin.parent_path());check(latinCatalog.reload().ok,"ISO-8859-1 ESI accepted");
        auto latinView=snapshot({online(false,0,8,0x1600,0x7000,1)},0,8);latinCatalog.decorate(latinView);
        check(pdoEntryName(latinView.slaves[0].pdoEntries[0],false)==u8"Température","Latin-1 labels converted to UTF-8");

        fs::remove(latin);EsiCatalog removedFile(latin.parent_path());
        auto withoutFile=snapshot({online(false,0,8,0x1600,0x7000,1)},0,8);removedFile.decorate(withoutFile);
        check(withoutFile.esiFiles==0&&withoutFile.slaves[0].esiFile.empty()&&withoutFile.slaves[0].pdoEntries[0].names.empty(),"Removing an ESI file leaves no persisted metadata in a new instance");

#ifdef NICEECATM_SOURCE_DIR
        const fs::path sourceRoot=NICEECATM_SOURCE_DIR;EsiCatalog hardware(sourceRoot/"resources"/"esi");
        auto hardwareView=snapshot({online(false,0,8,0x1a00,0x3101,1,0),online(false,8,8,0x1a01,0x3101,2,1),online(true,0,8,0x1600,0x3001,1,2),online(true,8,8,0x1601,0x3001,2,2)},16,16);
        Slave lan;lan.position=3;lan.vendor=0xe00004d8;lan.product=13;lan.revision=1;lan.inputBits=lan.outputBits=8;lan.inputLayoutKnown=lan.outputLayoutKnown=true;
        lan.pdoEntries={online(false,0,8,0x1a00,0x3101,1,0),online(true,0,8,0x1600,0x3001,1,1)};hardwareView.slaves.push_back(lan);hardware.decorate(hardwareView);
        check(hardwareView.esiFiles>=2&&hardwareView.slaves[0].esiStatus=="ESI matched"&&hardwareView.slaves[1].esiStatus=="ESI matched","User ESI files match both physical device identities");
        check(hardwareView.slaves[0].pdoEntries[0].type=="UINT8"&&pdoEntryName(hardwareView.slaves[0].pdoEntries[0],true)=="Output","AX58100 BITARR8 and nonstandard RxPDO index parsed");
        check(pdoEntryName(hardwareView.slaves[1].pdoEntries[1],false)=="Input"&&hardwareView.slaves[1].pdoEntries[1].type=="UINT8","LAN9252 BITARR8 and nonstandard TxPDO index parsed");
#endif
        std::cout<<checks<<" ESI checks passed.\n";return 0;
    }catch(const std::exception& e){std::cerr<<"ESI check failed: "<<e.what()<<'\n';return 1;}
}
