// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#include "app/cli.hpp"
#include "ui/workbench.hpp"
#include <SDL3/SDL.h>
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include <iostream>
#include <string>
#include <cstdlib>
#include <cstring>
const char* niceecatmFontData();
const char* niceecatmMonoFontData();
int runDesktop(int argc,char**argv) {
    int frames=0,device=0,width=1366,height=768;bool smoke=false,smokeCyclic=false;std::string screenshot,language,tab;
    for(int i=1;i<argc;++i){std::string a=argv[i];if(a=="--ui-smoke")smoke=true;
      else if(a=="--ui-smoke-cyclic"){smoke=true;smokeCyclic=true;}
      else if(a=="--frames"&&i+1<argc)frames=std::max(1,std::atoi(argv[++i]));
      else if(a=="--screenshot"&&i+1<argc)screenshot=argv[++i];
      else if(a=="--language"&&i+1<argc){language=argv[++i];if(language!="en"&&language!="zh-CN")return 2;}
      else if(a=="--tab"&&i+1<argc)tab=argv[++i];
      else if(a=="--device"&&i+1<argc)device=std::max(0,std::atoi(argv[++i]));
      else if(a=="--width"&&i+1<argc)width=std::max(1100,std::atoi(argv[++i]));
      else if(a=="--height"&&i+1<argc)height=std::max(720,std::atoi(argv[++i]));
      else {std::cerr<<"Unknown GUI argument: "<<a<<'\n';return 2;}}
    if(smokeCyclic){frames=std::max(60,frames);if(tab.empty())tab="diagnostics";}
    if(!SDL_Init(SDL_INIT_VIDEO)){std::cerr<<SDL_GetError()<<'\n';return 1;}
    auto* window=SDL_CreateWindow("NiceECatM · EtherCAT Workbench",width,height,SDL_WINDOW_RESIZABLE|SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if(!window){SDL_Quit();return 1;}
    SDL_SetWindowMinimumSize(window,1100,720);
    auto* renderer=SDL_CreateRenderer(window,nullptr);
    if(!renderer)renderer=SDL_CreateRenderer(window,"software");
    if(!renderer){SDL_DestroyWindow(window);SDL_Quit();return 1;}
    SDL_SetRenderVSync(renderer,1);
    IMGUI_CHECKVERSION();ImGui::CreateContext();
    auto&io=ImGui::GetIO();io.IniFilename=nullptr;io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;
    ImFontConfig fontCfg;fontCfg.OversampleH=2;fontCfg.OversampleV=1;
    auto* regular=io.Fonts->AddFontFromMemoryCompressedBase85TTF(niceecatmFontData(),14.0f,&fontCfg);
    auto* small=io.Fonts->AddFontFromMemoryCompressedBase85TTF(niceecatmFontData(),12.0f,&fontCfg);
    auto* title=io.Fonts->AddFontFromMemoryCompressedBase85TTF(niceecatmFontData(),20.0f,&fontCfg);
    auto* value=io.Fonts->AddFontFromMemoryCompressedBase85TTF(niceecatmFontData(),28.0f,&fontCfg);
    ImFontConfig monoCfg;monoCfg.SizePixels=14;monoCfg.OversampleH=2;monoCfg.OversampleV=1;
    auto* mono=io.Fonts->AddFontFromMemoryCompressedBase85TTF(niceecatmMonoFontData(),14.0f,&monoCfg);
    ImFontConfig fallbackCfg=fontCfg;fallbackCfg.MergeMode=true;
    io.Fonts->AddFontFromMemoryCompressedBase85TTF(niceecatmFontData(),13.0f,&fallbackCfg);
    niceecatm::configureWorkbenchFonts(regular,small,title,value,mono);
    ImGui_ImplSDL3_InitForSDLRenderer(window,renderer);ImGui_ImplSDLRenderer3_Init(renderer);
    niceecatm::configureWorkbench(language.empty()?nullptr:language.c_str(),tab.empty()?nullptr:tab.c_str(),device);
    niceecatm::configureWorkbenchSource(smoke);
    niceecatm::Controller controller;
    if(smoke){niceecatm::Command c;c.kind=niceecatm::CommandKind::Scan;c.connection.simulation=true;c.connection.primary="sim0";controller.submit(c);}
    else {niceecatm::Command c;c.kind=niceecatm::CommandKind::RefreshAdapters;c.connection.simulation=false;controller.submit(c);}
    bool done=false,cyclicRequested=false;int frame=0,exitCode=0,cyclicSamples=0;uint64_t lastCyclicSample=0;
    while(!done){SDL_Event event;while(SDL_PollEvent(&event)){
      ImGui_ImplSDL3_ProcessEvent(&event);
      if(event.type==SDL_EVENT_QUIT||(event.type==SDL_EVENT_WINDOW_CLOSE_REQUESTED&&event.window.windowID==SDL_GetWindowID(window)))done=true;
    }
      if(smokeCyclic) {
        const auto snapshot=controller.snapshot();
        if(!cyclicRequested&&snapshot.simulation&&snapshot.mapped&&!snapshot.busy) {
          niceecatm::Command command;command.kind=niceecatm::CommandKind::State;command.state=niceecatm::AlState::Op;
          cyclicRequested=controller.submit(command)!=0;
        }
        if(snapshot.running&&snapshot.actualCycleUs>0&&snapshot.cycles!=lastCyclicSample) {
          lastCyclicSample=snapshot.cycles;++cyclicSamples;
        }
      }
      ImGui_ImplSDLRenderer3_NewFrame();ImGui_ImplSDL3_NewFrame();ImGui::NewFrame();niceecatm::drawWorkbench(controller);
      // TextEditor is a third-party ImGui widget and sets WantTextInput
      // during rendering.  SDL3's backend updates IME state before the frame,
      // so explicitly mirror the request here for the next frame; otherwise
      // key presses such as Backspace work while printable text events never
      // reach the editor.
      if(io.WantTextInput) {
        if(!SDL_TextInputActive(window))SDL_StartTextInput(window);
      } else if(SDL_TextInputActive(window))SDL_StopTextInput(window);
      ImGui::Render();
      SDL_SetRenderScale(renderer,io.DisplayFramebufferScale.x,io.DisplayFramebufferScale.y);
      SDL_SetRenderDrawColor(renderer,242,246,249,255);SDL_RenderClear(renderer);
      ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(),renderer);
      ++frame;
      if(frames>0&&frame>=frames&&(!smokeCyclic||cyclicSamples>=3||frame>=frames+300)){
        if(smokeCyclic) {
          const auto snapshot=controller.snapshot();
          if(!snapshot.simulation||!snapshot.running||cyclicSamples<3) {
            std::cerr<<"Cyclic UI smoke did not produce populated simulation history.\n";exitCode=1;
          }
          if(tab=="diagnostics") {
            int checked=0;
            for(const auto* child:ImGui::GetCurrentContext()->Windows) {
              const char* leaf=std::strrchr(child->Name,'/');leaf=leaf?leaf+1:child->Name;
              if(!child->Active||(std::strncmp(leaf,"diagnostics-history_",20)&&std::strncmp(leaf,"diagnostics-page_",17)))continue;
              ++checked;
              if(child->ScrollMax.x>0.5f||child->ScrollMax.y>0.5f) {
                std::cerr<<"Cyclic diagnostics layout overflow: "<<child->Name<<" ("<<child->ScrollMax.x<<", "<<child->ScrollMax.y<<")\n";exitCode=1;
              }
            }
            if(checked!=2){std::cerr<<"Cyclic diagnostics layout check did not find both windows.\n";exitCode=1;}
          }
        }
        if(!screenshot.empty()){auto*surface=SDL_RenderReadPixels(renderer,nullptr);if(!surface||!SDL_SaveBMP(surface,screenshot.c_str())){std::cerr<<SDL_GetError()<<'\n';if(surface)SDL_DestroySurface(surface);niceecatm::shutdownScripts();return 1;}SDL_DestroySurface(surface);}
        done=true;
      }
      SDL_RenderPresent(renderer);SDL_Delay(5);
    }
    niceecatm::shutdownScripts();
    SDL_StopTextInput(window);
    ImGui_ImplSDLRenderer3_Shutdown();ImGui_ImplSDL3_Shutdown();ImGui::DestroyContext();SDL_DestroyRenderer(renderer);SDL_DestroyWindow(window);SDL_Quit();return exitCode;
}
