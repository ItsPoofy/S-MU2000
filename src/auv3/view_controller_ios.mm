// license:BSD-3-Clause
//
// The AUv3's screen, iOS side. Same panel the AUv2 and VST3 show on macOS,
// through iOS's asking order.
//
// The twin (view_controller.mm) documents the sharing: one engine, one editor,
// and the AUv3 answers requestViewController with the controller built here.
// The AU hands its engine over directly - it is born inside the extension, so
// no kEngineProperty indirection like the AUv2's. If the host drops the unit
// before the view (auval does), the panel outlives it through the owner rule
// in panel_uiview.h rather than dangling.

#import "view_controller_ios.h"

#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudioKit/CoreAudioKit.h>

#include "ui/file_ask_ios.h"
#include "vst3/engine.h"
#include "vst3/panel_uiview.h"

@implementation SMU2000ViewControllerV3 {
	smu2000::vst3::engine *_eng;
	// The machine itself; the panel view holds the same one. AUAudioUnit does
	// not refer back to this controller, so it is not a cycle
	AUAudioUnit *_au;
}

- (instancetype)initWithEngine:(smu2000::vst3::engine *)eng audioUnit:(AUAudioUnit *)au
{
	self = [super initWithNibName:nil bundle:nil];
	if (self) {
		_eng = eng;
		_au = au;
	}
	return self;
}

- (void)loadView
{
	// The editors in this panel have file requests, and this process answers them
	// with document pickers - which is what makes them offer buttons instead of
	// the path box they fall back to without dialogs. The standalone's main does
	// the same at start-up.
	ui::enable_file_dialogs();
	// CGSizeZero: the size is the panel's business (the AUv2 takes the host's
	// word for it instead)
	UIView *panel = _eng ? smu2000::vst3::make_panel_uiview(*_eng, CGSizeZero, _au) : nil;
	self.view = panel ? panel
	                  : [[UIView alloc] initWithFrame:CGRectMake(0, 0,
	                                                            smu2000::vst3::kPanelWidth,
	                                                            smu2000::vst3::kPanelHeight)];
	self.preferredContentSize = self.view.frame.size;
}

@end