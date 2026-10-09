// license:BSD-3-Clause
//
// The editors' file requests, answered with document pickers.
//
// The shared xgui layer asks for a file and waits: ask_open_file(),
// ask_open_wav(), ask_open_card(), ask_open_dls(), ask_open_midi(),
// ask_save_file(). Windows answers with the Win32 common dialogs (see
// src/ui/pc_window.cpp), and this file is the answer for iOS - which is what
// lets set_file_dialogs(true) stand here, so the editors offer the buttons
// rather than the path box they fall back to when a platform has no dialogs.
//
// Three things are iOS's own, and all three come from the sandbox:
//
// - **A picked file has no path we may keep.** The grant dies with the process
//   and there is no security-scoped bookmark in this SDK, so anything the
//   editors keep a *path* to (a card image, a DLS bank, a playlist entry) is
//   copied into the container first and the container's own path is what goes
//   back. The ROM import already works this way (ui/rom_import_ios.mm).
// - **There is no save panel.** A save request writes the bytes to a temporary
//   file and hands that to an export picker, which is the iOS equivalent of
//   "save somewhere": Files, iCloud Drive, a USB drive, anywhere the user
//   points it.
// - **Nothing may be opened while a frame is being drawn.** The request is only
//   taken in service_file_asks(), which both window layers call after painting -
//   the same rule the Windows message loop follows, for the same reason.
//
// One flow at a time, like the ROM import: the picker's delegate property is
// weak, so the delegate is held here for the presentation.

#ifndef S_MU2000_UI_FILE_ASK_IOS_H
#define S_MU2000_UI_FILE_ASK_IOS_H

#pragma once

@class UIView;

namespace ui {

// Says that this platform has file dialogs, which is what switches the editors
// from the path box to the buttons. Called once at start-up by both front ends.
void enable_file_dialogs();

// Takes the request the editors left, if any, and presents the picker that
// answers it. Called after painting, never inside a frame.
void service_file_asks(UIView *view);

} // namespace ui

#endif // S_MU2000_UI_FILE_ASK_IOS_H