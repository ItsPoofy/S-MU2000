// license:BSD-3-Clause
#pragma once
#include "midi_routes.h"
#include "texts.h"
#include "imgui.h"
#include <functional>

namespace ui {
inline void draw_midi_device_list(const midi_routing &routing, const std::vector<std::string> &available,
                                  bool output, float height, const std::function<void(midi_routing)> &change)
{
	const auto &routes = output ? routing.outputs : routing.inputs;
	auto names = available;
	for (const auto &r : routes)
		if (std::find(names.begin(), names.end(), r.device) == names.end()) names.push_back(r.device);
	const int ports = output ? 3 : 5;
	ImGui::PushID(output ? "outputs" : "inputs");
	if (ImGui::BeginTable("devices", ports + 1, ImGuiTableFlags_BordersOuter | ImGuiTableFlags_RowBg |
	                     ImGuiTableFlags_ScrollY, ImVec2(0, height))) {
		ImGui::TableSetupColumn("device", ImGuiTableColumnFlags_WidthStretch);
		for (int p = 0; p < ports; p++)
			ImGui::TableSetupColumn("route", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * (output ? 4.3f : 1.6f));
		if (names.empty()) {
			ImGui::TableNextRow(); ImGui::TableNextColumn();
			ImGui::TextDisabled("%s", UI_TEXT(menu_no_devices, "(No devices)"));
		}
		ImGuiListClipper clipper;
		clipper.Begin(int(names.size()));
		while (clipper.Step()) for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
			const auto &name = names[size_t(i)];
			const bool connected = std::find(available.begin(), available.end(), name) != available.end();
			ImGui::TableNextRow(); ImGui::TableNextColumn();
			if (connected) ImGui::TextUnformatted(name.c_str());
			else ImGui::TextDisabled("%s", name.c_str());
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s%s%s", name.c_str(), connected ? "" : " — ",
				                  connected ? "" : UI_TEXT(settings_disconnected, "Disconnected"));
			}
			for (int p = 0; p < ports; p++) {
				ImGui::TableNextColumn();
				const char *labels[] = {"THRU A", "THRU B", "MU OUT"};
				const std::string label = (output ? std::string(labels[p]) : std::string(1, char('A' + p))) + "##" + name;
				const unsigned mask = midi_route_mask(routes, name);
				ImGui::BeginDisabled(!connected && !(mask & (1u << p)));
				ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.5f, 0.5f));
				if (ImGui::Selectable(label.c_str(), (mask & (1u << p)) != 0)) {
					auto next = routing;
					set_midi_route(output ? next.outputs : next.inputs, name, mask ^ (1u << p));
					change(std::move(next));
				}
				ImGui::PopStyleVar();
				ImGui::EndDisabled();
			}
		}
		ImGui::EndTable();
	}
	ImGui::PopID();
}
} // namespace ui
