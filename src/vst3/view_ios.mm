// license:BSD-3-Clause
//
// The panel's Metal view and its window, for iOS. Mirrors view_mac.mm's
// role (SMUPlugView + mac_window there, SMUPlugUIView + ios_window here):
// layer-hosted Metal + Dear ImGui at 30 Hz, touch into plug verbs, the
// card menu and alert in UIKit, the five PC windows through the iOS host.
// panel_uiview.mm holds only the frame + make_panel_uiview, the way
// panel_nsview.mm holds only the frame + make_panel_view.

// UIKit and Metal before everything C++: imgui_shell.h's inline helpers use
// UIView/CAMetalLayer directly (not forward declarations), and inline bodies
// check at definition - so the frameworks must already be imported. Same order
// as window_ios.mm, which documents the rule.
#import <UIKit/UIKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CADisplayLink.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#import <TargetConditionals.h>

#include "engine.h"
#include "plug_window.h"
#include "view.h"

#include "ios/rom_import.h"

#include "ui/font_file.h"
#include "ui/fx_editor.h"
#include "ui/imgui_shell.h"
#include "ui/master_editor.h"
#include "ui/sampling_editor.h"
#include "ui/menu.h"
#include "ui/menu_ios.h"
#include "ui/overview.h"
#include "ui/part_shapes.h"
#include "ui/pc_editor.h"
#include "ui/pc_host.h"
#include "ui/pc_window.h"

// The panel's view. +layerClass Metal, display link at the same 30 Hz the mac
// timer and the standalone use, so all three panels animate alike. Panel input
// stays shared, in the same top-left, y-down space the hit testing is written
// in (UIKit is y-down natively, so unlike the NSView twin there is nothing to
// flip).
@interface SMUPlugUIView : UIView
{
@public
	smu2000::vst3::plug_view *_owner;
	ImGuiContext *_ctx;
	ui::im::fonts _fonts;
	CADisplayLink *_link;
	int _taps;
}
- (instancetype)initWithOwner:(smu2000::vst3::plug_view *)owner
                       width:(int)w
                      height:(int)h;
- (void)tick:(CADisplayLink *)link;
@end

@implementation SMUPlugUIView {
	id<MTLDevice> _device;
	id<MTLCommandQueue> _queue;
	CAMetalLayer *_layer;
}

+ (Class)layerClass
{
	return [CAMetalLayer class];
}

- (instancetype)initWithOwner:(smu2000::vst3::plug_view *)owner
                       width:(int)w
                      height:(int)h
{
	self = [super initWithFrame:CGRectMake(0, 0, w, h)];
	if (self) {
		self->_owner = owner;
		self.multipleTouchEnabled = YES;
		self.backgroundColor = UIColor.blackColor;
	}
	return self;
}

- (BOOL)startImgui
{
	if (!ui::imshell::metal_attach(self, _device, _queue))
		return NO;
	self->_layer = (CAMetalLayer *)self.layer;
	self->_ctx = ui::imshell::new_context();
	ImGui_ImplMetal_Init(_device);
	self->_fonts = ui::imshell::panel_fonts();
	if (self->_owner)
		self->_owner->log_line("panel timer: imgui on");
	return YES;
}

- (void)layoutSubviews
{
	[super layoutSubviews];
	ui::imshell::metal_sync(self->_layer, self);
	if (self->_owner) {
		// Follow the host: GarageBand sizes this view, and the panel must hear
		// the real size. plug_view::onSize clamps to its 640x180 minimum and
		// resizes the shared ui::panel to match - without this call the panel
		// keeps its creation size (1000x400) and overflows smaller host areas,
		// cut right and bottom. Below 640 wide it still overflows (the minimum
		// is the shared VST3 contract, not iOS's to change); iPads are wider.
		const CGRect b = self.bounds;
		Steinberg::ViewRect r(0, 0, (Steinberg::int32)b.size.width,
		                      (Steinberg::int32)b.size.height);
		self->_owner->onSize(&r);
	}
}

- (void)startLink
{
	if (_link)
		return;
	_link = [CADisplayLink displayLinkWithTarget:self selector:@selector(tick:)];
	_link.preferredFramesPerSecond = 30;
	[_link addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
}

- (void)stopLink
{
	[_link invalidate];
	_link = nil;
}

- (void)tick:(CADisplayLink *)link
{
	(void)link;
	if (!self->_ctx || !self->_layer || !self->_owner)
		return;
	const CGRect b = self.bounds;
	const CGFloat scale = self.contentScaleFactor;
	smu2000::vst3::plug_view *owner = self->_owner;
	ui::im::fonts fonts = self->_fonts;
	// Same call the mac twin makes: the plug_view repaints into the background
	// list through ui::imshell::metal_paint, which is shared as it stands.
	ui::imshell::metal_paint(self->_ctx, self->_layer, _queue,
	                         (float)b.size.width, (float)b.size.height, (float)scale,
	                         ^(ImDrawList *dl) {
		                         owner->repaint(dl, fonts,
		                                        (int)b.size.width, (int)b.size.height);
	                         });
}

// The first taps, logged like the mac twin's noteClick: when touch does not
// reach the panel, the question is always whether the gesture arrived, on which
// thread, and on what view - so the answer is logged rather than reconstructed.
- (void)noteTap:(UITouch *)t
{
	if (!_owner || _taps >= 8)
		return;
	_taps++;
	UIView *hit = [self hitTest:[t locationInView:self.superview] withEvent:nil];
	char b[200];
	std::snprintf(b, sizeof(b), "タップ %d 回目: 主の糸 %s、当たった先 %s",
	              _taps, [NSThread isMainThread] ? "yes" : "no",
	              hit ? NSStringFromClass([hit class]).UTF8String : "なし");
	_owner->log_line(b);
}

// One finger presses, drags and releases; a second finger while held is the
// right button (the card slot answers it). plug_view has a separate mouse_right
// verb, unlike ui::app's right flag - and unlike the standalone there is no
// hit-test result to fall back on, so a lone tap never retries as right: small
// buttons keep the two-finger gesture, documented as such.
- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
	if (!_owner)
		return;
	BOOL second = NO;
	for (UITouch *t in [event allTouches]) {
		if (t.phase == UITouchPhaseBegan && ![touches containsObject:t])
			second = YES;
	}
	if ([touches count] > 1)
		second = YES;
	for (UITouch *t in touches) {
		[self noteTap:t];
		const CGPoint p = [t locationInView:self];
		if (second)
			_owner->mouse_right((int)p.x, (int)p.y);
		else
			_owner->mouse_down((int)p.x, (int)p.y);
	}
}

- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
	(void)event;
	if (!_owner)
		return;
	for (UITouch *t in touches) {
		const CGPoint p = [t locationInView:self];
		_owner->mouse_drag((int)p.x, (int)p.y);
	}
}

- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
	(void)touches;
	(void)event;
	if (!_owner)
		return;
	_owner->mouse_up();
}

- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
	(void)touches;
	(void)event;
	if (!_owner)
		return;
	_owner->mouse_up();
}

- (void)dealloc
{
	[self stopLink];
	if (self->_ctx) {
		ImGuiContext *ctx = self->_ctx;
		ui::imshell::metal_stop(ctx);
		self->_ctx = nullptr;
	}
}

@end

// The topmost presenter, for alerts, pickers and sheets: the foreground scene's
// key window, topmost presented. Same need as pc_window_ios.mm's helper, local
// for the same reason (no shared UIKit-glue header exists yet).
static UIViewController *top_presenter()
{
	UIViewController *top = nil;
	for (UIScene *scene in [UIApplication sharedApplication].connectedScenes) {
		if ([scene activationState] != UISceneActivationStateForegroundActive)
			continue;
		if (![scene isKindOfClass:[UIWindowScene class]])
			continue;
		for (UIWindow *w in [(UIWindowScene *)scene windows]) {
			if ([w isKeyWindow]) {
				top = w.rootViewController;
				break;
			}
		}
		if (top)
			break;
	}
	while (top && [top presentedViewController])
		top = [top presentedViewController];
	return top;
}


@interface SMUCardPicker
    : NSObject <UIDocumentPickerDelegate>
@end

@implementation SMUCardPicker {
	std::function<void(const std::string &)> _done;
}

- (instancetype)initWithCallback:(std::function<void(const std::string &)> )done
{
	self = [super init];
	if (self)
		_done = done;
	return self;
}

// A picked file lives outside the sandbox: take security-scoped access, copy it
// into the container (the engine reads it by path long after the picker is
// gone), then hand the container path over. Without the copy the card would
// break the moment the scope lapses.
- (void)documentPicker:(UIDocumentPickerViewController *)controller
    didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls
{
	(void)controller;
	NSURL *url = [urls firstObject];
	if (!url) {
		if (_done)
			_done(std::string());
		return;
	 }
	std::string dest;
	if ([url startAccessingSecurityScopedResource]) {
		NSString *docs = [NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,
		                                                      NSUserDomainMask, YES)
		    firstObject];
		NSString *dir = [docs stringByAppendingPathComponent:@"SMU2000"];
		[[NSFileManager defaultManager] createDirectoryAtPath:dir
		                          withIntermediateDirectories:YES
		                                           attributes:nil
		                                                error:nil];
		NSString *name = [url lastPathComponent];
		if (!name || [name length] == 0)
			name = @"smartmedia.img";
		NSString *dst = [dir stringByAppendingPathComponent:name];
		[[NSFileManager defaultManager] removeItemAtPath:dst error:nil];
		NSError *e = nil;
		if ([[NSFileManager defaultManager] copyItemAtURL:url
		                                            toURL:[NSURL fileURLWithPath:dst]
		                                            error:&e])
			dest = [dst UTF8String];
		[url stopAccessingSecurityScopedResource];
	}
	if (_done)
		_done(dest);
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)controller
{
	(void)controller;
	if (_done)
		_done(std::string());
}

@end


// The frame the host is handed: a plain view holding the strong owner reference
// the lifetime rule needs (mac's SMU2000PanelView carries the same property).
// The panel itself lives in the plug_window's subview, not here.

namespace smu2000 {
namespace vst3 {

// Document pick for card images: UIDocumentPickerViewController cannot be a C++
// object, so this one holds the callback the C++ side gives it.


class ios_window : public plug_window
{
public:
	explicit ios_window(plug_view &owner) : m_owner(owner) {}
	~ios_window() override { detach(); }

	bool attach(void *parent, int w, int h) override;
	void detach() override;
	void set_size(int w, int h) override;
	void card_menu(int x, int y) override;
	// The plug-in's own right-click menu, which plug_window leaves empty by
	// default ("a plug-in has no settings of its own"). On iOS it has one:
	// installing the ROM images, which are never in a distributable build.
	void panel_menu(int x, int y) override;
	void alert(const std::string &text) override;
	void pc_frame(::xg::model &m, const ::ui::xg_snapshot &ram,
	              ::ui::bridge &br) override;
	void open_pc_window(int kind) override;

	void pickCardItem(int itemId);

private:
	plug_view &m_owner;
	SMUPlugUIView *m_view = nil;
	// The PC windows gui.exe shows (overview, editor, insertion, part voice,
	// master). Same content as on macOS and Windows; only the hosting differs -
	// and the host is ui::pc_window's iOS twin (pc_window_ios.mm), so opening
	// one here presents it exactly as the standalone does.
	ui::pc_window m_list{ std::make_unique<ui::overview>() };
	ui::pc_window m_editor{ std::make_unique<ui::pc_editor>() };
	ui::pc_window m_fx{ std::make_unique<ui::fx_editor>() };
	ui::pc_window m_shapes{ std::make_unique<ui::part_shapes>() };
	ui::pc_window m_master{ std::make_unique<ui::master_editor>() };
	ui::pc_window m_sampling{ std::make_unique<ui::sampling_editor>() };

	void open_pc(ui::pc_window &w);
	void show_alert(const char *title, const std::string &text);
};

bool ios_window::attach(void *parent, int w, int h)
{
	if (m_view || !parent)
		return false;

	// The host hands over its view as a bare pointer; on this platform that is
	// a UIView (kPlatformTypeUIView). Never nil in practice, but a bad pointer
	// here would fault rather than fail, so check the obvious.
	UIView *host = (__bridge UIView *)parent;
	if (!host || ![host isKindOfClass:[UIView class]])
		return false;

	m_view = [[SMUPlugUIView alloc] initWithOwner:&m_owner width:w height:h];
	if (!m_view)
		return false;

	// Subview, not the content view: the host owns the window and may put other
	// things around us. Anchored on all sides so resizes track the host.
	m_view.translatesAutoresizingMaskIntoConstraints = NO;
	[host addSubview:m_view];
	[NSLayoutConstraint activateConstraints:@[
		[m_view.leadingAnchor constraintEqualToAnchor:host.leadingAnchor],
		[m_view.trailingAnchor constraintEqualToAnchor:host.trailingAnchor],
		[m_view.topAnchor constraintEqualToAnchor:host.topAnchor],
		[m_view.bottomAnchor constraintEqualToAnchor:host.bottomAnchor],
	]];
	// No renderer, no custom view: the host falls back to generic
	// parameters, the way the headless Linux build answers
	if (![m_view startImgui]) {
		[m_view removeFromSuperview];
		m_view = nil;
		return false;
	}
	[m_view startLink];
	return true;
}

void ios_window::detach()
{
	if (!m_view)
		return;
	[m_view stopLink];
	[m_view removeFromSuperview];
	m_view = nil;
}

void ios_window::set_size(int w, int h)
{
	(void)w;
	(void)h;
	// Anchors track the host, so there is nothing to set: the display link
	// reads bounds every frame. (The NSView twin resizes explicitly because
	// AppKit frames do not follow.)
}

void ios_window::open_pc(ui::pc_window &w)
{
	std::string err;
	if (!w.show(err))
		alert(err.empty() ? std::string("the window cannot be opened") : err);
}

// Driven at the panel's repaint rate. Hidden windows cost nothing
void ios_window::pc_frame(::xg::model &m, const ::ui::xg_snapshot &ram, ::ui::bridge &br)
{
	ui::pc_frame_all(m_list, m_editor, m_fx, m_shapes, m_master, m_sampling, m, ram, br,
	                 [this](ui::pc_window &w) { open_pc(w); });
}

void ios_window::open_pc_window(int kind)
{
	open_pc(*pc_window_for_kind(kind, m_list, m_editor, m_fx, m_shapes, m_master, m_sampling));
}

void ios_window::alert(const std::string &text)
{
	show_alert("S-MU2000", text);
}

void ios_window::show_alert(const char *title, const std::string &text)
{
	UIViewController *vc = top_presenter();
	if (!vc)
		return;
	NSString *t = title ? [NSString stringWithUTF8String:title] : @"S-MU2000";
	NSString *m = [NSString stringWithUTF8String:text.c_str()];
	UIAlertController *a = [UIAlertController alertControllerWithTitle:t
	                                                           message:m
	                                                    preferredStyle:UIAlertControllerStyleAlert];
	[a addAction:[UIAlertAction actionWithTitle:@"OK"
	                                      style:UIAlertActionStyleDefault
	                                    handler:nil]];
	[vc presentViewController:a animated:YES completion:nil];
}

// The SmartMedia menu, from the same menu_plug_card groups macOS renders into
// NSMenu, presented through the shared menu_ios helper - the standalone's MIDI
// picker and this share the presentation, so both read as menus now. The menu
// is the app's own: every group in order, titled ones nested with chevrons.
// Dispatch stays here (pickCardItem below), because the plug-in acts on card_*
// verbs rather than menu_chosen ids.
void ios_window::card_menu(int x, int y)
{
	if (!m_view)
		return;

	ui::plug_menu_state s{ m_owner.card_path(), m_owner.card_ready() };
	std::vector<ui::menu_group> groups = ui::menu_plug_card(s);
	if (groups.empty())
		return;
	// Raw this, like mac's SMUCardMenu target holding _owner/_win: the menu
	// lives seconds and the window outlives it (both die with the view).
	ios_window *win = this;
	show_menu_groups(m_view, CGPointMake(x, y), groups, [win](int itemId) {
		win->pickCardItem(itemId);
	});
}

// The plug-in's panel menu: the shared ui/menu.h content (the PC windows, as
// the Windows and mac plug-ins show here) plus the iOS-only ROM import group,
// which is the one setting this platform has. The ROM images cannot be shipped,
// and an extension has no container app of its own to import them, so the
// picker lives here.
void ios_window::panel_menu(int x, int y)
{
	if (!m_view)
		return;
	std::vector<ui::menu_group> groups = ui::menu_plug_panel();
	append_rom_import_group(groups);
	if (groups.empty())
		return;
	UIView *here = m_view;
	show_menu_groups(m_view, CGPointMake(x, y), groups, [here](int itemId) {
		handle_rom_import_item(here, itemId);
	});
}

// The picking half of card_menu, mirroring SMUCardMenu.choose: item for item
// (save panel, open panel, eject, List, Editor). iOS differs in exactly one
// place: no save panel exists, so New N MB creates directly in Documents with
// an auto name instead of asking. Open goes through the document picker (copied
// into the container - the engine reads the path long after), which is the
// import path the standalone is still missing.
void ios_window::pickCardItem(int itemId)
{
	if (itemId >= ui::ID_PLUG_CARD_NEW16 && itemId <= ui::ID_PLUG_CARD_NEW128) {
		const int mb = 16 << (itemId - ui::ID_PLUG_CARD_NEW16);
		NSString *docs = [NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,
		                                                      NSUserDomainMask, YES)
		    firstObject];
		NSString *dir = [docs stringByAppendingPathComponent:@"SMU2000"];
		[[NSFileManager defaultManager] createDirectoryAtPath:dir
		                          withIntermediateDirectories:YES
		                                           attributes:nil
		                                                error:nil];
		NSString *name = [NSString stringWithFormat:@"smartmedia-%dMB.img", mb];
		NSString *dst = [dir stringByAppendingPathComponent:name];
		m_owner.card_make([dst UTF8String], mb);
		return;
	}
	if (itemId == ui::ID_PLUG_CARD_OPEN) {
		UTType *img = [UTType typeWithFilenameExtension:@"img"];
		NSArray<UTType *> *types = img ? @[ img ] : @[];
		UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc]
			initForOpeningContentTypes:types];
		SMUCardPicker *delegate = [[SMUCardPicker alloc]
		    initWithCallback:[this](const std::string &path) {
			    if (!path.empty())
				    m_owner.card_insert_path(path);
		    }];
		// Retained by the presented controller for the pick's duration; the
		// callback holds only this (see the note above), so no cycle.
		picker.delegate = delegate;
		UIViewController *vc = top_presenter();
		if (vc)
			[vc presentViewController:picker animated:YES completion:nil];
		return;
	}
	if (itemId == ui::ID_PLUG_CARD_EJECT)
		m_owner.card_eject();
	else if (itemId == ui::ID_PLUG_LIST)
		open_pc_window(PC_LIST);
	else if (itemId == ui::ID_PLUG_EDITOR)
		open_pc_window(PC_EDITOR);
}

// The platform type string this build answers to: UIView pointer (iOS).
// plug_view::attached() compares against plug_window_type(), so this one line
// is what lets the shared view attach on iOS.
const char *plug_window_type()
{
	return Steinberg::kPlatformTypeUIView;
}

plug_window *plug_window_create(plug_view &owner)
{
	return new ios_window(owner);
}




} // namespace vst3
} // namespace smu2000
