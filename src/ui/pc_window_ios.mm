// license:BSD-3-Clause
//
// pc_window for iOS: the XG editor host.
//
// The third of the per-platform twins (pc_window.cpp for Win32/D3D11,
// pc_window_mac.mm for AppKit/Metal, this for UIKit/Metal). Each editor gets a
// UIViewController presented full-screen over the panel, with its own Metal layer,
// its own ImGui context and its own CJK font - one context per window, as on macOS,
// because ImGui attaches to the current one.
//
// Rendering rides the main 30 Hz tick, not a second display link: ui::app's
// frame_work() calls pc_window::frame() on all five editors every frame (the same
// arrangement as the mac timer), and frame() draws only while its view
// controller is on screen. Input is per-view: each editor view feeds touches into
// its own context's io, so two windows never fight over one pointer. The editor
// views ARE ImGui widgets (unlike the panel's raw draw list), so io.MouseDown is
// the whole of it - no app verbs.
//
// A fullscreen modal has no close box, so each editor sits in a navigation
// controller whose Done button hides it. That is the only UIKit chrome in the
// file; everything else is the Metal/ImGui shell both twins share.
//
// Not yet: keyboard (editors have text fields; UIKeyCommand/UITextInput next) and
// file drops (no UIDropInteraction yet - set_drop_handler is accepted and ignored,
// as on the panel).

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CADisplayLink.h>
#import <UIKit/UIKit.h>

#include "ui/pc_window.h"

#include "ui/font_file.h"
#include "ui/imgui_shell.h"
#include "ui/xg_ui.h"

// The per-editor view: +layerClass Metal, own touches into its own context. Same
// shape as SMUView in window_ios.mm, minus the display link (frame() drives) and
// the app pointer (editors talk to the bridge through draw(), not verbs).
@interface PCEditView : UIView
{
@public
	ImGuiContext *ctx;
}
@end

@implementation PCEditView

+ (Class)layerClass
{
	return [CAMetalLayer class];
}

- (instancetype)initWithFrame:(CGRect)frame
{
	self = [super initWithFrame:frame];
	if (self) {
		self.multipleTouchEnabled = YES;
		self.backgroundColor = UIColor.blackColor;
	}
	return self;
}

- (void)pushTouch:(UITouch *)t down:(BOOL)down
{
	if (!self->ctx)
		return;
	ImGui::SetCurrentContext(self->ctx);
	ImGuiIO &io = ImGui::GetIO();
	const CGPoint p = [t locationInView:self];
	io.MousePos = ImVec2((float)p.x, (float)p.y);
	io.MouseDown[0] = down ? YES : NO;
}

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
	(void)event;
	for (UITouch *t in touches)
		[self pushTouch:t down:YES];
}

- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
	(void)event;
	for (UITouch *t in touches)
		[self pushTouch:t down:YES];
}

- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
	(void)event;
	for (UITouch *t in touches)
		[self pushTouch:t down:NO];
}

- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
	(void)touches;
	(void)event;
	if (!self->ctx)
		return;
	ImGui::SetCurrentContext(self->ctx);
	ImGui::GetIO().MouseDown[0] = false;
}

@end

// A view controller per editor: holds the Metal view, carries the Done button's
// navigation item, and tells the pc_window when it goes away so visible() tracks
// dismissal (including the swipe that full-screen does not have - belt and
// braces for presentation styles to come).
@interface PCEditController : UIViewController
@end

@implementation PCEditController
- (void)viewDidLoad
{
	[super viewDidLoad];
	self.navigationItem.leftBarButtonItem = [[UIBarButtonItem alloc]
		initWithBarButtonSystemItem:UIBarButtonSystemItemDone
		                     target:self
		                     action:@selector(done:)];
}

- (void)done:(id)sender
{
	(void)sender;
	[self dismissViewControllerAnimated:YES completion:nil];
}
@end

namespace ui {

// The Objective-C side, held in a C++ struct the way pc_window_mac.mm holds its
// `host`. m_ns is a void* because pc_window.h is a C++ header and must not mention
// UIKit; under ARC the pointers below are managed.
namespace {

struct host {
	UIViewController *vc = nil;   // the navigation controller presented
	PCEditView *view = nil;
	CAMetalLayer *layer = nil;
	id<MTLDevice> dev = nil;
	id<MTLCommandQueue> queue = nil;
};

NSString *title_of_view(const imgui_view &view)
{
	// wchar_t is UTF-32 on Apple, so the title crosses as UTF-32LE bytes.
	const wchar_t *w = view.title();
	if (!w)
		return @"Editor";
	return [[NSString alloc] initWithBytes:w
	                                length:std::wcslen(w) * sizeof(wchar_t)
	                              encoding:NSUTF32LittleEndianStringEncoding];
}

// The view controller to present from: the topmost of the foreground scene,
// so an editor opened from a menu (no view at hand) still lands correctly.
UIViewController *top_presenter()
{
	UIViewController *top = nil;
	for (UIScene *scene in [UIApplication sharedApplication].connectedScenes) {
		if ([scene activationState] != UISceneActivationStateForegroundActive)
			continue;
		if (![scene isKindOfClass:UIWindowScene.class])
			continue;
		UIWindow *win = nil;
		for (UIWindow *w in [(UIWindowScene *)scene windows]) {
			if ([w isKeyWindow]) {
				win = w;
				break;
			}
		}
		if (!win)
			continue;
		top = win.rootViewController;
		break;
	}
	while (top && [top presentedViewController])
		top = [top presentedViewController];
	return top;
}

} // namespace

pc_window::~pc_window()
{
	// Same teardown as the mac twin: the ImGui context and the panel's user
	// textures have to go before the context does, and metal_stop does both.
	if (m_imgui) {
		ImGuiContext *ctx = m_imgui;
		imshell::metal_stop(ctx);
		m_imgui = nullptr;
	}
	// The host struct is a plain C++ allocation holding ARC-managed pointers: the
	// pointers die with the struct, so this is just delete.
	delete (host *)m_ns;
	m_ns = nullptr;
}

bool pc_window::create(std::string &err)
{
	host *h = new host;
	m_ns = h;

	h->dev = MTLCreateSystemDefaultDevice();
	if (!h->dev) {
		delete h;
		m_ns = nullptr;
		err = UI_TEXT(dlg_metal_fail, "Cannot use Metal");
		return false;
	}
	h->queue = [h->dev newCommandQueue];

	// One context per window (ImGui attaches to the current one) - the same as
	// pc_window_mac.mm and pc_window.cpp. Same style, same keyboard nav, no
	// imgui.ini littered into the container.
	IMGUI_CHECKVERSION();
	m_imgui = ImGui::CreateContext();
	ImGui::SetCurrentContext(m_imgui);
	ImGuiIO &io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.IniFilename = nullptr;

	ImGui::StyleColorsDark();
	ImGuiStyle &st = ImGui::GetStyle();
	st.FrameRounding = 3;

	// The one shared font setup: ui/font_file.h asks CoreText for a face by
	// family name (never a hard-coded path), like the mac twin.
	add_cjk_font(io.Fonts);

	ImGui_ImplMetal_Init(h->dev);

	PCEditView *view = [[PCEditView alloc] initWithFrame:[UIScreen mainScreen].bounds];
	view->ctx = m_imgui;
	h->view = view;
	h->layer = (CAMetalLayer *)view.layer;
	h->layer.device = h->dev;
	h->layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
	h->layer.framebufferOnly = YES;

	PCEditController *vc = [[PCEditController alloc] init];
	vc.title = title_of_view(*m_view);
	vc.view = view;
	UINavigationController *nav = [[UINavigationController alloc] initWithRootViewController:vc];
	nav.modalPresentationStyle = UIModalPresentationFullScreen;
	h->vc = nav;
	return true;
}

void pc_window::destroy()
{
	if (m_imgui) {
		ImGuiContext *ctx = m_imgui;
		imshell::metal_stop(ctx);
		m_imgui = nullptr;
	}
	if (host *h = (host *)m_ns) {
		[h->vc dismissViewControllerAnimated:YES completion:nil];
		delete h;
		m_ns = nullptr;
	}
}

bool pc_window::show(std::string &err)
{
	if (!m_ns && !create(err))
		return false;
	host *h = (host *)m_ns;
	if ([h->vc presentingViewController])
		return true;   // already up; showing it again is a no-op, not an error
	UIViewController *presenter = top_presenter();
	if (!presenter) {
		err = "no view controller to present from";
		return false;
	}
	[presenter presentViewController:h->vc animated:YES completion:nil];
	return true;
}

void pc_window::hide()
{
	if (host *h = (host *)m_ns)
		[h->vc dismissViewControllerAnimated:YES completion:nil];
}

bool pc_window::visible() const
{
	// Presented (not merely created): presentingViewController is non-null
	// exactly while on screen, which also tracks user dismissal for free.
	const host *h = (const host *)m_ns;
	return h && h->vc && [h->vc presentingViewController] != nil;
}

void pc_window::shutdown(bridge &br)
{
	// Exactly the mac twin: tell the contents they closed (unmutes the overview,
	// releases what they held) under their own context.
	if (!m_imgui)
		return;
	ImGui::SetCurrentContext(m_imgui);
	m_view->hidden(br);
}

void pc_window::frame(xg::model &m, const xg_snapshot &ram, bridge &br)
{
	host *h = (host *)m_ns;
	const bool shown = m_imgui && h && h->view && [h->vc presentingViewController] &&
	                   h->view.window;
	if (m_was_visible && !shown && m_imgui) {
		ImGui::SetCurrentContext(m_imgui);
		m_view->hidden(br);   // dismissed mid-gesture: release held-down buttons
	}
	m_was_visible = shown;
	if (!shown)
		return;
	ImGui::SetCurrentContext(m_imgui);
	ImGuiIO &io = ImGui::GetIO();

	const CGRect b = [h->view bounds];
	const CGFloat scale = [h->view contentScaleFactor];
	io.DisplaySize = ImVec2(float(b.size.width), float(b.size.height));
	io.DisplayFramebufferScale = ImVec2(float(scale), float(scale));

	// The drawable decides the render pass, and the pass is wanted before
	// NewFrame (the same order the Metal example - and the mac twin - use).
	imshell::metal_sync(h->layer, h->view);
	id<CAMetalDrawable> drawable = [h->layer nextDrawable];
	if (!drawable)
		return;
	MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
	pass.colorAttachments[0].texture = drawable.texture;
	pass.colorAttachments[0].loadAction = MTLLoadActionClear;
	pass.colorAttachments[0].clearColor = MTLClearColorMake(0.10, 0.10, 0.11, 1.0);
	pass.colorAttachments[0].storeAction = MTLStoreActionStore;

	ImGui_ImplMetal_NewFrame(pass);
	ImGui::NewFrame();
	m_view->draw(m, ram, br);
	xgui::drag_flush(br);   // mouse-driven values, sent thinned
	ImGui::Render();

	id<MTLCommandBuffer> buf = [h->queue commandBuffer];
	id<MTLRenderCommandEncoder> enc = [buf renderCommandEncoderWithDescriptor:pass];
	ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), buf, enc);
	[enc endEncoding];
	[buf presentDrawable:drawable];
	[buf commit];
	// no wait: the main 30 Hz tick (frame_work) decides the pace, as on macOS
}

} // namespace ui