#pragma once
#include "explorer/core.hpp"
#include <shobjidl.h>

namespace explorer {

class NativeNamespaceActions;

enum class SelectionAction { All, None, Invert };

// Uses the actual view's documented bulk APIs. Call on its owning STA; PIDLs
// stay relative to that view and both interfaces must have its COM identity.
// Headless operations require an owned HWND on the current private-desktop
// thread. No filesystem or user data is modified.
HRESULT changeShellSelection(IFolderView2* folderView, IShellView* shellView,
                             SelectionAction action, NativeNamespaceActions* nativeActions = nullptr,
                             bool headless = false) noexcept;

}
