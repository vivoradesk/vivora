// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#ifdef VIVORA_MACOS

#include "app/gui/mac_activation.h"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

namespace vivora::gui {

bool launched_as_login_item() {
    // The launch Apple event carries a flag saying the app was opened by the
    // login-items machinery rather than by a person. It is the only reliable
    // way to tell the two apart -- the process arguments and parent are the
    // same either way.
    NSAppleEventDescriptor* ev =
        [[NSAppleEventManager sharedAppleEventManager] currentAppleEvent];
    if (!ev) return false;
    if ([ev eventClass] != kCoreEventClass || [ev eventID] != kAEOpenApplication)
        return false;
    NSAppleEventDescriptor* prop = [ev paramDescriptorForKeyword:keyAEPropData];
    return prop != nil && [prop enumCodeValue] == keyAELaunchedAsLogInItem;
}

void activate_app() {
    // Vivora sets LSUIElement, so it has no Dock icon and macOS never brings
    // it forward on its own. That is right for a menu-bar app -- until
    // something else launches it, at which point the window opens behind
    // every other window with nothing in the Dock to click.
    //
    // The case that made this matter: granting Screen Recording, where macOS
    // itself quits and reopens the app. From the user's side the app simply
    // vanished.
    [NSApp activateIgnoringOtherApps:YES];
}

} // namespace vivora::gui

#endif // VIVORA_MACOS
