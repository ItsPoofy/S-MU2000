// license:BSD-3-Clause
//
// Which view controller a sheet is presented from.
//
// Three iOS features present something and two window layers do it for them: the
// ROM import, the Bluetooth MIDI setup and the MIDI file picker, on either the
// standalone's window (window_ios.mm) or the extension's (vst3/view_ios.mm). It is
// a shared header rather than a window-layer service because both front ends link
// it and neither window layer is the other's - the extension has no window_ios.mm.
//
// The _ios suffix is the point: this is iOS code, not a shared interface with an
// iOS implementation behind it, and it says so at the include the same way
// keymap_ios.h, session_ios.h and window_ios.mm do. A reader meeting
// ui::presenter_for in rom_import_ios.mm should not have to open the header to learn
// which platform it is for.

#ifndef S_MU2000_UI_PRESENTER_IOS_H
#define S_MU2000_UI_PRESENTER_IOS_H

@class UIView;
@class UIViewController;

namespace ui {

// The view's own controller, found by walking up to the view whose next
// responder is one. Else the foreground scene's key window's root - iOS has more
// than one window, and the right one is the foreground scene's, which is the rule
// pc_window_ios.mm already follows. Nil when there is nothing to present from,
// which callers report rather than assume.
UIViewController *presenter_for(UIView *view);

} // namespace ui

#endif // S_MU2000_UI_PRESENTER_IOS_H