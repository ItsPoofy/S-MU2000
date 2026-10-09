// license:BSD-3-Clause
//
// The machine's front panel inside a UIView, for the iOS AUv3.
//
// The twin of src/vst3/panel_nsview.h (NSView, macOS plug-in formats): one editor
// for one open instance, the same smu2000::vst3::plug_view inside, the same engine
// and the same editor. What differs is only how a host asks for a view, which is
// why the three formats - and now iOS - share one engine and one editor.
//
// The sizes are the same numbers as panel_nsview.h (kPanelWidth/Height/MinW/MinH),
// duplicated rather than included: that header imports Cocoa and cannot be read
// on iOS. If those numbers ever change, change them here too - the comment on
// each says so.
//
// Objective-C++ only. plug_view's entry points are opaque (void *) for the same
// reason as the NSView twin's.

#ifndef S_MU2000_VST3_PANEL_UIVIEW_H
#define S_MU2000_VST3_PANEL_UIVIEW_H

#pragma once

#import <UIKit/UIKit.h>

namespace smu2000 {
namespace vst3 {

class engine;

// The panel's own size, and the smallest a host may ask for before it is given
// the panel's size rather than a squeezed one. Same numbers and same clamping
// as panel_nsview.h (VST3 onSize/checkSizeConstraint there); duplicated because
// that header imports Cocoa. Change both together.
constexpr int kPanelWidth  = 1000;
constexpr int kPanelHeight = 400;
constexpr int kPanelMinW   = 640;
constexpr int kPanelMinH   = 180;

// One editor for one open instance, or nil if it cannot be built. Every call
// makes a view, which is what the AUv3 view controller needs. `preferred`
// smaller than the minimum above means "you choose", and the panel's own size
// is used - CGSizeZero says so plainly.
//
// `owner` is whatever owns `eng`, and the view holds a strong reference to it
// for as long as it lives (same rule as the NSView twin: a host may let go of
// the plug-in before the view it was handed, and teardown writes back through
// the engine, so the view must outlive it).
UIView *make_panel_uiview(engine &eng, CGSize preferred, id owner);

} // namespace vst3
} // namespace smu2000

#endif // S_MU2000_VST3_PANEL_UIVIEW_H