// license:BSD-3-Clause
#pragma once
#include "midi_in.h"
#include "midi_out.h"
#include "midi_routes.h"
#include "midi_split.h"
#include <memory>
#include <array>
#include <type_traits>

namespace ui {
// Reconfigure only while the engine is paused. Each physical endpoint is
// opened once; per-input framing prevents independent senders' running status
// and fragmented SysEx from interleaving on a shared synth port.
template <typename Input = midi_in, typename Output = midi_out>
class basic_midi_router {
	template <typename Port> struct endpoint {
		std::string name;
		unsigned mask = 0;
		Port port;
	};
	struct input_endpoint : endpoint<Input> { basic_midi_split<65536> messages{true}; };
	template <typename Entry> using endpoints = std::vector<std::unique_ptr<Entry>>;
	endpoints<input_endpoint> m_inputs;
	endpoints<endpoint<Output>> m_outputs;
	std::array<basic_midi_split<65536>, 4> m_output_messages{{{true}, {true}, {true}, {true}}};
	midi_routing m_routes;
	template <typename Entry>
	static bool prepare(const std::vector<midi_route> &routes, const endpoints<Entry> &current,
	                    const std::vector<std::string> &available, const std::vector<midi_route> &previous, endpoints<Entry> &added,
	                    bool missing_ok, std::string &error)
	{
		for (const auto &route : routes) {
			if (std::any_of(current.begin(), current.end(), [&](const auto &e) { return e->name == route.device; })) continue;
			const bool retained = midi_route_mask(previous, route.device) != 0;
			const auto it = std::find(available.begin(), available.end(), route.device);
			if (it == available.end()) {
				if (missing_ok || retained) continue;
				error = route.device + ": device is disconnected"; return false;
			}
			auto e = std::make_unique<Entry>(); e->name = route.device;
			std::string detail;
			if (!e->port.open(int(it - available.begin()), detail) || e->port.device_name() != route.device) {
				if (detail.empty()) detail = "device list changed while opening";
				error = route.device + ": " + detail;
				if (missing_ok || retained) continue;
				return false;
			}
			added.push_back(std::move(e));
		}
		return true;
	}
	template <typename Entry>
	static void commit(endpoints<Entry> &current, endpoints<Entry> &added,
	                   const std::vector<midi_route> &routes, const std::vector<std::string> &available)
	{
		for (auto &e : added) current.push_back(std::move(e));
		for (auto &e : current) e->mask = midi_route_mask(routes, e->name);
		std::erase_if(current, [&](const auto &e) {
			return !e->mask || std::find(available.begin(), available.end(), e->name) == available.end();
		});
	}
public:
	bool apply(const midi_routing &routes, bool missing_ok, std::string &error)
	{
		error.clear();
		const auto ins = Input::list(), outs = Output::list();
		endpoints<input_endpoint> inputs; endpoints<endpoint<Output>> outputs;
		if (!prepare(routes.inputs, m_inputs, ins, m_routes.inputs, inputs, missing_ok, error) ||
		    !prepare(routes.outputs, m_outputs, outs, m_routes.outputs, outputs, missing_ok, error)) return false;
		commit(m_inputs, inputs, routes.inputs, ins);
		commit(m_outputs, outputs, routes.outputs, outs);
		m_routes = routes;
		return true;
	}
	bool needs_refresh(const midi_routing &routes, const std::vector<std::string> &ins,
	                   const std::vector<std::string> &outs) const
	{
		const auto differs = [](const auto &wanted, const auto &active, const auto &available) {
			for (const auto &e : active)
				if (std::find(available.begin(), available.end(), e->name) == available.end()) return true;
			for (const auto &r : wanted) if (std::find(available.begin(), available.end(), r.device) != available.end())
				if (std::none_of(active.begin(), active.end(), [&](const auto &e) { return e->name == r.device; })) return true;
			return false;
		};
		return differs(routes.inputs, m_inputs, ins) || differs(routes.outputs, m_outputs, outs);
	}
	template <typename Receive> void drain(Receive &&receive)
	{
		for (auto &e : m_inputs) {
			struct context { std::remove_reference_t<Receive> *receive; unsigned mask; } ctx{&receive, e->mask};
			const auto emit = [](void *ptr, const u8 *bytes, size_t n) {
				auto &ctx = *static_cast<context *>(ptr);
				for (int port = 0; port < 5; port++) if (ctx.mask & (1u << port)) (*ctx.receive)(port, bytes, n);
			};
			u8 byte;
			// Bound work even if a virtual source continuously refills its ring.
			for (size_t n = 0; n < 65536 && e->port.pop(byte); n++) {
				e->messages.feed(&byte, 1, emit, &ctx);
			}
		}
	}
	void send(int source, u8 byte)
	{
		if (source < 0 || size_t(source) >= m_output_messages.size()) return;
		struct context { basic_midi_router *self; int source; } ctx{this, source};
		m_output_messages[size_t(source)].feed(&byte, 1, [](void *ptr, const u8 *bytes, size_t n) {
			auto &ctx = *static_cast<context *>(ptr);
			for (auto &e : ctx.self->m_outputs) if (e->mask & (1u << ctx.source))
				e->port.send(bytes, n);
		}, &ctx);
	}
	void close() { m_inputs.clear(); m_outputs.clear(); m_routes = {}; for (auto &parser : m_output_messages) parser.reset(); }
};
using midi_router = basic_midi_router<>;
} // namespace ui
