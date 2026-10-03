// license:BSD-3-Clause
//
// The iOS app class. Same shape as ui/app_mac.h and ui/app_win.h: one window
// file (ui/window_ios.mm), one app class, one main (src/ios/smoke.mm).
//
// Sixteen pure virtuals, and this file is where the iOS answers come from.
// Most of them are stubbed with a plain reason rather than implemented, because
// step 1 of the port is pixels only: no audio device, no file dialogs. Each stub
// says what it would do when that step arrives, so the list reads as the
// remaining work rather than as unfinished business.

#ifndef S_MU2000_UI_APP_IOS_H
#define S_MU2000_UI_APP_IOS_H

#pragma once

#include <cstdio>
#include <string>

#include "app.h"

namespace ui {

// The iOS sandbox, which is where a Mac-style ini would go. Kept beside the
// other settings paths so nothing has to learn a second convention.
std::string settings_file_path();

class gui_app : public app
{
public:
	// mi is MIDI IN A-D, mu2000::MIDI_PORTS of them
	gui_app(bridge &b, midi_in *mi,
	        midi_out &tha, midi_out &thb, midi_out &muo)
	    : app(b, mi, tha, thb, muo) {}

	// ---- settings

	std::string settings_path() const override { return settings_file_path(); }

	// ---- audio
	//
	// Nothing yet, on purpose: make_audio is called from the boot thread once
	// the firmware is up, and wiring AVAudioSession + a CoreAudio output unit is
	// the next step, not this one. audio_out_mac.cpp is the reference and is
	// nearly reusable - mach_absolute_time and os/workgroup.h both exist on iOS
	// (verified against the iPhoneSimulator SDK).

	void make_audio() override {}
	void say_audio_opened(bool) override {}
	void say_audio_running() override {}
	u64 audio_drops() override { return 0; }
	void print_audio_details() override {}
	bool open_main_window(const char *, int, int) override { return true; }

	// The panel window is a UIView in window_ios.mm, and UIKit drives it from
	// CADisplayLink, so this hands control back to the run loop rather than
	// pumping in a loop like the mac and Windows shells do. Returning here is
	// correct: CADisplayLink then calls back on its own.
	void pump_window(const char *, int, int) override {}

	// ---- the status line's middle fragment
	//
	// Shared code calls this only when out && out->produced(), so with no audio
	// device it never runs. The count is still reported rather than left blank,
	// because a zero reads as "measured, nothing dropped" and is misleading.
	void format_middle(char *dst, std::size_t n) override
	{
		std::snprintf(dst, n, UI_TEXT(status_middle_mac_fmt, "late %llu"),
		              (unsigned long long)(out ? out->starved() : 0));
	}

	// Advances the machine by one display frame's worth of samples
	// (AUDIO_RATE/30 at 30 Hz tick = real time), discarding the audio.
	//
	// Without this the machine freezes one step past boot: boot() only runs until
	// mu.midi_ready(), and on desktop the audio callback keeps calling run_sample
	// forever after. The LCD then shows whatever was last drawn (起動中...) because
	// the main-screen redraw happens in firmware ticks that never execute. The
	// display link calls this before every paint, so the panel shows a living
	// machine rather than a paused one.
	//
	// Only once booted (state == 1): running samples through an unbooted machine is
	// harmless but pointless, and on a missing-ROM launch eng exists without ever
	// having booted.
	//
	// All on the main thread today: boot ran here, the tick runs here, and there is
	// no audio thread yet. When AVAudioEngine lands this moves into its fill
	// callback - same samples, same rate, different thread - and this method goes
	// away rather than being kept as a second path.
	void pump_realtime()
	{
		if (!eng || !state || state->load() != 1)
			return;
		s32 l, r;
		for (u32 i = 0; i < AUDIO_RATE / 30; i++)
			eng->mu.run_sample(l, r);
	}

	// ---- dialogs: UIAlertController and UIDocumentPickerViewController
	//
	// Not yet. Each returns empty, which the shared code reads as "cancelled",
	// so a menu item behaves rather than misbehaving until the port reaches it.

	void menu_error(const std::string &) override {}
	void menu_note(const std::string &) override {}
	std::string ask_card_open_path() override { return std::string(); }
	std::string ask_card_save_path() override { return std::string(); }
	std::string ask_midi_file_path() override { return std::string(); }
	bool confirm_factory_reset() override { return false; }

	// An editor window comes up, or says why it could not. The five PC editors
	// each want their own window; iOS gives them one UIViewController presented
	// over the panel, so this opens the window and reports the failure in the log
	// (there is no alert yet - menu_error is the stub above).
	void open_pc_window(pc_window &w) override
	{
		std::string err;
		if (!w.show(err))
			std::fprintf(stderr, "Cannot open: %s\n", err.c_str());
	}
};

extern gui_app *g_gui;                  // set once main has made the app

} // namespace ui

#endif // S_MU2000_UI_APP_IOS_H