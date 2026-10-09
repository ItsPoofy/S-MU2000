// license:BSD-3-Clause
//
// The AUv3's screen contract. Same role as auv3/view_controller.h on macOS:
// one shared panel, two ways to ask for it.
//
// macOS decides by extension kind whether a host may ask for a screen at all,
// and the iOS rule is the same: only com.apple.AudioUnit-UI gets asked. In that
// kind the principal class is itself a view controller and the factory too -
// Apple's own template puts AUViewController + AUAudioUnitFactory in one class,
// and so does factory_ios.mm. This header is what both sides of that class use:
// the factory embeds this controller's view, and the AU answers
// requestViewController with it.

#ifndef S_MU2000_AUV3_VIEW_CONTROLLER_IOS_H
#define S_MU2000_AUV3_VIEW_CONTROLLER_IOS_H

#pragma once

#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudioKit/CoreAudioKit.h>

// The panel's engine. The VST3 type never appears here, so a forward
// declaration is enough - same as the mac twin's.
namespace smu2000 {
namespace vst3 {
class engine;
} // namespace vst3
} // namespace smu2000

// One panel. The contents are the UIView src/vst3/panel_uiview.mm makes - the
// same editor AUv2 and VST3 show on macOS, now hosted for UIKit. No image is
// held twice.
@interface SMU2000ViewControllerV3 : AUViewController
- (instancetype)initWithEngine:(smu2000::vst3::engine *)eng audioUnit:(AUAudioUnit *)au;
@end

#endif // S_MU2000_AUV3_VIEW_CONTROLLER_IOS_H