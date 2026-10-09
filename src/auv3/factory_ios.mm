// license:BSD-3-Clause
//
// factory_ios.mm - the AUv3 extension's entry point for iOS, **with a view**.
//
// One class does both jobs, exactly like factory.mm on macOS (and like Apple's
// own Audio Unit Extension template, which puts AUViewController +
// AUAudioUnitFactory in one class):
//   createAudioUnitWithComponentDescription:error:  AU を作る (factory)
//   loadView                                        パネルを貼る (screen)
// The panel itself is view_controller_ios.mm's, one shared editor with the
// AUv2 and VST3 - no second copy of anything.
//
// An empty frame until the panel exists: the host may ask for the screen before
// the unit (Apple's template makes the view create the AU in viewDidLoad for
// the same reason), so either order must work. kEmptyWidth/Height match the
// mac twin's.

#import "audio_unit.h"
#import "view_controller_ios.h"

#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudioKit/CoreAudioKit.h>
#import <UIKit/UIKit.h>

// The frame a host is handed before the panel exists. It is only that: the
// real panel arrives through installPanel once both sides are there. Below the
// imports because CGFloat comes from CoreGraphics (via UIKit).
static const CGFloat kEmptyWidth  = 640;
static const CGFloat kEmptyHeight = 180;

@interface SMU2000FactoryV3 : AUViewController <AUAudioUnitFactory>
@end

// The unit is made on the XPC thread that asks for it and the screen is laid
// on the main thread, so whichever side reads holds it atomically (the
// default). Same discipline as factory.mm: touching a view controller off the
// main thread throws, and the throw unwinds through ExtensionFoundation and
// takes the extension with it.
@interface SMU2000FactoryV3 ()
@property (atomic, strong) SMU2000AudioUnitV3 *unit;
@end

@implementation SMU2000FactoryV3 {
	SMU2000ViewControllerV3 *_panel;
	UIView *_frame;
}

// Part of the adopted protocol, not an extra: AUAudioUnitFactory includes
// NSExtensionRequestHandling (AUAudioUnitImplementation.h). iOS hosts the
// extension through ExtensionFoundation, which sends beginRequest first, and a
// factory that does not answer it is what "unrecognized selector" (-10863) on
// instantiation is. Long-lived by design: never complete the request.
- (void)beginRequestWithExtensionContext:(NSExtensionContext *)context
{
	(void)context;
}

// The extension is asked to make one AUAudioUnit. Its engine is what the panel
// draws, once there is a panel.
- (AUAudioUnit *)createAudioUnitWithComponentDescription:(AudioComponentDescription)desc
                                                   error:(NSError **)error
{
	self.unit = [[SMU2000AudioUnitV3 alloc] initWithComponentDescription:desc error:error];
	[self installPanel];   // arrives on the XPC thread: handed to main inside
	return self.unit;
}

- (void)loadView
{
	_frame = [[UIView alloc] initWithFrame:CGRectMake(0, 0, kEmptyWidth, kEmptyHeight)];
	self.view = _frame;
	[self installPanel];
}

// Pasted once, when unit and frame are both there. **View work on main.**
- (void)installPanel
{
	if (![NSThread isMainThread]) {
		dispatch_async(dispatch_get_main_queue(), ^{ [self installPanel]; });
		return;
	}
	SMU2000AudioUnitV3 *unit = self.unit;
	if (!_frame || !unit || _panel)
		return;
	_panel = [[SMU2000ViewControllerV3 alloc] initWithEngine:[unit panelEngine]
	                                              audioUnit:unit];
	if (!_panel.view)
		return;
	_panel.view.frame = _frame.bounds;
	_panel.view.autoresizingMask = UIViewAutoresizingFlexibleWidth |
	                               UIViewAutoresizingFlexibleHeight;
	[_frame addSubview:_panel.view];
	self.preferredContentSize = _panel.preferredContentSize;
}

@end