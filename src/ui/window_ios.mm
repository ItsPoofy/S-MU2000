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
// The panel shell: touch as the mouse (second finger held is right-click),
// the Smart Keyboard as the panel keys, and the MIDI setup entries appended
// to the context menu. Text input lives in the editors (pc_window_ios.mm);
// this view never summons the software keyboard.

#import <UIKit/UIKit.h>
#import <QuartzCore/CADisplayLink.h>

#include <cstdio>

#include "ui/window_ios.h"

#include "ui/app_ios.h"
#include "ui/menu_ios.h"
#include "ios/keymap_ios.h"
#include "ios/midi_setup.h"
#include "ios/rom_import.h"

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
	// Keyboard focus follows the topmost surface: the panel holds it whenever
	// no modal is up, so a Smart Keyboard's keys reach app->key() without a
	// tap first. Editors and sheets take it while shown (their own views
	// become first responder); the guard hands it back when they close.
	// Edit menus need no keys and change nothing.
	if (self.window && ![self isFirstResponder]) {
		if (![self.window.rootViewController presentedViewController])
			[self becomeFirstResponder];
	}
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
// Touch is the mouse (press/drag/release, second finger held down is the
// right button); the Smart Keyboard is the panel keys. The view is first
// responder for the keyboard's sake only - it is not UIKeyInput, so no
// software keyboard ever shows for the panel. Key events become app->key()
// through the shared iOS map (ios/keymap_ios.h), the same codes
// window_mac.mm hands over: F2-F5 open windows, letters press panel buttons
// while held (key-up releases, which is why UIKeyCommand is not the
// mechanism - it reports no release).
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
// and the menu flag macOS turns into an NSMenu. On iOS it becomes an action
// sheet (showMenuAt:below), so a requested menu opens rather than being logged.
// Returns what the press did, so the caller can fall through to the other button
// when this one was inert (touchesBegan below).
- (ui::mouse_out)pressAt:(CGPoint)p right:(BOOL)right
{
	ui::mouse_out o;
	if (!self->app)
		return o;
	o = self->app->mouse_down((int)p.x, (int)p.y, right ? true : false);
	if (o.show_menu)
		[self showMenuAt:p];
	return o;
}

// The context menu, from the same menu_groups the desktop renders into HMENU
// and NSMenu, acted on through the same menu_chosen(id).
//
// A UIEditMenuInteraction presents it: the compact anchored bubble iOS uses for
// pop-up menus (checkmarks, real submenus, no Cancel row). The menu is the
// app's own - every group in context_menu() rendered in order, titled groups
// nested with chevrons exactly as NSMenu nests them on macOS. The presenting
// lives in the shared menu_ios helper; see it for the one documented difference
// (UIMenu has no separator element, so separator items render as nothing).
- (void)showMenuAt:(CGPoint)p
{
	ui::gui_app *theApp = self->app;
	if (!theApp)
		return;
	std::vector<ui::menu_group> groups = theApp->context_menu((int)p.x, (int)p.y);
	// The app's menu first, then the iOS-only groups: ROM import (the images
	// are never in a distributable build, so the user brings them) and
	// Bluetooth/network MIDI setup, which have no desktop counterpart - macOS
	// does both in Audio MIDI Setup. Appended, never regrouped.
	append_rom_import_group(groups);
	append_midi_setup_group(groups);
	if (groups.empty())
		return;
	UIView *here = self;
	show_menu_groups(self, p, groups, [theApp, here, p](int itemId) {
		if (handle_rom_import_item(here, itemId))
			return;
		if (handle_midi_setup_item(here, p, itemId))
			return;
		theApp->menu_chosen(itemId);
	});
}

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
	(void)event;
	// Tapping the panel takes keyboard focus with it: the Smart Keyboard's
	// keys go to the first responder, and nothing else here wants them while
	// no editor or sheet is up. Harmless when already first responder.
	[self becomeFirstResponder];
	// First finger takes button 0 and the pointer; a second finger while it is
	// held takes button 1. Two fingers landing in the same event both read as
	// begun: the first enumerated takes the pointer, which is arbitrary but
	// harmless - a two-finger tap still opens the menu either way.
	for (UITouch *t in touches) {
		const CGPoint p = [t locationInView:self];
		if (!self->leftTouch) {
			self->leftTouch = t;
			[self pushTouch:t down:YES right:NO];
			// A tap where the left button does nothing falls through to the
			// right button: on desktop the jacks already open their menus on
			// plain left-click, and a two-finger tap is hard to land on small
			// buttons. Only when the left press was fully inert (no control
			// pressed, no window, no menu) - a pressed button must not also
			// pop a menu. The left press happened first but did nothing
			// observable, and its release below clears it.
			ui::mouse_out o = [self pressAt:p right:NO];
			if (!o.panel_pressed && !o.opened_window && !o.show_menu)
				[self pressAt:p right:YES];
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

// ---- keyboard ---------------------------------------------------------------

// First responder for hardware keys only. No UIKeyInput conformance here, so
// becoming first responder never summons the software keyboard - the panel
// has no text fields. (The editors' views are UIKeyInput and take over while
// a field is active; this reclaims on the next tap.)
- (BOOL)canBecomeFirstResponder
{
	return YES;
}

- (void)didMoveToWindow
{
	[super didMoveToWindow];
	// Leaving the window with a panel button latched (key still held) would
	// stick it until pressed again: let go of everything on the way out.
	if (!self.window && self->app)
		self->app->focus_lost();
}

// One press to app->key(), down and up. Unmapped keys fall through to super
// (responder chain, then the system beep) rather than being swallowed: this
// view owns no keys except the panel's.
- (void)pushPress:(UIPress *)press down:(BOOL)down
{
	const int code = panel_code_from_press(press);
	if (code >= 0 && self->app)
		self->app->key(code, down ? true : false);
}

- (void)pressesBegan:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)event
{
	BOOL handled = NO;
	for (UIPress *p in presses) {
		if (panel_code_from_press(p) >= 0) {
			[self pushPress:p down:YES];
			handled = YES;
		}
	}
	if (!handled)
		[super pressesBegan:presses withEvent:event];
}

- (void)pressesEnded:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)event
{
	BOOL handled = NO;
	for (UIPress *p in presses) {
		if (panel_code_from_press(p) >= 0) {
			[self pushPress:p down:NO];
			handled = YES;
		}
	}
	if (!handled)
		[super pressesEnded:presses withEvent:event];
}

- (void)pressesCancelled:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)event
{
	(void)event;
	// A key held across an interruption must come up, or its panel button
	// latches until pressed again.
	for (UIPress *p in presses)
		[self pushPress:p down:NO];
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