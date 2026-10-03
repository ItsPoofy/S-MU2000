// license:BSD-3-Clause
//
// factory_ios.mm - the AUv3 extension's entry point for iOS, **without a view**.
//
// This is the same extension as factory.mm, with one deliberate difference: the factory
// is an NSObject and not an AUViewController, which is the `com.apple.AudioUnit` extension
// kind rather than `com.apple.AudioUnit-UI`. Apple spells the pairing out in the
// Audio Unit Extension template's TemplateInfo.plist:
//
//   com.apple.AudioUnit      principal class is NSObject + AUAudioUnitFactory
//   com.apple.AudioUnit-UI   principal class is AUViewController + AUAudioUnitFactory
//
// The trade is one-way and worth taking first: with no UI, **no view layer has to exist
// yet**, so the whole AppKit/UIKit port (src/vst3/panel_nsview.h, view_controller.h,
// and view_mac.mm's 679 lines) can be deferred until after the questions that actually
// matter have been answered - does the extension build, install, register, and make a
// sound on iOS at all. Sound does not need a view: `com.apple.AudioUnit` sounds exactly
// the same, it is only the host's request for a view that never arrives.
//
// What is kept from factory.mm verbatim is the part that is not about AppKit: the unit is
// built on the XPC thread that asks for it, and the whole createAudioUnit path.
//
// The main-thread discipline in factory.mm's installPanel is about touching a
// NSViewController off the main thread, and there is no view here to touch, so it does not
// apply. Adding the UIKit view later means reintroducing it - see doc/ios-auv3.md.

#import <Foundation/Foundation.h>
#import <AudioToolbox/AudioToolbox.h>

#import "audio_unit.h"

@interface SMU2000FactoryV3 : NSObject <AUAudioUnitFactory>
@property (atomic, strong) SMU2000AudioUnitV3 *unit;
@end

@implementation SMU2000FactoryV3

// The extension is asked to make one AUAudioUnit. Its engine is what the panel draws,
// once there is a panel.
- (AUAudioUnit *)createAudioUnitWithComponentDescription:(AudioComponentDescription)desc
                                                   error:(NSError **)error
{
	self.unit = [[SMU2000AudioUnitV3 alloc] initWithComponentDescription:desc error:error];
	return self.unit;
}

@end