// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#include "script/session.hpp"
#include <stdexcept>
#include <utility>

namespace niceecatm::script {

Session::Session(Controller& controller)
    : controller_(controller), bridge_(controller), runtime_({
        [this](const ScriptRequest& request){return submit(request);},
        [this](const ScriptRequest& request,HostResult& result){return bridge_.execute(request,result);},
        [this]{releaseController();}
    }) {}

Session::~Session() {stop();}

bool Session::start(std::string name,std::string source) {
    stop();
    startError_.clear();
    if(!runtime_.load(std::move(name),std::move(source)))return false;
    if(!controller_.setScriptRunning(true)) {
        startError_="Wait for the FoE transfer or BOOT transition to finish before starting a script.";
        return false;
    }
    try {
        controller_.enableScriptSnapshots(true);
        runtime_.updateSnapshot(controller_.scriptSnapshot());
        stopping_=false;
        dispatcher_=std::thread(&Session::dispatchUpdates,this);
        if(runtime_.start())return true;
    } catch(const std::exception& error) {
        startError_=error.what();
    }
    releaseController();
    stop();
    return false;
}

void Session::releaseController() {
    controller_.enableScriptSnapshots(false);
    controller_.setScriptRunning(false);
}

void Session::stop() {
    {
        std::lock_guard<std::mutex> lock(requestMutex_);
        stopping_=true;
        if(pending_)bridge_.cancel(pending_);
        pending_=0;
    }
    runtime_.stop(); // Also wakes a Lua worker waiting on a host result.
    if(dispatcher_.joinable())dispatcher_.join();
}

uint64_t Session::submit(const ScriptRequest& request) {
    std::lock_guard<std::mutex> lock(requestMutex_);
    if(stopping_)return 0;
    std::string error;
    pending_=bridge_.submit(request,&error);
    if(!pending_&&!error.empty())throw std::runtime_error(error);
    return pending_;
}

void Session::dispatchUpdates() {
    uint64_t version=0;
    Snapshot snapshot;
    while(controller_.waitForScriptUpdate(version,snapshot)) {
        std::lock_guard<std::mutex> lock(requestMutex_);
        if(stopping_)return;
        Result result;
        if(pending_&&bridge_.result(pending_,result)) {
            // A command can complete after the wait captured its snapshot.
            // Publish its state before resuming Lua (e.g. scan then devices).
            runtime_.updateSnapshot(controller_.scriptSnapshot());
            runtime_.complete(pending_,{result.ok,std::move(result.message),std::move(result.data),std::move(result.adapters)});
            pending_=0;
        } else {
            runtime_.updateSnapshot(snapshot);
        }
    }
}

View Session::snapshot() const {
    auto view=runtime_.snapshot();
    if(!startError_.empty()){view.status=Status::Error;view.error=startError_;}
    return view;
}
}
