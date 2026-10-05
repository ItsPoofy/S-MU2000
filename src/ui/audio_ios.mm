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
// (kAudioUnitSubType_RemoteIO, SetRenderCallback and AudioOutputUnitStart/Stop
// are all declared - verified, not assumed), and RemoteIO would give this two
// things: an s16 stream format with no float conversion, and control over the
// unit. What it costs is everything the engine does for free: route changes,
// interruptions, and node format conversion. The workgroup argument is gone -
// it used to be the tie-breaker, on the assumption that only a unit we own
// could be asked for one; AVAudioIONode.audioUnit turns out to answer it (see
// realtime_workgroup() below), so the engine keeps the lot and we still get the
// group. If dropouts ever appear that worst_ms cannot explain, RemoteIO is
// still the documented fallback, for the s16 path.
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
#include "ui/cpu_meter.h"
#include "ui/resampler.h"

#include <mach/mach_time.h>

#include <algorithm>
#include <atomic>
#include <cmath>
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
	cpu_meter meter;   // recent load for the display (issue #80, shared helper)

	// When the worst spike happened, and how many there were. A single 40 ms
	// callback is four times a 512-frame buffer, so the question is not whether
	// it cracks but what stalls it: cold pages on first touch of the ROM data,
	// the card_lock shared with the display-link pump, or the compiler. The
	// index and the clock are two stores on a path that already does six.
	std::atomic<u64> worst_index{0};
	std::atomic<double> worst_at{0.0};
	std::atomic<u64> spikes{0};          // callbacks over 20 ms
	std::atomic<u64> started_ms{0};
	std::atomic<u64> callbacks{0};

	// Route-change / interruption observers, registered once. Without these
	// the first category flip (picking the mic) or the first headphone plug
	// silences output until restart: AVAudioEngine stops itself on route
	// changes and interruptions and waits to be started again.
	id route_obs = nil, interrupt_obs = nil;
	bool observing = false;

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
			im->meter.add(double(busy) / mach_tps(), double(n) / im->dev_rate);
			const u64 index = im->callbacks.fetch_add(1, std::memory_order_relaxed) + 1;
			if (const double ms = 1000.0 * double(busy) / mach_tps(); ms > 20.0)
				im->spikes.fetch_add(1, std::memory_order_relaxed);
			u64 worst = im->worst_ticks.load(std::memory_order_relaxed);
			while (busy > worst &&
			       !im->worst_ticks.compare_exchange_weak(worst, busy,
			                                             std::memory_order_relaxed)) {
			}
			// Which callback was the worst, and when: printed on stop, so a
			// one-off (cold pages, first touch of the ROM data) can be told
			// apart from a stall that recurs.
			if (busy >= worst) {
				im->worst_index.store(index, std::memory_order_relaxed);
				const double t0 = double(mach_absolute_time());
				im->worst_at.store(
				    (t0 - double(im->started_ms.load(std::memory_order_relaxed))) /
				        mach_tps() * 1000.0,
				    std::memory_order_relaxed);
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
	// (Re)start what was running across route changes (category flips,
	// headphone plug/unplug) and interruptions (call, Siri): the engine stops
	// itself on both and never resumes alone. The render block and nodes
	// survive stop/start, so this is a restart, not a rebuild.
	if (!m_impl->observing) {
		m_impl->observing = true;
		audio_out::impl *restart = m_impl.get();
		m_impl->route_obs = [[NSNotificationCenter defaultCenter]
		    addObserverForName:AVAudioSessionRouteChangeNotification
		                object:nil
		                 queue:nil
		            usingBlock:^(NSNotification *note) {
			            (void)note;
			            if (restart->running.load(std::memory_order_acquire) &&
			                restart->engine) {
				            [restart->engine stop];
				            NSError *e = nil;
				            if (![restart->engine startAndReturnError:&e])
					            std::fprintf(stderr, "[ios] audio: restart failed: %s\n",
					                         e ? [[e localizedDescription] UTF8String] : "?");
			            }
		            }];
		m_impl->interrupt_obs = [[NSNotificationCenter defaultCenter]
		    addObserverForName:AVAudioSessionInterruptionNotification
		                object:nil
		                 queue:nil
		            usingBlock:^(NSNotification *note) {
			            NSNumber *type = note.userInfo[AVAudioSessionInterruptionTypeKey];
			            NSNumber *opts = note.userInfo[AVAudioSessionInterruptionOptionKey];
			            if (type && type.unsignedIntegerValue == AVAudioSessionInterruptionTypeEnded &&
			                opts && (opts.unsignedIntegerValue &
			                         AVAudioSessionInterruptionOptionShouldResume) &&
			                restart->running.load(std::memory_order_acquire) &&
			                restart->engine) {
				            NSError *e = nil;
				            if (![restart->engine startAndReturnError:&e])
					            std::fprintf(stderr, "[ios] audio: resume failed: %s\n",
					                         e ? [[e localizedDescription] UTF8String] : "?");
			            }
		            }];
	}
	{
		char name[128] = {};
		std::snprintf(name, sizeof(name), "iOS %.0f Hz", rate);
		m_impl->dev_name = name;
	}
	m_impl->running.store(true, std::memory_order_release);
	m_impl->callbacks.store(0, std::memory_order_relaxed);
	m_impl->spikes.store(0, std::memory_order_relaxed);
	m_impl->started_ms.store(uint64_t(mach_absolute_time() * 1000.0 / mach_tps()),
	                        std::memory_order_relaxed);
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
// The device-owned audio workgroup, for the parallel slave thread to join
// (Apple's parallel real-time threads pattern; the join itself is in
// compat/realtime.h). AVAudioIONode hands out the unit the engine renders
// through, and off that unit this is the same property audio_out_mac.cpp
// reads - AUAudioUnit.osWorkgroup is bridged to it. The group belongs to the
// device, not to the unit, so there is one whichever way the output was
// opened: RemoteIO was never a requirement for it.
//
// Null until the engine is running, which is when the front end asks.
void *audio_out::realtime_workgroup()
{
	if (!m_impl || !m_impl->engine || !m_impl->engine.isRunning)
		return nullptr;
	// +0: the C getter's contract, so unretained. The group is owned by the
	// device under the unit, not by the engine, which is what lets this be
	// stored as a bare handle and outlive a stop/start of the engine.
	__unsafe_unretained os_workgroup_t wg = nullptr;
	UInt32 size = sizeof(wg);
	if (AudioUnitGetProperty(m_impl->engine.outputNode.audioUnit,
	                         kAudioOutputUnitProperty_OSWorkgroup,
	                         kAudioUnitScope_Global, 0, &wg, &size) == noErr)
		return (__bridge void *)wg;
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
	// What the worst spike was, and when: a 40 ms callback is four times a
	// 512-frame buffer, so this line is where the crack gets explained (or not).
	const u64 calls = m_impl->callbacks.load(std::memory_order_relaxed);
	if (calls) {
		const double worst_ms = 1000.0 * double(m_impl->worst_ticks.load(std::memory_order_relaxed)) /
		                        mach_tps();
		std::fprintf(stderr,
		             "[ios] audio: %llu callbacks, worst %.1f ms at #%llu (t=%.1fs), "
		             "%llu over 20 ms\n",
		             (unsigned long long)calls, worst_ms,
		             (unsigned long long)m_impl->worst_index.load(std::memory_order_relaxed),
		             m_impl->worst_at.load(std::memory_order_relaxed) / 1000.0,
		             (unsigned long long)m_impl->spikes.load(std::memory_order_relaxed));
	}
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

double audio_out::cpu_recent() const
{
	return m_impl->meter.value();
}

// ---- audio_in ---------------------------------------------------------------
//
// Recording through the same engine shape as output: AVAudioEngine's input
// node with a tap, into the same ring/resampler/pop contract as the mac HAL
// backend (44100 Hz s16 stereo, 50 ms target, drop past 200 ms). No RemoteIO
// needed: the tap converts to interleaved float32 stereo at the hardware
// rate, and ui::resampler covers the rest exactly like everywhere else.
//
// Four iOS-only facts, all in start():
// - The microphone needs permission (NSMicrophoneUsageDescription in the
//   plist). Undetermined asks and fails this pick with "pick again"; denied
//   fails with where to re-allow. The menu's list() asks early (fire and
//   forget) so the prompt is usually answered before the pick.
// - Input needs the PlayAndRecord category; output runs Playback. Switching
//   re-routes output to the earpiece unless DefaultToSpeaker is set, so it
//   is - otherwise picking the mic silences the speaker. The switch can
//   blip output; that is the OS re-routing, not a bug here.
// - Names are route input ports ("Built-in Microphone", ...); empty means
//   the first one, the same default rule as the mac backend.
// - The input engine is separate from the output engine: stopping output
//   must not kill recording and vice versa.

struct audio_in::impl {
	static constexpr u32 RING = 1 << 16, MASK = RING - 1;      // 約 1.5 秒
	static constexpr u32 TARGET_FRAMES = 2205;                  // 50ms
	static constexpr u32 DROP_FRAMES = 8820;                    // 200ms を超えたら捨てる

	AVAudioEngine *engine = nil;

	resampler rs;
	std::vector<s16> staging;    // tap frames as s16 stereo (the resampler's input)
	std::vector<float> conv;
	std::vector<s16> out16;

	std::vector<s16> m_ring = std::vector<s16>(size_t(RING) * 2);
	std::atomic<u32> m_w{0}, m_r{0};
	std::atomic<u64> m_empty{0}, m_dropped{0};
	std::atomic<bool> running{false};

	std::string dev_name, fmt_line;
	double dev_rate = double(AUDIO_RATE);

	// Route changes stop the input engine too (mic unplug, category flips);
	// restart it the same way as output.
	id route_obs = nil;
	bool observing = false;

	// 44100Hz 16bit 2ch を輪に積む。溢れる分は捨てる (same as the mac backend:
	// the tap outruns the synth's pop when the machine is busy).
	void push(const s16 *frames, u32 n)
	{
		u32 wr = m_w.load(std::memory_order_relaxed);
		for (u32 i = 0; i < n; i++) {
			const u32 rd = m_r.load(std::memory_order_acquire);
			if (((wr + 1) & MASK) == rd)
				break;
			m_ring[wr * 2] = frames[i * 2];
			m_ring[wr * 2 + 1] = frames[i * 2 + 1];
			wr = (wr + 1) & MASK;
			m_w.store(wr, std::memory_order_release);
		}
	}
};

audio_in::audio_in()
	: m_impl(std::make_unique<impl>())
{
}

audio_in::~audio_in()
{
	stop();
}

// The available inputs, by port name - not the current route's inputs, which
// are empty while the session is Playback (output started at boot, before any
// input was ever picked). availableInputs is category-independent hardware
// capability, so the mic lists with no permission and no session change.
// Nothing connects here: permission, category and capture all wait for an
// explicit pick in start().
std::vector<std::string> audio_in::list()
{
	NSArray<AVAudioSessionPortDescription *> *inputs =
	    [[AVAudioSession sharedInstance] availableInputs];
	std::vector<std::string> names;
	for (AVAudioSessionPortDescription *p in inputs) {
		const char *n = [[p portName] UTF8String];
		if (n)
			names.emplace_back(n);
	}
	return names;
}

bool audio_in::start(const std::string &device, std::string &err)
{
	stop();
	const std::vector<std::string> names = list();
	std::string want = device;
	if (want.empty()) {
		if (names.empty()) {
			err = "録音デバイスが無い";
			return false;
		}
		want = names[0];
	} else if (std::find(names.begin(), names.end(), want) == names.end()) {
		err = "その名前の録音デバイスは無い";
		return false;
	}

	const AVAudioApplicationRecordPermission perm =
	    AVAudioApplication.sharedInstance.recordPermission;
	if (perm != AVAudioApplicationRecordPermissionGranted) {
		if (perm == AVAudioApplicationRecordPermissionUndetermined)
			[AVAudioApplication requestRecordPermissionWithCompletionHandler:^(BOOL granted) {
				(void)granted;
			}];
		err = (perm == AVAudioApplicationRecordPermissionDenied)
		    ? "マイクが拒否されている（設定アプリで許可）"
		    : "マイクの許可を求めた。もう一度選ぶ";
		return false;
	}

	AVAudioSession *s = [AVAudioSession sharedInstance];
	NSError *e = nil;
	// PlayAndRecord for input; DefaultToSpeaker keeps output where Playback
	// had it (without this the speaker goes quiet and sound moves to the
	// earpiece the moment recording starts).
	if (![s setCategory:AVAudioSessionCategoryPlayAndRecord
	          withOptions:AVAudioSessionCategoryOptionDefaultToSpeaker
	                error:&e]) {
		err = std::string("AVAudioSession category: ") +
		      (e ? [[e localizedDescription] UTF8String] : "?");
		return false;
	}
	if (![s setActive:YES error:&e]) {
		err = std::string("AVAudioSession activate: ") +
		      (e ? [[e localizedDescription] UTF8String] : "?");
		return false;
	}

	AVAudioEngine *engine = [[AVAudioEngine alloc] init];
	AVAudioInputNode *node = [engine inputNode];
	const double rate = [[node outputFormatForBus:0] sampleRate];
	if (!(rate > 0.0)) {
		err = "録音の形式が読めない";
		return false;
	}
	m_impl->dev_rate = rate;
	m_impl->rs.configure(rate, double(AUDIO_RATE));
	// Interleaved float32 stereo at the hardware rate: the tap converts
	// whatever the hardware offers (usually mono), so the block below always
	// sees LRLR and folds nothing itself.
	AVAudioFormat *tap = [[AVAudioFormat alloc] initWithCommonFormat:AVAudioPCMFormatFloat32
	                                                      sampleRate:rate
	                                                        channels:2
	                                                     interleaved:YES];
	if (!tap) {
		err = "録音の形式を決められない";
		return false;
	}
	audio_in::impl *im = m_impl.get();
	[node installTapOnBus:0 bufferSize:1024 format:tap
	                block:^(AVAudioPCMBuffer *buf, AVAudioTime *when) {
		                (void)when;
		                if (!buf || buf.frameLength == 0)
			                return;
		                const UInt32 n = buf.frameLength;
		                const float *f = buf.floatChannelData ? buf.floatChannelData[0] : nullptr;
		                if (!f)
			                return;
		                im->staging.resize(size_t(n) * 2);
		                for (UInt32 i = 0; i < n; i++) {
			                for (u32 c = 0; c < 2; c++) {
				                const float v = f[size_t(i) * 2 + c];
				                im->staging[size_t(i) * 2 + c] =
				                    s16(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
			                }
		                }
		                if (im->rs.direct()) {
			                im->push(im->staging.data(), n);
			                return;
		                }
		                for (UInt32 at = 0; at < n;) {
			                const UInt32 k = std::min<UInt32>(1024, n - at);
			                im->rs.push(im->staging.data() + size_t(at) * 2, int(k));
			                at += k;
			                const int m = im->rs.output_available();
			                if (m <= 0)
				                continue;
			                im->conv.resize(size_t(m) * 2);
			                im->out16.resize(size_t(m) * 2);
			                im->rs.pull(im->conv.data(), m);
			                for (size_t j = 0; j < im->out16.size(); j++)
				                im->out16[j] = s16(std::lround(
				                    std::clamp(im->conv[j], -1.0f, 1.0f) * 32767.0f));
			                im->push(im->out16.data(), u32(m));
		                }
	                }];
	if (![engine startAndReturnError:&e]) {
		[node removeTapOnBus:0];
		err = std::string("AVAudioEngine input start: ") +
		      (e ? [[e localizedDescription] UTF8String] : "?");
		return false;
	}

	m_impl->engine = engine;
	m_impl->dev_name = want;
	if (!m_impl->observing) {
		m_impl->observing = true;
		audio_in::impl *restart = m_impl.get();
		m_impl->route_obs = [[NSNotificationCenter defaultCenter]
		    addObserverForName:AVAudioSessionRouteChangeNotification
		                object:nil
		                 queue:nil
		            usingBlock:^(NSNotification *note) {
			            (void)note;
			            if (restart->running.load(std::memory_order_acquire) &&
			                restart->engine) {
				            [restart->engine stop];
				            NSError *e = nil;
				            if (![restart->engine startAndReturnError:&e])
					            std::fprintf(stderr, "[ios] audio in: restart failed: %s\n",
					                         e ? [[e localizedDescription] UTF8String] : "?");
			            }
		            }];
	}
	{
		char line[160] = {};
		std::snprintf(line, sizeof line, "iOS / %.0f Hz 2ch float32 → 44100 Hz",
		              rate);
		m_impl->fmt_line = line;
	}
	m_impl->m_w.store(0);
	m_impl->m_r.store(0);
	m_impl->running.store(true, std::memory_order_release);
	std::fprintf(stderr, "[ios] audio in: %s (%.0f Hz)\n", want.c_str(), rate);
	return true;
}

void audio_in::stop()
{
	if (!m_impl)
		return;
	m_impl->running.store(false, std::memory_order_release);
	AVAudioEngine *engine = m_impl->engine;
	m_impl->engine = nil;
	if (engine) {
		[[engine inputNode] removeTapOnBus:0];
		[engine stop];
	}
}

bool audio_in::running() const
{
	return m_impl && m_impl->running.load(std::memory_order_acquire);
}

std::string audio_in::device_name() const
{
	return m_impl->dev_name;
}

std::string audio_in::format_line() const
{
	return m_impl->fmt_line;
}

u64 audio_in::empty_count() const
{
	return m_impl->m_empty.load();
}

u64 audio_in::dropped_count() const
{
	return m_impl->m_dropped.load();
}

void audio_in::pop(s32 &l, s32 &r)
{
	audio_in::impl &im = *m_impl;
	u32 rd = im.m_r.load(std::memory_order_relaxed);
	const u32 wr = im.m_w.load(std::memory_order_acquire);
	u32 level = (wr - rd) & audio_in::impl::MASK;
	if (level > audio_in::impl::DROP_FRAMES) {
		rd = (wr - audio_in::impl::TARGET_FRAMES) & audio_in::impl::MASK;
		level = audio_in::impl::TARGET_FRAMES;
		im.m_dropped.fetch_add(1, std::memory_order_relaxed);
	}
	if (!level) {
		l = r = 0;
		if (running())
			im.m_empty.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	l = im.m_ring[rd * 2];
	r = im.m_ring[rd * 2 + 1];
	im.m_r.store((rd + 1) & audio_in::impl::MASK, std::memory_order_relaxed);
}

} // namespace ui