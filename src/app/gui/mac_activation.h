#pragma once

#ifdef VIVORA_MACOS

namespace vivora::gui {

// True when the app was started by the login-items machinery rather than by
// somebody opening it. A menu-bar app should stay in the background then.
bool launched_as_login_item();

// Bring the app to the front. Needed because Vivora sets LSUIElement, so it
// has no Dock icon and macOS will not raise it on a background launch --
// notably the quit-and-reopen macOS performs after a permission is granted.
void activate_app();

} // namespace vivora::gui

#endif // VIVORA_MACOS
