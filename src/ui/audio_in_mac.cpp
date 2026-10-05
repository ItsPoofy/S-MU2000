// license:BSD-3-Clause
//
// The macOS half of Apple audio, recording side: the questions the engine
// cannot answer, and nothing else.
//
// The input path that used to live here - its own AudioUnit, its own input
// callback, the float-to-s16 conversion, the ring and the resampler - is
// audio_apple.mm now, shared with iOS, which taps an input node to get the same
// buffers. What is left is the HAL: which devices can record, which one a
// remembered name means, and the three properties that pin the unit to it.
//
// The queries themselves are in ui/hal_mac.h, because the playback and
// recording halves used to carry byte-identical copies of them.

#include "audio_in.h"
#include "audio_out.h"          // AUDIO_RATE, shared with the output side
#include "audio_apple.h"
#include "hal_mac.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace ui {

// ---- The macOS answers to the shared core (see ui/audio_apple.h) ------------

namespace apple {

// Nothing to ask: macOS has no per-app microphone permission, and no session to
// move into a recording category.
bool input_permission(std::string &)
{
	return true;
}

bool session_open_input(std::string &)
{
	return true;
}

std::vector<std::string> input_list()
{
	return hal::names(hal::direction::input);
}

// The same rule as the output side: empty means the system default, and exact
// (a menu selection) accepts only a whole name so a device that has gone cannot
// quietly become another one.
device_ref resolve_input(const std::string &name, bool exact)
{
	device_ref dev;
	dev.id = hal::find_device(name, exact, hal::direction::input);
	dev.found = dev.id != kAudioObjectUnknown;
	if (dev.found)
		dev.name = hal::name_of(dev.id);
	return dev;
}

// The input bus on, the output bus off, then the device - the same three
// properties in the same order as the backend this replaces, set on the unit
// AVAudioEngine hands out instead of one of our own. Without the first two the
// unit would try to play as well as record.
bool pin_input(AudioUnit unit, const device_ref &dev, std::string &err)
{
	if (unit == nullptr || dev.id == kAudioObjectUnknown)
		return true;
	UInt32 on = 1, off = 0;
	if (AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO,
	                         kAudioUnitScope_Input, 1, &on, sizeof(on)) != noErr ||
	    AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO,
	                         kAudioUnitScope_Output, 0, &off, sizeof(off)) != noErr ||
	    AudioUnitSetProperty(unit, kAudioOutputUnitProperty_CurrentDevice,
	                         kAudioUnitScope_Global, 0, &dev.id, sizeof(dev.id)) != noErr) {
		err = "録音デバイスを選べない";
		return false;
	}
	return true;
}

// The line the front ends print under the input device. The channel count is
// the device's own, as it always was, rather than the two channels of the
// format the shared core asks the tap for.
std::string input_label(const device_ref &dev, double rate, u32 channels)
{
	const u32 ch = dev.id != kAudioObjectUnknown ? hal::input_channels(dev.id) : channels;
	char line[160] = {};
	std::snprintf(line, sizeof line, "CoreAudio / %.0f Hz %u ch float32 \xe2\x86\x92 44100 Hz",
	              rate, ch);
	return line;
}

} // namespace apple

// ---- audio_in: the shell the shared core is driven through ------------------
//
// The interface is unchanged and every method is one line, because the work
// behind it is audio_apple.mm's.

struct audio_in::impl {
	std::unique_ptr<apple_audio_in> core = std::make_unique<apple_audio_in>();
};

audio_in::audio_in()
	: m_impl(std::make_unique<impl>())
{
}

audio_in::~audio_in()
{
	stop();
}

std::vector<std::string> audio_in::list()
{
	return apple::input_list();
}

bool audio_in::start(const std::string &device, std::string &err)
{
	return m_impl->core->start(device, err);
}

void audio_in::stop()
{
	m_impl->core->stop();
}

bool audio_in::running() const
{
	return m_impl->core->running();
}

void audio_in::pop(s32 &l, s32 &r)
{
	m_impl->core->pop(l, r);
}

std::string audio_in::device_name() const
{
	return m_impl->core->device_name();
}

std::string audio_in::format_line() const
{
	return m_impl->core->format_line();
}

u64 audio_in::empty_count() const
{
	return m_impl->core->empty_count();
}

u64 audio_in::dropped_count() const
{
	return m_impl->core->dropped_count();
}

} // namespace ui