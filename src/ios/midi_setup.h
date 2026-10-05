// license:BSD-3-Clause
//
// Bluetooth and network MIDI setup for the iOS standalone.
//
// CoreMIDI on iOS only lists endpoints that are already connected
// (MIDIGetNumberOfSources/Destinations in ui/midi_in_mac.cpp and
// ui/midi_out_mac.cpp, reused unchanged here). USB MIDI appears once plugged
// in, but Bluetooth LE peripherals and network sessions never appear without
// explicit setup:
//
// - Bluetooth needs Apple's pairing UI (CoreAudioKit): CABTMIDICentralViewController
//   to connect to BLE gear, CABTMIDILocalPeripheralViewController to advertise
//   this device. There is no API for custom pairing UI.
// - Network MIDI is off until the app enables MIDINetworkSession and sets a
//   connection policy; then its endpoints join the same lists automatically.
//
// The settings surface stays shared: this file only appends one titled group to
// the menu the app already builds (ui/menu.h), and handles its three items.
// The pairing sheets themselves are Apple's code, not a second UI codebase.
// The AUv3 needs none of this: the host routes MIDI to the extension.
//
// Objective-C++ only (UIKit + std::function). Plain C++ callers cannot see it,
// which is fine: both callers are .mm files.

#ifndef S_MU2000_IOS_MIDI_SETUP_H
#define S_MU2000_IOS_MIDI_SETUP_H

#pragma once

#import <UIKit/UIKit.h>

#include <vector>

namespace ui {
struct menu_group;
}

// Item ids for the appended group. 6000+ sits outside every ID_BASE..ID_BASE+255
// port range and every single id in ui/menu.h (whose static_assert guards only
// its own lists), so these never alias a port and menu_chosen must never see
// them: handle_midi_setup_item runs first and returns whether it took the id.
enum : int {
	ID_IOS_BT_CONNECT = 6000,
	ID_IOS_BT_ADVERTISE = 6001,
	ID_IOS_NET_MIDI = 6002,
};

// Appends the "Bluetooth & network MIDI" group (titles from ui/texts.h, so it
// localizes like the rest). A no-op when groups is empty is the caller's job:
// this always appends.
void append_midi_setup_group(std::vector<ui::menu_group> &groups);

// Acts on one of the ids above: presents Apple's pairing sheet for the two
// Bluetooth rows (anchored at the tap point), toggles the network session for
// the third. Returns true when it took the id; anything else belongs to
// menu_chosen.
bool handle_midi_setup_item(UIView *view, CGPoint at, int itemId);

// The network session switch, persisted in NSUserDefaults ("smu_network_midi").
bool network_midi_enabled();

// Applies the stored switch at startup: enabling the session is what makes
// network endpoints exist for midi_in/out::list(). Called once from app.mm;
// opening the menu later needs nothing, menu_snapshot() re-enumerates.
void apply_stored_midi_setup();

#endif // S_MU2000_IOS_MIDI_SETUP_H
