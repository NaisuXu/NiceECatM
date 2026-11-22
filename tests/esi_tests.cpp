// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#include "esi/catalog.hpp"
#include "ui/messages.hpp"
#include <algorithm>
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
bool logContains(const std::vector<std::string>& lines,const std::string& text) {
    return std::any_of(lines.begin(),lines.end(),[&](const std::string& line){return line.find(text)!=std::string::npos;});
}
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
        auto absent=snapshot({},8,0);catalog.decorate(absent);
        const auto missingLogs=catalog.scanDiagnostics(absent);
        check(logContains(missingLogs,"ESI directory does not exist | "+library.u8string())&&logContains(missingLogs,"ProductCode=0x00010200"),"Missing library reports the searched directory and hardware identity");
        const auto source=library/fs::u8path(u8"信号.xml");
        const auto labels="<Name LcId=\"1033\">Setpoint &amp; limit</Name><Name LcId=\"2052\">速度设定</Name>";
        const auto fields=entry("#x7000",1,3,labels,"UINT3")+entry("0",0,1,"","")+entry("#x7000",2,12,"<Name>Position</Name>","UINT12");
        write(source,document(pdo("RxPdo","#x1600",fields)));
        check(catalog.reload().ok,"ESI loads from a Unicode library and filename");
        auto s=snapshot({online(false,0,3,0x1600,0x7000,1,2),online(false,3,1,0x1600,0,0,2),online(false,4,12,0x1600,0x7000,2,2)},0,16);
        catalog.decorate(s);
        check(s.esiFiles==1&&s.esiDevices==1&&s.slaves[0].esiStatus=="ESI matched","Exact identity and online layout matched");
        check(catalog.scanDiagnostics(s).empty(),"Successful ESI matches produce no diagnostic log");
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
        wrongRevision.slaves[0].name=u8"设备 A";
        const auto revisionLogs=catalog.scanDiagnostics(wrongRevision);
        check(revisionLogs.size()==2&&logContains(revisionLogs,"RevisionNo=0x00000002")&&logContains(revisionLogs,source.filename().u8string()+" (RevisionNo=0x00000001)"),"Revision mismatch identifies the live revision and candidate XML revision");
        check(detail::translateToolMessage(revisionLogs[0])==u8"ESI XML 搜索目录 | "+library.u8string()&&detail::translateToolMessage(revisionLogs[1]).find(u8"ESI 修订号不匹配 | #1 设备 A")!=std::string::npos,"Chinese diagnostics preserve Unicode directories and device names");
        check(detail::translateToolMessage("Slave 12: CoE descriptions unavailable for 108 mapped object(s).")=="从站 12 有 108 个映射对象未能读取 CoE 描述。","Chinese CoE log retains arbitrary slave and object counts");
        check(detail::translateToolMessage("Slave 1: CoE descriptions unavailable for 3 mapped object(s). Stopped after three consecutive SDO Information request failures.")=="从站 1 有 3 个映射对象未能读取 CoE 描述。连续三次 SDO Information 请求失败，已停止查询描述。","Chinese CoE log includes the optional cause");
        auto repeatedMismatch=wrongRevision;repeatedMismatch.slaves.resize(32,wrongRevision.slaves[0]);
        const auto limited=catalog.scanDiagnostics(repeatedMismatch);
        check(limited.size()==10&&limited.back()=="Additional slaves with ESI diagnostics | 24","Large topologies have bounded ESI diagnostics");
        auto gap=snapshot({online(false,1,3,0x1600,0x7000,1),online(false,4,12,0x1600,0x7000,2)},0,16);catalog.decorate(gap);
        check(gap.slaves[0].pdoEntries[0].names.empty(),"Incomplete actual layout cannot bind ESI");
        auto overrun=snapshot({online(false,0,17,0x1600,0x7000,1)},0,16);catalog.decorate(overrun);
        check(overrun.slaves[0].pdoEntries[0].names.empty(),"Out of bounds actual layout cannot bind ESI");
        auto noEsi=snapshot({online(false,0,16,0x1600,0x7000,1)},0,16);noEsi.slaves[0].vendor=0x1234;
        noEsi.slaves[0].pdoEntries[0].type="INT16";noEsi.slaves[0].pdoEntries[0].names={{0,"Trusted online name"}};catalog.decorate(noEsi);
        check(noEsi.slaves[0].pdoEntries[0].type=="INT16"&&pdoEntryName(noEsi.slaves[0].pdoEntries[0],true)=="Trusted online name","Missing ESI preserves trusted online type and name");
        check(logContains(catalog.scanDiagnostics(noEsi),"No matching ESI description | #1")&&logContains(catalog.scanDiagnostics(noEsi),"VendorId=0x00001234"),"Unmatched vendor includes the connected hardware identity");

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
        const auto partialLogs=dynamic.scanDiagnostics(conflicting);
        check(logContains(partialLogs,"ESI matched; partial PDO description")&&logContains(partialLogs,"dynamic.xml"),"Partially bound online layouts identify the matching XML");
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

        const std::string objectTypes=R"(<DataTypes>
          <DataType><Name>SignedAlias</Name><BitSize>16</BitSize><BaseType>INT</BaseType></DataType>
          <DataType><Name>Record</Name><BitSize>48</BitSize>
            <SubItem><SubIdx>0</SubIdx><Name>Number of entries</Name><Type>USINT</Type><BitSize>8</BitSize><Flags><Access>ro</Access></Flags></SubItem>
            <SubItem><SubIdx>1</SubIdx><Name>Gain</Name><DisplayName LcId="1033">Input gain</DisplayName><DisplayName LcId="2052">输入增益</DisplayName><Type>SignedAlias</Type><BitSize>16</BitSize></SubItem>
            <SubItem><SubIdx>3</SubIdx><Name>Command</Name><Type>UINT</Type><BitSize>16</BitSize><Flags><Access WriteRestrictions="OP">wo</Access></Flags></SubItem>
          </DataType>
          <DataType><Name>WordArray</Name><BaseType>UINT</BaseType><BitSize>64</BitSize><ArrayInfo><LBound>1</LBound><Elements>4</Elements></ArrayInfo></DataType>
          <DataType><Name>Assignment</Name><BitSize>80</BitSize>
            <SubItem><SubIdx>0</SubIdx><Name>Number of entries</Name><Type>USINT</Type><BitSize>8</BitSize><Flags><Access>ro</Access></Flags></SubItem>
            <SubItem><Name>Elements</Name><Type>WordArray</Type><BitSize>64</BitSize><Flags><Access WriteRestrictions="PreOp">rw</Access></Flags></SubItem>
          </DataType>
        </DataTypes>)";
        const std::string objectList=R"(<Objects>
          <Object><Index>#x8000</Index><Name>Parameters</Name><Type>Record</Type><BitSize>48</BitSize><Flags><Access WriteRestrictions="PreOP_SafeOP">rw</Access></Flags></Object>
          <Object><Index>#x1008</Index><Name LcId="1033">Device name</Name><Type>STRING(8)</Type><BitSize>64</BitSize><Info><DefaultString>offline</DefaultString></Info><Flags><Access>ro</Access></Flags></Object>
          <Object><Index>#x1c12</Index><Name>Assignments</Name><Type>Assignment</Type><BitSize>80</BitSize><Flags><Access>rw</Access></Flags></Object>
          <Object><Index>#x2000</Index><Name>Operating counter</Name><Type>UDINT</Type><BitSize>32</BitSize><Flags><Access ReadRestrictions="OP" WriteRestrictions="PreOP_SafeOP">rw</Access></Flags></Object>
          <Object><Index>#x2001</Index><Name>Unknown access</Name><Type>UINT</Type><BitSize>16</BitSize></Object>
          <Object><Index>#x2002</Index><Name>Unrecognized restriction</Name><Type>USINT</Type><BitSize>8</BitSize><Flags><Access WriteRestrictions="Other">rw</Access></Flags></Object>
        </Objects>)";
        const auto dictionaryXml="<Profile><Dictionary>"+objectTypes+objectList+"</Dictionary></Profile>";
        write(dictionarySource,document("","1",dictionaryXml));
        check(dictionaryCatalog.reload().ok,"Dictionary-only ESI loads without PDO definitions");
        auto odView=snapshot({},0,16);odView.slaves[0].outputLayoutKnown=false;
        auto liveDictionary=std::make_shared<SdoDictionary>();liveDictionary->status="Existing live description";odView.slaves[0].sdoDictionary=liveDictionary;
        dictionaryCatalog.decorate(odView);
        const auto offline=odView.slaves[0].esiSdoDictionary;
        check(offline&&offline->complete&&offline->objects.size()==6,"Full object dictionary is independent of actual PDO layout");
        check(odView.slaves[0].sdoDictionary==liveDictionary,"ESI decoration preserves online dictionary ownership");
        const auto& deviceName=offline->objects[0];const auto& assignments=offline->objects[1];const auto& counter=offline->objects[2];const auto& parameters=offline->objects[5];
        check(deviceName.index==0x1008&&deviceName.entries.size()==1&&deviceName.entries[0].subindex==0&&deviceName.entries[0].type=="STRING"&&deviceName.entries[0].bitLength==64,"String scalar metadata resolves without treating its default as a live value");
        check(deviceName.entries[0].accessKnown&&deviceName.entries[0].access==7&&deviceName.entries[0].esi&&!deviceName.entries[0].online,"Offline read-only rights and source retained");
        check(counter.index==0x2000&&counter.entries[0].type=="UINT32"&&counter.entries[0].dataType==7&&counter.entries[0].access==28,"Read/write state restrictions map to CoE access bits");
        check(!offline->objects[3].entries[0].accessKnown&&!offline->objects[4].entries[0].accessKnown,"Absent or unknown access information remains unknown");
        check(parameters.index==0x8000&&parameters.entries.size()==3&&parameters.maxSubindex==3&&parameters.entries[1].subindex==1,"Record preserves sparse actual subindices");
        check(parameters.entries[1].type=="INT16"&&parameters.entries[1].access==31&&parameters.entries[2].access==32,"Aliases resolve and subitem rights override/inherit object rights");
        check(parameters.entries[1].names.size()==2&&parameters.entries[1].names[1].text==u8"输入增益","Record display names retain declared languages");
        check(assignments.index==0x1c12&&assignments.entries.size()==5&&assignments.maxSubindex==4,"Array container expands bounded SDO subindices and preserves subindex zero");
        check(assignments.entries[4].subindex==4&&assignments.entries[4].type=="UINT16"&&assignments.entries[4].bitLength==16&&assignments.entries[4].access==15,"Array element width excludes container alignment and recognizes legacy PreOp spelling");
        check(assignments.entries[4].names[0].text=="Elements [4]","Array members have distinct readable labels");
        auto odWrongRevision=odView;odWrongRevision.slaves[0].revision=2;dictionaryCatalog.decorate(odWrongRevision);
        check(!odWrongRevision.slaves[0].esiSdoDictionary&&odWrongRevision.slaves[0].sdoDictionary==liveDictionary,"Revision mismatch clears stale offline dictionary but preserves online metadata");
        auto odWrongVendor=odView;odWrongVendor.slaves[0].vendor=1;dictionaryCatalog.decorate(odWrongVendor);
        check(!odWrongVendor.slaves[0].esiSdoDictionary,"Dictionary identity requires the exact vendor");
        const auto dictionaryDuplicate=dictionarySource.parent_path()/"duplicate.xml";
        write(dictionaryDuplicate,document("","1",dictionaryXml));check(dictionaryCatalog.reload().ok,"Equivalent dictionaries load");
        dictionaryCatalog.decorate(odView);check(odView.esiDevices==1&&odView.slaves[0].esiSdoDictionary,"Equivalent dictionary-only descriptions deduplicate");
        auto changedObjects=objectList;changedObjects.replace(changedObjects.find("Operating counter"),17,"Different counter");
        write(dictionaryDuplicate,document("","1","<Profile><Dictionary>"+objectTypes+changedObjects+"</Dictionary></Profile>"));
        check(dictionaryCatalog.reload().ok,"Different dictionaries retain their identity conflict");dictionaryCatalog.decorate(odView);
        check(odView.esiDevices==2&&!odView.slaves[0].esiSdoDictionary&&odView.slaves[0].esiStatus=="Ambiguous ESI device identity","Equal PDO layouts cannot hide conflicting SDO dictionaries");
        fs::remove(dictionaryDuplicate);fs::remove(dictionarySource);check(dictionaryCatalog.reload().ok,"Removed dictionary files reload cleanly");dictionaryCatalog.decorate(odView);
        check(!odView.slaves[0].esiSdoDictionary&&odView.slaves[0].sdoDictionary==liveDictionary,"Removing ESI clears offline dictionary on an existing snapshot");

        const auto edgeSource=temp.directory/"dictionary-edges"/"edges.xml";EsiCatalog edgeCatalog(edgeSource.parent_path());
        const std::string multiArray="<DataTypes><DataType><Name>Matrix</Name><BaseType>UINT</BaseType><BitSize>64</BitSize><ArrayInfo><LBound>1</LBound><Elements>2</Elements></ArrayInfo><ArrayInfo><LBound>1</LBound><Elements>2</Elements></ArrayInfo></DataType></DataTypes><Objects><Object><Index>#x2100</Index><Name>Matrix</Name><Type>Matrix</Type><BitSize>64</BitSize></Object></Objects>";
        write(edgeSource,document("","1","<Profile><Dictionary>"+multiArray+"</Dictionary></Profile>"));check(edgeCatalog.reload().ok,"Unsupported multidimensional arrays do not hide the device description");
        auto edgeView=snapshot({},0,0);edgeCatalog.decorate(edgeView);
        check(edgeView.slaves[0].esiSdoDictionary&&!edgeView.slaves[0].esiSdoDictionary->complete&&edgeView.slaves[0].esiSdoDictionary->objects[0].entries.empty(),"Unsupported array shape is partial and never invents SDO addresses");
        const std::string duplicateObject="<Objects><Object><Index>#x2000</Index><Type>UINT</Type><BitSize>16</BitSize></Object><Object><Index>#x2000</Index><Type>UDINT</Type><BitSize>32</BitSize></Object></Objects>";
        const std::string cyclicType="<DataTypes><DataType><Name>A</Name><BaseType>B</BaseType></DataType><DataType><Name>B</Name><BaseType>A</BaseType></DataType></DataTypes><Objects><Object><Index>#x2000</Index><Type>A</Type></Object></Objects>";
        const std::string duplicateSub="<DataTypes><DataType><Name>R</Name><SubItem><SubIdx>1</SubIdx><Type>UINT</Type><BitSize>16</BitSize></SubItem><SubItem><SubIdx>1</SubIdx><Type>UINT</Type><BitSize>16</BitSize></SubItem></DataType></DataTypes><Objects><Object><Index>#x2000</Index><Type>R</Type></Object></Objects>";
        for(const auto& bad:std::vector<std::string>{duplicateObject,cyclicType,duplicateSub}) {
            write(edgeSource,document("","1","<Profile><Dictionary>"+bad+"</Dictionary></Profile>"));
            check(!edgeCatalog.reload().ok,"Duplicate object/subindex or cyclic alias is rejected instead of choosing arbitrary dictionary metadata");
        }

        const auto invalidLib=temp.directory/"invalid";
        const auto invalid=invalidLib/"invalid.xml";const auto valid=invalidLib/"valid.xml";
        write(invalid,"<EtherCATInfo>");write(valid,document(""));
        EsiCatalog invalidCatalog(invalidLib);check(!invalidCatalog.reload().ok,"Malformed library file reports a load failure");
        Snapshot partiallyValid;invalidCatalog.decorate(partiallyValid);
        check(partiallyValid.esiFiles==1&&partiallyValid.esiDevices==1,"A malformed file does not hide valid descriptions in the same library");
        auto invalidView=snapshot({},0,0);invalidCatalog.decorate(invalidView);
        const auto invalidLogs=invalidCatalog.scanDiagnostics(invalidView);
        check(logContains(invalidLogs,"Malformed ESI XML")&&logContains(invalidLogs,"invalid.xml")&&logContains(invalidLogs,invalidLib.u8string()),"Malformed XML produces a file-specific load diagnostic alongside valid devices");
        for(const auto& data:std::vector<std::string>{document(pdo("RxPdo","#x1600",entry("#x10000",1,16,""))),document(pdo("RxPdo","#x1600",entry("#x7000",256,16,""))),document(pdo("RxPdo","#x1600",entry("#x7000",1,0,""))),document(pdo("RxPdo","#x1600",entry("#x7000",1,524289,""))),document("","4294967296"),"<!DOCTYPE EtherCATInfo SYSTEM 'file:///never-read'>"+document(""),document("")+"<extra/>"}) {
            write(invalid,data);check(!invalidCatalog.reload().ok,"Malformed identity, unsafe widths and external declarations rejected");
        }
        write(invalid,document(pdo("RxPdo","#x1600",entry("#x7000",1,16,std::string("<Name>")+char(0xff)+"</Name>"))));
        check(!invalidCatalog.reload().ok,"Invalid UTF-8 is rejected when the library loads");
        const auto conflictSource=library/"conflict.xml";write(conflictSource,document(pdo("RxPdo","#x1600",entry("#x7000",1,16,"<Name>Different</Name>"))));
        check(catalog.reload().ok,"Conflicting identity is retained for explicit ambiguity reporting");
        auto ambiguous=fresh;catalog.decorate(ambiguous);
        check(ambiguous.slaves[0].esiStatus=="Ambiguous ESI device identity"&&ambiguous.esiFiles==2,"Conflicting identities do not silently override earlier description");
        const auto ambiguousLogs=catalog.scanDiagnostics(ambiguous);
        check(logContains(ambiguousLogs,"conflict.xml")&&logContains(ambiguousLogs,source.filename().u8string()),"Conflicting descriptions list both XML files");
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
        check(logContains(removedFile.scanDiagnostics(withoutFile),"No XML files in the ESI directory | "+latin.parent_path().u8string()),"Empty directory has a different diagnostic from a missing directory");

#ifdef NICEECATM_SOURCE_DIR
        const fs::path sourceRoot=NICEECATM_SOURCE_DIR;EsiCatalog hardware(sourceRoot/"resources"/"esi");
        auto hardwareView=snapshot({online(false,0,8,0x1a00,0x3101,1,0),online(false,8,8,0x1a01,0x3101,2,1),online(true,0,8,0x1600,0x3001,1,2),online(true,8,8,0x1601,0x3001,2,2)},16,16);
        Slave lan;lan.position=3;lan.vendor=0xe00004d8;lan.product=13;lan.revision=1;lan.inputBits=lan.outputBits=8;lan.inputLayoutKnown=lan.outputLayoutKnown=true;
        lan.pdoEntries={online(false,0,8,0x1a00,0x3101,1,0),online(true,0,8,0x1600,0x3001,1,1)};hardwareView.slaves.push_back(lan);hardware.decorate(hardwareView);
        check(hardwareView.esiFiles>=2&&hardwareView.slaves[0].esiStatus=="ESI matched"&&hardwareView.slaves[1].esiStatus=="ESI matched","User ESI files match both physical device identities");
        check(hardwareView.slaves[0].pdoEntries[0].type=="UINT8"&&pdoEntryName(hardwareView.slaves[0].pdoEntries[0],true)=="Output","AX58100 BITARR8 and nonstandard RxPDO index parsed");
        check(pdoEntryName(hardwareView.slaves[1].pdoEntries[1],false)=="Input"&&hardwareView.slaves[1].pdoEntries[1].type=="UINT8","LAN9252 BITARR8 and nonstandard TxPDO index parsed");
        check(!hardwareView.slaves[0].esiSdoDictionary&&!hardwareView.slaves[1].esiSdoDictionary,"PDO-only manufacturer ESI files do not invent SDO dictionaries");
#endif
        std::cout<<checks<<" ESI checks passed.\n";return 0;
    }catch(const std::exception& e){std::cerr<<"ESI check failed: "<<e.what()<<'\n';return 1;}
}
