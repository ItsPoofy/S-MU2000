// license:BSD-3-Clause
//
// Audio out (and the list() half of audio in) on iOS: AVAudioSession +
// AVAudioEngine, written from scratch against the same class interfaces.
//
// Why from scratch: iOS ships no public AudioHardware HAL - AudioObject* appears
// in no header, only in CoreAudio.tbd - so audio_out_mac.cpp (device enumeration
// by property query, hog mode, raw mode) cannot be ported. On iOS choosing output
// is a session route, not a property: one route at a time, described by
// AVAudioSession.currentRoute, and the app plays through it. There is no device
// menu to fill with anything else, which is why list() reports the route.
//
// What this is: an AVAudioSourceNode whose render block calls the stored fill_fn
// (16-bit stereo interleaved at 44100 Hz, the same contract as everywhere) and
// converts to the float the engine pulls. The engine is asked for 44100 Hz and
// usually grants it; when it does not, ui::resampler bridges the gap instead of
// playing at the wrong speed.
//
// Why the engine and not a raw RemoteIO output unit: both are public API on iOS
// (kAudioUnitSubType_RemoteIO, SetRenderCallback, AudioOutputUnitStart/Stop and
// kAudioOutputUnitProperty_OSWorkgroup are all declared - verified, not assumed),
// and RemoteIO would give two things this does not: the unit handle to read the
// workgroup from (so the slave could join the device group, as on macOS) and an
// s16 stream format with no float conversion. What it costs is everything the
// engine does for free: route changes, interruptions, and node format conversion.
// While output sounds clean there is no evidence the workgroup matters, so the
// engine stays; if dropouts ever appear that worst_ms cannot explain, RemoteIO
// is the documented fallback and this paragraph is its shopping list.
//
// Threading matches the desktop shape: the render block runs on the engine's
// real-time thread and calls eng->fill through the stored lambda, which takes the
// same card_lock the display-link pump takes - so the two pumpers are mutually
// excluded rather than racing. The pump (gui_app::pump_realtime) stands down once
// produced() goes non-zero, so overlap is one handoff blip, not a second path.
//
// Lifetime matches it too: the block captures the impl pointer raw, the way the
// mac render callback takes a refCon. impl is owned by audio_out, which the app
// keeps in a static (app_ios.h's make_audio, like app_mac.cpp) and never destroys,
// so the block cannot outlive it. stop() tears the engine down first anyway.

// AVFAudio directly rather than through AVFoundation's re-export: this file needs
// the session, the engine and the source node, and all three live here. Verified
// against the iPhoneSimulator SDK headers (AVAudioSourceNode.h, AVAudioEngine.h,
// AVAudioSession.h) rather than remembered - the render block puts frameCount
// before the buffer list, initWithRealtimeSafeRenderBlock needs iOS 27 (we floor
// at 17), and connect:to:fromBus:toBus:format:error: likewise, so this uses the
// error-less connect the 17.0 target allows.
#import <AVFAudio/AVFAudio.h>
#import <Foundation/Foundation.h>

#include "ui/audio_in.h"
#include "ui/audio_out.h"
#include "ui/resampler.h"

#include <mach/mach_time.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

namespace ui {

// Mach ticks per second, for the cpu_percent/worst_ms the status line reads.
// Resolved once: the timebase does not change under a process.
static double mach_tps()
{
	static double tps = 0.0;
	if (tps == 0.0) {
		mach_timebase_info_data_t tb{};
		mach_timebase_info(&tb);
		tps = 1e9 * double(tb.denom) / double(tb.numer);
	}
	return tps;
}

static void zero_buffers(AudioBufferList *abl)
{
	if (!abl)
		return;
	for (UInt32 i = 0; i < abl->mNumberBuffers; i++) {
		if (abl->mBuffers[i].mData)
			std::memset(abl->mBuffers[i].mData, 0, abl->mBuffers[i].mDataByteSize);
	}
}

struct audio_out::impl {
	fill_fn fill = nullptr;

	AVAudioEngine *engine = nil;
	AVAudioSourceNode *src = nil;

	ui::resampler resamp;
	std::vector<s16> scratch;   // fill target at 44100 Hz; grown, never shrunk

	std::atomic<bool> running{false};
	std::atomic<u32> buffer_frames{0};
	std::atomic<u64> produced{0}, starved{0};
	std::atomic<u64> busy_ticks{0}, worst_ticks{0};

	double dev_rate = double(AUDIO_RATE);
	std::string dev_name;

	// Where a capture would be appended; owned by audio_out so it survives
	// stop(). Always null: --dump-dev capture is not wired on iOS yet.
	std::vector<s16> *cap = nullptr;
};

audio_out::audio_out()
	: m_impl(std::make_unique<impl>())
{
	m_impl->scratch.resize(size_t(AUDIO_RATE) / 10 * 2);   // 100 ms headroom
}

audio_out::~audio_out()
{
	stop();
}

// The one route, by its port name ("iPhone Speaker", "AirPods", ...). Empty when
// nothing is attached, which the picker shows as empty rather than lying - the
// same answer the stub gave, now for a real reason.
std::vector<std::string> audio_out::list()
{
	AVAudioSession *s = [AVAudioSession sharedInstance];
	AVAudioSessionRouteDescription *route = [s currentRoute];
	std::vector<std::string> names;
	for (AVAudioSessionPortDescription *p in [route outputs]) {
		const char *n = [[p portName] UTF8String];
		if (n)
			names.emplace_back(n);
	}
	return names;
}

bool audio_out::start(int latency_ms, fill_fn fill, std::string &err, bool exclusive,
                      const std::string &device, bool raw, bool exact)
{
	// exclusive asks for hog mode, raw bypasses the system mixer, device/exact
	// pick among devices: none of those exist on iOS (one route, always mixed),
	// so all four are accepted and ignored rather than failing the open.
	(void)exclusive;
	(void)device;
	(void)raw;
	(void)exact;
	if (m_impl->running)
		stop();
	m_impl->fill = fill;

	AVAudioSession *s = [AVAudioSession sharedInstance];
	NSError *e = nil;
	if (![s setCategory:AVAudioSessionCategoryPlayback error:&e]) {
		err = std::string("AVAudioSession category: ") +
		      (e ? [[e localizedDescription] UTF8String] : "?");
		return false;
	}
	// Asked, not promised: the session may grant another rate (hardware at 48k),
	// and the resampler below covers the difference instead of failing.
	if (![s setPreferredSampleRate:double(AUDIO_RATE) error:&e])
		std::fprintf(stderr, "[ios] audio: 44100 Hz refused, taking what comes\n");
	if (latency_ms > 0 &&
	    ![s setPreferredIOBufferDuration:double(latency_ms) / 1000.0 error:&e])
		std::fprintf(stderr, "[ios] audio: buffer duration refused\n");
	if (![s setActive:YES error:&e]) {
		err = std::string("AVAudioSession activate: ") +
		      (e ? [[e localizedDescription] UTF8String] : "?");
		return false;
	}

	double rate = [s sampleRate];
	if (!(rate > 0.0))
		rate = double(AUDIO_RATE);
	m_impl->dev_rate = rate;
	m_impl->resamp.configure(double(AUDIO_RATE), rate);
	if (!m_impl->resamp.direct())
		std::fprintf(stderr, "[ios] audio: resampling %.0f -> %.0f Hz\n",
		             double(AUDIO_RATE), rate);

	AVAudioEngine *engine = [[AVAudioEngine alloc] init];
	audio_out::impl *im = m_impl.get();
	AVAudioSourceNode *src = [[AVAudioSourceNode alloc]
		initWithRenderBlock:^OSStatus(BOOL *isSilence, const AudioTimeStamp *ts,
		                              AVAudioFrameCount n, AudioBufferList *abl) {
			(void)ts;
			im->buffer_frames.store(n, std::memory_order_relaxed);
			if (!abl || abl->mNumberBuffers < 1) {
				if (isSilence)
					*isSilence = YES;
				return noErr;
			}
			if (!im->running.load(std::memory_order_acquire) || !im->fill) {
				zero_buffers(abl);
				if (isSilence)
					*isSilence = YES;
				return noErr;
			}
			const u64 t0 = mach_absolute_time();
			// The device wants n frames at its own rate; the machine makes them
			// at 44100. input_needed() says how many machine frames that takes,
			// so one fill() covers the callback exactly - no drift, no stash.
			const int want = im->resamp.direct() ? int(n) : im->resamp.input_needed(int(n));
			if (want > 0) {
				if (im->scratch.size() < size_t(want) * 2)
					im->scratch.resize(size_t(want) * 2);
				im->fill(im->scratch.data(), u32(want));
			}
			float *f = static_cast<float *>(abl->mBuffers[0].mData);
			const u32 floats = abl->mBuffers[0].mDataByteSize / sizeof(float);
			if (f && floats >= n * 2) {
				if (im->resamp.direct()) {
					const s16 *sv = im->scratch.data();
					for (u32 i = 0; i < n * 2; i++)
						f[i] = float(sv[i]) * (1.0f / 32768.0f);
				} else {
					im->resamp.push(im->scratch.data(), want);
					im->resamp.pull(f, int(n));
				}
			} else {
				zero_buffers(abl);
			}
			const u64 t1 = mach_absolute_time();
			const u64 busy = t1 - t0;
			im->busy_ticks.fetch_add(busy, std::memory_order_relaxed);
			u64 worst = im->worst_ticks.load(std::memory_order_relaxed);
			while (busy > worst &&
			       !im->worst_ticks.compare_exchange_weak(worst, busy,
			                                             std::memory_order_relaxed)) {
			}
			im->produced.fetch_add(n, std::memory_order_relaxed);
			if (isSilence)
				*isSilence = NO;
			return noErr;
		}];
	// Interleaved float32 stereo: the block above writes one buffer of LRLR, so
	// the connection format says so rather than converting behind our back.
	AVAudioFormat *fmt = [[AVAudioFormat alloc] initWithCommonFormat:AVAudioPCMFormatFloat32
	                                                      sampleRate:rate
	                                                        channels:2
	                                                     interleaved:YES];
	if (!fmt) {
		err = "cannot describe float32 stereo";
		return false;
	}
	[engine attachNode:src];
	[engine connect:src to:[engine outputNode] fromBus:0 toBus:0 format:fmt];
	if (![engine startAndReturnError:&e]) {
		err = std::string("AVAudioEngine start: ") +
		      (e ? [[e localizedDescription] UTF8String] : "?");
		return false;
	}

	m_impl->engine = engine;
	m_impl->src = src;
	{
		char name[128] = {};
		std::snprintf(name, sizeof(name), "iOS %.0f Hz", rate);
		m_impl->dev_name = name;
	}
	m_impl->running.store(true, std::memory_order_release);
	std::fprintf(stderr, "[ios] audio: %s\n", m_impl->dev_name.c_str());
	return true;
}

std::string audio_out::device_name() const
{
	return m_impl->dev_name;
}

bool audio_out::exclusive() const
{
	return false;
}

#if defined(__APPLE__)
// No workgroup: AVAudioEngine owns its real-time threads and offers no handle
// for ours. Null is what the header says to expect when unavailable.
void *audio_out::realtime_workgroup()
{
	return nullptr;
}
#endif

void audio_out::set_capture(const std::string &path)
{
	// Same convention as the mac and Linux backends: the flag says capture was
	// asked for, so m_capturing is live bookkeeping rather than a dead member.
	// Nothing is recorded yet (the render block does not append), so
	// write_capture below still refuses - flag without capture, honestly so.
	m_cap_path = path;
	m_capturing = !path.empty();
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
	if (!m_impl->running.exchange(false))
		return;
	// Stop first so no new block enters, then release: a block already inside
	// still holds the raw impl pointer, the same shape as the mac refCon. The app
	// keeps this object in a static and never destroys it mid-render, so the
	// window is theoretical - but stop-before-release is what keeps it so.
	AVAudioEngine *engine = m_impl->engine;
	m_impl->src = nil;
	m_impl->engine = nil;
	if (engine)
		[engine stop];
	m_impl->fill = nullptr;
}

u64 audio_out::produced() const
{
	return m_impl->produced.load(std::memory_order_relaxed);
}

u32 audio_out::buffer_frames() const
{
	return m_impl->buffer_frames.load(std::memory_order_relaxed);
}

u64 audio_out::starved() const
{
	return m_impl->starved.load(std::memory_order_relaxed);
}

bool audio_out::mmcss() const
{
	// The engine's render thread is real-time by construction - the counterpart
	// of registering with MMCSS on Windows, with nothing to register.
	return m_impl->running.load(std::memory_order_relaxed);
}

double audio_out::cpu_percent() const
{
	const u64 busy = m_impl->busy_ticks.load(std::memory_order_relaxed);
	const u64 prod = m_impl->produced.load(std::memory_order_relaxed);
	if (prod == 0)
		return 0.0;
	// Fraction of one device-rate second spent rendering, as a percent.
	return 100.0 * double(busy) / (mach_tps() * double(prod) / m_impl->dev_rate);
}

double audio_out::worst_ms() const
{
	return 1000.0 * double(m_impl->worst_ticks.load(std::memory_order_relaxed)) /
	       mach_tps();
}

// ---- audio_in ---------------------------------------------------------------
//
// Input stays a stub: RemoteIO recording is the next piece after output proves
// the session and the engine, and ui::app only needs these symbols to link.

struct audio_in::impl {
	std::atomic<bool> running{false};
	std::atomic<u64> empty_count{0};
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
	return {};
}

bool audio_in::start(const std::string &, std::string &err)
{
	err = "no audio input on iOS yet (needs AVAudioEngine + RemoteIO)";
	return false;
}

void audio_in::stop()
{
	if (m_impl)
		m_impl->running = false;
}

bool audio_in::running() const
{
	return m_impl->running.load();
}

std::string audio_in::device_name() const
{
	return {};
}

std::string audio_in::format_line() const
{
	return {};
}

u64 audio_in::empty_count() const
{
	return m_impl->empty_count.load();
}

u64 audio_in::dropped_count() const
{
	// Nothing is recorded, so nothing is ever dropped for lack of room.
	return 0;
}

void audio_in::pop(s32 &l, s32 &r)
{
	// Silence while there is nothing to read: audio_in.h documents 0 while the
	// ring is empty, so this is the documented empty case, not a special one.
	l = 0;
	r = 0;
}

} // namespace ui