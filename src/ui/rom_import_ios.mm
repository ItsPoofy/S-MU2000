// license:BSD-3-Clause
//
// See rom_import.h for what this is and why it exists.

#import "ui/rom_import_ios.h"


#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#import <objc/runtime.h>

#include "compat/paths.h"
#include "roms_dir.h"
#include "ui/menu.h"
#include "ui/presenter_ios.h"
#include "ui/rom_locate.h"
#include "ui/texts.h"

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

// The launch-time hook (rom_import.h). Process-wide: one importer is up at a
// time, and the standalone is the only caller that arms it.
static std::function<void()> g_rom_import_done;

// The importer outlives several presentations (explanation, picker, result), so
// something has to hold it: the picker's delegate property is weak
// (UIDocumentPickerViewController.h:65) and the caller keeps no reference. One
// flow at a time, released when the flow ends.
static id g_importer = nil;

// Where the set is copied: config_dir()/roms, the shared ROM search's own first
// candidate (src/rom_search.h), so an install needs no search order change and a
// baked bundle copy stays a fallback. Static, at file scope: the ObjC class
// below cannot live inside a namespace.
static std::string rom_container_dir()
{
	const std::string conf = smu2000::config_dir();
	if (conf.empty())
		return {};
	return smu2000::join(conf, "roms");
}

// The localized tables hand out const char*; the alerts want NSString.
static NSString *ns(const char *s)
{
	return s ? [NSString stringWithUTF8String:s] : @"";
}

// One button row, our own action so the flow can continue from it.
static void say(UIView *view, NSString *title, NSString *message,
                NSString *goTitle, void (^go)(void), NSString *stopTitle,
                void (^stop)(void))
{
	std::fprintf(stderr, "[ios] roms: %s\n",
	             message ? ([message UTF8String] ?: "") : "");
	UIViewController *presenter = ui::presenter_for(view);
	if (!presenter) {
		std::fprintf(stderr, "[ios] roms: (no view controller for the message)\n");
		return;
	}
	UIAlertController *a = [UIAlertController alertControllerWithTitle:title
	                                                           message:message
	                                                    preferredStyle:UIAlertControllerStyleAlert];
	if (go)
		[a addAction:[UIAlertAction actionWithTitle:goTitle
		                                     style:UIAlertActionStyleDefault
		                                   handler:^(UIAlertAction *) { go(); }]];
	if (stop)
		[a addAction:[UIAlertAction actionWithTitle:stopTitle
		                                     style:UIAlertActionStyleCancel
		                                   handler:^(UIAlertAction *) { stop(); }]];
	[presenter presentViewController:a animated:YES completion:nil];
}

// The importer hangs off the picker as an associated object, as the menu
// presenter does in ui/menu_ios.mm. The delegate property is weak
// (UIDocumentPickerViewController.h:65), and the flow itself is held by
// g_importer - this keeps it alive for the presentation even if that changes.
static const void *kImporterKey = &kImporterKey;

// The picker and the copy.
@interface SMURomImporter : NSObject <UIDocumentPickerDelegate>
- (instancetype)initWithView:(UIView *)view explain:(BOOL)explain;
- (void)start;
- (void)endFlow;
- (void)presentPicker;
- (void)explainThenPick;
- (void)complain:(NSString *)message;
@end

@implementation SMURomImporter {
	__weak UIView *_view;
	BOOL _explain;
}

- (instancetype)initWithView:(UIView *)view explain:(BOOL)explain
{
	self = [super init];
	if (self) {
		_view = view;
		_explain = explain;
	}
	return self;
}

- (void)start
{
	if (_explain)
		[self explainThenPick];
	else
		[self presentPicker];
}

// The shared "you need these, here is how" text (ui::roms_needed_message) -
// the same words the desktop shows when it cannot find the ROMs. Only on the
// first open of a flow; a retry goes straight back to the picker, because the
// message below already said all of this.
- (void)explainThenPick
{
	UIView *view = _view;
	__weak SMURomImporter *weakSelf = self;
	say(view, ns(UI_TEXT(menu_roms_title, "ROM files")),
	    ns(ui::roms_needed_message().c_str()),
	    ns(UI_TEXT(dlg_roms_pick, "Select ROM folder...")), ^{ [weakSelf presentPicker]; },
	    @"Not now", ^{ [weakSelf endFlow]; });
}

- (void)presentPicker
{
	// asCopy:NO, and not as a style choice: UIKit throws "folder import is not
	// supported, use asCopy:false" for a folder type with asCopy:YES. So the
	// folder is picked where it lies (Files, iCloud Drive, a drive) and copied
	// in below - the grant lasts for this run only, and the copy is what
	// survives a relaunch.
	UIDocumentPickerViewController *picker =
	    [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeFolder]
	                                                               asCopy:NO];
	picker.delegate = self;
	picker.allowsMultipleSelection = NO;
	// A titled navigation bar around it: the picker on its own says nothing
	// about what is being asked for.
	UINavigationController *nav =
	    [[UINavigationController alloc] initWithRootViewController:picker];
	nav.navigationBar.prefersLargeTitles = NO;
	objc_setAssociatedObject(picker, kImporterKey, self,
	                         OBJC_ASSOCIATION_RETAIN_NONATOMIC);
	UIViewController *presenter = ui::presenter_for(_view);
	if (!presenter) {
		std::fprintf(stderr, "[ios] roms: no view controller to present the picker\n");
		[self endFlow];
		return;
	}
	[presenter presentViewController:nav animated:YES completion:nil];
}

- (void)documentPicker:(UIDocumentPickerViewController *)controller
    didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls
{
	(void)controller;
	UIView *view = _view;
	NSURL *url = urls.firstObject;
	if (!url || !view) {
		[self complain:@"Nothing was picked."];
		return;
	}
	// The grant the picker handed us, held for the copy below. A no-op when the
	// folder is already inside the container.
	const bool scoped = [url startAccessingSecurityScopedResource];
	const std::string from = std::string([url.path UTF8String] ?: "");
	const std::string dest = rom_container_dir();
	// The shared validation: empty when it is not a set, and it accepts the
	// parent of a roms/ folder too - exactly what the desktop picker accepts.
	const std::string set_dir = smu2000::accept_roms_choice(from);
	std::string why;
	if (set_dir.empty()) {
		// The shared "not a set" wording, listing what is missing.
		[self complain:ns(ui::roms_bad_message(from).c_str())];
		if (scoped)
			[url stopAccessingSecurityScopedResource];
		return;
	}
	if (dest.empty()) {
		[self complain:@"The container directory cannot be resolved."];
		if (scoped)
			[url stopAccessingSecurityScopedResource];
		return;
	}
	if (!smu2000::install_roms(smu2000::fs::path(set_dir),
	                          smu2000::fs::path(dest), why)) {
		[self complain:ns(why.c_str())];
		if (scoped)
			[url stopAccessingSecurityScopedResource];
		return;
	}
	// Remembered the shared way, so the search finds the set from now on.
	ui::remember_roms_dir(dest);
	// The destination goes to the log, not to the alert: it lives inside the
	// app's container, which the user has no way to reach, so a path there is
	// noise to them and useful only to us.
	std::fprintf(stderr, "[ios] roms: installed in %s\n", dest.c_str());
	if (scoped)
		[url stopAccessingSecurityScopedResource];
	// A caller that skipped the boot because there were no ROMs boots here, so
	// the panel is already alive behind the message that follows. Moved out and
	// cleared first: the menu item is still there for a second install, and this
	// hook is boot-once work - run twice it would load_machine and eng.boot() from
	// the main thread while the render thread is inside engine::fill, which is
	// not a sequence anything else on this platform takes (engine::restart drops
	// the state and waits for in_fill; this did neither).
	if (g_rom_import_done) {
		auto once = std::move(g_rom_import_done);
		g_rom_import_done = nullptr;
		once();
	}
	__weak SMURomImporter *weakSelf = self;
	say(view, ns(UI_TEXT(menu_roms_title, "ROM files")),
	    ns(UI_TEXT(dlg_roms_installed, "The ROM files were imported.")), nil, nil,
	    @"OK", ^{
		[weakSelf endFlow];
	    });
}

// An error is not the end of the flow: the picker goes straight back up, because
// the user came here to install a set and the folder they picked is one step
// from being right.
- (void)complain:(NSString *)message
{
	UIView *view = _view;
	__weak SMURomImporter *weakSelf = self;
	say(view, ns(UI_TEXT(menu_roms_title, "ROM files")), message,
	    @"Try again", ^{ [weakSelf presentPicker]; },
	    @"Cancel", ^{ [weakSelf endFlow]; });
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)controller
{
	(void)controller;
	[self endFlow];
}

- (void)endFlow
{
	g_importer = nil;
}

@end

// Starts the flow, holding the importer for its duration. `explain` shows the
// what-and-where message first; the auto-prompt at launch passes YES, a retry
// from an error does not.
static void start_rom_flow(UIView *view, bool explain)
{
	if (!view || g_importer)
		return;
	SMURomImporter *importer = [[SMURomImporter alloc] initWithView:view explain:explain];
	g_importer = importer;
	[importer start];
}

std::string ios_rom_dir()
{
	return rom_container_dir();
}

void set_rom_import_done(std::function<void()> on_done)
{
	g_rom_import_done = std::move(on_done);
}

bool prompt_for_roms(UIView *view)
{
	if (!view)
		return false;
	start_rom_flow(view, true);
	return true;
}

void append_rom_import_group(std::vector<ui::menu_group> &groups)
{
	ui::menu_group g;
	g.title = UI_TEXT(menu_roms_title, "ROM files");
	ui::menu_item install;
	install.label = UI_TEXT(menu_roms_install, "Install ROM files...");
	install.id = ID_IOS_INSTALL_ROMS;
	g.items.push_back(install);
	groups.push_back(g);
}

bool handle_rom_import_item(UIView *view, int itemId)
{
	if (itemId != ID_IOS_INSTALL_ROMS || !view)
		return false;
	start_rom_flow(view, true);
	return true;
}
