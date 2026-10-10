// license:BSD-3-Clause
#pragma once
#include <algorithm>
#include <string>
#include <vector>

namespace ui {
struct midi_route {
	std::string device;
	unsigned ports = 0;
	bool operator==(const midi_route &) const = default;
};
struct midi_routing {
	std::vector<midi_route> inputs, outputs;
	bool operator==(const midi_routing &) const = default;
};
inline unsigned midi_route_mask(const std::vector<midi_route> &routes, const std::string &name)
{
	for (const auto &route : routes) if (route.device == name) return route.ports;
	return 0;
}
inline void set_midi_route(std::vector<midi_route> &routes, const std::string &name, unsigned mask)
{
	if (name.empty()) return;
	auto it = std::find_if(routes.begin(), routes.end(), [&](const auto &r) { return r.device == name; });
	if (!mask) { if (it != routes.end()) routes.erase(it); }
	else if (it == routes.end()) routes.push_back({name, mask});
	else it->ports = mask;
}
inline void clear_midi_column(std::vector<midi_route> &routes, int column)
{
	for (auto &route : routes) route.ports &= ~(1u << column);
	std::erase_if(routes, [](const auto &route) { return !route.ports; });
}
inline std::string midi_column_name(const std::vector<midi_route> &routes, int column)
{
	std::string result;
	for (const auto &route : routes) if (route.ports & (1u << column)) {
		if (!result.empty()) result += ", ";
		result += route.device;
	}
	return result;
}
inline midi_routing midi_routes_with_editor(midi_routing routes, const std::string &editor)
{
	set_midi_route(routes.outputs, editor, midi_route_mask(routes.outputs, editor) | 8u);
	return routes;
}
} // namespace ui
