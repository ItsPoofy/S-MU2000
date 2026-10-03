// license:BSD-3-Clause
//
// The iOS window: a UIView with a CAMetalLayer, painting the same panel the macOS
// window paints. The drawing is shared; only the shell here is iOS's.
//
// ui/imgui_shell.h already isolates the platform view: metal_paint, metal_stop,
// new_context and panel_fonts take a layer or nothing and speak no AppKit, and
// metal_attach/metal_sync have iOS branches. So this file is the equivalent of
// window_mac.mm's shell - tick, resize, paint - without the AppKit input, menus and
// drag-and-drop that file also carries.
//
// Step 1 of the iOS port: pixels only. No touch, no keyboard, no audio. What it
// answers is whether the ImGui panel renders at all on iOS, which is the same
// question for the app and for the AUv3 because both paint through panel.cpp.

#import <UIKit/UIKit.h>
#import <QuartzCore/CADisplayLink.h>

#include <cstdio>

#include "ui/window_ios.h"

#include "ui/app_ios.h"

// imgui_shell.h imports Metal and QuartzCore itself now, so nothing here has to
// remember to do it first - which is what ui/app.h -> ui/shot.h -> imgui_shell.h
// made necessary: this header is reached from the shared front end, not just from
// this file.
#include "ui/imgui_shell.h"

// SMUView is declared here rather than in the header for the reason window_ios.h
// gives: an @interface cannot live inside a namespace, and this file is already the
// only place that needs the type.
//
// @interface rather than a C++ class on purpose: CADisplayLink retains its target,
// and a C++ object with a non-trivial destructor cannot safely be that target.
// Objective-C gives correct ownership here for free.
@interface SMUView : UIView
{
@public
	// Read by the paint lambda below, which is a C++ block rather than a method.
	// Qualified because this @interface is at global scope: an ObjC declaration cannot
	// sit inside a namespace, so unlike window_mac.mm's SMUView there is no
	// namespace-scope using-directive to lean on here.
	ui::gui_app *app;
	ImGuiContext *ctx;
	ui::im::fonts fonts;
	CADisplayLink *link;
	int frames;
@private
	// Which finger drives which button. The first finger down takes button 0 and
	// the pointer; a second finger while it is held takes button 1. Remembering
	// this per touch is what makes each finger release the button it pressed:
	// counting "fingers still down" at release time gets it wrong when the first
	// finger lifts while the second is still held. Under ARC these are __strong
	// by default, so assigning retains and nil-ing releases - no lifecycle code.
	UITouch *leftTouch;
	UITouch *rightTouch;
}
- (instancetype)initWithFrame:(CGRect)frame app:(ui::gui_app *)a;
// The device and queue stay in the class extension below: they are Metal objects,
// nothing outside this file needs them, and an ivar would want them forward-declared.
- (BOOL)start;
- (void)startLink;
- (void)stopLink;
- (void)tick:(CADisplayLink *)link;
@end

@implementation SMUView {
	id<MTLDevice> _device;
	id<MTLCommandQueue> _queue;
	CAMetalLayer *_layer;
}

// +layerClass rather than setting the layer afterwards. UIView honours this
// override - it is the documented way to get a Metal-backed view - so the drawable
// is the view's own backing store and there is nothing to host or resize.
+ (Class)layerClass
{
	return [CAMetalLayer class];
}

- (instancetype)initWithFrame:(CGRect)frame app:(ui::gui_app *)a
{
	self = [super initWithFrame:frame];
	if (self) {
		self->app = a;
		self->_layer = (CAMetalLayer *)self.layer;
		// masksToBounds only. There is deliberately no gravity here: that is a
		// CAGravityLayer property, and it positions *sub*layers inside a layer,
		// which is not what this is. On macOS metal_attach builds a layer and hands
		// it to the view, so something has to size it; on iOS +layerClass makes the
		// layer the view's own backing store, and its bounds follow the view's frame
		// by themselves. drawableSize is the only thing layoutSubviews has to sync.
		self.layer.masksToBounds = YES;
		// Multitouch is off by default on UIView, and the two-finger right-click
		// below needs the second finger to arrive at all. Without this, taps work
		// but context menus (the MIDI port picker among them) are unreachable.
		self.multipleTouchEnabled = YES;
		self.backgroundColor = UIColor.blackColor;
	}
	return self;
}

- (BOOL)start
{
	// imgui_shell's iOS branch: the layer is already ours, so this makes the device
	// and queue and checks the pixel format took.
	if (!ui::imshell::metal_attach(self, _device, _queue))
		return NO;

	self->ctx = ui::imshell::new_context();
	ImGui_ImplMetal_Init(_device);
	self->fonts = ui::imshell::panel_fonts();
	// panel.cpp rasterises its own six sizes on resize, so hand it the first size
	// here rather than waiting for a layout pass.
	if (self->app)
		self->app->resized((int)self.bounds.size.width, (int)self.bounds.size.height);
	return YES;
}

- (void)layoutSubviews
{
	[super layoutSubviews];
	// The size log is the resize diagnostic: the panel overflowed right and bottom
	// with black only on top, which means scale/offset were computed for a bigger
	// view than the window (stale resize dimensions). If this never fires on
	// rotation or Stage Manager resize, that is the bug, not the panel math.
	std::fprintf(stderr, "[ios] layoutSubviews: %.0f x %.0f @%.0f\n",
	             self.bounds.size.width, self.bounds.size.height, self.contentScaleFactor);
	ui::imshell::metal_sync(self->_layer, self);
	if (self->app)
		self->app->resized((int)self.bounds.size.width, (int)self.bounds.size.height);
}

// The 30 Hz tick, standing in for window_mac.mm's NSTimer.
//
// CADisplayLink rather than a timer for one reason that matters here: it fires once
// per screen refresh and, unlike a timer, keeps firing while the user drags or
// scrolls, so the panel does not freeze mid-gesture. PreferredFramesPerSecond is 30
// to match the desktop front ends; the panel is not worth 120 Hz.
- (void)startLink
{
	self->link = [CADisplayLink displayLinkWithTarget:self selector:@selector(tick:)];
	self->link.preferredFramesPerSecond = 30;
	[self->link addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
}

- (void)stopLink
{
	[self->link invalidate];
	self->link = nil;
}

- (void)tick:(CADisplayLink *)sender
{
	(void)sender;
	if (!self->ctx || !self->_layer)
		return;
	// The paint lambda is the mac window's, unchanged: app->paint_main does
	// frame_work() (the panel tick and the PC windows) and then the picture. Nothing
	// iOS-specific happens in here, which is the whole point - the panel does not
	// know what it is being drawn on.
	//
	// pump_realtime() first, so the frame shows a living machine: without it the SH2
	// executes nothing after boot() returns (there is no audio callback to drive it
	// yet) and the LCD freezes on whatever boot drew last. See app_ios.h.
	if (app)
		app->pump_realtime();
	const CGRect b = self.bounds;
	const CGFloat scale = self.contentScaleFactor;
	// Trailing underscores, because these locals shadow the @public ivars of the
	// same name and -Wshadow-ivar says so. The ivars stay as they are: app.mm never
	// touches them, and renaming the interface would churn the header for nothing.
	ui::gui_app *app_ = self->app;
	ui::im::fonts fonts_ = self->fonts;
	ImGuiContext *ctx_ = self->ctx;
	ui::imshell::metal_paint(ctx_, self->_layer, _queue,
	                     (float)b.size.width, (float)b.size.height, (float)scale,
	                     ^(ImDrawList *dl) {
		                     app_->paint_main(dl, fonts_, (int)b.size.width);
	                     });
	self->frames++;
}

// ---- touch ------------------------------------------------------------------
//
// The only input step 1 has: one finger is the mouse (press/drag/release),
// a second finger held down is the right button. No keyboard yet - the panel's
// single-key shortcuts (A=PLAY, E=EDIT, ...) and the editors' text fields need
// UIKeyCommand / UITextInput, which is the next piece, not this one.
//
// Two sinks, because the UI has two halves. The panel itself is raw ImDrawList,
// not ImGui widgets, so it only hears ui::app verbs - mouse_down/mouse_drag/
// mouse_up, exactly what window_mac.mm calls. Setting io.MouseDown alone does
// nothing for it (that was the first version of this code, and taps did
// nothing). io.MousePos/MouseDown are still fed as well: the toolbar strip and
// the future editors ARE ImGui widgets and will need them.
//
// Coordinates pass through in points, the same units macOS passes in view
// coordinates: the panel maps screen->logical itself (panel::at()/scale()), so
// there is no conversion to get wrong.
- (void)pushTouch:(UITouch *)t down:(BOOL)down right:(BOOL)right
{
	if (!self->ctx)
		return;
	ImGui::SetCurrentContext(self->ctx);
	ImGuiIO &io = ImGui::GetIO();
	if (!right) {
		const CGPoint p = [t locationInView:self];
		io.MousePos = ImVec2((float)p.x, (float)p.y);
		io.MouseDown[0] = down ? true : false;
	} else {
		io.MouseDown[1] = down ? true : false;
	}
}

// The app-verb half of a press: panel.press/drag through mouse_down/mouse_drag,
// and the menu flag macOS turns into an NSMenu. There is no native menu on iOS
// yet (UIContextMenuInteraction or an action sheet), so a requested menu is
// logged rather than dropped silently - dropping it would look exactly like the
// tap doing nothing, which is what sent us here.
- (void)pressAt:(CGPoint)p right:(BOOL)right
{
	if (!self->app)
		return;
	ui::mouse_out o = self->app->mouse_down((int)p.x, (int)p.y, right ? true : false);
	if (o.show_menu)
		std::fprintf(stderr, "[ios] menu requested at %.0f,%.0f (no native menu yet)\n",
		             p.x, p.y);
	(void)o;
}

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
	(void)event;
	// First finger takes button 0 and the pointer; a second finger while it is
	// held takes button 1. Two fingers landing in the same event both read as
	// begun: the first enumerated takes the pointer, which is arbitrary but
	// harmless - a two-finger tap still opens the menu either way.
	for (UITouch *t in touches) {
		const CGPoint p = [t locationInView:self];
		if (!self->leftTouch) {
			self->leftTouch = t;
			[self pushTouch:t down:YES right:NO];
			[self pressAt:p right:NO];
		} else if (!self->rightTouch && t != self->leftTouch) {
			self->rightTouch = t;
			[self pushTouch:t down:YES right:YES];
			[self pressAt:p right:YES];
		}
	}
}

- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
	(void)event;
	// Only button 0 tracks movement; a drifting second finger must not throw
	// the pointer across the panel while a menu is open.
	for (UITouch *t in touches) {
		if (t != self->rightTouch) {
			[self pushTouch:t down:YES right:NO];
			if (self->app) {
				const CGPoint p = [t locationInView:self];
				self->app->mouse_drag((int)p.x, (int)p.y);
			}
		}
	}
}

- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
	(void)event;
	// Each finger releases the button it pressed, which the per-touch ivars
	// above are for. The app itself is single-press like the mac (mouse_up takes
	// no coordinates and clears the press), so a finger still held after the
	// other lifts must press again to act - documented, not ideal, and the same
	// shape as two overlapping clicks on desktop.
	for (UITouch *t in touches) {
		if (t == self->rightTouch) {
			[self pushTouch:t down:NO right:YES];
			self->rightTouch = nil;
		} else if (t == self->leftTouch) {
			[self pushTouch:t down:NO right:NO];
			self->leftTouch = nil;
		}
	}
	if (self->app)
		self->app->mouse_up();
}

- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
	// A cancelled touch (phone call, gesture recognizer stealing it) must release
	// both buttons it could be holding. Failing to do this leaves a stuck button
	// that keeps "pressing" whatever is under the pointer until the next tap.
	(void)touches;
	(void)event;
	if (self->app)
		self->app->mouse_up();
	if (!self->ctx)
		return;
	ImGui::SetCurrentContext(self->ctx);
	ImGuiIO &io = ImGui::GetIO();
	io.MouseDown[0] = false;
	io.MouseDown[1] = false;
	self->leftTouch = nil;
	self->rightTouch = nil;
}

- (void)dealloc
{
	[self stopLink];
	// No local and no [super dealloc]: metal_stop takes the context by reference
	// and nulls it, so the ivar can go straight in - and under ARC (which the iOS
	// ObjC++ files now use, like every other ObjC++ file here) calling super
	// dealloc is an error rather than an omission. The warning that used to fire
	// here (-Wobjc-missing-super-calls) was really saying this file was built
	// without ARC, which was never intended.
	if (self->ctx)
		ui::imshell::metal_stop(self->ctx);
}

@end

namespace ui {

// Not blocking, unlike run_window(): UIKit owns the run loop, and a display link
// added to the main run loop drives the panel from inside it. So this makes the view,
// starts the link, and hands the view back - the caller attaches it and lets UIKit
// run.
UIView *make_ios_view(gui_app &a, int w, int h)
{
	SMUView *v = [[SMUView alloc] initWithFrame:CGRectMake(0, 0, w, h) app:&a];
	if (![v start]) {
		NSLog(@"[ios] no Metal device: the panel cannot be drawn");
		return nil;
	}
	[v startLink];
	return v;
}

} // namespace ui