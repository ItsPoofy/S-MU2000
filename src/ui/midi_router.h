// license:BSD-3-Clause
#pragma once
#include "midi_in.h"
#include "midi_out.h"
#include "midi_routes.h"
#include "midi_split.h"
#include <memory>
#include <array>
#include <atomic>
#include <type_traits>

namespace ui {
// Open/close drivers while audio runs; pause only to replace endpoint lists.
// Route masks are atomic, so column-only changes need no pause. Each physical
// endpoint opens once. Per-input framing keeps independent senders separate.
// Input and output messages are all-or-nothing; SysEx over 65,536 bytes is dropped.
template <typename Input = midi_in, typename Output = midi_out>
class basic_midi_router {
	template <typename Port> struct endpoint {
		std::string name;
		std::atomic<unsigned> mask{0};
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
	                   const std::vector<midi_route> &routes, const std::vector<std::string> &available, endpoints<Entry> &retired)
	{
		for (auto &e : added) current.push_back(std::move(e));
		for (auto &e : current) e->mask = midi_route_mask(routes, e->name);
		std::erase_if(current, [&](auto &e) {
			if (e->mask.load() && std::find(available.begin(), available.end(), e->name) != available.end()) return false;
			retired.push_back(std::move(e));
			return true;
		});
	}
public:
	template <typename Publish>
	bool apply(const midi_routing &routes, bool missing_ok, std::string &error, Publish &&publish)
	{
		error.clear();
		const auto ins = Input::list(), outs = Output::list();
		endpoints<input_endpoint> inputs, retired_inputs;
		endpoints<endpoint<Output>> outputs, retired_outputs;
		if (!prepare(routes.inputs, m_inputs, ins, m_routes.inputs, inputs, missing_ok, error) ||
		    !prepare(routes.outputs, m_outputs, outs, m_routes.outputs, outputs, missing_ok, error)) return false;
		const auto removes = [](const auto &current, const auto &wanted, const auto &available) {
			return std::any_of(current.begin(), current.end(), [&](const auto &e) {
				return !midi_route_mask(wanted, e->name) || std::find(available.begin(), available.end(), e->name) == available.end();
			});
		};
		if (inputs.empty() && outputs.empty() && !removes(m_inputs, routes.inputs, ins) && !removes(m_outputs, routes.outputs, outs)) {
			for (auto &e : m_inputs) e->mask.store(midi_route_mask(routes.inputs, e->name));
			for (auto &e : m_outputs) e->mask.store(midi_route_mask(routes.outputs, e->name));
		} else {
			// Retired ports close after publish returns, with audio running.
			retired_inputs.reserve(m_inputs.size()); retired_outputs.reserve(m_outputs.size());
			publish([&] {
				commit(m_inputs, inputs, routes.inputs, ins, retired_inputs);
				commit(m_outputs, outputs, routes.outputs, outs, retired_outputs);
			});
		}
		m_routes = routes;
		return true;
	}
	bool apply(const midi_routing &routes, bool missing_ok, std::string &error)
	{
		return apply(routes, missing_ok, error, [](auto &&commit) { commit(); });
	}
	bool connected(bool output, const std::string &name) const
	{
		const auto contains = [&](const auto &ports) {
			return std::any_of(ports.begin(), ports.end(), [&](const auto &e) { return e->name == name; });
		};
		return output ? contains(m_outputs) : contains(m_inputs);
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
			struct context { std::remove_reference_t<Receive> *receive; unsigned mask; } ctx{&receive, e->mask.load()};
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
			for (auto &e : ctx.self->m_outputs) if (e->mask.load() & (1u << ctx.source))
				e->port.send(bytes, n);
		}, &ctx);
	}
	void close() { m_inputs.clear(); m_outputs.clear(); m_routes = {}; for (auto &parser : m_output_messages) parser.reset(); }
};
using midi_router = basic_midi_router<>;
} // namespace ui
