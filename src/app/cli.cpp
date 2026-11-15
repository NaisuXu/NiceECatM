// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#include "app/cli.hpp"
#include "build_version.hpp"
#include "esi/catalog.hpp"
#include "platform/paths.hpp"
#include "ethercat/backend.hpp"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <algorithm>
namespace {
std::string quote(const std::string& s) {
    std::ostringstream o;o<<'"';
    for(unsigned char c:s) { if(c=='"'||c=='\\')o<<'\\'<<c;else if(c<32)o<<"\\u"<<std::hex<<std::setw(4)<<std::setfill('0')<<int(c);else o<<c; }
    return o.str()+'"';
}
int number(const std::string& text,int maximum) {
    size_t end=0;auto n=std::stoll(text,&end,0);
    if(end!=text.size()||n<0||n>maximum)throw std::runtime_error("Argument outside allowed range: "+text);
    return static_cast<int>(n);
}
void print(niceecatm::Snapshot s,const niceecatm::Result& r) {
    niceecatm::EsiCatalog catalog(niceecatm::executableDirectory()/"esi");
    catalog.decorate(s);
    std::cout<<"{\n  \"ok\": "<<(r.ok?"true":"false")<<", \"simulation\": "<<(s.simulation?"true":"false")
    <<",\n  \"message\": "<<quote(r.message)<<",\n  \"driver\": "<<quote(s.driver)<<",\n  \"data\": "<<quote(niceecatm::hexBytes(r.data))<<",\n  \"expectedWkc\": "<<s.expectedWkc
    <<",\n  \"esiDirectory\": "<<quote(s.esiDirectory)<<", \"esiFiles\": "<<s.esiFiles<<", \"esiDevices\": "<<s.esiDevices<<",\n  \"adapters\": [";
    bool first=true;for(const auto& a:s.adapters){if(!first)std::cout<<',';first=false;std::cout<<"\n    {\"name\": "<<quote(a.name)<<", \"description\": "<<quote(a.description)<<'}';}
    std::cout<<"\n  ],\n  \"slaves\": [";first=true;
    for(const auto& d:s.slaves){if(!first)std::cout<<',';first=false;
      std::cout<<"\n    {\"position\": "<<d.position<<", \"name\": "<<quote(d.name)<<", \"vendor\": "<<d.vendor<<", \"product\": "<<d.product<<", \"revision\": "<<d.revision<<", \"serial\": "<<d.serial
      <<", \"state\": "<<quote(niceecatm::stateName(d.state))<<", \"stateRaw\": "<<d.state<<", \"alCode\": "<<d.alCode<<", \"inputBits\": "<<d.inputBits<<", \"outputBits\": "<<d.outputBits<<", \"inputOffset\": "<<d.inputOffset<<", \"outputOffset\": "<<d.outputOffset<<", \"dc\": "<<(d.dc?"true":"false")<<", \"mailboxProtocols\": "<<d.mailboxProtocols
      <<", \"inputLayoutKnown\": "<<(d.inputLayoutKnown?"true":"false")<<", \"outputLayoutKnown\": "<<(d.outputLayoutKnown?"true":"false")
      <<", \"pdoIssue\": "<<quote(d.pdoIssue)<<", \"esiFile\": "<<quote(d.esiFile)<<", \"esiStatus\": "<<quote(d.esiStatus)<<", \"pdoEntries\": [";
      bool firstEntry=true;
      for(const auto& entry:d.pdoEntries) {
          if(!firstEntry)std::cout<<',';
          firstEntry=false;
          std::cout<<"{\"input\":"<<(entry.input?"true":"false")<<",\"padding\":"<<(entry.padding?"true":"false")
          <<",\"bitOffset\":"<<entry.bitOffset<<",\"bitLength\":"<<entry.bitLength<<",\"pdoIndex\":"<<entry.pdoIndex
          <<",\"index\":"<<entry.index<<",\"subindex\":"<<unsigned(entry.subindex)<<",\"syncManager\":"<<entry.syncManager
          <<",\"type\":"<<quote(entry.type)<<",\"name\":"<<quote(niceecatm::pdoEntryName(entry,false))
          <<",\"nameZh\":"<<quote(niceecatm::pdoEntryName(entry,true))<<",\"source\":"<<quote(entry.source)<<'}';
      }
      std::cout<<"]}";
    }std::cout<<"\n  ]\n}\n";
}
}
int runCli(int argc,char**argv) {
    niceecatm::Connection cfg;cfg.simulation=false;
    bool adapters=false;std::string operation="scan",state;
    int slave=1,offset=0x130,length=6,index=0x1008,sub=0;
    for(int i=1;i<argc;++i){std::string a=argv[i];
      auto value=[&](){if(i+1>=argc)throw std::runtime_error("Missing value for "+a);return std::string(argv[++i]);};
      if(a=="--help") {
        std::cout<<"NiceECatM "<<NICEECATM_VERSION<<" native workbench\n\n"
        <<"  NiceECatM                          Open GUI (English by default)\n"
        <<"  NiceECatM --adapters               List real capture adapters as JSON\n"
        <<"  NiceECatM --scan --interface NAME  Scan/map, without requesting OP\n"
        <<"  NiceECatM --read-register 0x130 --length 6 --slave 1 --interface NAME\n"
        <<"  NiceECatM --sdo-read 0x1008 --sub 0 --length 128 --interface NAME\n"
        <<"  NiceECatM --state PRE-OP --slave 0 --interface NAME\n\n"
        <<"Options: --simulate, --secondary NAME (same-bus redundancy), --overlap\n"
        <<"         --cycle-us 500|1000|2000|5000|10000|20000|50000|100000\n"
        <<"GUI smoke capture: --ui-smoke --frames 30 --screenshot PATH.bmp\n"
        <<"GUI startup: --language en|zh-CN --tab device|registers|pdo|mailbox|sdo|diagnostics|scripts\n"
        <<"Scan configures EtherCAT slaves and mapping; no output values are written by CLI.\n";return 0;
      } else if(a=="--adapters")adapters=true;
      else if(a=="--simulate")cfg.simulation=true;
      else if(a=="--scan")operation="scan";
      else if(a=="--interface")cfg.primary=value();
      else if(a=="--secondary"){cfg.secondary=value();cfg.redundant=true;}
      else if(a=="--overlap")cfg.overlap=true;
      else if(a=="--cycle-us")cfg.cycleUs=number(value(),100000);
      else if(a=="--slave")slave=number(value(),199);
      else if(a=="--length")length=number(value(),4096);
      else if(a=="--read-register"){operation="register";offset=number(value(),65535);}
      else if(a=="--sdo-read"){operation="sdo";index=number(value(),65535);}
      else if(a=="--sub")sub=number(value(),255);
      else if(a=="--state"){operation="state";state=value();}
      else throw std::runtime_error("Unknown argument: "+a);
    }
    if(cfg.cycleUs!=500&&cfg.cycleUs!=1000&&cfg.cycleUs!=2000&&cfg.cycleUs!=5000&&cfg.cycleUs!=10000&&cfg.cycleUs!=20000&&cfg.cycleUs!=50000&&cfg.cycleUs!=100000)throw std::runtime_error("Unsupported cycle");
    auto backend=cfg.simulation?niceecatm::makeSimulation():niceecatm::makeSoem();
    auto r=backend->adapters();
    if(adapters){print(backend->snapshot(),r);return r.ok?0:1;}
    if(cfg.primary.empty()){
      if(cfg.simulation)cfg.primary="sim0";
      else throw std::runtime_error("Select an adapter explicitly with --interface; use --adapters first.");
    }
    r=backend->scan(cfg);
    if(r.ok&&operation=="register")r=backend->readRegister(slave,static_cast<uint16_t>(offset),length);
    if(r.ok&&operation=="sdo")r=backend->readSdo(slave,static_cast<uint16_t>(index),static_cast<uint8_t>(sub),length);
    if(r.ok&&operation=="state"){
      niceecatm::AlState requested;
      if(state=="INIT")requested=niceecatm::AlState::Init;
      else if(state=="PRE-OP")requested=niceecatm::AlState::PreOp;
      else if(state=="SAFE-OP")requested=niceecatm::AlState::SafeOp;
      else if(state=="OP")requested=niceecatm::AlState::Op;
      else throw std::runtime_error("State must be INIT, PRE-OP, SAFE-OP or OP");
      r=backend->state(slave,requested);
    }
    print(backend->snapshot(),r);backend->close();return r.ok?0:1;
}
