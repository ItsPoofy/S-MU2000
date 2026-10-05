// license:BSD-3-Clause
//
// See midi_setup.h for what this is and why it exists.

#import "ios/midi_setup.h"

#include "ui/menu.h"
#include "ui/texts.h"

#import <CoreAudioKit/CABTMIDICentralViewController.h>
#import <CoreAudioKit/CABTMIDILocalPeripheralViewController.h>
#import <CoreMIDI/MIDINetworkSession.h>

#import <objc/runtime.h>

static NSString *const kNetworkMidiKey = @"smu_network_midi";

void append_midi_setup_group(std::vector<ui::menu_group> &groups)
{
	ui::menu_group g;
	g.title = UI_TEXT(menu_bt_title, "Bluetooth & network MIDI");
	ui::menu_item connect;
	connect.label = UI_TEXT(menu_bt_connect, "Connect Bluetooth MIDI...");
	connect.id = ID_IOS_BT_CONNECT;
	g.items.push_back(connect);
	ui::menu_item advertise;
	advertise.label = UI_TEXT(menu_bt_advertise, "Advertise this device...");
	advertise.id = ID_IOS_BT_ADVERTISE;
	g.items.push_back(advertise);
	ui::menu_item net;
	net.label = UI_TEXT(menu_net_midi, "Network MIDI");
	net.id = ID_IOS_NET_MIDI;
	net.checked = network_midi_enabled();
	g.items.push_back(net);
	groups.push_back(g);
}

bool network_midi_enabled()
{
	return [[NSUserDefaults standardUserDefaults] boolForKey:kNetworkMidiKey];
}

static void apply_network_midi(bool on)
{
	MIDINetworkSession *session = [MIDINetworkSession defaultSession];
	// Anyone may connect: this is a synth, not a secret. The desktop leaves
	// the session to Audio MIDI Setup; here there is no such app, so the
	// toggle owns the policy too.
	session.connectionPolicy = MIDINetworkConnectionPolicy_Anyone;
	session.enabled = on ? YES : NO;
	[[NSUserDefaults standardUserDefaults] setBool:on forKey:kNetworkMidiKey];
}

void apply_stored_midi_setup()
{
	// Stored OFF (the default) still writes through: a session left enabled
	// by an older install or a crash must not survive the switch.
	apply_network_midi(network_midi_enabled());
}

// The Done button's target. Held by association on the navigation controller
// (the button does not retain it), released with the sheet.
@interface SMUMidiSetupCloser : NSObject
- (void)dismiss:(id)sender;
@end

@implementation SMUMidiSetupCloser
- (void)dismiss:(id)sender
{
	(void)sender;
	UIViewController *presented = nil;
	for (UIWindowScene *scene in [UIApplication sharedApplication].connectedScenes) {
		if (![scene isKindOfClass:[UIWindowScene class]])
			continue;
		for (UIWindow *w in [(UIWindowScene *)scene windows]) {
			if (w.isKeyWindow && w.rootViewController.presentedViewController)
				presented = w.rootViewController.presentedViewController;
		}
	}
	[presented dismissViewControllerAnimated:YES completion:nil];
}
@end

static const void *kCloserKey = &kCloserKey;

// -[UIScreen applicationFrame] is deprecated since iOS 9, and on this runtime
// it returns a NaN-height rect. Apple's BT MIDI controllers still build their
// table straight from it (disassembled loadView: mainScreen -> applicationFrame
// -> initWithFrame:style:, no other input), so every sheet dies in
// CALayerInvalidGeometry. Substitute bounds once, before presenting them:
// strictly more correct than NaN, and nothing modern should be calling this.
// dispatch_once: process-wide, one method, no per-presentation cost.
static void patch_application_frame()
{
	static dispatch_once_t once;
	dispatch_once(&once, ^{
		Method m = class_getInstanceMethod([UIScreen class], @selector(applicationFrame));
		if (!m)
			return;
		IMP fixed = imp_implementationWithBlock(^CGRect(id self) {
			return [(UIScreen *)self bounds];
		});
		method_setImplementation(m, fixed);
	});
}

// The view controller to present from: the view's own, found by walking up to
// the view whose next responder is a controller. Falls back to the foreground
// scene's key window, the same rule pc_window_ios.mm uses.
static UIViewController *presenter_for_view(UIView *view)
{
	for (UIView *up = view; up; up = up.superview) {
		if ([up.nextResponder isKindOfClass:[UIViewController class]])
			return (UIViewController *)up.nextResponder;
	}
	for (UIScene *scene in [UIApplication sharedApplication].connectedScenes) {
		if (![scene isKindOfClass:[UIWindowScene class]])
			continue;
		if ([(UIWindowScene *)scene activationState] !=
		    UISceneActivationStateForegroundActive)
			continue;
		for (UIWindow *w in [(UIWindowScene *)scene windows]) {
			if (w.isKeyWindow)
				return w.rootViewController;
		}
	}
	return nil;
}

static void present_bt_controller(UIView *view, CGPoint at, UIViewController *bt)
{
	(void)at;
	UIViewController *presenter = presenter_for_view(view);
	if (!presenter)
		return;
	// Apple's own recipe (QA1831) wraps these in a navigation controller with
	// Done. Presented as a form sheet, NOT a popover: UIPopoverPresentationController
	// forces the child to load while sizing it, and the BT controllers build
	// their table with a NaN-height frame on that path (CALayerInvalidGeometry
	// crash, proven on the simulator). A sheet gives them real bounds at load.
	SMUMidiSetupCloser *closer = [[SMUMidiSetupCloser alloc] init];
	bt.navigationItem.rightBarButtonItem =
	    [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone
	                                                  target:closer
	                                                  action:@selector(dismiss:)];
	UINavigationController *nav =
	    [[UINavigationController alloc] initWithRootViewController:bt];
	objc_setAssociatedObject(nav, kCloserKey, closer, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
	// A real size BEFORE presenting: sheet sizing force-loads the BT view to
	// measure it, and Apple's loadView builds its table from the deprecated
	// applicationFrame (see patch_application_frame above), which is only
	// valid once something has measured it. 540x620 up front.
	bt.preferredContentSize = CGSizeMake(540, 620);
	// Fullscreen, not a sheet: every sheet variant (popover, form sheet) loads
	// the BT view while measuring it, and each measuring path has produced the
	// same NaN table frame. Fullscreen hands it window bounds up front, so
	// there is nothing to measure through.
	nav.modalPresentationStyle = UIModalPresentationFullScreen;
	// The menu this was picked from dismisses on selection, but the dismissal
	// is still in flight when this runs: presenting on a controller that is
	// already presenting (or dismissing) warns and force-loads the BT view
	// mid-transition, which is the NaN crash. So wait past the dismissal,
	// then clear anything still up before presenting.
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.4 * NSEC_PER_SEC)),
	               dispatch_get_main_queue(), ^{
		               if (presenter.presentedViewController) {
			               [presenter dismissViewControllerAnimated:NO
			                                        completion:^{
				                                        [presenter presentViewController:nav
				                                                              animated:YES
				                                                            completion:nil];
			                                        }];
		               } else {
			               [presenter presentViewController:nav
			                                     animated:YES
			                                   completion:nil];
		               }
	               });
}

bool handle_midi_setup_item(UIView *view, CGPoint at, int itemId)
{
	switch (itemId) {
	case ID_IOS_BT_CONNECT:
	case ID_IOS_BT_ADVERTISE: {
		patch_application_frame();
		UIViewController *bt = (itemId == ID_IOS_BT_CONNECT)
		    ? (UIViewController *)[[CABTMIDICentralViewController alloc] init]
		    : (UIViewController *)[[CABTMIDILocalPeripheralViewController alloc] init];
		present_bt_controller(view, at, bt);
		return true;
	}
	case ID_IOS_NET_MIDI:
		// The menu that held the checkmark is already dismissing (selection
		// dismisses); the next open re-snapshots and shows the new state.
		apply_network_midi(!network_midi_enabled());
		return true;
	default:
		return false;
	}
}
