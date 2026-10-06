#pragma once

#include <windows.h>
#include <array>

namespace explorer {

using FocusKeyboardState = std::array<BYTE, 256>;
using NativeFocusAction = HRESULT (*)(void* context);

// The same bounded transition reducer is used by the native hook and by the
// deterministic tests. Only decoded left/right Shift events are accepted.
struct FocusShiftTransitions {
    unsigned count = 0;
    BYTE sidesSeen = 0, sidesDown = 0;
    bool ambiguous = false;
    void observe(UINT side, bool down) noexcept;
    void observeMessage(WPARAM key, LPARAM bits, HKL layout) noexcept;
};

// KeyboardProc's scan code, not the generic VK_SHIFT alone, identifies a side.
// An unsupported/missing scan or a contradictory explicit side returns zero.
UINT focusShiftMessageSide(WPARAM key, LPARAM bits, HKL layout) noexcept;

// Preserve every fresh non-Shift byte and every Shift low/reserved bit. On
// ambiguity the entire fresh table is returned unchanged with a failure.
HRESULT mergeFocusKeyboardState(const FocusKeyboardState& original,
    const FocusKeyboardState& fresh, const FocusShiftTransitions& transitions,
    FocusKeyboardState* restored) noexcept;

struct FocusKeyboardReadback {
    HRESULT originalRead = E_PENDING, hookInstall = E_PENDING;
    HRESULT neutralWrite = E_PENDING, neutralRead = E_PENDING;
    HRESULT action = E_PENDING, hookRemove = E_PENDING;
    HRESULT freshRead = E_PENDING, merge = E_PENDING;
    HRESULT restoreWrite = E_PENDING, restoreRead = E_PENDING;
    DWORD thread = 0;
    unsigned removedShiftEvents = 0, nonremovedShiftNotifications = 0;
    bool neutralized = false, ambiguous = false, hookDetached = false;
    bool freshNonShiftPreserved = false, exactRestoredTable = false;
};

// One calling-thread-only WH_KEYBOARD hook surrounds the action when Shift is
// down. No global hook, physical input injection, or message pumping is used.
// Posted key messages cannot become native input transitions. Actual input is
// merged only when side/initial-state/fresh-state evidence is coherent; any
// ambiguous observation preserves fresh Shift state and fails. Callers must
// check this HRESULT before treating their focus transition as successful.
HRESULT focusWithNeutralShift(NativeFocusAction action, void* context,
    FocusKeyboardReadback* readback = nullptr) noexcept;

} // namespace explorer
