// license:BSD-3-Clause
//
// Audio on iOS: **stubs**, and that is a platform fact rather than an omission.
//
// The interface is right - these are the same audio_out and audio_in classes the Mac
// front end uses, same pimpl, same fill_fn signature - but the inside is empty. That
// is because **iOS does not ship the AudioHardware HAL.** audio_out_mac.cpp (582
// lines) and audio_in_mac.cpp (469) are built on AudioObjectGetPropertyData,
// kAudioHardwarePropertyDevices and AudioObjectFind. CoreAudio.framework exists on iOS
// but carries only three headers (AudioHardwareBase.h, AudioServerPlugIn.h,
// CoreAudioTypes.h) and no umbrella, and the HAL entry points appear in no public
// header - only in the link stub CoreAudio.tbd. There is no public way to enumerate
// or open an audio device on iOS.
//
// iOS requires AVAudioSession and AVAudioEngine (or RemoteIO): choosing an output is
// a session/route question, and the pull model is float32 rather than the interleaved
// s32 fill callback this interface hands over. So the real backend is ~1,050 lines to
// write, not to port - which is why this file is the honest "links, and says why not
// yet" version rather than a fake working one. See doc/ios-auv3.md.
//
// MIDI did not have this problem: CoreMIDI.h is complete on iOS, so midi_in_mac.cpp
// and midi_out_mac.cpp compile unchanged and are linked for real. That asymmetry is
// why only audio is stubbed.
//
// Both impl structs are kept - audio_out::impl carries the fill callback and the
// atomics the accessors read, and audio_in::impl the ring - so that the moment an
// AVAudioEngine backend lands, it fills these in rather than reshaping the interface.
// Every accessor above them is already inline in the headers; only the out-of-line
// declarations are defined here.

#include "ui/audio_in.h"
#include "ui/audio_out.h"

#include <atomic>
#include <cstdio>
#include <vector>

namespace ui {

// Said once, from the first call a UI would make. The device menus call list() on
// every right-click and the status line asks on every frame, so this would otherwise
// be one line per frame.
static void note_once()
{
	static bool said = false;
	if (said)
		return;
	said = true;
	std::fprintf(stderr,
	             "[ios] audio: no device backend yet. iOS has no AudioHardware HAL, so "
	             "audio_out_mac.cpp cannot be ported; this needs AVAudioSession + "
	             "AVAudioEngine.\n");
}

// ---- audio_out --------------------------------------------------------------
//
// The shape audio_out_mac.cpp's impl has, minus everything that needs the HAL: no
// AudioUnit, no AudioDeviceID, no render callback and its trampoline. What is kept
// (fill, the atomics) is what the accessors in the header read, so a backend can
// drop in without the interface noticing.

struct audio_out::impl
{
	fill_fn fill = nullptr;

	std::atomic<bool> running{false};
	std::atomic<u32>  buffer_frames{0};
	std::atomic<u64>  produced{0}, starved{0};
	std::atomic<bool> realtime{false};   // macOS: the CoreAudio callback is RT by nature
	double            tps = 1.0;

	// Where a capture would be appended; owned by audio_out so it survives stop().
	// Always null: no backend, so set_capture has nothing to feed.
	std::vector<s16> *cap = nullptr;
};

audio_out::audio_out()
	: m_impl(std::make_unique<impl>())
{
	note_once();
}

// No devices, because there is no HAL to ask. An empty list is what the picker
// expects when nothing is pluggable, so the menu shows empty rather than lying.
std::vector<std::string> audio_out::list()
{
	note_once();
	return {};
}

bool audio_out::start(int, fill_fn fill, std::string &err, bool, const std::string &, bool, bool)
{
	note_once();
	m_impl->fill = fill;
	err = "no audio backend on iOS yet (needs AVAudioEngine)";
	return false;
}

std::string audio_out::device_name() const
{
	return {};
}

bool audio_out::exclusive() const
{
	return false;
}

#if defined(__APPLE__)
// A real-time workgroup does not exist here. os/workgroup.h does compile on iOS, but
// there is no CoreAudio render callback to boost, because there is no callback at all
// yet; null is what audio_out.h says to expect when unavailable.
void *audio_out::realtime_workgroup()
{
	return nullptr;
}
#endif

void audio_out::set_capture(const std::string &)
{
}

u64 audio_out::capture_frames() const
{
	return 0;
}

bool audio_out::write_capture(std::string &)
{
	return false;
}

void audio_out::stop()
{
	if (m_impl)
		m_impl->running = false;
}

// The progress accessors. These are declared rather than inline in the Apple block
// of audio_out.h (unlike buffer_frames(), produced() on audio_in and friends, which
// are inline), because they read the impl's atomics rather than audio_out's own
// members. With no backend they all report zero / false, which is the truth: nothing
// is producing and nothing is late.
u64 audio_out::produced() const
{
	return m_impl ? m_impl->produced.load() : 0;
}

u64 audio_out::starved() const
{
	return m_impl ? m_impl->starved.load() : 0;
}

bool audio_out::mmcss() const
{
	return m_impl ? m_impl->realtime.load() : false;
}

double audio_out::cpu_percent() const
{
	return 0.0;
}

double audio_out::worst_ms() const
{
	return 0.0;
}

// ---- audio_in ---------------------------------------------------------------
//
// The shape audio_in_mac.cpp's impl has: the ring pop() reads, without the input
// AudioUnit that would fill it.

struct audio_in::impl
{
	std::atomic<bool> running{false};
	std::atomic<u64>  empty_count{0};
	std::vector<s16>  ring;   // empty, and stays empty: no backend pushes into it
};

audio_in::audio_in()
	: m_impl(std::make_unique<impl>())
{
	note_once();
}

std::vector<std::string> audio_in::list()
{
	note_once();
	return {};
}

bool audio_in::start(const std::string &, std::string &err)
{
	note_once();
	err = "no audio input on iOS yet (needs AVAudioEngine + RemoteIO)";
	return false;
}

void audio_in::stop()
{
	if (m_impl)
		m_impl->running = false;
}

void audio_in::pop(s32 &l, s32 &r)
{
	// Silence while there is nothing to read. audio_in.h says pop() returns 0 while
	// the ring is empty, so this is the documented empty case, not a special one.
	l = 0;
	r = 0;
}

} // namespace ui