// license:BSD-3-Clause
//
// ios/smoke.mm - the thinnest possible iOS host for the AUv3 extension.
//
// iOS does not discover a bare .appex: only extensions inside a containing app are
// registered. So testing the extension needs an app, and this is the smallest one that
// answers the question worth answering:
//
//     does the extension register, find its ROMs, and produce audio on iOS?
//
// It answers that with **offline rendering**, deliberately, because offline needs no
// AVAudioSession, no output route and no audio hardware - so it runs identically on the
// simulator and on a device, and a failure here is the engine rather than the audio stack.
// The peak sample count is the result: non-zero means the machine is making a sound.
//
// What this deliberately does NOT do, because the standalone app will: choose audio and
// MIDI devices, show the panel, or set an audio session. It exists to be small.
//
// It also settles a question the study could not answer from the source: whether an iOS app
// can load its own extension **in-process**. AUAudioUnit instantiateWithComponentDescription:
// searches AudioComponents by type/subtype/manufacturer, which only sees extensions that
// registered - so a successful instantiation means registration worked. Whether the audio
// unit then runs in this process or is serviced across the XPC boundary is what the log
// line reports.

#import <UIKit/UIKit.h>
#import <AudioToolbox/AudioToolbox.h>
#import <AVFoundation/AVFoundation.h>

// Same identity as packaging/auv3-ios-appex-Info.plist. If these drift, the lookup below
// silently finds nothing and the log says "no component", which reads like a broken
// extension rather than a mismatched constant.
static const AudioComponentDescription kDesc = {
	.componentType = 'aumu',
	.componentSubType = 'SMU3',
	.componentManufacturer = 'Trbh',
};

// A UISceneDelegate, and the plist must name this class as the scene delegate. Both are
// load-bearing; see the long note on scene:willConnectToSession:options: below.
@interface SmokeDelegate : UIResponder <UIWindowSceneDelegate>
@property (nonatomic, strong) UIWindow *window;
@end

@implementation SmokeDelegate

// A UISceneDelegate, not a UIApplicationDelegate, and that is not a style choice.
//
// The first version of this file was a plain UIApplicationDelegate with no
// UIApplicationSceneManifest, on the reasoning that "a scene-based app gets no delegate
// callbacks until a scene connects, so the test would never run". That reasoning was
// half right and got the conclusion exactly backwards: on this iOS the scene lifecycle
// is no longer optional. UIKit raises a runtime issue named
// NoSceneLifecycleAdoption while connecting the first scene and traps the process with
// SIGTRAP - see ~/Library/Logs/DiagnosticReports/S-MU2000-*.ips, whose faulting frame is
// ___UIApplicationEvaluateRuntimeIssueForNoSceneLifecycleAdoption_block_invoke.
//
// So the window has to be built from the UIWindowScene handed to us, and the delegate has
// to be a scene delegate, and the plist has to declare it. Three places, all required.
- (void)scene:(UIScene *)scene
	willConnectToSession:(UISceneSession *)session
	options:(UISceneConnectionOptions *)opts
{
	if (![scene isKindOfClass:UIWindowScene.class])
		return;

	// A window built from the scene, not from UIScreen bounds: on the scene lifecycle the
	// window is a child of the scene and must be initialised with it.
	self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
	UIViewController *vc = [[UIViewController alloc] init];
	vc.view.backgroundColor = UIColor.blackColor;
	self.window.rootViewController = vc;
	[self.window makeKeyAndVisible];

	// Let the run loop settle so the log is readable, then test on the main thread so
	// any framework exception lands in this process rather than nowhere.
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)),
	               dispatch_get_main_queue(), ^{ [self test]; });
}

- (void)log:(NSString *)fmt, ...
{
	va_list ap;
	va_start(ap, fmt);
	NSString *s = [[NSString alloc] initWithFormat:fmt arguments:ap];
	va_end(ap);
	// NSLog so `xcrun simctl spawn booted log stream` sees it
	NSLog(@"[smoke] %@", s);
}

- (void)test
{
	AudioComponent c = AudioComponentFindNext(NULL, &kDesc);
	if (!c) {
		[self log:@"FAIL no component: the .appex did not register"];
		return;
	}
	// AudioComponent is an opaque pointer - it has no .description, so the
	// description we searched with is the one to hand over. The find above is only
	// here to prove the extension registered at all.
	const AudioComponentDescription d = kDesc;
	NSError *err = nil;
	AUAudioUnit *au = [[AUAudioUnit alloc] initWithComponentDescription:d error:&err];
	if (!au) {
		[self log:@"FAIL instantiate: %@", err];
		return;
	}
	[self log:@"instantiated %c%c%c%c/%c%c%c%c",
	          d.componentType, d.componentType >> 8, d.componentType >> 16, d.componentType >> 24,
	          d.componentSubType, d.componentSubType >> 8, d.componentSubType >> 16,
	          d.componentSubType >> 24];

	if (au.inputBusses.count == 0 || au.outputBusses.count == 0) {
		[self log:@"FAIL no busses: %lu in %lu out",
		          (unsigned long)au.inputBusses.count, (unsigned long)au.outputBusses.count];
		return;
	}

	// The buses take an AVAudioFormat, not an AudioStreamBasicDescription
	AVAudioFormat *fmt = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:44100.0
	                                                                 channels:2];
	if (![au.inputBusses[0] setFormat:fmt error:&err] ||
	    ![au.outputBusses[0] setFormat:fmt error:&err]) {
		[self log:@"FAIL setFormat: %@", err];
		return;
	}
	if (![au allocateRenderResourcesAndReturnError:&err]) {
		[self log:@"FAIL allocateRenderResources: %@", err];
		return;
	}
	[self log:@"render resources allocated - the machine is booting"];

	// AUAudioUnit exposes no AudioUnit handle; the render block is the way in. Its
	// signature puts frameCount *before* outputBusNumber, the opposite of
	// AudioUnitRender, which is the kind of thing worth not guessing at.
	AURenderBlock render = au.renderBlock;
	if (!render) {
		[self log:@"FAIL no renderBlock"];
		return;
	}
	[self log:@"renderBlock present (the unit is reachable from this process)"];

	// Render a couple of seconds: boot is not instant, and a peak of zero early on
	// would say nothing.
	const AUAudioFrameCount kChunk = 512;
	const int kChunks = 44100 * 3 / (int)kChunk;
	int16_t *buf = (int16_t *)calloc(kChunk * 2, sizeof(int16_t));
	if (!buf) {
		[self log:@"FAIL out of memory"];
		return;
	}

	int32_t peak = 0;
	long nonzero = 0;
	for (int n = 0; n < kChunks; n++) {
		AudioBufferList abl;
		abl.mNumberBuffers = 1;
		abl.mBuffers[0].mNumberChannels = 2;
		abl.mBuffers[0].mDataByteSize = kChunk * 4;
		abl.mBuffers[0].mData = buf;

		AudioUnitRenderActionFlags flags = 0;
		const AudioTimeStamp ts = { .mSampleTime = (Float64)(n * kChunk) };
		const AUAudioUnitStatus st = render(&flags, &ts, kChunk, 0, &abl, nil);
		if (st != noErr) {
			[self log:@"FAIL render at chunk %d: 0x%08x", n, (unsigned)st];
			free(buf);
			return;
		}
		for (int i = 0; i < (int)(kChunk * 2); i++) {
			const int32_t v = buf[i] < 0 ? -buf[i] : buf[i];
			if (v > peak)
				peak = v;
			if (v)
				nonzero++;
		}
	}

	[self log:@"RESULT peak=%d nonzero=%ld of %d -> %@",
	          peak, nonzero, (long)kChunks * kChunk * 2,
	          peak > 0 ? @"SOUND" : @"SILENT"];
	free(buf);
}

@end

// The fourth argument is the *application* delegate class name, and it must be nil here.
//
// The first version of this file passed NSStringFromClass([SmokeDelegate class]), left over
// from when SmokeDelegate was a UIApplicationDelegate. After it became a
// UIWindowSceneDelegate that argument became a lie: UIKit instantiated it as the app delegate,
// found it does not configure scenes, and raised NoSceneLifecycleAdoption - trapping with
// SIGTRAP even though the scene manifest in the plist was present and correct. The manifest
// alone does not satisfy the check; UIKit also expects the app delegate to be either absent
// or scene-aware.
//
// nil means "the default UIApplication", which is the standard Xcode template and leaves the
// scene delegate to be resolved from UISceneDelegateClassName in Info.plist.
int main(int argc, char *argv[])
{
	@autoreleasepool {
		return UIApplicationMain(argc, argv, nil, nil);
	}
}