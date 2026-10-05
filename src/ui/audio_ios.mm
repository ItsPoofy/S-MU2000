// license:BSD-3-Clause
//
// The iOS half of Apple audio. The render path - engine, source node, render
// block, resampler, meters, capture, workgroup - is shared with macOS and lives
// in audio_apple.mm. What stays here is what only iOS has: the session
// (category, permission, route and interruption observers, port names), the
// answers the shared core asks for, and the input half, which is a second
// AVAudioEngine tapping the input node and moves into the shared core the same
// way output did.
//
// Why this file was written from scratch in the first place: iOS ships no
// public AudioHardware HAL - AudioObject* appears in no header, only in
// CoreAudio.tbd - so audio_out_mac.cpp (device enumeration by property query,
// hog mode, raw mode) cannot be ported and does not compile there at all. On
// iOS choosing output is a session route rather than a property: one route at a
// time, described by AVAudioSession.currentRoute, and the app plays through it.
// There is no device menu to fill with anything else, which is why list()
// reports the route.
//
// What is *not* iOS-only, and used to look as if it were: the engine's nodes
// hand out the very AudioUnit a hand-written backend would own, so the device,
// the buffer size, the stream format and the workgroup are the same properties
// on both systems. Only the session and the device list are ours to answer.

#import <AVFAudio/AVFAudio.h>
#import <Foundation/Foundation.h>

#include "ui/audio_apple.h"
#include "ui/audio_in.h"
#include "ui/audio_out.h"
#include "ui/resampler.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace ui {

struct audio_out::impl {
	std::unique_ptr<apple_audio_out> core = std::make_unique<apple_audio_out>();

	// Route-change / interruption observers, registered once. Without these
	// the first category flip (picking the mic) or the first headphone plug
	// silences output until restart: AVAudioEngine stops itself on route
	// changes and interruptions and waits to be started again. They call back
	// into the shared core, which owns the engine and the nodes.
	id route_obs = nil, interrupt_obs = nil;
	bool observing = false;
};

audio_out::audio_out()
	: m_impl(std::make_unique<impl>())
{
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
	return apple::output_list();
}

bool audio_out::start(int latency_ms, fill_fn fill, std::string &err, bool exclusive,
                      const std::string &device, bool raw, bool exact)
{
	// exclusive asks for hog mode, raw bypasses the system mixer: neither
	// exists on iOS (one route, always mixed), so both are accepted and
	// ignored rather than failing the open. device/exact go to the shared core,
	// which asks this file's pin_output() below.
	(void)exclusive;
	(void)raw;
	apple_audio_out::request r;
	r.latency_ms = latency_ms;
	r.device = device;
	r.exact = exact;
	if (!m_impl->core->start(r, std::move(fill), err))
		return false;

	if (!m_impl->observing) {
		m_impl->observing = true;
		apple_audio_out *core = m_impl->core.get();
		m_impl->route_obs = [[NSNotificationCenter defaultCenter]
		    addObserverForName:AVAudioSessionRouteChangeNotification
		                object:nil
		                 queue:nil
		            usingBlock:^(NSNotification *note) {
			            (void)note;
			            core->restart();
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
			                         AVAudioSessionInterruptionOptionShouldResume))
			            core->restart();
		            }];
	}
	return true;
}

void audio_out::stop()
{
	m_impl->core->stop();
}

std::string audio_out::device_name() const
{
	return m_impl->core->device_name();
}

bool audio_out::exclusive() const
{
	return m_impl->core->exclusive();
}

#if defined(__APPLE__)
void *audio_out::realtime_workgroup()
{
	return m_impl->core->realtime_workgroup();
}
#endif

void audio_out::set_capture(const std::string &path)
{
	m_impl->core->set_capture(path);
}

u64 audio_out::capture_frames() const
{
	return m_impl->core->capture_frames();
}

bool audio_out::write_capture(std::string &err)
{
	return m_impl->core->write_capture(err);
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

// ---- The iOS answers to the shared core (see ui/audio_apple.h) --------------
//
// The session is the whole of what only iOS has: it owns the category, the
// rate, how much the system buffers, and when the route changes under us. It
// is also why the engine is the right tool here rather than a raw RemoteIO
// unit - a unit would make every one of these ours to get right, and getting
// one wrong means silence.

namespace apple {

bool session_open(int latency_ms, std::string &err)
{
	AVAudioSession *s = [AVAudioSession sharedInstance];
	NSError *e = nil;
	if (![s setCategory:AVAudioSessionCategoryPlayback error:&e]) {
		err = std::string("AVAudioSession category: ") +
		      (e ? [[e localizedDescription] UTF8String] : "?");
		return false;
	}
	// Asked, not promised: the session may grant another rate (hardware at
	// 48k), and the shared core's resampler covers the difference instead of
	// failing to open.
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
	return true;
}

// The one route, by its port name. There is no choice to make: the system owns
// the route, and it changes when headphones appear or a pair connects.
std::vector<std::string> output_list()
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

// Which device a name means: there is only ever the one route, so any name
// resolves to it and is found. A remembered name goes stale the moment AirPods
// connect, so refusing one here would turn a cosmetic mismatch into a silent
// app - the route is not ours to refuse.
device_ref resolve_output(const std::string &name, bool)
{
	device_ref dev;
	dev.id = 0;   // no HAL to name a device with
	dev.name = name;
	dev.found = true;
	return dev;
}

// Nothing to pin: the session chose the route and the unit follows it.
bool pin_output(AudioUnit, const device_ref &, std::string &)
{
	return true;
}

void unpin_output()
{
}

// No hog mode on iOS: nothing else can share the route through us, and the
// system mixer is not ours to take over. So nothing is ever given back either.
device_claim take_output(const device_ref &, std::string &)
{
	return device_claim();
}

void release_output(const device_claim &)
{
}

// The IO buffer duration was asked of the session in session_open(), and the
// device under the unit is not ours to resize. Zero says so.
u32 request_buffer_frames(const device_ref &, int)
{
	return 0;
}

std::string output_label(const device_ref &, double rate)
{
	char name[128] = {};
	std::snprintf(name, sizeof(name), "iOS %.0f Hz", rate);
	return name;
}

// ---- The input side ---------------------------------------------------------

// The available inputs, by port name - not the current route's inputs, which
// are empty while the session is Playback (output started at boot, before any
// input was ever picked). availableInputs is category-independent hardware
// capability, so the mic lists with no permission and no session change.
// Nothing connects here: permission, category and capture all wait for start().
std::vector<std::string> input_list()
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

// Which port a name means. Empty means the first one, the default rule of every
// backend here; an unknown name is refused rather than quietly recorded, which
// is what the menu expects when the remembered device is gone.
device_ref resolve_input(const std::string &name, bool)
{
	const std::vector<std::string> names = input_list();
	device_ref dev;
	if (name.empty()) {
		if (names.empty())
			return dev;                    // found stays false: nothing to record from
		dev.name = names.front();
		return dev;
	}
	if (std::find(names.begin(), names.end(), name) == names.end())
		return dev;
	dev.name = name;
	return dev;
}

// The microphone permission, asked here and only here: a launch that never
// records never prompts. Undetermined asks and fails this pick with "pick
// again"; denied says where to re-allow.
bool input_permission(std::string &err)
{
	const AVAudioApplicationRecordPermission perm =
	    AVAudioApplication.sharedInstance.recordPermission;
	if (perm == AVAudioApplicationRecordPermissionGranted)
		return true;
	if (perm == AVAudioApplicationRecordPermissionUndetermined)
		[AVAudioApplication requestRecordPermissionWithCompletionHandler:^(BOOL granted) {
			(void)granted;
		}];
	err = (perm == AVAudioApplicationRecordPermissionDenied)
	    ? "マイクが拒否されている（設定アプリで許可）"
	    : "マイクの許可を求めた。もう一度選ぶ";
	return false;
}

// PlayAndRecord for input; DefaultToSpeaker keeps output where Playback had it
// (without this the speaker goes quiet and sound moves to the earpiece the
// moment recording starts). The switch can blip output; that is the OS
// re-routing, not a bug here.
bool session_open_input(std::string &err)
{
	AVAudioSession *s = [AVAudioSession sharedInstance];
	NSError *e = nil;
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
	return true;
}

// Here is what this file never did: ask the session for the port that was
// picked. macOS pins a device by setting a property on the unit the engine
// hands out; iOS has no HAL, and the way to choose an input port is the
// session's own - setPreferredInput:, which is also what makes the choice
// survive the route changes the observer below restarts through.
bool pin_input(AudioUnit, const device_ref &dev, std::string &)
{
	AVAudioSession *s = [AVAudioSession sharedInstance];
	for (AVAudioSessionPortDescription *p in [s availableInputs]) {
		const char *n = [[p portName] UTF8String];
		if (!n || dev.name != n)
			continue;
		NSError *e = nil;
		if (![s setPreferredInput:p error:&e]) {
			std::fprintf(stderr, "[ios] audio in: preferred input refused: %s\n",
			             e ? [[e localizedDescription] UTF8String] : "?");
			return false;
		}
		std::fprintf(stderr, "[ios] audio in: input port %s\n", n);
		return true;
	}
	return true;   // the name is not one the session offers: its own choice stands
}

std::string input_label(const device_ref &, double rate, u32 channels)
{
	char line[160] = {};
	std::snprintf(line, sizeof line, "iOS / %.0f Hz %uch float32 \xe2\x86\x92 44100 Hz",
	              rate, channels);
	return line;
}

} // namespace apple

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

// ---- audio_in: the shell the shared core is driven through ------------------
//
// The interface is unchanged and every method is one line: the tap, the ring,
// the resampler and the counters are audio_apple.mm's, shared with macOS.

struct audio_in::impl {
	std::unique_ptr<apple_audio_in> core = std::make_unique<apple_audio_in>();

	// A route change stops the input engine too (mic unplugged, category
	// flipped), so it is restarted the same way output is.
	id route_obs = nil;
	bool observing = false;
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
	if (!m_impl->core->start(device, err))
		return false;
	if (!m_impl->observing) {
		m_impl->observing = true;
		apple_audio_in *core = m_impl->core.get();
		m_impl->route_obs = [[NSNotificationCenter defaultCenter]
		    addObserverForName:AVAudioSessionRouteChangeNotification
		                object:nil
		                 queue:nil
		            usingBlock:^(NSNotification *note) {
			            (void)note;
			            core->restart();
		            }];
	}
	return true;
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
