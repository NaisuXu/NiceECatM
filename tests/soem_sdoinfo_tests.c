// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

/* Production SDO Information parser against mocked mailbox ownership and
 * deliberately malformed replies. No capture driver or network is used. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "soem/soem.h"
#include "ec_coe.c"

static ec_mbxbuft outgoing,incoming;
static int mode,variant,sent,received,outgoing_owned,incoming_owned,failures;
static uint32 last_abort;
#define CHECK(x) do {if(!(x)){fprintf(stderr,"SDO Info mode=%d variant=%d line=%d: %s\n",mode,variant,__LINE__,#x);++failures;}}while(0)
ec_timet osal_current_time(void) {ec_timet t={0};return t;}
void osal_timer_start(osal_timert* t,uint32 timeout) {(void)t;(void)timeout;}
boolean osal_timer_is_expired(osal_timert* t) {(void)t;return variant==14;}
void ecx_pusherror(ecx_contextt* c,const ec_errort* e) {(void)c;last_abort=(uint32)e->AbortCode;}
void ecx_packeterror(ecx_contextt* c,uint16 s,uint16 i,uint8 sub,uint16 error) {(void)c;(void)s;(void)i;(void)sub;(void)error;}
uint8 ec_nextmbxcnt(uint8 c) {return c==7?1:c+1;}
void ec_clearmbx(ec_mbxbuft* mbx) {memset(mbx,0,sizeof(*mbx));}
ec_mbxbuft* ecx_getmbx(ecx_contextt* c) {(void)c;if(variant==9)return NULL;CHECK(!outgoing_owned);outgoing_owned=1;return &outgoing;}
int ecx_dropmbx(ecx_contextt* c,ec_mbxbuft* mbx) {
    (void)c;
    if(mbx==&incoming){CHECK(incoming_owned);incoming_owned=0;}
    else if(mbx==&outgoing){CHECK(outgoing_owned);outgoing_owned=0;}
    else CHECK(0);
    return 1;
}
int ecx_mbxsend(ecx_contextt* c,uint16 slave,ec_mbxbuft* mbx,int timeout) {
    const ec_SDOservicet* request=(const ec_SDOservicet*)mbx;
    (void)c;(void)slave;(void)timeout;
    CHECK(mbx==&outgoing&&outgoing_owned);outgoing_owned=0;++sent;
    CHECK(request->Opcode==(mode==0?ECT_GET_ODLIST_REQ:mode==1?ECT_GET_OD_REQ:ECT_GET_OE_REQ));
    return 1;
}
int ecx_mbxreceive(ecx_contextt* c,uint16 slave,ec_mbxbuft** mbx,int timeout) {
    ec_SDOservicet* response=(ec_SDOservicet*)&incoming;
    int current;
    (void)c;(void)slave;
    *mbx=NULL;
    if(timeout==0){if(variant==9){CHECK(!incoming_owned);incoming_owned=1;*mbx=&incoming;return 1;}return 0;}
    current=received++;
    if(variant==1)return 1;
    if(variant==10)return 0;
    memset(incoming,0,sizeof(incoming));
    response->MbxHeader.mbxtype=ECT_MBXT_COE;
    response->CANOpen=htoes(ECT_COES_SDOINFO<<12);
    response->Opcode=mode==0?ECT_GET_ODLIST_RES:mode==1?ECT_GET_OD_RES:ECT_GET_OE_RES;
    if(mode==0) {
        response->MbxHeader.length=htoes(current==0?10:8);
        response->Fragments=htoes(current==0?1:0);
        if(current==0){response->wdata[0]=htoes(1);response->wdata[1]=htoes(0x1000);}
        else response->wdata[0]=htoes(0x8000);
    } else if(mode==1) {
        response->MbxHeader.length=htoes(16);response->wdata[0]=htoes(0x1000);
        response->wdata[1]=htoes(7);response->bdata[4]=3;response->bdata[5]=9;
        memcpy(&response->bdata[6],"Test",4);
    } else {
        response->MbxHeader.length=htoes(20);response->wdata[0]=htoes(0x1000);response->bdata[2]=3;
        response->bdata[3]=7;response->wdata[2]=htoes(6);response->wdata[3]=htoes(16);response->wdata[4]=htoes(63);
        memcpy(&response->wdata[5],"Gain",4);
    }
    if(variant==2)response->MbxHeader.length=htoes(mode==0?7:mode==1?11:15);
    if(variant==3)response->MbxHeader.length=htoes(EC_MAXMBX);
    if(variant==4)response->MbxHeader.mbxtype=ECT_MBXT_FOE;
    if(variant==5)response->CANOpen=htoes(ECT_COES_SDORES<<12);
    if(variant==6)response->Opcode=ECT_GET_OE_REQ;
    if(variant==7||variant==8){response->Opcode=ECT_SDOINFO_ERROR;response->MbxHeader.length=htoes(variant==7?10:6);response->ldata[0]=htoel(0x06090011);}
    if(variant==11)response->wdata[0]=htoes(mode==0?2:0x1001);
    if(variant==12){if(mode==0)response->MbxHeader.length=htoes(9);else response->Fragments=htoes(1);}
    if(variant==13){if(mode==0&&current>0)response->Fragments=htoes(1);else if(mode)response->Opcode|=0x80;}
    if(variant==14){if(mode==1)response->MbxHeader.length=0;else if(mode==2)response->bdata[2]=4;}
    if(variant==15) {
        /* The complete 1024-element destination cannot hold another fragment. */
        int n=250,i;
        response->MbxHeader.length=htoes((uint16)(6+(current==0?2:0)+n*2));
        response->Fragments=htoes((uint16)(8-current));
        if(current==0)response->wdata[0]=htoes(1);
        for(i=0;i<n;++i)response->wdata[i+(current==0?1:0)]=htoes((uint16)(0x1000+current*250+i));
    }
    if(variant==16){response->Fragments=0;response->Opcode|=0x80;}
    if(variant==17&&current==0)response->Fragments=htoes(2);
    if(variant==18&&current==0)response->Opcode|=0x80;
    CHECK(!incoming_owned);incoming_owned=1;*mbx=&incoming;return 1;
}
int main(void) {
    ecx_contextt* context=(ecx_contextt*)calloc(1,sizeof(*context));
    ec_ODlistt* objects=(ec_ODlistt*)calloc(1,sizeof(*objects));
    ec_OElistt* entries=(ec_OElistt*)calloc(1,sizeof(*entries));
    int result;
    if(!context||!objects||!entries)return 1;
    for(mode=0;mode<3;++mode)for(variant=0;variant<=(mode==0?18:14);++variant) {
        memset(objects,0,sizeof(*objects));memset(entries,0,sizeof(*entries));
        objects->Slave=1;objects->Entries=1;objects->Index[0]=0x1000;
        sent=received=outgoing_owned=incoming_owned=0;last_abort=0;
        result=mode==0?ecx_readODlist(context,1,objects):mode==1?ecx_readODdescription(context,0,objects):ecx_readOEsingle(context,0,3,objects,entries);
        CHECK((result>0)==(variant==0||variant==18));
        CHECK(!outgoing_owned&&!incoming_owned);
        if(variant==0||variant==18) {
            if(mode==0)CHECK(objects->Entries==2&&objects->Index[0]==0x1000&&objects->Index[1]==0x8000);
            if(mode==1)CHECK(objects->DataType[0]==7&&objects->MaxSub[0]==3&&!strcmp(objects->Name[0],"Test"));
            if(mode==2)CHECK(entries->Entries==1&&entries->DataType[3]==6&&entries->ObjAccess[3]==63&&!strcmp(entries->Name[3],"Gain"));
        }
        if(variant==7)CHECK(last_abort==0x06090011);
        if(variant==8)CHECK(last_abort==0);
        if(variant==9)CHECK(sent==0);
        if(variant==14&&mode==0)CHECK(received==1);
        if(variant==15)CHECK(objects->Entries==1000&&received==5);
    }
    free(entries);free(objects);free(context);
    if(failures)return 1;
    puts("49 SDO Information scenarios passed: multipart lists, identity, length, capacity, Abort, deadline and mailbox ownership");
    return 0;
}
