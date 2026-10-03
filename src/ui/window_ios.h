// license:BSD-3-Clause
//
// The iOS window system's services.
//
// Nothing in this header mentions UIKit. window_ios.mm implements it. That is the
// rule ui/window_mac.h sets and it is worth keeping: an @interface cannot be
// declared inside a namespace, so a header that exposed the view class would either
// break the build ("Objective-C declarations may only appear in global scope") or
// push the class to global scope and lose the ui:: namespacing. Declaring SMUView
// in the .mm, the way window_mac.mm does, avoids both.
//
// It is also why the Metal and QuartzCore imports that imgui_shell.h needs are not
// here: imgui_shell.h is reached only from the .mm, which imports them first.
//
// The twins are ui/window_mac.h, ui/window_win.h and ui/window_sdl.h.

#ifndef S_MU2000_UI_WINDOW_IOS_H
#define S_MU2000_UI_WINDOW_IOS_H

#pragma once

namespace ui {

class gui_app;

// A UIView of the given size, wired to the app, painting the panel at 30 Hz.
//
// Returns the view, or nullptr when Metal is unavailable so the caller can say so
// rather than showing a blank screen with no explanation. **The caller must put the
// returned view in the hierarchy** (as the root view controller's view, or as a
// subview): a view that is made but never attached draws nothing, and the symptom is
// a black screen with no error anywhere - which is exactly what the first version of
// this did, by returning bool and dropping the view on the floor.
//
// Step 2 of the port: one finger is the mouse, a held second finger is the right
// button (context menus, including the MIDI port picker). No keyboard yet - the
// panel's single-key shortcuts and the editors' text fields need UIKeyCommand /
// UITextInput next - and no audio device.
UIView *make_ios_view(gui_app &a, int w, int h);

} // namespace ui

#endif // S_MU2000_UI_WINDOW_IOS_H