// license:BSD-3-Clause
//
// See menu_ios.h for what this is, and why it is a file of its own.

#import "ui/menu_ios.h"

#include "ui/menu.h"

// One presenter per show: it holds the groups, the callback and the interaction
// for exactly as long as the menu is up. The lifetime chain is one link per
// holder, and a missing link is a dangling delegate: the view retains the
// interaction (addInteraction), the interaction retains the presenter through
// an associated object (the delegate property is weak), and willDismiss removes
// the interaction - releasing the chain. Blocks hold groups/callback by value.
#import <objc/runtime.h>

static const void *kPresenterKey = &kPresenterKey;
@interface SMUMenuPresenter : NSObject <UIEditMenuInteractionDelegate>
{
@public
	std::vector<ui::menu_group> groups;
	std::function<void(int)> onPick;
	UIEditMenuInteraction *interaction;
	UIView *view;
}
@end

@implementation SMUMenuPresenter

- (nullable UIMenu *)editMenuInteraction:(UIEditMenuInteraction *)sender
                  menuForConfiguration:(UIEditMenuConfiguration *)configuration
                      suggestedActions:(NSArray<UIMenuElement *> *)suggestedActions
{
	(void)sender;
	(void)configuration;
	(void)suggestedActions;
	if (groups.empty())
		return nil;
	NSMutableArray<UIMenuElement *> *top = [NSMutableArray array];
	for (const ui::menu_group &g : groups) {
		NSMutableArray<UIMenuElement *> *rows = [NSMutableArray array];
		for (const ui::menu_item &item : g.items) {
			if (item.separator)
				continue;
			NSString *title = [NSString stringWithUTF8String:item.label.c_str()];
			const int itemId = item.id;
			std::function<void(int)> pick = onPick;
			UIAction *a = [UIAction actionWithTitle:title
			                                 image:nil
			                            identifier:nil
			                               handler:^(__kindof UIAction *act) {
				                               (void)act;
				                               if (pick)
					                               pick(itemId);
			                               }];
			if (!item.enabled)
				a.attributes = UIMenuElementAttributesDisabled;
			if (item.checked)
				a.state = UIMenuElementStateOn;
			if (!item.shortcut.empty())
				a.discoverabilityTitle =
				    [NSString stringWithUTF8String:item.shortcut.c_str()];
			[rows addObject:a];
		}
		if ([rows count] == 0)
			continue;
		if (g.title.empty()) {
			[top addObjectsFromArray:rows];
		} else {
			NSString *head = [NSString stringWithUTF8String:g.title.c_str()];
			[top addObject:[UIMenu menuWithTitle:head children:rows]];
		}
	}
	if ([top count] == 0)
		return nil;
	return [UIMenu menuWithTitle:@"" children:top];
}

- (void)editMenuInteraction:(UIEditMenuInteraction *)sender
    willDismissMenuForConfiguration:(UIEditMenuConfiguration *)configuration
                           animator:(id<UIEditMenuInteractionAnimating>)animator
{
	(void)configuration;
	(void)animator;
	// End of life: dropping the interaction releases the associated presenter
	// (and its groups) with it. Without this every opened menu leaks its
	// presenter into its view, forever.
	[view removeInteraction:sender];
}

@end

void show_menu_groups(UIView *view, CGPoint at,
                      const std::vector<ui::menu_group> &groups,
                      std::function<void(int)> onPick)
{
	if (!view || groups.empty())
		return;
	SMUMenuPresenter *presenter = [[SMUMenuPresenter alloc] init];
	presenter->groups = groups;
	presenter->onPick = onPick;
	presenter->view = view;
	presenter->interaction =
	    [[UIEditMenuInteraction alloc] initWithDelegate:presenter];
	// Retained alongside the interaction (delegate is weak): dies on dismiss.
	objc_setAssociatedObject(presenter->interaction, kPresenterKey, presenter,
	                         OBJC_ASSOCIATION_RETAIN_NONATOMIC);
	[view addInteraction:presenter->interaction];
	UIEditMenuConfiguration *config =
	    [UIEditMenuConfiguration configurationWithIdentifier:nil sourcePoint:at];
	[presenter->interaction presentEditMenuWithConfiguration:config];
}
