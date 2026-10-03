// license:BSD-3-Clause
//
// pc_window for iOS: the editor host, step 1 of the port.
//
// The third of the per-platform twins (pc_window.cpp for Win32/D3D11,
// pc_window_mac.mm for AppKit/Metal, this for UIKit/Metal) and by far the
// smallest, deliberately. Step 1 never opens an editor; the file exists because
// ui::app refers to these methods unconditionally - the five pc_window members are
// constructed by ui::app's constructor, frame_work() ticks all five every frame,
// and the destructor tears them down. Without it the standalone does not link.
//
// So this answers "can an editor exist on iOS" as cheaply as possible. Real here:
//
//   the destructor    destroys the ImGui context
//   visible / frame   the checks a 30 Hz tick makes sixty times a second
//   show / hide / shutdown   the rest, once an editor can actually open
//
// Not real: show() reports that it is not implemented rather than opening a window
// that stays black. open_pc_window in app_ios.h logs the reason, so choosing an
// editor says why it cannot open instead of doing nothing at all. The editor views
// themselves (fx_editor.cpp and its neighbours) are linked and do compile - they are
// the same objects the macOS front end uses - and only the hosting and the input
// are missing.
//
// The header's non-Windows member block is used as it stands: m_ns holds the
// Objective-C objects, which is why it is a void* rather than a typed pointer -
// pc_window.h is a C++ header and must not mention UIKit.

#import <UIKit/UIKit.h>

#include "ui/pc_window.h"

#include "ui/imgui_shell.h"

namespace ui {

// The Objective-C side, held in a C++ struct the way pc_window_mac.mm holds its
// `host`. m_ns is a void* because pc_window.h is a C++ header and must not mention
// UIKit; under ARC the pointers below are managed, so there is no release call and
// no leak on the destructor. (An explicit [(id)m_ns release] would not compile here:
// AUV3_FLAGS puts -fobjc-arc on every Objective-C++ file.)
namespace {

struct host {
	UIViewController *vc = nil;
	CAMetalLayer     *layer = nil;
	id<MTLDevice>     dev = nil;
	id<MTLCommandQueue> queue = nil;
	bool              on_screen = false;
};

} // namespace

pc_window::~pc_window()
{
	// Same teardown as the mac twin's: the ImGui context and the panel's user
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
	// Not implemented in step 1. create() is where the UIViewController and its
	// CAMetalLayer would be made and new_context() called; show() below is the
	// caller-facing half and refuses before reaching here.
	err = "the iOS editor host is not implemented yet (step 2)";
	return false;
}

void pc_window::destroy()
{
}

bool pc_window::show(std::string &err)
{
	return create(err);
}

void pc_window::hide()
{
	host *h = (host *)m_ns;
	if (h && h->on_screen) {
		[h->vc dismissViewControllerAnimated:YES completion:nil];
		h->on_screen = false;
	}
}

bool pc_window::visible() const
{
	// No view controller is made in step 1, so this is always false - and frame()
	// returns on it, which is why an unopened editor costs nothing per frame.
	const host *h = (const host *)m_ns;
	return h && h->on_screen;
}

void pc_window::shutdown(bridge &br)
{
	// The editor's contents were never opened, so there is nothing to tell. The
	// mac twin unmutes the overview and does its other teardown here.
	(void)br;
}

void pc_window::frame(xg::model &m, const xg_snapshot &ram, bridge &br)
{
	// A no-op while not visible, which is the same first line the mac and Windows
	// versions open with. Once create() works this becomes the editor's NewFrame,
	// its paint through imgui_view, and ImGui::Render.
	(void)m;
	(void)ram;
	(void)br;
	// The hiding transition, tracked the way the twins track it: m_was_visible
	// catches the frame a window disappears, which is when teardown runs. There is
	// nothing to tear down yet - create() is still refused - so this only ever
	// observes false->false; but the bookkeeping stays live for step 2 instead of
	// rotting, and it retires the -Wunused-private-field warning by using the
	// member rather than by pragmas.
	if (m_was_visible && !visible()) {
	}
	m_was_visible = visible();
}

} // namespace ui