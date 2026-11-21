#pragma once
#include <imgui.h>
namespace Theme {
inline void Apply() {
	ImGuiStyle& s = ImGui::GetStyle();
	ImVec4* c = ImGui::GetStyle().Colors;
	s.WindowRounding = 8.0f;
	s.FrameRounding = 6.0f;
	s.GrabRounding = 4.0f;
	s.TabRounding = 6.0f;
	s.ScrollbarRounding = 6.0f;
	s.PopupRounding = 6.0f;
	s.ChildRounding = 6.0f;
	s.WindowPadding = ImVec2(12, 12);
	s.FramePadding = ImVec2(8, 4);
	s.ItemSpacing = ImVec2(8, 6);
	s.ItemInnerSpacing = ImVec2(6, 4);
	s.WindowBorderSize = 1.0f;
	s.FrameBorderSize = 0.0f;
	s.WindowTitleAlign = ImVec2(0.5f, 0.5f);
	c[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.07f, 0.10f, 0.98f);
	c[ImGuiCol_ChildBg] = ImVec4(0.08f, 0.09f, 0.12f, 1.00f);
	c[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.09f, 0.12f, 0.98f);
	c[ImGuiCol_Border] = ImVec4(0.15f, 0.18f, 0.25f, 0.60f);
	c[ImGuiCol_FrameBg] = ImVec4(0.10f, 0.12f, 0.16f, 1.00f);
	c[ImGuiCol_FrameBgHovered] = ImVec4(0.14f, 0.17f, 0.23f, 1.00f);
	c[ImGuiCol_FrameBgActive] = ImVec4(0.18f, 0.21f, 0.28f, 1.00f);
	c[ImGuiCol_TitleBg] = ImVec4(0.06f, 0.07f, 0.10f, 1.00f);
	c[ImGuiCol_TitleBgActive] = ImVec4(0.08f, 0.10f, 0.14f, 1.00f);
	c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.04f, 0.05f, 0.07f, 0.60f);
	c[ImGuiCol_MenuBarBg] = ImVec4(0.08f, 0.09f, 0.12f, 1.00f);
	c[ImGuiCol_ScrollbarBg] = ImVec4(0.06f, 0.07f, 0.10f, 0.60f);
	c[ImGuiCol_ScrollbarGrab] = ImVec4(0.20f, 0.24f, 0.32f, 0.80f);
	c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.25f, 0.30f, 0.40f, 0.80f);
	c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.30f, 0.36f, 0.48f, 1.00f);
	c[ImGuiCol_CheckMark] = ImVec4(0.35f, 0.88f, 0.55f, 1.00f);
	c[ImGuiCol_SliderGrab] = ImVec4(0.35f, 0.65f, 1.00f, 0.80f);
	c[ImGuiCol_SliderGrabActive] = ImVec4(0.45f, 0.75f, 1.00f, 1.00f);
	c[ImGuiCol_Button] = ImVec4(0.15f, 0.20f, 0.30f, 1.00f);
	c[ImGuiCol_ButtonHovered] = ImVec4(0.20f, 0.28f, 0.42f, 1.00f);
	c[ImGuiCol_ButtonActive] = ImVec4(0.25f, 0.35f, 0.52f, 1.00f);
	c[ImGuiCol_Header] = ImVec4(0.12f, 0.16f, 0.24f, 1.00f);
	c[ImGuiCol_HeaderHovered] = ImVec4(0.16f, 0.22f, 0.34f, 1.00f);
	c[ImGuiCol_HeaderActive] = ImVec4(0.20f, 0.28f, 0.42f, 1.00f);
	c[ImGuiCol_Separator] = ImVec4(0.15f, 0.18f, 0.25f, 0.60f);
	c[ImGuiCol_SeparatorHovered] = ImVec4(0.25f, 0.30f, 0.42f, 1.00f);
	c[ImGuiCol_SeparatorActive] = ImVec4(0.35f, 0.40f, 0.55f, 1.00f);
	c[ImGuiCol_ResizeGrip] = ImVec4(0.20f, 0.24f, 0.32f, 0.40f);
	c[ImGuiCol_ResizeGripHovered] = ImVec4(0.30f, 0.36f, 0.48f, 0.80f);
	c[ImGuiCol_ResizeGripActive] = ImVec4(0.35f, 0.65f, 1.00f, 1.00f);
	c[ImGuiCol_Tab] = ImVec4(0.10f, 0.13f, 0.18f, 1.00f);
	c[ImGuiCol_TabHovered] = ImVec4(0.18f, 0.24f, 0.36f, 1.00f);
	c[ImGuiCol_TabActive] = ImVec4(0.14f, 0.20f, 0.30f, 1.00f);
	c[ImGuiCol_TextSelectedBg] = ImVec4(0.35f, 0.65f, 1.00f, 0.35f);
	c[ImGuiCol_DragDropTarget] = ImVec4(0.35f, 0.65f, 1.00f, 0.90f);
	c[ImGuiCol_NavCursor] = ImVec4(0.35f, 0.65f, 1.00f, 0.70f);
}
}
