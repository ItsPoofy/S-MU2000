// license:BSD-3-Clause
//
// CoreAudio output for macOS. Same interface as audio_out.cpp (WASAPI).
//
// The rule from doc/design.md carries over unchanged: **we own no clock**.
// CoreAudio asks for N frames and we make exactly those N. An output AudioUnit
// expresses that directly, so this file is mostly plumbing. There is no worker
// thread here because CoreAudio already calls us on its real-time HAL thread.

#include "audio_out.h"
#include "compat/cli_text.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <mach/mach_time.h>
#include <os/workgroup.h>
#include <unistd.h>          // getpid(), for hog mode

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace ui {

namespace {

// mach_absolute_time ticks to seconds. 24MHz on Apple silicon, but read the
// timebase rather than assuming anything
double ticks_per_sec()
{
	mach_timebase_info_data_t tb;
	mach_timebase_info(&tb);
	return 1e9 * double(tb.denom) / double(tb.numer);
}

AudioDeviceID default_output_device()
{
	// AudioObjectPropertyAddress is { selector, scope, element } in that order
	AudioObjectPropertyAddress addr = {
		kAudioHardwarePropertyDefaultOutputDevice,
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMain
	};
	AudioDeviceID dev = kAudioObjectUnknown;
	UInt32 size = sizeof(dev);
	if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr,
	                               &size, &dev) != noErr)
		return kAudioObjectUnknown;
	return dev;
}

// Does this device have anything to play through? The device list also holds
// input-only devices, and offering those as an output would be a lie
bool has_output(AudioDeviceID dev)
{
	AudioObjectPropertyAddress addr = {
		kAudioDevicePropertyStreams,
		kAudioObjectPropertyScopeOutput,
		kAudioObjectPropertyElementMain
	};
	UInt32 size = 0;
	if (AudioObjectGetPropertyDataSize(dev, &addr, 0, nullptr, &size) != noErr)
		return false;
	return size >= sizeof(AudioStreamID);
}

// Named rather than called device_name(), which would collide with the member
// function of the same name wherever one is in scope
std::string name_of(AudioDeviceID dev)
{
	AudioObjectPropertyAddress addr = {
		kAudioObjectPropertyName,
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMain
	};
	CFStringRef name = nullptr;
	UInt32 size = sizeof(name);
	if (AudioObjectGetPropertyData(dev, &addr, 0, nullptr, &size, &name) != noErr || !name)
		return {};
	char buf[256] = {};
	const bool ok = CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8);
	CFRelease(name);
	return ok ? std::string(buf) : std::string();
}

std::vector<AudioDeviceID> output_devices()
{
	AudioObjectPropertyAddress addr = {
		kAudioHardwarePropertyDevices,
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMain
	};
	UInt32 size = 0;
	if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, nullptr, &size) != noErr)
		return {};
	std::vector<AudioDeviceID> devs(size / sizeof(AudioDeviceID));
	if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size,
	                               devs.data()) != noErr)
		return {};
	devs.erase(std::remove_if(devs.begin(), devs.end(),
	                          [](AudioDeviceID d) { return !has_output(d); }),
	           devs.end());
	return devs;
}

std::string lowered(const std::string &s)
{
	std::string out;
	out.reserve(s.size());
	for (char c : s)
		out.push_back(char(std::tolower((unsigned char)c)));
	return out;
}

// The output whose name contains `want`, or the system default when nothing was
// asked for. Matching a part of a name is what the Windows side does, and it is
// what --audio documents
AudioDeviceID find_device(const std::string &want, bool exact)
{
	if (want.empty())
		return default_output_device();

	const std::string needle = lowered(want);
	const auto devices = output_devices();
	for (int pass = 0; pass < (exact ? 1 : 2); pass++)
	for (AudioDeviceID d : devices) {
		const std::string name = lowered(name_of(d));
		if (pass == 0 ? name == needle : name.find(needle) != std::string::npos)
			return d;
	}
	return kAudioObjectUnknown;
}

AudioObjectPropertyAddress hog_address()
{
	AudioObjectPropertyAddress addr = {
		kAudioDevicePropertyHogMode,
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMain
	};
	return addr;
}

// Exclusive access to the device -- this platform's counterpart of WASAPI's
// exclusive mode.
//
// **The property is a toggle and the value written is ignored.** Apple's rule:
// holding it is released when we write, somebody else holding it is left alone,
// and writing while nobody holds it takes it. So it is read first. That matters
// here because a device that cannot mix is handed to whoever starts IO on it:
// by the time this is called we may already have it, and writing would give it
// away -- which stops the IO we just started.
//
// `took_it` says whether the write is what got it, and so whether it has to be
// written again to hand it back
bool take_hog(AudioDeviceID dev, bool &took_it)
{
	took_it = false;
	if (dev == kAudioObjectUnknown)
		return false;
	const pid_t me = getpid();
	AudioObjectPropertyAddress addr = hog_address();

	pid_t holder = -1;
	UInt32 size = sizeof(holder);
	if (AudioObjectGetPropertyData(dev, &addr, 0, nullptr, &size, &holder) != noErr)
		return false;
	if (holder == me)
		return true;
	if (holder != -1)
		return false;          // another process owns it; writing would change nothing

	pid_t value = me;          // ignored on the way in, the new owner on the way out
	if (AudioObjectSetPropertyData(dev, &addr, 0, nullptr, sizeof(value), &value) != noErr)
		return false;
	if (value != me)
		return false;
	took_it = true;
	return true;
}

// Hand it back. Only for a device this process took by writing: one it holds
// because it started IO is released when that IO stops
void release_hog(AudioDeviceID dev)
{
	if (dev == kAudioObjectUnknown)
		return;
	pid_t value = -1;
	AudioObjectPropertyAddress addr = hog_address();
	AudioObjectSetPropertyData(dev, &addr, 0, nullptr, sizeof(value), &value);
}

// A 44 byte canonical WAV header, 16bit 2ch. The same shape live's --wav writes
void write_wav_header(std::FILE *f, u32 frames, u32 rate, u32 channels)
{
	const u32 data = frames * channels * 2;
	const u32 riff = 36 + data;
	const u16 ch = u16(channels), bits = 16, align = u16(channels * 2);
	const u32 bytes = rate * align;
	std::fwrite("RIFF", 1, 4, f);
	std::fwrite(&riff, 4, 1, f);
	std::fwrite("WAVEfmt ", 1, 8, f);
	const u32 fmt_size = 16;
	const u16 pcm = 1;
	std::fwrite(&fmt_size, 4, 1, f);
	std::fwrite(&pcm, 2, 1, f);
	std::fwrite(&ch, 2, 1, f);
	std::fwrite(&rate, 4, 1, f);
	std::fwrite(&bytes, 4, 1, f);
	std::fwrite(&align, 2, 1, f);
	std::fwrite(&bits, 2, 1, f);
	std::fwrite("data", 1, 4, f);
	std::fwrite(&data, 4, 1, f);
}

// Best effort: ask the device for a buffer matching the requested latency.
// CoreAudio only accepts a value inside the device's range, and it may round, so
// read back what we actually got. Returning 0 means "leave it at the default".
u32 set_device_buffer_frames(AudioDeviceID dev, int latency_ms, u32 requested, u32 rate)
{
	AudioObjectPropertyAddress addr = {
		kAudioDevicePropertyBufferFrameSize,
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMain
	};

	if (!requested && latency_ms <= 0) {
		UInt32 got = 0, size = sizeof(got);
		return AudioObjectGetPropertyData(dev, &addr, 0, nullptr, &size, &got) == noErr ? got : 0;
	}
	UInt32 wanted = requested ? requested : UInt32((u64(rate) * u64(latency_ms > 0 ? latency_ms : 0)) / 1000);
	if (wanted < 32)
		wanted = 32;

	AudioValueRange range{};
	UInt32 size = sizeof(range);
	addr.mSelector = kAudioDevicePropertyBufferFrameSizeRange;
	if (AudioObjectGetPropertyData(dev, &addr, 0, nullptr, &size, &range) == noErr) {
		if (wanted < UInt32(range.mMinimum)) wanted = UInt32(range.mMinimum);
		if (wanted > UInt32(range.mMaximum)) wanted = UInt32(range.mMaximum);
	}

	addr.mSelector = kAudioDevicePropertyBufferFrameSize;
	UInt32 got = wanted;
	size = sizeof(got);
	const auto status = AudioObjectSetPropertyData(dev, &addr, 0, nullptr, size, &wanted);
	if (AudioObjectGetPropertyData(dev, &addr, 0, nullptr, &size, &got) != noErr)
		return status == noErr ? wanted : 0;
	return got;
}

} // namespace

struct audio_out::impl
{
	AudioUnit unit = nullptr;
	fill_fn   fill;
	audio_stream_renderer renderer;
	audio_stream_options stream;
	u32 rate = AUDIO_RATE, channels = 2;
	bool software_conversion = false;
	AudioDeviceID dev = kAudioObjectUnknown;
	bool      hog_owned = false;   // we have the device to ourselves
	bool      hog_took = false;    // and taking it is what got it, so we give it back

	// Where the render callback appends what it is about to hand over. Owned by
	// audio_out, not by impl, so that it survives stop(); null when --dump-dev was
	// not asked for
	std::vector<s16> *cap = nullptr;

	std::atomic<bool> running{false};
	std::atomic<u32>  buffer_frames{0};
	std::atomic<u64>  produced{0}, starved{0};
	std::atomic<u64>  busy_ticks{0}, worst_ticks{0};
	cpu_meter         meter;        // recent load for the display (issue #80)
	std::atomic<bool> realtime{false};
	double            tps = 1.0;

	// Scratch for the (unusual) non-interleaved case. Allocated up front so the
	// render callback itself never allocates
	std::vector<s16> scratch;

	// The callback has to be a member: the nested impl is private, so a free
	// function could not name it to cast the refCon
	static OSStatus render_cb(void *ref, AudioUnitRenderActionFlags *, const AudioTimeStamp *,
	                          UInt32, UInt32 frames, AudioBufferList *io)
	{
		auto *self = static_cast<impl *>(ref);
		if (self)
			self->render(frames, io);
		return noErr;
	}

	void render(UInt32 frames, AudioBufferList *io)
	{
		if (!fill)
			return;

		const u64 t0 = mach_absolute_time();

		s16 *interleaved = nullptr;
		if (size_t(frames) * channels > scratch.size()) {
			for (UInt32 b = 0; b < io->mNumberBuffers; b++)
				if (io->mBuffers[b].mData) std::memset(io->mBuffers[b].mData, 0, io->mBuffers[b].mDataByteSize);
			starved.fetch_add(1);
			return;
		}
		interleaved = io->mNumberBuffers == 1 ? static_cast<s16 *>(io->mBuffers[0].mData) : scratch.data();
		if (!interleaved) return;
		if (software_conversion)
			renderer.render(interleaved, frames, channels, stream.left, stream.right, fill, audio_stream_renderer::pcm16);
		else
			fill(interleaved, frames); // original 44.1 kHz stereo; CoreAudio converts
		if (io->mNumberBuffers > 1) {
			unsigned ch = 0;
			for (UInt32 b = 0; b < io->mNumberBuffers; b++) {
				const unsigned count = io->mBuffers[b].mNumberChannels;
				auto *dst = static_cast<s16 *>(io->mBuffers[b].mData);
				if (dst) for (UInt32 i = 0; i < frames; i++)
					for (unsigned c = 0; c < count; c++)
						dst[size_t(i) * count + c] = ch + c < channels ? interleaved[size_t(i) * channels + ch + c] : 0;
				ch += count;
			}
		}
		// --dump-dev. Growing the buffer in the callback is allocation on the
		// real-time thread, which is a compromise -- but it is a diagnosis aid,
		// and live's own --wav already does the same thing for the same reason
		if (cap && interleaved)
			cap->insert(cap->end(), interleaved, interleaved + size_t(frames) * channels);

		const u64 took = mach_absolute_time() - t0;
		busy_ticks.fetch_add(took, std::memory_order_relaxed);
		meter.add(double(took) / tps, double(frames) / rate);
		u64 worst = worst_ticks.load(std::memory_order_relaxed);
		while (took > worst &&
		       !worst_ticks.compare_exchange_weak(worst, took, std::memory_order_relaxed)) {
		}
		produced.fetch_add(frames, std::memory_order_relaxed);

		// CoreAudio has no "frames still queued" number the way WASAPI does, so
		// an underrun cannot be read off directly. Use the same proxy live was
		// already tracking: a fill that took longer than the block it was making
		// means the device would have run dry.
		if (double(took) / tps > double(frames) / rate)
			starved.fetch_add(1, std::memory_order_relaxed);
	}
};

std::vector<std::string> audio_out::list()
{
	std::vector<std::string> names;
	for (AudioDeviceID d : output_devices()) {
		const std::string n = name_of(d);
		if (!n.empty())
			names.push_back(n);
	}
	return names;
}

std::string audio_out::default_device_name() { return name_of(default_output_device()); }

audio_out::audio_out() = default;

audio_out::~audio_out()
{
	stop();
}bool audio_out::start(int latency_ms, fill_fn fill, std::string &err, bool exclusive,
                      const std::string &device, bool raw, bool exact)
{
	if (!valid_audio_request(m_stream)) { err = "Invalid audio stream settings"; return false; }
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

	AudioStreamBasicDescription hardware{};
	UInt32 hardware_size = sizeof(hardware);
	if (AudioUnitGetProperty(up->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output,
	                         0, &hardware, &hardware_size) != noErr)
		return dispose("Cannot read the device stream format");
	m_info = {};
	for (UInt32 c = 0; c < hardware.mChannelsPerFrame; c++)
		m_info.channels.push_back("Output " + std::to_string(c + 1));
	AudioObjectPropertyAddress rate_addr = { kAudioDevicePropertyAvailableNominalSampleRates,
	    kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
	UInt32 range_size = 0;
	std::vector<AudioValueRange> ranges;
	if (AudioObjectGetPropertyDataSize(dev, &rate_addr, 0, nullptr, &range_size) == noErr) {
		ranges.resize(range_size / sizeof(AudioValueRange));
		if (AudioObjectGetPropertyData(dev, &rate_addr, 0, nullptr, &range_size, ranges.data()) != noErr) ranges.clear();
	}
	for (int rate : { 8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000, 176400, 192000 })
		for (const auto &range : ranges)
			if (rate >= range.mMinimum && rate <= range.mMaximum) { m_info.rates.push_back(rate); break; }
	if (m_info.rates.empty()) m_info.rates.push_back(int(hardware.mSampleRate));
	if (!valid_audio_route(m_stream, hardware.mChannelsPerFrame)) return dispose("The selected output channels are unavailable");
	up->software_conversion = custom_audio_format(m_stream);
	up->rate = up->software_conversion ? (m_stream.sample_rate ? u32(m_stream.sample_rate) : u32(hardware.mSampleRate)) : AUDIO_RATE;
	up->channels = up->software_conversion ? hardware.mChannelsPerFrame : 2;
	up->stream = m_stream;
	if (up->software_conversion) up->renderer.configure(int(up->rate), m_stream.quality);
	m_info.rate = int(up->rate);
	m_info.buffer_rate = int(hardware.mSampleRate);
	m_capture_rate = up->rate;
	m_capture_channels = up->channels;
	// Auto/default routing keeps the original client format and conversion path.
	AudioStreamBasicDescription fmt{};
	fmt.mSampleRate       = up->rate;
	fmt.mFormatID         = kAudioFormatLinearPCM;
	fmt.mFormatFlags      = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
	fmt.mFramesPerPacket  = 1;
	fmt.mChannelsPerFrame = up->channels;
	fmt.mBitsPerChannel   = 16;
	fmt.mBytesPerFrame    = up->channels * 2;
	fmt.mBytesPerPacket   = up->channels * 2;
	if (AudioUnitSetProperty(up->unit, kAudioUnitProperty_StreamFormat,
	                         kAudioUnitScope_Input, 0, &fmt, sizeof(fmt)) != noErr)
		return dispose(CLI_T("Cannot set the audio format", "音声の形式を指定できない"));

	AURenderCallbackStruct cb{};
	cb.inputProc       = impl::render_cb;
	cb.inputProcRefCon = up.get();
	if (AudioUnitSetProperty(up->unit, kAudioUnitProperty_SetRenderCallback,
	                         kAudioUnitScope_Input, 0, &cb, sizeof(cb)) != noErr)
		return dispose(CLI_T("Cannot install the audio callback", "音声の呼び出し口を繋げない"));

	// The device buffer size drives the latency. Without this CoreAudio would use
	// its default (often 512 frames, ~11.6ms)
	u32 buf = set_device_buffer_frames(dev, latency_ms, u32(m_stream.buffer_frames), u32(hardware.mSampleRate));
	if (!buf)
		buf = 512;
	up->buffer_frames.store(buf);

	// Client-rate conversion can enlarge a hardware slice. Allocate before IO.
	UInt32 max_slice = std::max(4096u, UInt32(std::ceil(buf * double(up->rate) / hardware.mSampleRate)));
	AudioUnitSetProperty(up->unit, kAudioUnitProperty_MaximumFramesPerSlice,
	                     kAudioUnitScope_Global, 0, &max_slice, sizeof(max_slice));
	UInt32 slice_size = sizeof(max_slice);
	AudioUnitGetProperty(up->unit, kAudioUnitProperty_MaximumFramesPerSlice,
	                     kAudioUnitScope_Global, 0, &max_slice, &slice_size);
	up->scratch.resize(size_t(max_slice) * up->channels);


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
		if (!up->hog_owned && m_stream.strict) {
			AudioOutputUnitStop(up->unit);
			return dispose("The device refused exclusive access");
		}
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
}

void audio_out::stop()
{
	if (!m_impl)
		return;
	if (m_impl->unit) {
		// After Stop + Uninitialize no render callback is in flight, so the impl
		// (and the fill function it holds) can be torn down safely
		AudioOutputUnitStop(m_impl->unit);
		AudioUnitUninitialize(m_impl->unit);
		AudioComponentInstanceDispose(m_impl->unit);
		m_impl->unit = nullptr;
	}
	// Hand the device back before the impl goes away
	if (m_impl->hog_took) {
		release_hog(m_impl->dev);
		m_impl->hog_took = false;
	}
	m_impl->running.store(false);
	m_impl.reset();
}

std::string audio_out::device_name() const
{
	return m_impl ? name_of(m_impl->dev) : std::string();
}

// The HAL output unit's audio workgroup, for a parallel render thread to
// join (Apple's parallel real-time threads pattern). Null when the unit is
// down or the property is unavailable.
void *audio_out::realtime_workgroup()
{
	if (!m_impl || !m_impl->unit)
		return nullptr;
	if (__builtin_available(macOS 11.0, *)) {
		os_workgroup_t wg = nullptr;
		UInt32 size = sizeof(wg);
		if (AudioUnitGetProperty(m_impl->unit, kAudioOutputUnitProperty_OSWorkgroup,
		                         kAudioUnitScope_Global, 0, &wg, &size) == noErr)
			return wg;
	}
	return nullptr;
}

bool audio_out::exclusive() const
{
	return m_impl && m_impl->hog_owned;
}

void audio_out::set_capture(const std::string &path)
{
	m_cap_path  = path;
	m_capturing = !path.empty();
	m_cap.clear();
}

bool audio_out::running() const { return m_impl && m_impl->running.load(); }

u64 audio_out::capture_frames() const
{
	return u64(m_cap.size() / m_capture_channels);
}

bool audio_out::write_capture(std::string &err)
{
	if (m_cap_path.empty()) {
		err = CLI_T("No output file was given", "書き出す先が決まっていない");
		return false;
	}
	std::FILE *f = std::fopen(m_cap_path.c_str(), "wb");
	if (!f) {
		err = CLI_T("Cannot write: ", "書けない: ") + m_cap_path;
		return false;
	}
	const u32 frames = u32(m_cap.size() / m_capture_channels);
	write_wav_header(f, frames, m_capture_rate, m_capture_channels);
	const std::size_t wrote = m_cap.empty()
	    ? 0 : std::fwrite(m_cap.data(), sizeof(s16), m_cap.size(), f);
	const bool ok = std::fclose(f) == 0 && wrote == m_cap.size();
	if (!ok)
		err = CLI_T("The write ended early: ", "書き込みが途中で終わった: ") + m_cap_path;
	return ok;
}

u32 audio_out::buffer_frames() const { return m_impl ? m_impl->buffer_frames.load() : 0; }
u64 audio_out::produced() const      { return m_impl ? m_impl->produced.load() : 0; }
u64 audio_out::starved() const       { return m_impl ? m_impl->starved.load() : 0; }
bool audio_out::mmcss() const        { return m_impl && m_impl->realtime.load(); }

double audio_out::cpu_percent() const
{
	if (!m_impl)
		return 0.0;
	const u64 done = m_impl->produced.load();
	if (!done)
		return 0.0;
	const double audio = double(done) / m_impl->rate;
	const double busy  = double(m_impl->busy_ticks.load()) / m_impl->tps;
	return 100.0 * busy / audio;
}

double audio_out::cpu_recent() const
{
	return m_impl ? m_impl->meter.value() : 0.0;
}

double audio_out::worst_ms() const
{
	if (!m_impl)
		return 0.0;
	return 1000.0 * double(m_impl->worst_ticks.load()) / m_impl->tps;
}

} // namespace ui
