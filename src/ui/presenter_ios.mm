// license:BSD-3-Clause
//
// The walk to a view controller to present from. See ui/presenter_ios.h.

#import <UIKit/UIKit.h>

#include "ui/presenter_ios.h"

namespace ui {

// From a controller, to the one nothing is presented from. UIKit refuses a
// presentation on a controller that is already presenting - it logs and does
// nothing - and every editor here is presented modally from the window's root,
// so the root is that controller for as long as an editor is up. Callers that
// present on the strength of a view's own controller (the file asks, the ROM
// import, the MIDI file panel) would therefore be refused every time one is open,
// which is exactly when a file request is made: it comes from inside the editor.
static UIViewController *top_of(UIViewController *root)
{
	UIViewController *top = root;
	while (top && [top presentedViewController])
		top = [top presentedViewController];
	return top;
}

UIViewController *presenter_for(UIView *view)
{
	if (!view)
		return nil;
	for (UIView *up = view; up; up = up.superview) {
		if ([up.nextResponder isKindOfClass:[UIViewController class]])
			return top_of((UIViewController *)up.nextResponder);
	}
	for (UIScene *scene in [UIApplication sharedApplication].connectedScenes) {
		if (![scene isKindOfClass:[UIWindowScene class]])
			continue;
		if ([(UIWindowScene *)scene activationState] != UISceneActivationStateForegroundActive)
			continue;
		for (UIWindow *w in [(UIWindowScene *)scene windows]) {
			if (w.isKeyWindow)
				return top_of(w.rootViewController);
		}
	}
	return top_of(view.window.rootViewController);
}

} // namespace ui
