// license:BSD-3-Clause
//
// The walk to a view controller to present from. See ui/presenter_ios.h.

#import <UIKit/UIKit.h>

#include "ui/presenter_ios.h"

namespace ui {

UIViewController *presenter_for(UIView *view)
{
	if (!view)
		return nil;
	for (UIView *up = view; up; up = up.superview) {
		if ([up.nextResponder isKindOfClass:[UIViewController class]])
			return (UIViewController *)up.nextResponder;
	}
	for (UIScene *scene in [UIApplication sharedApplication].connectedScenes) {
		if (![scene isKindOfClass:[UIWindowScene class]])
			continue;
		if ([(UIWindowScene *)scene activationState] != UISceneActivationStateForegroundActive)
			continue;
		for (UIWindow *w in [(UIWindowScene *)scene windows]) {
			if (w.isKeyWindow)
				return w.rootViewController;
		}
	}
	return view.window.rootViewController;
}

} // namespace ui
