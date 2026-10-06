// license:BSD-3-Clause
//
// The shared iOS key map: UIPress to ImGui keys and panel codes.
//
// Two sinks, like everywhere else. The editors feed ImGui (key events for
// navigation and shortcuts, characters through UIKeyInput's insertText:),
// the panel feeds ui::app verbs (single characters and F2-F5, exactly what
// window_mac.mm's codeForEvent hands over). What is shared is the HID usage
// table and the modifier feed, so both stay in one place.
//
// Header-only (inline), the way ui/keymap.h is: no build system changes.
// Objective-C++ only (UIKit + UIPress). Plain C++ callers cannot see it,
// which is fine: both callers are .mm files.

#ifndef S_MU2000_IOS_KEYMAP_H
#define S_MU2000_IOS_KEYMAP_H

#pragma once

#import <UIKit/UIKit.h>

#include "imgui.h"

#include "ui/menu.h"   // KEY_F2..F5, the shared F-key codes

// A hardware key press to the ImGui key for it, or ImGuiKey_None. Letters,
// digits, arrows, navigation and F-keys: what the editors navigate and
// shortcut with. Punctuation maps to None on purpose for KEY events, but
// printable text DOES also arrive through presses (see PCEditView): a bare
// UIKeyInput responder gets raw presses only - the system synthesizes
// insertText:/deleteBackward: solely for full UITextInput (and for the
// software keyboard). The simulator's "hardware keyboard" behaves the same
// way, which is why shortcuts worked there while typing did not.
inline ImGuiKey imgui_key_from_hid(NSInteger usage)
{
	switch (usage) {
	case UIKeyboardHIDUsageKeyboardA: return ImGuiKey_A;
	case UIKeyboardHIDUsageKeyboardB: return ImGuiKey_B;
	case UIKeyboardHIDUsageKeyboardC: return ImGuiKey_C;
	case UIKeyboardHIDUsageKeyboardD: return ImGuiKey_D;
	case UIKeyboardHIDUsageKeyboardE: return ImGuiKey_E;
	case UIKeyboardHIDUsageKeyboardF: return ImGuiKey_F;
	case UIKeyboardHIDUsageKeyboardG: return ImGuiKey_G;
	case UIKeyboardHIDUsageKeyboardH: return ImGuiKey_H;
	case UIKeyboardHIDUsageKeyboardI: return ImGuiKey_I;
	case UIKeyboardHIDUsageKeyboardJ: return ImGuiKey_J;
	case UIKeyboardHIDUsageKeyboardK: return ImGuiKey_K;
	case UIKeyboardHIDUsageKeyboardL: return ImGuiKey_L;
	case UIKeyboardHIDUsageKeyboardM: return ImGuiKey_M;
	case UIKeyboardHIDUsageKeyboardN: return ImGuiKey_N;
	case UIKeyboardHIDUsageKeyboardO: return ImGuiKey_O;
	case UIKeyboardHIDUsageKeyboardP: return ImGuiKey_P;
	case UIKeyboardHIDUsageKeyboardQ: return ImGuiKey_Q;
	case UIKeyboardHIDUsageKeyboardR: return ImGuiKey_R;
	case UIKeyboardHIDUsageKeyboardS: return ImGuiKey_S;
	case UIKeyboardHIDUsageKeyboardT: return ImGuiKey_T;
	case UIKeyboardHIDUsageKeyboardU: return ImGuiKey_U;
	case UIKeyboardHIDUsageKeyboardV: return ImGuiKey_V;
	case UIKeyboardHIDUsageKeyboardW: return ImGuiKey_W;
	case UIKeyboardHIDUsageKeyboardX: return ImGuiKey_X;
	case UIKeyboardHIDUsageKeyboardY: return ImGuiKey_Y;
	case UIKeyboardHIDUsageKeyboardZ: return ImGuiKey_Z;
	case UIKeyboardHIDUsageKeyboard1: return ImGuiKey_1;
	case UIKeyboardHIDUsageKeyboard2: return ImGuiKey_2;
	case UIKeyboardHIDUsageKeyboard3: return ImGuiKey_3;
	case UIKeyboardHIDUsageKeyboard4: return ImGuiKey_4;
	case UIKeyboardHIDUsageKeyboard5: return ImGuiKey_5;
	case UIKeyboardHIDUsageKeyboard6: return ImGuiKey_6;
	case UIKeyboardHIDUsageKeyboard7: return ImGuiKey_7;
	case UIKeyboardHIDUsageKeyboard8: return ImGuiKey_8;
	case UIKeyboardHIDUsageKeyboard9: return ImGuiKey_9;
	case UIKeyboardHIDUsageKeyboard0: return ImGuiKey_0;
	case UIKeyboardHIDUsageKeyboardReturnOrEnter: return ImGuiKey_Enter;
	case UIKeyboardHIDUsageKeyboardEscape: return ImGuiKey_Escape;
	case UIKeyboardHIDUsageKeyboardDeleteOrBackspace: return ImGuiKey_Backspace;
	case UIKeyboardHIDUsageKeyboardTab: return ImGuiKey_Tab;
	case UIKeyboardHIDUsageKeyboardSpacebar: return ImGuiKey_Space;
	case UIKeyboardHIDUsageKeyboardDeleteForward: return ImGuiKey_Delete;
	case UIKeyboardHIDUsageKeyboardHome: return ImGuiKey_Home;
	case UIKeyboardHIDUsageKeyboardEnd: return ImGuiKey_End;
	case UIKeyboardHIDUsageKeyboardPageUp: return ImGuiKey_PageUp;
	case UIKeyboardHIDUsageKeyboardPageDown: return ImGuiKey_PageDown;
	case UIKeyboardHIDUsageKeyboardRightArrow: return ImGuiKey_RightArrow;
	case UIKeyboardHIDUsageKeyboardLeftArrow: return ImGuiKey_LeftArrow;
	case UIKeyboardHIDUsageKeyboardDownArrow: return ImGuiKey_DownArrow;
	case UIKeyboardHIDUsageKeyboardUpArrow: return ImGuiKey_UpArrow;
	case UIKeyboardHIDUsageKeyboardF1: return ImGuiKey_F1;
	case UIKeyboardHIDUsageKeyboardF2: return ImGuiKey_F2;
	case UIKeyboardHIDUsageKeyboardF3: return ImGuiKey_F3;
	case UIKeyboardHIDUsageKeyboardF4: return ImGuiKey_F4;
	case UIKeyboardHIDUsageKeyboardF5: return ImGuiKey_F5;
	case UIKeyboardHIDUsageKeyboardF6: return ImGuiKey_F6;
	case UIKeyboardHIDUsageKeyboardF7: return ImGuiKey_F7;
	case UIKeyboardHIDUsageKeyboardF8: return ImGuiKey_F8;
	case UIKeyboardHIDUsageKeyboardF9: return ImGuiKey_F9;
	case UIKeyboardHIDUsageKeyboardF10: return ImGuiKey_F10;
	case UIKeyboardHIDUsageKeyboardF11: return ImGuiKey_F11;
	case UIKeyboardHIDUsageKeyboardF12: return ImGuiKey_F12;
	default: break;
	}
	return ImGuiKey_None;
}

// The held modifiers into ImGui, the way pc_window_mac.mm's feedModifiers does
// (Shift/Ctrl/Alt/Super; Caps Lock is not a panel control and stays out).
inline void feed_key_modifiers(ImGuiIO &io, UIKeyModifierFlags flags)
{
	io.AddKeyEvent(ImGuiMod_Shift, (flags & UIKeyModifierShift) != 0);
	io.AddKeyEvent(ImGuiMod_Ctrl, (flags & UIKeyModifierControl) != 0);
	io.AddKeyEvent(ImGuiMod_Alt, (flags & UIKeyModifierAlternate) != 0);
	io.AddKeyEvent(ImGuiMod_Super, (flags & UIKeyModifierCommand) != 0);
}

// A hardware key press to the shared app key space (menu.h KEY_F2..F5 and the
// panel characters), or -1 when it is not one. Mirrors window_mac.mm's
// codeForEvent: F2-F5 by usage, then the lowercased character (Return and both
// Deletes are panel buttons), then nothing. No MAC_KEY_FUNCTION_BASE tail:
// that is macOS's encoding for keys the app ignores anyway.
inline int panel_code_from_press(UIPress *press)
{
	if (!press.key)
		return -1;
	switch (press.key.keyCode) {
	case UIKeyboardHIDUsageKeyboardF2: return ui::KEY_F2;
	case UIKeyboardHIDUsageKeyboardF3: return ui::KEY_F3;
	case UIKeyboardHIDUsageKeyboardF4: return ui::KEY_F4;
	case UIKeyboardHIDUsageKeyboardF5: return ui::KEY_F5;
	default: break;
	}
	NSString *chars = [press.key.charactersIgnoringModifiers lowercaseString];
	if ([chars length] >= 1) {
		const unichar c = [chars characterAtIndex:0];
		if (c == '\r' || c == 0x7f || c == 0x08 || (c >= 0x20 && c < 0x7f))
			return (int)c;
	}
	return -1;
}

#endif // S_MU2000_IOS_KEYMAP_H
