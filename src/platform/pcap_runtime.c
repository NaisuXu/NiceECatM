// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

/* Narrow Windows adaptation for SOEM's libpcap calls.
 * Load only from the OS Npcap directory or System32, never the working directory.
 * No driver or service is installed/started by this module. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#define HAVE_REMOTE
#include <pcap.h>
#include <stdio.h>
#include <string.h>
#include "pcap_runtime.h"

static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
static HMODULE module;
static char status_text[512] = "Npcap / WinPcap not loaded";
static char last_error[PCAP_ERRBUF_SIZE] = "";
static pcap_t* (*open_fn)(const char*, int, int, int, struct pcap_rmtauth*, char*);
static int (*find_fn)(pcap_if_t**, char*);
static void (*free_fn)(pcap_if_t*);
static void (*close_fn)(pcap_t*);
static int (*send_fn)(pcap_t*, const u_char*, int);
static int (*next_fn)(pcap_t*, struct pcap_pkthdr**, const u_char**);
static int (*nonblock_fn)(pcap_t*, int, char*);
static char* (*geterr_fn)(pcap_t*);
static const char* (*version_fn)(void);
static int (*compile_fn)(pcap_t*, struct bpf_program*, const char*, int, bpf_u_int32);
static int (*filter_fn)(pcap_t*, struct bpf_program*);
static void (*freecode_fn)(struct bpf_program*);

static BOOL CALLBACK load_capture(PINIT_ONCE ignored, PVOID parameter, PVOID* context)
{
    wchar_t system_dir[MAX_PATH], path[MAX_PATH];
    DWORD length;
    int npcap=1;
    (void)ignored; (void)parameter; (void)context;
    if (GetEnvironmentVariableW(L"NICEECATM_DISABLE_PCAP", path, MAX_PATH)>0) {
        strcpy(status_text, "Capture driver disabled by NICEECATM_DISABLE_PCAP");
        return TRUE;
    }
    length=GetSystemDirectoryW(system_dir, MAX_PATH);
    if (!length || length>MAX_PATH-25) return TRUE;
    wcscpy(path, system_dir); wcscat(path,L"\\Npcap\\wpcap.dll");
    module=LoadLibraryExW(path,NULL,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) {
        npcap=0;
        wcscpy(path, system_dir); wcscat(path,L"\\wpcap.dll");
        module=LoadLibraryExW(path,NULL,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    }
    if (!module) {
        strcpy(status_text,"Npcap / WinPcap DLL missing or could not be loaded; install Npcap (WinPcap API compatibility)");
        return TRUE;
    }
    /* Windows uses the same representation for these function pointers.
     * Copy the representation without accessing a typed pointer through an
     * incompatible FARPROC lvalue. The typedef rejects an unsupported ABI. */
#define BIND(name, symbol) do { \
    FARPROC address = GetProcAddress(module,symbol); \
    typedef char capture_pointer_size_must_match[(sizeof(name)==sizeof(address))?1:-1]; \
    (void)sizeof(capture_pointer_size_must_match); \
    memcpy(&(name),&address,sizeof(name)); \
} while(0)
    BIND(open_fn,"pcap_open"); BIND(find_fn,"pcap_findalldevs");
    BIND(free_fn,"pcap_freealldevs"); BIND(close_fn,"pcap_close");
    BIND(send_fn,"pcap_sendpacket"); BIND(next_fn,"pcap_next_ex");
    BIND(nonblock_fn,"pcap_setnonblock"); BIND(geterr_fn,"pcap_geterr");
    BIND(version_fn,"pcap_lib_version"); BIND(compile_fn,"pcap_compile");
    BIND(filter_fn,"pcap_setfilter"); BIND(freecode_fn,"pcap_freecode");
#undef BIND
    if (!open_fn || !find_fn || !free_fn || !close_fn || !send_fn || !next_fn || !nonblock_fn) {
        FreeLibrary(module); module=NULL;
        strcpy(status_text,"Capture DLL is incompatible: required pcap API entry point missing");
        return TRUE;
    }
    _snprintf(status_text,sizeof(status_text)-1,"%s: %s",npcap?"Npcap":"WinPcap / system capture",version_fn?version_fn():"runtime binding");
    status_text[sizeof(status_text)-1]=0;
    return TRUE;
}

int niceecatm_pcap_available(void) { InitOnceExecuteOnce(&once,load_capture,NULL,NULL); return module!=NULL; }
const char* niceecatm_pcap_status(void) { niceecatm_pcap_available(); return status_text; }
const char* niceecatm_pcap_error(void) { return last_error[0]?last_error:niceecatm_pcap_status(); }
const char* niceecatm_pcap_last_error(void) { return last_error; }

int pcap_findalldevs(pcap_if_t** devices,char* error)
{
    if (!niceecatm_pcap_available()) { *devices=NULL; strncpy(error,status_text,PCAP_ERRBUF_SIZE-1);error[PCAP_ERRBUF_SIZE-1]=0;return -1; }
    { int ret=find_fn(devices,error); if(ret<0) {strncpy(last_error,error,sizeof(last_error)-1);last_error[sizeof(last_error)-1]=0;}return ret; }
}
void pcap_freealldevs(pcap_if_t* devices) { if(niceecatm_pcap_available() && devices)free_fn(devices); }
pcap_t* pcap_open(const char* name,int snap,int flags,int timeout,struct pcap_rmtauth* auth,char* error)
{
    pcap_t* handle;
    (void)timeout;
    if(!niceecatm_pcap_available()) {strncpy(error,status_text,PCAP_ERRBUF_SIZE-1);error[PCAP_ERRBUF_SIZE-1]=0;return NULL;}
    last_error[0]=0;
    handle=open_fn(name,snap,flags,1,auth,error);
    if(!handle) {strncpy(last_error,error,sizeof(last_error)-1);last_error[sizeof(last_error)-1]=0;return NULL;}
    if(nonblock_fn(handle,1,error)<0) {strncpy(last_error,error,sizeof(last_error)-1);last_error[sizeof(last_error)-1]=0;close_fn(handle);return NULL;}
    if(compile_fn && filter_fn && freecode_fn) {
        struct bpf_program filter;
        if(compile_fn(handle,&filter,"ether proto 0x88a4",1,0xffffffff)==0) { filter_fn(handle,&filter); freecode_fn(&filter); }
    }
    return handle;
}
void pcap_close(pcap_t* handle) {if(niceecatm_pcap_available() && handle)close_fn(handle);}
int pcap_sendpacket(pcap_t* handle,const u_char* bytes,int length)
{
    int ret;
    if(!niceecatm_pcap_available() || !handle)return -1;
    ret=send_fn(handle,bytes,length);
    if(ret<0 && geterr_fn) {strncpy(last_error,geterr_fn(handle),sizeof(last_error)-1);last_error[sizeof(last_error)-1]=0;}
    return ret;
}
int pcap_next_ex(pcap_t* handle,struct pcap_pkthdr** header,const u_char** bytes)
{
    int ret;
    if(!niceecatm_pcap_available() || !handle)return -1;
    ret=next_fn(handle,header,bytes);
    if(ret<0 && geterr_fn) {strncpy(last_error,geterr_fn(handle),sizeof(last_error)-1);last_error[sizeof(last_error)-1]=0;}
    return ret;
}
#endif
