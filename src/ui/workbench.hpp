// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#pragma once
#include "master/controller.hpp"
struct ImFont;
namespace niceecatm {
// Called between ImGui::NewFrame and ImGui::Render. Owns UI drafts, never backend state.
void drawWorkbench(Controller& controller);
// Optional startup presentation override. It does not save a language preference
// or issue any master command.
void configureWorkbench(const char* language,const char* tab,int slave=0);
void configureWorkbenchSource(bool simulation);
void configureWorkbenchFonts(ImFont* regular,ImFont* small,ImFont* title,ImFont* value,ImFont* mono);
// Stops the optional script worker before the Controller owner is destroyed.
void shutdownScripts();
}
