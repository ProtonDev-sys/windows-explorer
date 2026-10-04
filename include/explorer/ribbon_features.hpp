#pragma once

#include <windows.h>

namespace explorer {

// Process-local read-only capability metadata. Checks retain their native
// HRESULT. Proven absence can return false with a failed HRESULT (for example
// missing mfplat.dll); unproven failures keep the conservative true default.
// Native command/device state remains authoritative for individual actions.
struct RibbonFeatures {
    bool bitLocker = true;
    bool mediaFoundation = true;
    bool discBurning = true;
    bool diskCleanup = true;
    HRESULT editionStatus = E_PENDING;
    HRESULT mediaFoundationStatus = E_PENDING;
    HRESULT discBurningStatus = E_PENDING;
    HRESULT diskCleanupStatus = E_PENDING;
};

// Invoke once when constructing a Ribbon, rather than during state refresh.
// Does not modify registry, account, devices, Shell policies or user settings.
// The return value is the first failed check; every individual result is filled.
HRESULT installedRibbonFeatures(RibbonFeatures* result) noexcept;

}
