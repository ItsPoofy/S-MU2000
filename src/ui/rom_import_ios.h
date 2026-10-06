// license:BSD-3-Clause
//
// Importing the ROM images on iOS, for both the standalone and the AUv3.
//
// A shared file rather than part of a window layer: both front ends import
// images - the standalone from its card menu, the extension from its own, since a
// plug-in has no container app of its own - and each has a window layer of its own
// (window_ios.mm and vst3/view_ios.mm) with neither linking the other, so this
// cannot live in either. presenter_ios.{h,mm} is here for the same reason. What
// belongs to one window is in that window: window_ios.mm holds the MIDI file
// panel and the Bluetooth MIDI sheets, because only the standalone has them.
//
// The images are Yamaha's, so no build we hand out carries them: the user brings
// their own dump. This is the iOS half of the flow the desktop already has in
// src/ui/rom_locate.h: find, explain, ask, validate, remember. The wording, the
// "what is missing" answer and the remembering are that file's
// (ui::roms_needed_message / roms_bad_message / remember_roms_dir), used here
// rather than reinvented, so every platform says the same thing.
//
// Two things are iOS's own:
// - The picker cannot be a blocking ask_roms_folder, the seam the desktop
//   implements per platform. A document picker has to be presented and answer
//   later, so the flow is a small state machine instead of a loop.
// - The set is copied rather than pointed at. A picked folder is granted for
//   that run only - iOS has no security-scoped bookmark, and no accessForFolder
//   API in this SDK - so install_roms() into the container is what survives a
//   relaunch. The desktop can point at the folder in place; only iOS copies.
//
// The destination is config_dir()/roms, the shared search's own first candidate
// (src/rom_search.h), so nothing in the search order changes and a baked bundle
// copy stays the fallback for local builds.
//
// Objective-C++ only (UIKit + std::function). Both callers are .mm files.


#ifndef S_MU2000_UI_ROM_IMPORT_IOS_H
#define S_MU2000_UI_ROM_IMPORT_IOS_H

#pragma once

#import <UIKit/UIKit.h>

#include <functional>
#include <string>
#include <vector>

namespace ui {
struct menu_group;
}

// Item id for the appended group. 6010 sits outside every ID_BASE..ID_BASE+255
// port range and the plug-in's own ids (100..121), so it never aliases.
enum : int {
	ID_IOS_INSTALL_ROMS = 6010,
};

// Called after a successful install, so a caller that found no ROMs at launch
// and skipped the boot can do it now. Not called on cancel or failure. One
// process-wide hook, replaced by the last setter (the app sets one, the plug-in
// sets none).
void set_rom_import_done(std::function<void()> on_done);

// Where an import lands: config_dir()/roms, the shared search's first candidate
// inside the container. Empty when the container cannot be resolved.
std::string ios_rom_dir();

// Appends the "ROM files" group (title from ui/texts.h, so it localizes).
void append_rom_import_group(std::vector<ui::menu_group> &groups);

// Acts on ID_IOS_INSTALL_ROMS: presents the picker and copies what comes back.
// Returns true when it took the id; anything else belongs to the caller.
bool handle_rom_import_item(UIView *view, int itemId);

// Presents the picker without a menu (launch with no ROMs found). Returns false
// when there is no view controller to present from.
bool prompt_for_roms(UIView *view);

#endif // S_MU2000_UI_ROM_IMPORT_IOS_H
