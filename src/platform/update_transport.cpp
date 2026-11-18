// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#include "update_check.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#elif defined(NICEECATM_HAVE_CURL)
#include <curl/curl.h>
#endif

namespace niceecatm::updates {
namespace {
constexpr std::size_t MaxBodyBytes=256*1024;
constexpr auto RequestBudget=std::chrono::seconds(10);
constexpr auto CancelInterval=std::chrono::milliseconds(50);
using Clock=std::chrono::steady_clock;

bool interrupted(const std::atomic<bool>& canceled,Clock::time_point deadline,
                 std::string& error) {
    if(canceled.load(std::memory_order_relaxed)) {
        error="Update check canceled.";
        return true;
    }
    if(Clock::now()>=deadline) {
        error="Update check timed out.";
        return true;
    }
    return false;
}

#ifdef _WIN32
struct InternetHandle {
    HINTERNET value=nullptr;
    explicit InternetHandle(HINTERNET handle):value(handle) {}
    ~InternetHandle() {if(value) WinHttpCloseHandle(value);}
    InternetHandle(const InternetHandle&)=delete;
    InternetHandle& operator=(const InternetHandle&)=delete;
};

// WinHTTP may finish callbacks after CloseHandle returns. The request keeps
// one reference until HANDLE_CLOSING, its final non-reentrant callback.
struct AsyncState {
    std::atomic<unsigned> references{1};
    std::mutex mutex;
    std::condition_variable changed;
    DWORD completed=0;
    DWORD error=0;
    DWORD bytes=0;
    std::array<char,8192> buffer{};
    void release() {
        if(references.fetch_sub(1,std::memory_order_acq_rel)==1) delete this;
    }
    void reset() {
        std::lock_guard<std::mutex> lock(mutex);
        completed=error=bytes=0;
    }
};

void CALLBACK statusCallback(HINTERNET,DWORD_PTR context,DWORD status,
                             LPVOID information,DWORD length) {
    auto* state=reinterpret_cast<AsyncState*>(context);
    if(!state) return;
    if(status==WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) {
        state->release();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if(status==WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) {
            state->error=information && length>=sizeof(WINHTTP_ASYNC_RESULT)
                ? static_cast<WINHTTP_ASYNC_RESULT*>(information)->dwError
                : ERROR_WINHTTP_INTERNAL_ERROR;
        } else if(status==WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE ||
                  status==WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE ||
                  status==WINHTTP_CALLBACK_STATUS_READ_COMPLETE) {
            state->completed=status;
            if(status==WINHTTP_CALLBACK_STATUS_READ_COMPLETE) state->bytes=length;
        }
    }
    state->changed.notify_one();
}

struct AsyncRequest {
    HINTERNET value=nullptr;
    AsyncState* state=nullptr;
    explicit AsyncRequest(HINTERNET handle):value(handle) {}
    ~AsyncRequest() {
        if(value) WinHttpCloseHandle(value);
        if(state) state->release();
    }
    AsyncRequest(const AsyncRequest&)=delete;
    AsyncRequest& operator=(const AsyncRequest&)=delete;
    bool initialize() {
        state=new AsyncState;
        DWORD_PTR context=reinterpret_cast<DWORD_PTR>(state);
        if(!WinHttpSetOption(value,WINHTTP_OPTION_CONTEXT_VALUE,&context,sizeof(context)))
            return false;
        state->references.fetch_add(1,std::memory_order_relaxed);
        if(WinHttpSetStatusCallback(value,statusCallback,
               WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS|WINHTTP_CALLBACK_FLAG_HANDLES,0)
           ==WINHTTP_INVALID_STATUS_CALLBACK) {
            state->release();
            return false;
        }
        return true;
    }
};

std::string winHttpError(DWORD code) {
    if(code==ERROR_WINHTTP_TIMEOUT) return "Update check timed out.";
    if(code==ERROR_WINHTTP_NAME_NOT_RESOLVED) return "Cannot resolve api.github.com.";
    if(code==ERROR_WINHTTP_SECURE_FAILURE) return "HTTPS certificate verification failed.";
    return "HTTPS request failed (WinHTTP "+std::to_string(code)+").";
}

bool waitFor(AsyncState& state,DWORD completion,const std::atomic<bool>& canceled,
             Clock::time_point deadline,std::string& error) {
    std::unique_lock<std::mutex> lock(state.mutex);
    for(;;) {
        if(interrupted(canceled,deadline,error)) return false;
        if(state.error) {
            error=winHttpError(state.error);
            return false;
        }
        if(state.completed==completion) return true;
        state.changed.wait_until(lock,std::min(deadline,Clock::now()+CancelInterval));
    }
}

Response fetchWindows(const std::atomic<bool>& canceled) {
    Response response;
    const auto deadline=Clock::now()+RequestBudget;
    if(interrupted(canceled,deadline,response.error)) return response;
    InternetHandle session(WinHttpOpen(L"NiceECatM",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,WINHTTP_FLAG_ASYNC));
    if(!session.value) {
        response.error=winHttpError(GetLastError());
        return response;
    }
    if(!WinHttpSetTimeouts(session.value,3000,3000,3000,3000)) {
        response.error=winHttpError(GetLastError());
        return response;
    }
    InternetHandle connection(WinHttpConnect(session.value,L"api.github.com",
        INTERNET_DEFAULT_HTTPS_PORT,0));
    if(!connection.value) {
        response.error=winHttpError(GetLastError());
        return response;
    }
    AsyncRequest request(WinHttpOpenRequest(connection.value,L"GET",
        L"/repos/NaisuXu/NiceECatM/releases/latest",nullptr,WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE));
    if(!request.value) {
        response.error=winHttpError(GetLastError());
        return response;
    }
    DWORD redirectPolicy=WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if(!WinHttpSetOption(request.value,WINHTTP_OPTION_REDIRECT_POLICY,
                         &redirectPolicy,sizeof(redirectPolicy)) || !request.initialize()) {
        response.error=winHttpError(GetLastError());
        return response;
    }
    constexpr wchar_t headers[]=L"Accept: application/vnd.github+json\r\n"
        L"X-GitHub-Api-Version: 2022-11-28\r\n";
    auto& state=*request.state;
    state.reset();
    if(!WinHttpSendRequest(request.value,headers,static_cast<DWORD>(-1L),
        WINHTTP_NO_REQUEST_DATA,0,0,reinterpret_cast<DWORD_PTR>(&state))) {
        response.error=winHttpError(GetLastError());
        return response;
    }
    if(!waitFor(state,WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE,canceled,deadline,
                response.error)) return response;
    state.reset();
    if(!WinHttpReceiveResponse(request.value,nullptr)) {
        response.error=winHttpError(GetLastError());
        return response;
    }
    if(!waitFor(state,WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE,canceled,deadline,
                response.error)) return response;
    DWORD status=0,statusBytes=sizeof(status);
    if(!WinHttpQueryHeaders(request.value,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,&status,&statusBytes,WINHTTP_NO_HEADER_INDEX)) {
        response.error=winHttpError(GetLastError());
        return response;
    }
    response.status=static_cast<int>(status);
    for(;;) {
        if(interrupted(canceled,deadline,response.error)) return response;
        state.reset();
        if(!WinHttpReadData(request.value,state.buffer.data(),
                            static_cast<DWORD>(state.buffer.size()),nullptr)) {
            response.error=winHttpError(GetLastError());
            return response;
        }
        if(!waitFor(state,WINHTTP_CALLBACK_STATUS_READ_COMPLETE,canceled,deadline,
                    response.error)) return response;
        if(state.bytes==0) return response;
        if(state.bytes>MaxBodyBytes-response.body.size()) {
            response.error="Update response is too large.";
            return response;
        }
        response.body.append(state.buffer.data(),state.bytes);
    }
}
#elif defined(NICEECATM_HAVE_CURL)
struct CurlHeaders {
    curl_slist* value=nullptr;
    ~CurlHeaders() {curl_slist_free_all(value);}
    bool add(const char* header) {
        auto* next=curl_slist_append(value,header);
        if(!next) return false;
        value=next;
        return true;
    }
};

struct CurlTransfer {
    CURL* easy=nullptr;
    CURLM* multi=nullptr;
    bool added=false;
    ~CurlTransfer() {
        if(added) curl_multi_remove_handle(multi,easy);
        if(easy) curl_easy_cleanup(easy);
        if(multi) curl_multi_cleanup(multi);
    }
};

struct CurlBody {
    Response& response;
    const std::atomic<bool>& canceled;
    bool tooLarge=false;
    bool allocationFailed=false;
};

std::size_t receiveBody(char* data,std::size_t size,std::size_t count,void* context) {
    auto& body=*static_cast<CurlBody*>(context);
    if(body.canceled.load(std::memory_order_relaxed)) return 0;
    if(size && count>(MaxBodyBytes-body.response.body.size())/size) {
        body.tooLarge=true;
        return 0;
    }
    const auto bytes=size*count;
    try {
        body.response.body.append(data,bytes);
    } catch(...) {
        body.allocationFailed=true;
        return 0;
    }
    return bytes;
}

Response fetchCurl(const std::atomic<bool>& canceled) {
    Response response;
    const auto deadline=Clock::now()+RequestBudget;
    if(interrupted(canceled,deadline,response.error)) return response;
    // C++ static initialization serializes our callers. Never globally clean up
    // libcurl: another component may still be using its process-wide state.
    static const CURLcode initialized=curl_global_init(CURL_GLOBAL_DEFAULT);
    if(initialized!=CURLE_OK) {
        response.error="Cannot initialize HTTPS support.";
        return response;
    }
    const auto* version=curl_version_info(CURLVERSION_NOW);
    // A blocking resolver cannot respect cancellation or NOSIGNAL timeouts.
    if(!version || !(version->features&CURL_VERSION_ASYNCHDNS)) {
        response.error="HTTPS update checks require asynchronous DNS support.";
        return response;
    }
    CurlHeaders headers;
    if(!headers.add("Accept: application/vnd.github+json") ||
       !headers.add("X-GitHub-Api-Version: 2022-11-28")) {
        response.error="Cannot allocate HTTPS request headers.";
        return response;
    }
    // Keep callback data alive until remove_handle/easy_cleanup have finished.
    CurlBody body{response,canceled};
    CurlTransfer transfer;
    transfer.easy=curl_easy_init();
    transfer.multi=curl_multi_init();
    if(!transfer.easy || !transfer.multi) {
        response.error="Cannot initialize HTTPS request.";
        return response;
    }
    if(curl_easy_setopt(transfer.easy,CURLOPT_URL,ApiUrl)!=CURLE_OK ||
       curl_easy_setopt(transfer.easy,CURLOPT_USERAGENT,"NiceECatM")!=CURLE_OK ||
       curl_easy_setopt(transfer.easy,CURLOPT_HTTPHEADER,headers.value)!=CURLE_OK ||
       curl_easy_setopt(transfer.easy,CURLOPT_NOSIGNAL,1L)!=CURLE_OK ||
       curl_easy_setopt(transfer.easy,CURLOPT_CONNECTTIMEOUT_MS,3000L)!=CURLE_OK ||
       curl_easy_setopt(transfer.easy,CURLOPT_TIMEOUT_MS,10000L)!=CURLE_OK ||
       curl_easy_setopt(transfer.easy,CURLOPT_FOLLOWLOCATION,0L)!=CURLE_OK ||
       curl_easy_setopt(transfer.easy,CURLOPT_SSL_VERIFYPEER,1L)!=CURLE_OK ||
       curl_easy_setopt(transfer.easy,CURLOPT_SSL_VERIFYHOST,2L)!=CURLE_OK ||
       curl_easy_setopt(transfer.easy,CURLOPT_WRITEFUNCTION,receiveBody)!=CURLE_OK ||
       curl_easy_setopt(transfer.easy,CURLOPT_WRITEDATA,&body)!=CURLE_OK) {
        response.error="Cannot configure HTTPS request.";
        return response;
    }
    if(curl_multi_add_handle(transfer.multi,transfer.easy)!=CURLM_OK) {
        response.error="Cannot start HTTPS request.";
        return response;
    }
    transfer.added=true;
    for(;;) {
        if(interrupted(canceled,deadline,response.error)) return response;
        int running=0;
        if(curl_multi_perform(transfer.multi,&running)!=CURLM_OK) {
            response.error="HTTPS transfer failed.";
            return response;
        }
        if(!running) break;
        int ready=0;
#if LIBCURL_VERSION_NUM >= 0x074200
        const auto result=curl_multi_poll(transfer.multi,nullptr,0,
            static_cast<int>(CancelInterval.count()),&ready);
#else
        const auto result=curl_multi_wait(transfer.multi,nullptr,0,
            static_cast<int>(CancelInterval.count()),&ready);
        if(result==CURLM_OK && !ready) std::this_thread::sleep_for(CancelInterval);
#endif
        if(result!=CURLM_OK) {
            response.error="HTTPS transfer failed.";
            return response;
        }
    }
    long status=0;
    curl_easy_getinfo(transfer.easy,CURLINFO_RESPONSE_CODE,&status);
    response.status=static_cast<int>(status);
    int remaining=0;
    while(auto* message=curl_multi_info_read(transfer.multi,&remaining)) {
        if(message->msg==CURLMSG_DONE && message->easy_handle==transfer.easy) {
            if(body.tooLarge) response.error="Update response is too large.";
            else if(body.allocationFailed) response.error="Cannot allocate update response.";
            else if(message->data.result!=CURLE_OK)
                response.error=std::string("HTTPS request failed: ")+
                    curl_easy_strerror(message->data.result)+".";
            return response;
        }
    }
    response.error="HTTPS request did not complete.";
    return response;
}
#endif
} // namespace

Response fetchLatest(const std::atomic<bool>& canceled) {
#ifdef _WIN32
    return fetchWindows(canceled);
#elif defined(NICEECATM_HAVE_CURL)
    return fetchCurl(canceled);
#else
    (void)canceled;
    return {0,{},"HTTPS update checking is not available in this build."};
#endif
}
} // namespace niceecatm::updates
