// license:BSD-3-Clause
//
// The iOS audio back end.
//
// AVAudioEngine, because iOS has no other way to reach the device: it ships
// CoreAudio.framework with three headers (AudioHardwareBase.h,
// AudioServerPlugIn.h, CoreAudioTypes.h), no umbrella, and
// AudioObjectGetPropertyData in no header at all - only in the CoreAudio.tbd link
// stub. So the AudioHardware HAL that src/ui/audio_out_mac.cpp and
// src/ui/audio_in_mac.cpp are built on cannot be used here, and neither can
// RemoteIO, which offers no way to ask the system which device to use. The
// engine's nodes do hand out the underlying AudioUnit (AVAudioIONode.audioUnit),
// which is where the workgroup read comes from.
//
// The engine, the source node, the render block, the resampler, the meters, the
// capture and the workgroup read are all here, with audio_out's and audio_in's
// own methods beside the code they forward to. What is not here is the short list
// of questions the engine cannot answer by itself - which route is live, what may
// we record, which input is available - declared at the bottom of this file and
// answered in audio_out_ios.mm and audio_in_ios.mm, one per direction as on every
// other platform.

#ifndef S_MU2000_UI_AUDIO_CORE_IOS_H
#define S_MU2000_UI_AUDIO_CORE_IOS_H

#include "ui/audio_out.h"

#include <AudioToolbox/AudioToolbox.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ui {

// The output half: an AVAudioSourceNode whose render block calls fill (44100 Hz
// s16 stereo interleaved, the contract everywhere) and converts to the float
// the engine pulls. It also owns the workgroup handle, so the parallel slave
// thread joins the group the device itself is in.
class ios_audio_out {
public:
	using fill_fn = audio_out::fill_fn;

	// What was asked for. An empty device means the route the system is on,
	// which is the whole of what can be asked for.
	struct request {
		int       latency_ms = 0;
		std::string device;
		bool      exact = false;     // a menu name must match a whole name
		bool      exclusive = false; // no counterpart on iOS, see take_output
	};

	ios_audio_out();
	~ios_audio_out();

	bool start(const request &r, fill_fn fill, std::string &err);
	void stop();

	// A route change or an interruption stops the engine and nothing resumes
	// it; the platform's observer calls this. The nodes and the render block
	// survive, so this is a restart and not a rebuild.
	void restart();

	bool running() const;
	const std::string &device_name() const;
	bool exclusive() const;
	void *realtime_workgroup();

	// What we hand the unit, written as a WAV: what the machine made, before
	// any format conversion, which is what a capture means.
	void set_capture(const std::string &path);
	u64 capture_frames() const;
	bool write_capture(std::string &err);

	u64 produced() const;
	u32 buffer_frames() const;
	u64 starved() const;
	bool mmcss() const;
	double cpu_percent() const;
	double worst_ms() const;
	double cpu_recent() const;

	ios_audio_out(const ios_audio_out &) = delete;
	ios_audio_out &operator=(const ios_audio_out &) = delete;

private:
	struct impl;
	std::unique_ptr<impl> m;
	// Outside impl so the capture survives stop(), the way audio_out's does.
	std::vector<s16> m_cap;
	std::string m_cap_path;
};

// The input half: the machine's A/D INPUT. The engine's input node with a tap,
// into the same ring and resampler as everywhere else (44100 Hz s16 stereo, a
// 50 ms target, dropped past 200 ms). The two clocks drift - the device runs at
// its own rate and the machine at 44100 - so the ring is the elastic part between
// them, exactly as audio_in.h describes.
//
// A second engine, on purpose: stopping output must not stop recording, and the
// reverse. The one thing both halves share is the AVAudioSession underneath them.
class ios_audio_in {
public:
	ios_audio_in();
	~ios_audio_in();

	// An empty name means the first input available, the default rule of every
	// backend here; otherwise it is a port the session offers, by name.
	bool start(const std::string &device, std::string &err);
	void stop();

	// A route change or an interruption stops the input engine too.
	void restart();

	bool running() const;
	const std::string &device_name() const;
	std::string format_line() const;
	u64 empty_count() const;
	u64 dropped_count() const;

	// The machine's next sample pair. Silent when there is nothing queued, which
	// is counted: a synth that never asks for input must not look starved.
	void pop(s32 &l, s32 &r);

	ios_audio_in(const ios_audio_in &) = delete;
	ios_audio_in &operator=(const ios_audio_in &) = delete;

private:
	struct impl;
	std::unique_ptr<impl> m;
};

// ---- The questions the engine cannot answer ---------------------------------
//
// One function per question, implemented in audio_out_ios.mm and
// audio_in_ios.mm; nothing above this line knows what a route or a port is.
namespace ios_audio {

// A device a request means. Resolved once and then passed around, so the buffer
// size, the pin and the label all describe the same thing rather than three
// independent guesses at what "AirPods" means a second later.
struct device_ref {
	// There is no handle to keep: the route is chosen by the session and the
	// engine follows it, so there is nothing to hold on to and nothing to look
	// up later. AudioDeviceID is not even declared on iOS, which is the whole
	// reason this type exists rather than a bare string.
	UInt32      id = 0;          // unused here, kept for the shape
	std::string name;            // what to show for it
	bool        found = true;    // false when a name was given and nothing matches
};

// What a device claim is: whether taking the device is what got it, in which case
// it has to be given back. Nothing can be taken on iOS, so `took` is always
// false - see take_output().
struct device_claim {
	UInt32 id = 0;
	bool   took = false;
};

// The session for output: category, sample rate, IO buffer duration, activated.
// There is deliberately no session_close(): deactivating belongs to whoever else
// shares the session (the input half does), and a half-answer would only invite
// the bug.
bool session_open(int latency_ms, std::string &err);

// What can be played through, by name, for the device menu: the session's current
// route, which is the only choice the system offers.
std::vector<std::string> output_list();

// Which device a name means: the route, whatever it is called, since there is
// only ever the one.
device_ref resolve_output(const std::string &name, bool exact);

// Ask for a buffer of about latency_ms and report what was granted. The session
// was already asked in session_open(), and the device underneath is not ours to
// resize, so 0 - which the core reads as "the platform decides".
u32 request_buffer_frames(const device_ref &dev, int latency_ms);

// Point the output at that device. There is nothing to do: the session chose the
// route and the unit follows it.
bool pin_output(AudioUnit unit, const device_ref &dev, std::string &err);
void unpin_output();

// Take the device for ourselves, so nothing else can play through it. No
// counterpart here: nothing can share the route through us, and the system mixer
// is not ours to take over. So nothing is ever given back either.
device_claim take_output(const device_ref &dev, std::string &err);
void release_output(const device_claim &claim);

// The name to show for what was opened: the route, at the rate it runs at.
std::string output_label(const device_ref &dev, double rate);

// ---- The session watchers --------------------------------------------------
//
// The engine stops itself when the route changes or a call arrives, and nothing
// restarts it, so each half asks to be told and calls its own restart() from the
// callback. AVAudioSession owns the observers and calls back only while the
// function is set - pass an empty one to stop being called, which is what stop()
// does. It is a function rather than a token the caller holds because the
// session is a process-wide object: keeping the tokens per device would mean a
// callback outliving the core it points at.
void watch_output_session(const std::function<void()> &on_change);
void watch_input_session(const std::function<void()> &on_change);

// ---- The input side, same questions ----------------------------------------

// Devices that can be recorded from, for the menu: the session's available
// inputs, which is hardware capability rather than the current route (the route's
// inputs are empty until a session category asks for them).
std::vector<std::string> input_list();

// May we record? Asks the microphone permission here and only here, so a launch
// that never records never prompts; an undetermined answer fails this pick with
// "pick again", a refusal says where to re-allow.
bool input_permission(std::string &err);

// The session, for recording: PlayAndRecord with DefaultToSpeaker (without that
// the speaker goes quiet and sound moves to the earpiece the moment recording
// starts), then activate.
bool session_open_input(std::string &err);

// Which device a name means, the same rule as the output side.
device_ref resolve_input(const std::string &name, bool exact);

// Point recording at that device: ask the session for that port.
bool pin_input(AudioUnit unit, const device_ref &dev, std::string &err);

// The line the front end prints under the input device.
std::string input_label(const device_ref &dev, double rate, u32 channels);

} // namespace ios_audio

} // namespace ui

#endif // S_MU2000_UI_AUDIO_CORE_IOS_H