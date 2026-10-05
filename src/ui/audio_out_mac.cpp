// license:BSD-3-Clause
//
// The macOS half of Apple audio: the questions the engine cannot answer, and
// nothing else.
//
// The render path - the AudioUnit lifecycle, the render callback, the meters,
// the workgroup, the WAV writer - used to live here and has moved to
// audio_apple.mm, which iOS uses too. What is left is what macOS alone can do:
// enumerate devices by HAL property query, resolve a name to one (whole-name
// first for a menu selection, then a substring, case-insensitively), resize that
// device's buffer, and take it for ourselves in hog mode. Every one of those
// goes through properties on the very AudioUnit AVAudioEngine hands out, so the
// engine could carry the rest - which is why the two files meet at
// ui/audio_apple.h.
//
// The rule from doc/design.md carries over unchanged: **we own no clock**.
// CoreAudio asks for N frames and we make exactly those N.

#include "audio_out.h"
#include "audio_apple.h"
#include "compat/cli_text.h"
#include "hal_mac.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <unistd.h>          // getpid(), for hog mode

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace ui {


// ---- The macOS answers to the shared core (see ui/audio_apple.h) ------------
//
// One function per question ui/audio_apple.mm asks, and no branch anywhere else
// on the platform. Each is the same code this file always ran, moved.

namespace apple {

// No session on macOS: there is no category to pick, nothing to activate and no
// permission to ask - the system default device is chosen by the system, and a
// device that goes away is answered by the HAL rather than by a notification.
// So the honest answer is yes, and the shared core takes the engine from here.
bool session_open(int, std::string &)
{
	return true;
}

std::vector<std::string> output_list()
{
	return hal::names(hal::direction::output);
}

// Which device a name means, and what to call it. Empty means the system
// default. A device that has gone leaves found false, so the caller can say so
// instead of opening whatever is left.
device_ref resolve_output(const std::string &name, bool exact)
{
	device_ref dev;
	dev.id = hal::find_device(name, exact);
	dev.found = dev.id != kAudioObjectUnknown;
	if (dev.found)
		dev.name = hal::name_of(dev.id);
	return dev;
}

// Best effort, as it always was: ask for a buffer matching the requested latency
// and report what the driver took. Zero means the write failed and the core
// falls back to whatever the first block turns out to be.
u32 request_buffer_frames(const device_ref &dev, int latency_ms)
{
	if (dev.id == kAudioObjectUnknown)
		return 0;
	return hal::set_buffer_frames(dev.id, latency_ms);
}

// The same property this file set on a unit of its own, now set on the unit the
// engine hands out. Note what it means, unchanged: the unit is pinned even for
// the system default, so a later change of the system default does not move us.
bool pin_output(AudioUnit unit, const device_ref &dev, std::string &err)
{
	if (unit == nullptr || dev.id == kAudioObjectUnknown)
		return true;
	if (AudioUnitSetProperty(unit, kAudioOutputUnitProperty_CurrentDevice,
	                         kAudioUnitScope_Global, 0, &dev.id, sizeof(dev.id)) != noErr) {
		err = "音声の出口を選べない";
		return false;
	}
	return true;
}

void unpin_output()
{
	// Nothing is remembered here: the pin is a property of the unit, and the
	// unit dies with the engine.
}

// Hog mode, with the same two rules as before. It is claimed after IO has
// started, because claiming first can leave a device that cannot be mixed
// unopenable. And it is given back only when taking it is what got it: a device
// some other process holds is not ours to release.
device_claim take_output(const device_ref &dev, std::string &err)
{
	device_claim claim;
	if (dev.id == kAudioObjectUnknown) {
		err = "音声の出口が見つからない";
		return claim;
	}
	claim.id = dev.id;
	bool took = false;
	if (!hal::take_hog(dev.id, took)) {
		std::fprintf(stderr, "[mac] hog refused: %s\n", dev.name.c_str());
		return claim;
	}
	claim.took = took;
	return claim;
}

void release_output(const device_claim &claim)
{
	if (claim.took && claim.id != kAudioObjectUnknown)
		hal::release_hog(claim.id);
}

// The device's own name, which is what the status line and the menu compare
// against. A device with no name (should not happen) falls back to the rate.
std::string output_label(const device_ref &dev, double rate)
{
	if (dev.id != kAudioObjectUnknown && !dev.name.empty())
		return dev.name;
	char name[128] = {};
	std::snprintf(name, sizeof(name), "%.0f Hz", rate);
	return name;
}

} // namespace apple

// ---- audio_out: the shell the shared core is driven through -----------------
//
// The interface is unchanged and every method is now one line, because the work
// behind it is audio_apple.mm's.

struct audio_out::impl {
	std::unique_ptr<apple_audio_out> core = std::make_unique<apple_audio_out>();
};

audio_out::audio_out()
	: m_impl(std::make_unique<impl>())
{
}

audio_out::~audio_out()
{
	stop();
}

std::vector<std::string> audio_out::list()
{
	return apple::output_list();
}

bool audio_out::start(int latency_ms, fill_fn fill, std::string &err, bool exclusive,
                      const std::string &device, bool raw, bool exact)
{
<<<<<<< HEAD
	(void)raw;                    // nothing to bypass on this side (audio_out.h)
	if (m_impl && m_impl->running.load())
		return true;
	stop();                       // drop any previous attempt

	// The port to open. Looked up first, so a name that matches nothing is
	// reported before anything is opened
	const AudioDeviceID dev = find_device(device, exact);
	if (dev == kAudioObjectUnknown) {
		err = device.empty() ? CLI_T("No audio output found", "音声の出口が見つからない")
		                     : CLI_T("No audio output with that name: ", "その名前の音声の出口が見つからない: ") + device;
		return false;
	}

	auto up = std::make_unique<impl>();
	up->tps  = ticks_per_sec();
	up->fill = std::move(fill);
	up->dev  = dev;
	if (m_capturing)
		up->cap = &m_cap;

	auto dispose = [&](const std::string &why) {
		if (up->unit) {
			AudioUnitUninitialize(up->unit);
			AudioComponentInstanceDispose(up->unit);
			up->unit = nullptr;
		}
		if (up->hog_took)
			release_hog(up->dev);
		err = why;
		return false;
	};

	// Which unit to open. DefaultOutput follows the system's default device, and
	// that is what an ordinary run wants. A named device, or exclusive access,
	// means the device has to be pinned: a DefaultOutput unit lets go of the
	// device as soon as it is hogged (the system moves its default elsewhere)
	AudioComponentDescription desc{};
	desc.componentType         = kAudioUnitType_Output;
	desc.componentSubType      = (!device.empty() || exclusive)
	    ? kAudioUnitSubType_HALOutput : kAudioUnitSubType_DefaultOutput;
	desc.componentManufacturer = kAudioUnitManufacturer_Apple;
	AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
	if (!comp)
		return dispose(CLI_T("No default audio output found", "既定の音声出力が見つからない"));
	if (AudioComponentInstanceNew(comp, &up->unit) != noErr || !up->unit)
		return dispose(CLI_T("Cannot create the AudioUnit", "AudioUnit を作れない"));

	// Open the device that was picked rather than whichever one the system is
	// defaulting to. Set before the format: that is checked against the device
	// that is open
	if (AudioUnitSetProperty(up->unit, kAudioOutputUnitProperty_CurrentDevice,
	                         kAudioUnitScope_Global, 0, &dev, sizeof(dev)) != noErr)
		return dispose(CLI_T("Cannot select the audio output", "音声の出口を選べない"));

	// Ask for the format we generate: 44100Hz, 16bit, stereo, interleaved. The
	// unit converts to whatever the device actually wants
	AudioStreamBasicDescription fmt{};
	fmt.mSampleRate       = AUDIO_RATE;
	fmt.mFormatID         = kAudioFormatLinearPCM;
	fmt.mFormatFlags      = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
	fmt.mFramesPerPacket  = 1;
	fmt.mChannelsPerFrame = 2;
	fmt.mBitsPerChannel   = 16;
	fmt.mBytesPerFrame    = 4;
	fmt.mBytesPerPacket   = 4;
	if (AudioUnitSetProperty(up->unit, kAudioUnitProperty_StreamFormat,
	                         kAudioUnitScope_Input, 0, &fmt, sizeof(fmt)) != noErr)
		return dispose(CLI_T("Cannot set the audio format", "音声の形式を指定できない"));

	AURenderCallbackStruct cb{};
	cb.inputProc       = impl::render_cb;
	cb.inputProcRefCon = up.get();
	if (AudioUnitSetProperty(up->unit, kAudioUnitProperty_SetRenderCallback,
	                         kAudioUnitScope_Input, 0, &cb, sizeof(cb)) != noErr)
		return dispose(CLI_T("Cannot install the audio callback", "音声の呼び出し口を繋げない"));

	// Room for the largest slice we might be asked for in one go
	constexpr u32 MAX_SLICE = 4096;
	AudioUnitSetProperty(up->unit, kAudioUnitProperty_MaximumFramesPerSlice,
	                     kAudioUnitScope_Global, 0, &MAX_SLICE, sizeof(MAX_SLICE));
	up->scratch.resize(size_t(MAX_SLICE) * 2);

	// The device buffer size drives the latency. Without this CoreAudio would use
	// its default (often 512 frames, ~11.6ms)
	u32 buf = set_device_buffer_frames(dev, latency_ms);
	if (!buf)
		buf = 512;
	up->buffer_frames.store(buf);

	if (AudioUnitInitialize(up->unit) != noErr)
		return dispose(CLI_T("Cannot initialise the audio output", "音声を初期化できない"));

	up->realtime.store(true);     // the HAL thread is already real-time
	if (AudioOutputUnitStart(up->unit) != noErr)
		return dispose(CLI_T("Cannot start playback", "再生を開始できない"));

	// Hog mode is claimed *after* IO has started. Apple's notes say a device that
	// cannot be mixed is held by whoever starts its IO first, so taking it
	// beforehand makes the device impossible to open -- and since a refused claim
	// still plays, the answer is reported through exclusive() rather than by
	// refusing to start
	if (exclusive) {
		up->hog_owned = take_hog(dev, up->hog_took);
		// Claiming it changes whether the device can be mixed, so the HAL tears
		// the device's IO down and builds it again. Without this second Start the
		// IO we began would stay stopped
		if (up->hog_owned) {
			AudioOutputUnitStop(up->unit);
			if (AudioOutputUnitStart(up->unit) != noErr)
				return dispose(CLI_T("Cannot start playback", "再生を開始できない"));
		}
	}

	up->running.store(true);
	m_impl = std::move(up);
	return true;
=======
	// raw has no meaning here and never did: the system does the format
	// conversion rather than a driver mixer, so there is nothing to bypass. It
	// is in the signature only so both platforms take the same call.
	(void)raw;
	apple_audio_out::request r;
	r.latency_ms = latency_ms;
	r.device = device;
	r.exact = exact;
	r.exclusive = exclusive;
	return m_impl->core->start(r, std::move(fill), err);
>>>>>>> 9b19041 (mac+ios: one audio render path, shared)
}

void audio_out::stop()
{
	m_impl->core->stop();
}

std::string audio_out::device_name() const
{
	return m_impl->core->device_name();
}

#if defined(__APPLE__)
void *audio_out::realtime_workgroup()
{
	return m_impl->core->realtime_workgroup();
}
#endif

bool audio_out::exclusive() const
{
	return m_impl->core->exclusive();
}

void audio_out::set_capture(const std::string &path)
{
	// The class carries the flag and the path outside impl on purpose (stop()
	// throws impl away, and what was captured has to outlive it), so both are
	// set here as the Linux backend sets them - and the core keeps its own pair,
	// which is the one the render block reads.
	m_cap_path = path;
	m_capturing = !path.empty();
	m_impl->core->set_capture(path);
}

u64 audio_out::capture_frames() const
{
	return m_impl->core->capture_frames();
}

bool audio_out::write_capture(std::string &err)
{
<<<<<<< HEAD
	if (m_cap_path.empty()) {
		err = CLI_T("No output file was given", "書き出す先が決まっていない");
		return false;
	}
	std::FILE *f = std::fopen(m_cap_path.c_str(), "wb");
	if (!f) {
		err = CLI_T("Cannot write: ", "書けない: ") + m_cap_path;
		return false;
	}
	const u32 frames = u32(m_cap.size() / 2);
	write_wav_header(f, frames);
	const std::size_t wrote = m_cap.empty()
	    ? 0 : std::fwrite(m_cap.data(), sizeof(s16), m_cap.size(), f);
	const bool ok = std::fclose(f) == 0 && wrote == m_cap.size();
	if (!ok)
		err = CLI_T("The write ended early: ", "書き込みが途中で終わった: ") + m_cap_path;
	return ok;
=======
	return m_impl->core->write_capture(err);
>>>>>>> 9b19041 (mac+ios: one audio render path, shared)
}

u64 audio_out::produced() const
{
	return m_impl->core->produced();
}

u32 audio_out::buffer_frames() const
{
	return m_impl->core->buffer_frames();
}

u64 audio_out::starved() const
{
	return m_impl->core->starved();
}

bool audio_out::mmcss() const
{
	return m_impl->core->mmcss();
}

double audio_out::cpu_percent() const
{
	return m_impl->core->cpu_percent();
}

double audio_out::worst_ms() const
{
	return m_impl->core->worst_ms();
}

double audio_out::cpu_recent() const
{
	return m_impl->core->cpu_recent();
}

} // namespace ui
