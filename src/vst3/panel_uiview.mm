// license:BSD-3-Clause
//
// The panel in a UIView frame. See panel_uiview.h for why this is its own
// file: it mirrors panel_nsview.mm (frame + make_panel_view there, frame +
// make_panel_uiview here). The Metal view, input, card menu and window live
// in view_ios.mm, mirroring view_mac.mm.

#import "panel_uiview.h"

#include "engine.h"
#include "plug_window.h"
#include "view.h"

#import <UIKit/UIKit.h>

#import <TargetConditionals.h>

@interface SMUPlugFrame : UIView
@property (nonatomic, strong) id owner;
@end
@implementation SMUPlugFrame
@end

namespace smu2000 {
namespace vst3 {

UIView *make_panel_uiview(engine &eng, CGSize preferred, id owner)
{
	eng.log_line("画面を作る");
	plug_view *plug = new plug_view(eng);

	int w = (int)preferred.width;
	int h = (int)preferred.height;
	if (w < kPanelMinW || h < kPanelMinH) {
		w = kPanelWidth;
		h = kPanelHeight;
	}
	// onSize clamps the way the VST3 host's size is clamped, and resizes the
	// panel to match, so the frame below is what the panel was laid out for
	Steinberg::ViewRect r(0, 0, w, h);
	plug->onSize(&r);
	w = plug->width();
	h = plug->height();

	UIView *view = [[SMUPlugFrame alloc] initWithFrame:CGRectMake(0, 0, w, h)];
	view.backgroundColor = UIColor.blackColor;
	// Follow later host resizes too, not just the creation size: without a
	// flexible mask a host that shrinks the view gets a panel drawn for the old
	// size (same stale-dimensions overflow as the standalone had before its
	// constraints). layoutSubviews above then refits from the true bounds.
	view.autoresizingMask = UIViewAutoresizingFlexibleWidth |
	                        UIViewAutoresizingFlexibleHeight;
	// attached() answers a VST3 tresult, where kResultOk is 0 -- so this is a
	// comparison and not a truth test, or a view that attached perfectly would
	// be thrown away
	const Steinberg::tresult ar = plug->attached((__bridge void *)view, plug_window_type());
	if (ar != Steinberg::kResultOk) {
		char b[64];
		std::snprintf(b, sizeof(b), "画面を貼れない: %d", int(ar));
		eng.log_line(b);
		plug->release();
		return nil;
	}
	// So the panel outlives the engine it draws (the owner rule from the header:
	// the view holds it strong, teardown writes back through it)
	((SMUPlugFrame *)view).owner = owner;
	eng.log_line("画面ができた");
	return view;
}

} // namespace vst3
} // namespace smu2000
