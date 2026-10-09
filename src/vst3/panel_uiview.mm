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
// The plug_view created for this frame, kept so dealloc can give it back. Without
// this the pointer is dropped on the floor when make_panel_uiview() returns and
// plug_view is never destroyed: removed() - which stops the display link and
// flushes the card - never runs, so the link keeps ticking against a UI the host
// has closed and the card is not written back. panel_nsview.mm holds it the same
// way, for the same reason.
@property (nonatomic, assign) smu2000::vst3::plug_view *plug;
@end

@implementation SMUPlugFrame

- (void)dealloc
{
	// The engine (owner, above) is still alive here, which is the order this
	// teardown needs. plug_view counts its own references (FUnknown's addRef /
	// release); make_panel_uiview took one, and this gives it back.
	if (_plug)
		_plug->release();
}

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
	// And the plug_view outlives the view: assigned after attached() succeeded,
	// because a failed attach releases it below and must not leave a dangling
	// pointer for dealloc to release again.
	((SMUPlugFrame *)view).plug = plug;
	eng.log_line("画面ができた");
	return view;
}

} // namespace vst3
} // namespace smu2000
