// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#include "ui/internal.hpp"
#include <algorithm>
#include <cstring>
namespace niceecatm::workbench {
namespace {
void drawIcon(Icon kind,ImVec2 p,float size,ImU32 color) {
    auto* draw=ImGui::GetWindowDrawList();
    if(!color)color=ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const auto point=[&](float x,float y){return ImVec2{p.x+x*size/24,p.y+y*size/24};};
    const auto stroke=[&](float x,float y,float a,float b){draw->AddLine(point(x,y),point(a,b),color,1.25f);};
    const auto rect=[&](float x,float y,float a,float b){draw->AddRect(point(x,y),point(a,b),color,1.5f,0,1.2f);};
    switch(kind) {
    case Icon::Network:rect(4,3,20,10);stroke(12,10,12,15);stroke(4,15,20,15);stroke(4,15,4,19);stroke(20,15,20,19);rect(1,19,7,23);rect(17,19,23,23);break;
    case Icon::Box:rect(4,4,20,20);stroke(4,9,20,9);stroke(9,4,9,9);break;
    case Icon::Table:rect(3,4,21,20);stroke(3,9,21,9);stroke(9,9,9,20);break;
    case Icon::Registers:rect(6,6,18,18);for(int i=8;i<=16;i+=4){stroke(i,2,i,6);stroke(i,18,i,22);stroke(2,i,6,i);stroke(18,i,22,i);}break;
    case Icon::Pulse:stroke(1,12,6,12);stroke(6,12,10,3);stroke(10,3,14,21);stroke(14,21,18,12);stroke(18,12,23,12);break;
    case Icon::Mail:rect(3,5,21,19);stroke(3,5,12,13);stroke(12,13,21,5);break;
    case Icon::Book:rect(5,3,20,22);stroke(9,3,9,22);stroke(12,9,17,9);stroke(12,13,17,13);break;
    case Icon::Logs:rect(3,4,21,20);stroke(7,9,10,12);stroke(10,12,7,15);stroke(13,15,17,15);break;
    case Icon::Clock:draw->AddCircle(point(12,12),9*size/24,color,20,1.25f);stroke(12,6,12,12);stroke(12,12,17,15);break;
    case Icon::Scan:draw->PathArcTo(point(12,12),8*size/24,.1f,5.2f,20);draw->PathStroke(color,0,1.25f);stroke(20,3,20,9);stroke(20,9,14,9);break;
    case Icon::Play:
        draw->PathLineTo(point(8,5));draw->PathLineTo(point(19,12));draw->PathLineTo(point(8,19));
        draw->PathFillConvex(color);break;
    case Icon::Stop:draw->AddRectFilled(point(7,7),point(17,17),color);break;
    }
}
}
void icon(Icon kind,float size,ImU32 color) {
    drawIcon(kind,ImGui::GetCursorScreenPos(),size,color);ImGui::Dummy({size,size});
}
bool iconButton(const char* id,Icon kind,const char* tooltip,ImVec2 size) {
    const auto start=ImGui::GetCursorScreenPos();const std::string hidden=std::string("##")+id;const bool pressed=ImGui::Button(hidden.c_str(),size);
    drawIcon(kind,{start.x+(size.x-14)/2,start.y+(size.y-14)/2},14,0);
    if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)&&tooltip)ImGui::SetTooltip("%s",tooltip);
    return pressed;
}
void pill(const char* text,ImVec4 color) {
    ImGui::PushFont(fonts.small,11);auto p=ImGui::GetCursorScreenPos();auto textSize=ImGui::CalcTextSize(text);
    ImVec2 size{textSize.x+12,20};auto* draw=ImGui::GetWindowDrawList();
    draw->AddRectFilled(p,{p.x+size.x,p.y+size.y},ImGui::GetColorU32({color.x,color.y,color.z,.075f}),3);
    draw->AddRect(p,{p.x+size.x,p.y+size.y},ImGui::GetColorU32({color.x,color.y,color.z,.18f}),3);
    draw->AddText({p.x+6,p.y+3},ImGui::GetColorU32(color),text);ImGui::Dummy(size);ImGui::PopFont();
}
void beginCard(const char* id,ImVec2 size,ImVec2 padding) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,padding);ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding,6);
    ImGui::PushStyleColor(ImGuiCol_ChildBg,{1,1,1,1});
    ImGui::BeginChild(id,size,ImGuiChildFlags_Borders|ImGuiChildFlags_AlwaysUseWindowPadding);
}
void endCard() {ImGui::EndChild();ImGui::PopStyleColor();ImGui::PopStyleVar(2);}
void cardHeader(const char* title,Icon kind) {
    icon(kind,14,ImGui::GetColorU32(teal));ImGui::SameLine(0,8);ImGui::TextColored(ink,"%s",title);
    ImGui::Spacing();ImGui::Separator();ImGui::Spacing();
}
} // namespace niceecatm::workbench
