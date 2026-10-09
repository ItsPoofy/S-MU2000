// license:BSD-3-Clause
#include "ui/midi_router.h"
#include "ui/audio_device_watch.h"
#include "ui/settings.h"
#include <deque>
#include <iostream>
#include <map>
#include <stdexcept>

static void require(bool condition, const char *message)
{
	if (!condition) throw std::runtime_error(message);
}

template <int Side> struct fake_port {
	inline static std::vector<std::string> names;
	inline static std::map<std::string, fake_port *> active;
	inline static std::map<std::string, int> opens;
	inline static std::map<std::string, std::vector<u8>> sent;
	inline static std::string fail;
	std::string name;
	std::deque<u8> queued;
	~fake_port() { if (!name.empty()) active.erase(name); }
	static auto list() { return names; }
	bool open(int index, std::string &error)
	{
		const auto &selected = names.at(size_t(index));
		if (selected == fail) { error = "open failed"; return false; }
		require(!active.contains(selected), "Physical device opened twice");
		name = selected; active[name] = this; opens[name]++;
		return true;
	}
	std::string device_name() const { return name; }
	bool pop(u8 &byte)
	{
		if (queued.empty()) return false;
		byte = queued.front(); queued.pop_front(); return true;
	}
	bool send(const u8 *bytes, size_t n) { sent[name].insert(sent[name].end(), bytes, bytes + n); return true; }
};
using input = fake_port<0>;
using output = fake_port<1>;
using router = ui::basic_midi_router<input, output>;
using messages = std::array<std::vector<std::vector<u8>>, 5>;
static void push(const std::string &device, std::initializer_list<u8> bytes)
{
	auto &queue = input::active.at(device)->queued;
	queue.insert(queue.end(), bytes.begin(), bytes.end());
}
static messages drain(router &r)
{
	messages received;
	auto receive = [&](int port, const u8 *p, size_t n) { received[size_t(port)].emplace_back(p, p + n); };
	r.drain(receive); // also exercises an lvalue callback
	return received;
}
static void migration()
{
	ui::settings_map legacy;
	input::names = {"A input", "B input", "C input", "D input", "E input"};
	output::names = {"A output", "B output", "MU output", "Editor output"};
	for (int p = 0; p < 5; p++) legacy.emplace_back(ui::SET_IN_KEYS[p], input::names[p]);
	legacy.insert(legacy.end(), {{ui::SET_OUT, output::names[0]}, {ui::SET_OUT_B, output::names[1]},
		{ui::SET_OUT_MU, output::names[2]}, {ui::SET_EDIT_OUT, output::names[3]}});
	ui::remembered settings;
	ui::apply_settings(legacy, settings);
	for (int p = 0; p < 5; p++)
		require(ui::midi_route_mask(settings.midi.inputs, input::names[p]) == (1u << p), "Migration changed an input assignment");
	for (int p = 0; p < 3; p++)
		require(ui::midi_route_mask(settings.midi.outputs, output::names[p]) == (1u << p), "Migration changed an output assignment");
	auto routes = ui::midi_routes_with_editor(settings.midi, settings.edit_out);
	require(ui::midi_route_mask(routes.outputs, "Editor output") == 8, "Editor acquired a THRU or MU OUT route");
	auto r = std::make_unique<router>(); std::string error;
	require(r->apply(routes, false, error) && output::sent.empty(), "Opening migrated routes sent unsolicited MIDI");
	for (int p = 0; p < 5; p++) push(input::names[p], {0x90, u8(60 + p), 100});
	const auto received = drain(*r);
	for (int p = 0; p < 5; p++)
		require(received[p] == std::vector<std::vector<u8>>{{0x90, u8(60 + p), 100}}, "Migration changed input delivery");
	for (int p = 0; p < 3; p++)
		for (u8 byte : {u8(0x90), u8(60 + p), u8(100)}) r->send(p, byte);
	for (int p = 0; p < 3; p++)
		require(output::sent[output::names[p]] == std::vector<u8>({0x90, u8(60 + p), 100}), "Migration changed output delivery");
	require(output::sent["Editor output"].empty(), "Ordinary MIDI reached the editor-only port");
	for (u8 byte : {0xc0, 5}) r->send(3, byte);
	require(output::sent["Editor output"] == std::vector<u8>({0xc0, 5}), "Explicit editor send lost its destination");
	const auto sent = output::sent; r->close();
	require(output::sent == sent, "Closing migrated routes sent unsolicited MIDI");
	output::sent.clear(); input::opens.clear(); output::opens.clear();
}
static void recovery()
{
 input::names = {}; output::names = {"Unavailable"}; output::fail = "Unavailable";
 auto r = std::make_unique<router>();
 ui::midi_routing routes{{}, {{"Unavailable", 1}}};
 ui::audio_device_watch inputs, outputs;
 int attempts = 0; std::string error;
 const auto refresh = [&] {
  const bool changed = inputs.changed(input::names, {}) | outputs.changed(output::names, {});
  if (changed && r->needs_refresh(routes, input::names, output::names)) {
   attempts++; r->apply(routes, true, error);
  }
 };
 for (int i = 0; i < 100; i++) refresh();
 require(attempts == 1 && !error.empty(), "Unavailable MIDI device retried without a list change");
 output::fail.clear(); output::names.push_back("New device"); refresh();
 require(attempts == 2 && output::active.contains("Unavailable"), "Changed device list did not allow MIDI recovery");
 r->close(); output::opens.clear();
}
static void run()
{
	input::names = {"Keyboard", "Pads", "Third"}; output::names = {"Synth", "Recorder", "Bad"};
	auto r = std::make_unique<router>();
	ui::midi_routing config{{{"Keyboard", 3}, {"Pads", 1}}, {{"Synth", 15}, {"Recorder", 4}}};
	std::string error;
	require(r->apply(config, false, error), "Initial routing failed");
	require(input::active.size() == 2 && output::active.size() == 2, "Wrong endpoint count");
	push("Keyboard", {0x90, 60}); push("Pads", {0x91, 64, 100});
	auto received = drain(*r);
	require(received[0] == std::vector<std::vector<u8>>{{0x91, 64, 100}} && received[1].empty(), "Incomplete messages interleaved");
	push("Keyboard", {100, 61, 110}); received = drain(*r);
	require(received[0] == std::vector<std::vector<u8>>{{0x90, 60, 100}, {0x90, 61, 110}} && received[1] == received[0], "Running status or input fan-out corrupted");
	push("Keyboard", {0xf0, 0x43, 0x10}); push("Pads", {0x91, 65, 0});
	received = drain(*r);
	require(received[0] == std::vector<std::vector<u8>>{{0x91, 65, 0}}, "Partial SysEx leaked");
	push("Keyboard", {0xf8, 0x4c, 0xf7}); received = drain(*r);
	require(received[0] == std::vector<std::vector<u8>>{{0xf8}, {0xf0, 0x43, 0x10, 0x4c, 0xf7}} && received[0] == received[1], "Fragmented SysEx or realtime corrupted");
	for (u8 byte : {0xf0, 0x43, 0x10}) r->send(0, byte);
	for (u8 byte : {0x91, 62, 100}) r->send(1, byte);
	for (u8 byte : {0x4c, 0xf7}) r->send(0, byte);
	for (u8 byte : {0xf0, 0x7e, 0xf7}) r->send(2, byte);
	for (u8 byte : {0xc0, 5}) r->send(3, byte); // selected editor destination, opened only once
	require(output::sent["Synth"] == std::vector<u8>({0x91, 62, 100, 0xf0, 0x43, 0x10, 0x4c, 0xf7, 0xf0, 0x7e, 0xf7, 0xc0, 5}), "Shared output streams interleaved");
	require(output::sent["Recorder"] == std::vector<u8>({0xf0, 0x7e, 0xf7}), "Output fan-out or isolation failed");
	const auto before = output::sent;
	auto failed = config;
	ui::set_midi_route(failed.inputs, "Third", 16);
	ui::set_midi_route(failed.outputs, "Bad", 1);
	output::fail = "Bad";
	require(!r->apply(failed, false, error) && !error.empty(), "Open failure was accepted");
	require(!input::active.contains("Third") && input::active.size() == 2 && output::sent == before, "Failed transaction changed working routes");
	push("Keyboard", {0x80, 60, 0}); received = drain(*r);
	require(received[0] == received[1] && received[1].size() == 1, "Rollback lost original input routing");
	input::names = {"Pads", "Third"};
	require(r->needs_refresh(config, input::list(), output::list()), "Disconnect was not detected");
	require(r->apply(config, true, error) && !input::active.contains("Keyboard"), "Disconnected input remained active");
	require(output::sent == before, "Disconnecting an input sent unsolicited MIDI");
	auto edited = config; ui::set_midi_route(edited.inputs, "Pads", 17);
	require(r->apply(edited, false, error), "Remembered disconnected device blocked unrelated edit");
	input::names = {"Third", "Pads", "Keyboard"}; // enumeration order changed
	require(r->needs_refresh(edited, input::list(), output::list()), "Reconnect was not detected");
	require(r->apply(edited, true, error), "Reconnect failed");
	push("Keyboard", {0x90, 70, 100}); received = drain(*r);
	require(received[0] == received[1] && received[1].size() == 1 && received[4].empty(), "Reconnect routed to wrong device/group");
	require(input::opens["Keyboard"] == 2 && input::opens["Pads"] == 1 && output::opens["Synth"] == 1, "Unchanged devices were reopened");
	push("Pads", {0x90, 72, 100}); received = drain(*r);
	require(received[0] == received[4] && received[4].size() == 1, "Port E route failed");
	ui::clear_midi_column(edited.inputs, 0);
	require(r->apply(edited, false, error), "Clearing route failed");
	require(output::sent == before, "Removing an input route sent unsolicited MIDI");
	push("Keyboard", {0x80, 70, 0}); received = drain(*r);
	require(received[0].empty() && received[1].size() == 1, "Disabling one route disabled other routes");
	// Large dumps stay whole, and malformed fragments cannot contaminate the next sender.
	auto &queue = input::active.at("Keyboard")->queued;
	queue.push_back(0xf0); for (int i = 0; i < 12000; i++) queue.push_back(1); queue.push_back(0xf7);
	received = drain(*r);
	require(received[1].size() == 1 && received[1][0].size() == 12002, "Large input SysEx truncated");
	push("Keyboard", {0x90, 60, 0x91, 61, 100}); received = drain(*r);
	require(received[1] == std::vector<std::vector<u8>>{{0x91, 61, 100}}, "Malformed short message leaked");
	const auto before_unassign = output::sent;
	auto no_inputs = edited; no_inputs.inputs.clear();
	require(r->apply(no_inputs, false, error) && input::active.empty(), "Input unassignment failed");
	require(output::sent == before_unassign, "Unassigning inputs sent unsolicited MIDI");
	auto no_outputs = edited; no_outputs.outputs.clear();
	require(r->apply(no_outputs, false, error), "Output unassignment failed");
	require(output::sent == before_unassign, "Unassigning an output sent unsolicited MIDI");
	r->close();
	require(output::sent == before_unassign, "Closing the router sent unsolicited MIDI");
	require(input::active.empty() && output::active.empty(), "Closing router leaked endpoints");
}
int main()
{
	try { migration(); recovery(); run(); std::cout << "MIDI fan-in/out, framing, rollback, hotplug and endpoint lifetime: PASS\n"; }
	catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
