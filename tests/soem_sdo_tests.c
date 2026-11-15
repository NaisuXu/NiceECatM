// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

/* Exercise patched upstream SDO uploads/downloads against mock mailbox I/O.
 * No adapters, drivers, device state changes or hardware are used by this test.
 * Built with function sections so unrelated ec_coe.c services are discarded. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "soem/soem.h"
#include "ec_coe.c"

static ec_mbxbuft outgoing, incoming;
static int scenario, sent, received, clock_ticks, allocated;
static int outgoing_owned, incoming_owned;
static int failures;
static uint32 last_abort;
static uint8 transmitted[256];
static int transmitted_size;
#define CHECK(condition) do { if(!(condition)) { fprintf(stderr,"SDO test %d failed at line %d: %s\n",scenario,__LINE__,#condition);++failures; } } while(0)

ec_timet osal_current_time(void) {ec_timet result={0};return result;}
void osal_timer_start(osal_timert* timer,uint32 timeout) {(void)timer;(void)timeout;clock_ticks=0;}
boolean osal_timer_is_expired(osal_timert* timer) {(void)timer;return ++clock_ticks>((scenario==11||scenario==19||scenario==32)?3:100);}
void ecx_pusherror(ecx_contextt* context,const ec_errort* error) {(void)context;last_abort=(uint32)error->AbortCode;}
void ecx_packeterror(ecx_contextt* context,uint16 slave,uint16 index,uint8 sub,uint16 error)
{(void)context;(void)slave;(void)index;(void)sub;(void)error;}
uint8 ec_nextmbxcnt(uint8 count) {return count==7?1:count+1;}
void ec_clearmbx(ec_mbxbuft* mailbox) {memset(mailbox,0,sizeof(*mailbox));}
ec_mbxbuft* ecx_getmbx(ecx_contextt* context)
{
    (void)context;++allocated;
    if(((scenario==16||scenario==39)&&allocated==1)||((scenario==17||scenario==40)&&allocated==2))return NULL;
    CHECK(!outgoing_owned);outgoing_owned=1;return &outgoing;
}
int ecx_dropmbx(ecx_contextt* context,ec_mbxbuft* mailbox)
{
    (void)context;
    if(mailbox==&incoming) {CHECK(incoming_owned);incoming_owned=0;}
    else if(mailbox==&outgoing) {CHECK(outgoing_owned);outgoing_owned=0;}
    else CHECK(0);
    return 1;
}
int ecx_mbxsend(ecx_contextt* context,uint16 slave,ec_mbxbuft* mailbox,int timeout)
{
    (void)context;(void)slave;(void)timeout;++sent;
    /* ecx_mbxsend consumes its pool buffer on success and failure. */
    CHECK(mailbox==&outgoing && outgoing_owned);outgoing_owned=0;
    if(scenario>=21) {
        ec_SDOt* request=(ec_SDOt*)mailbox;
        int count;
        const void* payload;
        if(sent==1 && (request->Command&2)) {count=4-((request->Command>>2)&3);payload=&request->ldata[0];}
        else if(sent==1) {count=etohs(request->MbxHeader.length)-10;payload=&request->ldata[1];}
        else {count=etohs(request->MbxHeader.length)-3;if((request->Command&1)&&count==7)count-=(request->Command&0x0e)>>1;payload=&request->Index;}
        if(count<0 || count>(int)sizeof(transmitted)-transmitted_size) {CHECK(0);return 0;}
        memcpy(transmitted+transmitted_size,payload,(size_t)count);transmitted_size+=count;
    }
    return (scenario==5||scenario==27) && sent==2?0:1;
}

static void header(int payload_length,uint8 command)
{
    ec_SDOt* response=(ec_SDOt*)&incoming;
    memset(incoming,0,sizeof(incoming));
    response->MbxHeader.mbxtype=ECT_MBXT_COE;
    response->MbxHeader.length=htoes((uint16)payload_length);
    response->CANOpen=htoes(ECT_COES_SDORES<<12);
    response->Command=command;
    response->Index=htoes(0x1008);response->SubIndex=0;
}
int ecx_mbxreceive(ecx_contextt* context,uint16 slave,ec_mbxbuft** mailbox,int timeout)
{
    ec_SDOt* response=(ec_SDOt*)&incoming;
    int current;
    (void)context;(void)slave;
    if(timeout==0) {
        if(scenario==16||scenario==39) {CHECK(!incoming_owned);incoming_owned=1;*mailbox=&incoming;return 1;}
        *mailbox=NULL;return 0;
    }
    current=received++;
    if(scenario==19) {*mailbox=NULL;return 1;}
    if(scenario>=21) {
        if((scenario==23&&current==0)||(scenario==25&&current==1)) {*mailbox=NULL;return 1;}
        if((scenario==24&&current==0)||(scenario==26&&current==1)) {*mailbox=NULL;return 0;}
        header(current==0?10:3,current==0?0x60:(uint8)(0x20|(((current-1)%2)*0x10)));
        response->Index=htoes(0x2000);response->SubIndex=1;
        if(scenario==28&&current==0)response->Command=0;
        if(scenario==29&&current==1)response->Command=0x30;
        if(scenario==30&&current==0)response->MbxHeader.length=htoes(5);
        if((scenario==31&&current==0)||(scenario==34&&current==1)) {
            response->MbxHeader.length=htoes(10);response->Command=ECT_SDO_ABORT;response->ldata[0]=htoel(0x06020000);
        }
        if(scenario==36&&current==0)response->SubIndex=2;
        if(scenario==37&&current==0)response->CANOpen=htoes(1<<12);
        if(scenario==41&&current==0)response->MbxHeader.length=htoes(EC_MAXMBX);
        if(scenario==42&&current==1)response->MbxHeader.length=htoes(2);
        if(scenario==43&&current==0) {response->MbxHeader.length=htoes(5);response->Command=ECT_SDO_ABORT;response->ldata[0]=htoel(0x06020000);}
        /* SOES and other slaves may echo Complete Access in the initial ack. */
        if(scenario>=47&&scenario<=53&&current==0)response->Command=scenario==52?0x71:scenario==53?0x61:0x70;
        CHECK(!incoming_owned);incoming_owned=1;*mailbox=&incoming;return 1;
    }
    if(current==0) {
        if(scenario==1) {
            header(10,0x43);memcpy(&response->ldata[0],"ABCD",4);
        } else {
            const int initial=scenario==2?5:3;
            header(10+initial,0x41);
            response->ldata[0]=htoel(scenario==7?0xffffffffu:scenario==2?5:scenario==11?100:10);
            memcpy(&response->ldata[1],"ABCDEF",initial);
            if(scenario==13) {response->MbxHeader.length=htoes(10);response->Command=ECT_SDO_ABORT;response->ldata[0]=htoel(0x06020000);}
            if(scenario==14)response->SubIndex=1;
            if(scenario==15)response->MbxHeader.length=htoes(5);
            if(scenario==18)response->Command=0x60;
        }
    } else {
        if(scenario==4) {*mailbox=NULL;return 0;}
        if(scenario==12) {*mailbox=NULL;return 1;}
        if(scenario==3) {header(10,0x01);memcpy(&response->Index,"DEFGHIJ",7);}
        else if(scenario==6) {header(23,0x01);memset(&response->Index,'X',20);}
        else if(scenario==8) {header(5,0x01);memcpy(&response->Index,"DE",2);}
        else if(scenario==9) {header(10,0x11);memcpy(&response->Index,"DEFGHIJ",7);}
        else if(scenario==10)header(3,0x00);
        else if(scenario==11) {header(4,(uint8)(((current-1)%2)*0x10));*((uint8*)&response->Index)='X';}
        else if(scenario==20)header(2,0x01);
        else {header(10,0x01);memcpy(&response->Index,"DEFGHIJ",7);}
    }
    CHECK(!incoming_owned);incoming_owned=1;*mailbox=&incoming;return 1;
}

int main(void)
{
    ecx_contextt* context=(ecx_contextt*)calloc(1,sizeof(ecx_contextt));
    int test;
    if(!context)return 1;
    for(test=1;test<=20;++test) {
        struct {uint8 before[16];uint8 value[128];uint8 after[16];} buffer;
        int size=128,result;
        memset(&buffer,0xa5,sizeof(buffer));memset(buffer.value,0,sizeof(buffer.value));
        scenario=test;sent=received=clock_ticks=allocated=outgoing_owned=incoming_owned=0;last_abort=0;
        if(test==6)size=10;
        result=ecx_SDOread(context,1,0x1008,0,FALSE,&size,buffer.value,1000000);
        if(test<=3) {
            CHECK(result>0);
            CHECK(size==(test==1?4:test==2?5:10));
            CHECK(memcmp(buffer.value,"ABCDEFGHIJ",(size_t)size)==0);
        } else CHECK(result<=0);
        CHECK(sent<10);CHECK(received<10);
        {int i;for(i=0;i<16;++i) {CHECK(buffer.before[i]==0xa5);CHECK(buffer.after[i]==0xa5);}}
        if(test==6)CHECK(buffer.value[10]==0);
        if(test==13)CHECK(last_abort==0x06020000);
        CHECK(!incoming_owned && !outgoing_owned);
    }
    for(test=21;test<=53;++test) {
        uint8 value[128];int i,result;
        int size=(test==21||test==47||test==50||test==52)?4:(test==35||test==48||test==51||test==53)?6:test==32?128:32;
        boolean complete_access=test==45||test==47||test==48||test==49||test==52||test==53;
        scenario=test;sent=received=clock_ticks=transmitted_size=allocated=outgoing_owned=incoming_owned=0;last_abort=0;
        context->slavelist[1].mbx_l=test==46?EC_MAXMBX+1:test==33?8:test==32?16:24;
        for(i=0;i<128;++i)value[i]=(uint8)i;
        result=ecx_SDOwrite(context,1,0x2000,complete_access?3:1,complete_access,size,test==38?NULL:value,test==44?0:1000000);
        if(test==21 || test==22 || test==35 || test==45 || test==47 || test==48 || test==49) {CHECK(result>0);CHECK(transmitted_size==size);CHECK(memcmp(transmitted,value,(size_t)size)==0);}
        else CHECK(result<=0);
        if(test==25 || test==26 || test==27 || test==29 || test==34)CHECK(sent==2);
        if(test==23 || test==24 || test==28 || test==30 || test==31 || test==36 || test==37)CHECK(sent==1);
        if(test==33 || test==38 || test==39 || test==44 || test==46)CHECK(sent==0);
        if(test==40 || test==41 || test==43)CHECK(sent==1);
        if(test==50 || test==51 || test==52 || test==53)CHECK(sent==1);
        if(test==42)CHECK(sent==2);
        if(test==31 || test==34)CHECK(last_abort==0x06020000);
        CHECK(sent<10);CHECK(received<10);
        if(test==43)CHECK(last_abort==0);
        CHECK(!incoming_owned && !outgoing_owned);
    }
    /* Invalid upload arguments must fail before touching the mailbox pool. */
    {
        uint8 value[4];int size=-1;
        scenario=0;sent=received=allocated=0;
        CHECK(ecx_SDOread(context,1,0x1008,0,FALSE,NULL,value,1000000)==0);
        CHECK(ecx_SDOread(context,1,0x1008,0,FALSE,&size,value,1000000)==0);
        size=4;
        CHECK(ecx_SDOread(context,1,0x1008,0,FALSE,&size,NULL,1000000)==0);
        CHECK(ecx_SDOread(context,1,0x1008,0,FALSE,&size,value,0)==0);
        CHECK(sent==0 && received==0 && allocated==0);
    }
    free(context);
    if(failures) {fprintf(stderr,"%d SDO checks failed\n",failures);return 1;}
    puts("57 SOEM v2.0.1 SDO scenarios passed: 20 uploads + 33 downloads + 4 invalid requests; bounded transfers, headers, identity, Complete Access, Abort, toggle, capacity and mailbox ownership");
    return 0;
}
