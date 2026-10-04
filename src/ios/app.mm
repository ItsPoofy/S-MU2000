// license:BSD-3-Clause
//
// ios/app.mm - the iOS standalone front end, step 1: the panel, drawn.
//
// This replaces the self-test harness (ios/smoke.mm, kept for the moment because
// it still answers "does the extension register?", which the standalone does not
// need). It is the iOS counterpart of src/gui.cpp, and it is short for the same
// reason gui.cpp is: the boot sequence, the panel and the MIDI routing live in
// ui/engine and ui/app, shared with the desktop front ends, so this file only has
// to make the app, load the ROMs and hand the panel a view.
//
// Step 1 deliberately stops before audio and input. What it answers is whether
// the ImGui panel renders on iOS at all - the same question for the app and for
// the AUv3, since both paint through ui/panel.cpp and neither knows what it is
// drawn on. Panel.cpp carries no hardcoded size: it takes the width paint_main
// passes and lays itself out, so the phone's dimensions are a matter of the host
// handing over a different number, not of changing the panel.
//
// No touch handling yet, so the panel shows but cannot be operated.

#import <UIKit/UIKit.h>

#include "compat/paths.h"
#include "mu2000.h"
#include "ui/app_ios.h"
#include "ui/engine.h"
#include "ui/layout.h"
#include "ui/options.h"
#include "ui/tool_args.h"
#include "ui/window_ios.h"

#import <TargetConditionals.h>

#include <cstdio>
#include <cstring>

namespace {

// The ROMs, found beside the binary.
//
// This is the one thing the portability study could not answer from the source,
// so it is worth being explicit about what is assumed. The ROMs are baked into
// the app bundle by `make ios-app-roms`, which puts them in Resources/roms. The
// engine resolves them from module_dir() + "/../Resources/roms", and on iOS the
// bundle is flat (the binary sits at the root, not in Contents/MacOS as on macOS),
// so "one level up from the binary" is the bundle root and the path is correct
// for both layouts. module_dir() itself is Mach-O header walking and has not been
// verified on iOS - if the panel comes up empty, this is the first thing to check.
std::string rom_dir()
{
	const std::string here = smu2000::module_dir(reinterpret_cast<const void *>(&rom_dir));
	if (here.empty())
		return {};
	// Beside the binary (S-MU2000.app/roms/), which is engine candidate 3b
	// (module_dir + "roms" in src/vst3/engine.cpp) - so no search is needed, only
	// this one path. Deliberately NOT ../Resources/roms: on the flat iOS bundle that
	// resolves to a sibling of the .app, and any subdirectory under the app's
	// Resources/ breaks ad-hoc codesign anyway (see the Makefile's IOS_PANEL_DIR
	// comment for the bisection that proved it).
	return here + "/roms";
}

} // namespace

@interface SMUAppDelegate : UIResponder <UIWindowSceneDelegate>
@property (nonatomic, strong) UIWindow *window;
@end

@implementation SMUAppDelegate

// The ROM directory is resolved here, in Objective-C, because this is a .mm and
// NSSearchPathForDirectoriesInDomains is the correct way to find a bundle on
// iOS - and because module_dir() is the fallback for when that fails.
- (void)scene:(UIScene *)scene
	willConnectToSession:(UISceneSession *)session
	options:(UISceneConnectionOptions *)opts
{
	// The very first statement, before anything that can fail or return. Two rounds
	// of debugging ended here: a stale UISceneDelegateClassName, then a view built but
	// never attached to the hierarchy. Both presented as the same thing - a black
	// screen and *no log output at all* - because a scene delegate that is never
	// called and one that is called but dies early look identical from outside.
	// fprintf to stderr rather than NSLog: NSLog goes through the unified log and
	// needs the right predicate to be seen, whereas stderr is picked up by a stream
	// on the process with no filtering.
	std::fprintf(stderr, "[ios] scene willConnectToSession\n");
	std::fflush(stderr);
	(void)session;
	(void)opts;
	if (![scene isKindOfClass:UIWindowScene.class]) {
		std::fprintf(stderr, "[ios] scene is not a UIWindowScene (%s)\n",
		             [[scene class] UTF8String]);
		return;
	}

	self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
	UIViewController *vc = [[UIViewController alloc] init];
	vc.view.backgroundColor = UIColor.blackColor;
	self.window.rootViewController = vc;
	[self.window makeKeyAndVisible];

	// The shared state and the app, laid out exactly as src/gui.cpp does: a
	// bridge, four MIDI inputs (mu2000::MIDI_PORTS of them) and three outputs
	// (THRU A, THRU B, and the machine's own OUT), then the app over them.
	static ui::bridge br;
	static ui::midi_in  midi_ports[mu2000::MIDI_PORTS];
	static ui::midi_out mout, mout_b, mout_mu;
	static ui::gui_app gui(br, midi_ports, mout, mout_b, mout_mu);
	ui::g_gui = &gui;
	gui.eng = nullptr;
	// Creates the audio objects now (needs no firmware); opening them waits for
	// boot below. Without this out stays null and start_audio refuses - which is
	// exactly the silence with no log line, since nothing ever tried.
	gui.make_audio();

	// The panel needs no audio to draw, so this boots the firmware first; the
	// audio device opens afterwards below, once the firmware is up.
	const std::string dir = rom_dir();
	std::fprintf(stderr, "[ios] ROM dir: %s\n", dir.c_str());

	static ui::engine eng(br, midi_ports[0]);
	gui.eng = &eng;
	gui.state = &eng.state;
	ui::engine_options eng_opts;

	// load_machine wants the parsed tool_args because it reads a.dir and a.usb_host
	// off them. Rather than run the argv parser - there is no command line on iOS -
	// this fills in the fields it uses and leaves the rest default.
	ui::tool_args a;
	a.dir = dir;
	// Without --layout, look through the usual places in order: exactly what the
	// shared parser does in tool_args.h. Skipping this was why art/real was never
	// used - apply_layout("") falls back to the built-in defaults and never
	// consults find_default(), so the bundled art/real/panel.txt sat there
	// unread while the panel drew from defaults. The log names the file so the
	// next run proves which layout won rather than leaving it to inspection.
	if (a.layout_path.empty())
		a.layout_path = ui::layout::find_default();
	std::fprintf(stderr, "[ios] layout: %s\n",
	             a.layout_path.empty() ? "(built-in defaults)" : a.layout_path.c_str());
	if (dir.empty() || !gui.load_machine(eng, a)) {
		std::fprintf(stderr, "[ios] no ROMs: the panel will come up empty\n");
	} else {
		// Boot the firmware so the panel shows the real machine rather than a blank
		// LCD. boot() runs the SH2 until it settles on its own display.
		if (eng.boot())
			std::fprintf(stderr, "[ios] booted\n");
		else
			std::fprintf(stderr, "[ios] boot failed: %s\n", eng.message.c_str());
		eng.state.store(1);
	}
	gui.wire_engine(eng, eng_opts);

	// What ui::app::run() does for every desktop main before showing the window:
	// LCD/panel sizing, the layout file (art/real included), remembered volume.
	// iOS never calls run() - it cannot, run() pumps its own event loop - and this
	// call was missing, so the layout path found above was never applied and the
	// panel kept its built-in defaults. That was the whole "no art/real on
	// screen": find_default() succeeding looks like success, but without
	// apply_layout nothing reads a single PNG. setup_for_window opens no audio
	// device (that is open_remembered_ports/make_audio, called later in run()),
	// so it is safe this early.
	ui::window_options win_opts;
	gui.setup_for_window(a, win_opts, false);

	// What run()'s boot thread does after boot: open the audio device now that
	// the firmware is up. a.latency is the shared default (30 ms); exclusive
	// would ask for hog mode, which does not exist on iOS. On failure the shared
	// code parks the engine (state 2) and says why - silence with a reason beats
	// silence without one. start_ad (recording input) is skipped: there is no
	// input backend yet.
	if (gui.start_audio(a.latency, false)) {
		gui.say_audio_opened(false);
		gui.say_audio_running();
	} else {
		std::fprintf(stderr, "[ios] audio start failed; panel runs silent\n");
	}
	gui.audio_ready.store(true);

	// The window system: a UIView with a CAMetalLayer and a 30 Hz CADisplayLink.
	const CGRect b = [UIScreen mainScreen].bounds;
	UIView *panel_view = ui::make_ios_view(gui, (int)b.size.width, (int)b.size.height);
	if (!panel_view) {
		std::fprintf(stderr, "[ios] no view: no Metal device\n");
		return;
	}
	// The view has to be in the hierarchy or it draws nothing, and nothing says so.
	// As a pinned subview, not as the root view: a manually assigned root view
	// keeps the fixed frame it was created with (UIScreen bounds at launch), so
	// in a smaller Stage Manager window it overflowed right and bottom while the
	// scale was computed for fullscreen - the "resize broken" that kept the panel
	// cut off. Anchors make the view track the window on rotation and resize, and
	// layoutSubviews refits the panel from the real size every time.
	panel_view.translatesAutoresizingMaskIntoConstraints = NO;
	[vc.view addSubview:panel_view];
	// Pinned to the safe area on all four sides, not the view edges: the panel's
	// top strip (List/Editor/... buttons) went under the iPad menu bar and the
	// status area with edge pins, making the editor-launching buttons visible
	// but untappable. The bars this leaves are UIKit's problem (letterbox), and
	// every control stays reachable - which is the whole point of a panel.
	UILayoutGuide *safe = vc.view.safeAreaLayoutGuide;
	[NSLayoutConstraint activateConstraints:@[
		[panel_view.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor],
		[panel_view.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor],
		[panel_view.topAnchor constraintEqualToAnchor:safe.topAnchor],
		[panel_view.bottomAnchor constraintEqualToAnchor:safe.bottomAnchor],
	]];

// Touch works now (one finger = mouse, held second finger = right button), so
// the panel can be operated; keyboard and audio are still missing.
	std::fprintf(stderr, "[ios] running: tap to press, hold a second finger for menus\n");
}

@end

int main(int argc, char *argv[])
{
	@autoreleasepool {
		// nil for both: the app delegate must be absent on this iOS, and the scene
		// delegate comes from UISceneDelegateClassName in Info.plist. Passing the
		// scene delegate here instead is what produced NoSceneLifecycleAdoption.
		return UIApplicationMain(argc, argv, nil, nil);
	}
}