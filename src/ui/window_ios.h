// license:BSD-3-Clause
//
// The iOS window system's services.
//
// No view class is declared here. window_ios.mm declares SMUView, the way
// window_mac.mm does, because an @interface cannot be declared inside a namespace:
// a header exposing it would either break the build ("Objective-C declarations may
// only appear in global scope") or push the class to global scope and lose the ui::
// namespacing.
//
// UIKit itself appears in exactly one place, the __OBJC__ block below. The header
// is reachable from plain C++ - src/ui/app_ios.cpp includes it through app_ios.h
// for open_midi_file_panel(), the way app_mac.h includes window_mac.h - and @class
// is not C++, so the two declarations that name a UIView are visible to the
// Objective-C++ callers and to nobody else.
//
// The Metal and QuartzCore imports that imgui_shell.h needs are not here either:
// imgui_shell.h is reached only from the .mm, which imports them first.
//
// The twins are ui/window_mac.h, ui/window_win.h and ui/window_sdl.h.

#ifndef S_MU2000_UI_WINDOW_IOS_H
#define S_MU2000_UI_WINDOW_IOS_H

#pragma once

#include "ui/menu.h"   // menu_group, for the group this window appends

// The Bluetooth and network MIDI menu ids. They live in the window layer's
// header because the rows are part of the window's own menu (window_ios.mm adds
// the group to the menus it builds), and ui::menu.h's ids are global for the same
// reason. They are deliberately outside the shared id space: this window has its
// own lists, so these never alias a port, and handle_midi_setup_item() runs
// before menu_chosen() and says whether it took the id.
enum : int {
	ID_IOS_BT_CONNECT = 6000,
	ID_IOS_BT_ADVERTISE = 6001,
	ID_IOS_NET_MIDI = 6002,
};

namespace ui {

class gui_app;

// A UIView of the given size, wired to the app, painting the panel at 30 Hz.
//
// Returns the view, or nullptr when Metal is unavailable so the caller can say so
// rather than showing a blank screen with no explanation. **The caller must put the
// returned view in the hierarchy** (as the root view controller's view, or as a
// subview): a view that is made but never attached draws nothing, and the symptom is
// a black screen with no error anywhere.
//
// Runs the document picker for a MIDI file and plays what comes back, which is
// why it returns nothing where window_mac.h's open_midi_file_panel() returns a
// path: a document picker answers later, so no path can come back from the call
// that starts it. The shared code reads an empty path as "cancelled" and does
// nothing, and the file plays itself once the picker has answered.
//
// The bytes are read while the picker still holds its grant - a picked file's
// access dies with the process on iOS, there being no security-scoped bookmark
// to keep - and handed to the shared player, so nothing is copied anywhere.
void open_midi_file_panel();

// ---- Bluetooth and network MIDI --------------------------------------------
//
// Apple's pairing sheets and the network session, in the window system because
// they are presented from it - which is where macOS keeps the matching pieces
// (CoreMIDI's session is the desktop's business, so there the menu only lists
// endpoints; here the toggle owns the policy too, because iOS has no Audio MIDI
// Setup app to do it).
//
// The ids are above; the two functions act on them.

// Appends the "Bluetooth & network MIDI" group (titles from ui/texts.h, so it
// localizes like the rest). Always appends; a group list that is empty is the
// caller's business.
void append_midi_setup_group(std::vector<menu_group> &groups);

// The network session switch, persisted in NSUserDefaults ("smu_network_midi").
bool network_midi_enabled();

// Applies the stored switch at startup: enabling the session is what makes
// network endpoints exist for midi_in/out::list(). Called once from the app's
// main; opening the menu later needs nothing, menu_snapshot() re-enumerates.
void apply_stored_midi_setup();

} // namespace ui

// ---- What names a UIView, so only Objective-C++ can call it -------------------
//
// Both are window-system plumbing for the .mm files: make_ios_view() for the app's
// main, handle_midi_setup_item() for this window's own menu dispatcher (which runs
// before menu_chosen() and says whether it took the id).

#ifdef __OBJC__

#import <CoreGraphics/CoreGraphics.h>   // CGPoint, for where a sheet is anchored

@class UIView;

namespace ui {

// A UIView of the given size, wired to the app, painting the panel at 30 Hz.
//
// Returns the view, or nullptr when Metal is unavailable so the caller can say so
// rather than showing a blank screen with no explanation. **The caller must put the
// returned view in the hierarchy** (as the root view controller's view, or as a
// subview): a view that is made but never attached draws nothing, and the symptom is
// a black screen with no error anywhere.
//
// One finger is the mouse, a held second finger is the right button (context menus,
// including the MIDI port picker). The hardware keys are in ui/keymap_ios.h.
UIView *make_ios_view(gui_app &a, int w, int h);

// Presents Apple's pairing sheet for the two Bluetooth rows, anchored at the tap
// point, and toggles the network session for the third. True when it took the id.
bool handle_midi_setup_item(UIView *view, CGPoint at, int itemId);

} // namespace ui

#endif // __OBJC__

#endif // S_MU2000_UI_WINDOW_IOS_H