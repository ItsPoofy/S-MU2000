// license:BSD-3-Clause
//
// The editors' file requests, answered with AppKit panels.
//
// The seam is xgui's (ask_open_file, ask_open_wav, ask_open_card, ask_open_dls,
// ask_open_midi, ask_save_file) and Windows answers it in its own render
// (src/ui/pc_window.cpp) while iOS answers it with document pickers
// (src/ui/file_ask_ios.mm). This is the macOS third of that set, and it lives in
// a file of its own rather than in window_mac.mm because the plug-in needs it
// too: both front ends link the editor windows (pc_window_mac.mm) and neither
// links the standalone's window.
//
// The panels here are the same three window_mac.mm opens for the ROM and MIDI
// paths, written out again because those are declared in window_mac.h, which is
// the standalone's header - a plug-in has no window_mac.o to link against, and
// two dozen lines of AppKit are cheaper than that coupling.
//
// Until this existed, file_dialogs() was false on this platform: the editors
// showed a path box, which is usable here (a typed path is reachable) but which
// the SysEx export/import and the voice library's save do without - those were
// simply missing.
//
// Nothing may be opened inside a frame, so service_file_asks() is called from the
// editor window's frame with the work handed to the main queue, which is where
// Windows posts its window message and where iOS calls it from its display link.

#ifndef S_MU2000_UI_FILE_ASK_MAC_H
#define S_MU2000_UI_FILE_ASK_MAC_H

#pragma once

namespace ui {

// Says that this platform has file dialogs, which is what switches the editors
// from the path box to the buttons. Called when an editor window is made.
void enable_file_dialogs();

// Takes the request the editors left, if any, and opens the panel that answers
// it. Called after painting, never inside a frame.
void service_file_asks();

} // namespace ui

#endif // S_MU2000_UI_FILE_ASK_MAC_H
