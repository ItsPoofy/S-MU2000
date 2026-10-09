// license:BSD-3-Clause
//
// menu_groups -> UIKit, in one place.
//
// The standalone (window_ios.mm) and the plug-in (view_ios.mm) both render the
// shared menu_groups model. A UIEditMenuInteraction presents it: the compact
// anchored bubble iOS uses for pop-up menus, which is the native counterpart of
// the NSMenu the mac front ends pop up.
//
// The menu is the app's, not this file's: every group in, every group out, in
// order, with the same shape ui/window_mac.mm gives NSMenu. A titled group is a
// submenu with a chevron, an untitled one is inline, checked is the native check
// state, disabled is the native disabled state. Nothing is regrouped, reordered
// or relocated - no invented "More" page, no splitting the footer out to keep a
// page short. Only separators are dropped: UIMenu has no separator element, so
// the app's separator items render as nothing.
//
// One place so the next presentation fix lands once, not twice.
//
// Objective-C++ only (UIKit + std::function). Plain C++ callers cannot see it,
// which is fine: both callers are .mm files.

#ifndef S_MU2000_UI_MENU_IOS_H
#define S_MU2000_UI_MENU_IOS_H

#pragma once

#import <UIKit/UIKit.h>

#include <functional>
#include <vector>

namespace ui {
struct menu_group;
}

// Shows the groups anchored at a point in the view, calling onPick(id) for the
// chosen item. Needs no view controller: the interaction presents from the view
// itself, so this also works where no presenter exists (plug-in views).
void show_menu_groups(UIView *view, CGPoint at,
                      const std::vector<ui::menu_group> &groups,
                      std::function<void(int)> onPick);

#endif // S_MU2000_UI_MENU_IOS_H