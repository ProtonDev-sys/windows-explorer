#include "explorer/focus_keyboard.hpp"

#include <limits>
#include <new>

namespace explorer {
namespace {
constexpr BYTE Generic = 1, Left = 2, Right = 4;
constexpr std::array<UINT, 3> ShiftKeys{VK_SHIFT, VK_LSHIFT, VK_RSHIFT};

BYTE shiftMask(const FocusKeyboardState& state) noexcept {
    return static_cast<BYTE>(((state[VK_SHIFT] & 0x80) ? Generic : 0) |
        ((state[VK_LSHIFT] & 0x80) ? Left : 0) | ((state[VK_RSHIFT] & 0x80) ? Right : 0));
}
BYTE withGeneric(BYTE sides) noexcept {
    sides &= Left | Right;
    return static_cast<BYTE>(sides | (sides ? Generic : 0));
}
HRESULT nativeError() noexcept {
    const auto error = GetLastError();
    return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
}
HRESULT readState(FocusKeyboardState& state) noexcept {
    SetLastError(ERROR_SUCCESS);
    return GetKeyboardState(state.data()) ? S_OK : nativeError();
}
HRESULT writeState(const FocusKeyboardState& state) noexcept {
    SetLastError(ERROR_SUCCESS);
    return SetKeyboardState(const_cast<BYTE*>(state.data())) ? S_OK : nativeError();
}
bool nonShiftEqual(const FocusKeyboardState& left, const FocusKeyboardState& right) noexcept {
    for (UINT key = 0; key < left.size(); ++key)
        if (key != VK_SHIFT && key != VK_LSHIFT && key != VK_RSHIFT && left[key] != right[key]) return false;
    return true;
}

struct HookContext {
    FocusShiftTransitions transitions;
    unsigned nonremoved = 0, depth = 0;
};
thread_local HookContext* activeHook = nullptr;
// Failed removal can leave the module-lifetime callback registered, but never
// a pointer to an expired stack. Refuse a second registration on that thread.
thread_local bool hookPoisoned = false;

LRESULT CALLBACK keyboardHook(int code, WPARAM key, LPARAM bits) noexcept {
    auto* context = activeHook;
    const bool shift = key == VK_SHIFT || key == VK_LSHIFT || key == VK_RSHIFT;
    if (!context || code < 0 || !shift || (code != HC_ACTION && code != HC_NOREMOVE))
        return CallNextHookEx(nullptr, code, key, bits);
    if (code == HC_NOREMOVE) {
        if (context->nonremoved != std::numeric_limits<unsigned>::max()) ++context->nonremoved;
        return CallNextHookEx(nullptr, code, key, bits);
    }
    // Record before chaining so a downstream hook cannot invert event order by
    // retrieving another message recursively. Such recursion fails closed.
    if (context->depth) context->transitions.ambiguous = true;
    ++context->depth;
    context->transitions.observeMessage(key, bits, GetKeyboardLayout(0));
    const auto result = CallNextHookEx(nullptr, code, key, bits);
    if (result != 0) context->transitions.ambiguous = true;
    --context->depth;
    return result;
}

class ShiftGuard final {
public:
    explicit ShiftGuard(FocusKeyboardReadback& report) noexcept : report_(report) {}
    ~ShiftGuard() { if (hook_ || changed_) finish(); }

    HRESULT prepare() noexcept {
        report_.thread = GetCurrentThreadId();
        if (activeHook || hookPoisoned) return HRESULT_FROM_WIN32(ERROR_BUSY);
        report_.originalRead = readState(original_);
        if (FAILED(report_.originalRead)) return report_.originalRead;
        if (!shiftMask(original_)) return S_FALSE;
        activeHook = &context_;
        SetLastError(ERROR_SUCCESS);
        hook_ = SetWindowsHookExW(WH_KEYBOARD, keyboardHook, nullptr, report_.thread);
        report_.hookInstall = hook_ ? S_OK : nativeError();
        if (!hook_) { activeHook = nullptr; return report_.hookInstall; }
        auto neutral = original_;
        for (const auto key : ShiftKeys) neutral[key] &= 0x7f;
        report_.neutralWrite = writeState(neutral);
        if (FAILED(report_.neutralWrite)) return report_.neutralWrite;
        changed_ = true;
        report_.neutralized = true;
        FocusKeyboardState actual{};
        report_.neutralRead = readState(actual);
        if (SUCCEEDED(report_.neutralRead) && (actual != neutral ||
            (GetKeyState(VK_SHIFT) & 0x8000) || (GetKeyState(VK_LSHIFT) & 0x8000) ||
            (GetKeyState(VK_RSHIFT) & 0x8000))) report_.neutralRead = E_FAIL;
        return report_.neutralRead;
    }

    HRESULT finish() noexcept {
        if (hook_) {
            // WH_KEYBOARD is scoped to this very thread. No callback can run
            // concurrently on another thread; clear TLS even on unhook error.
            activeHook = nullptr;
            SetLastError(ERROR_SUCCESS);
            report_.hookRemove = UnhookWindowsHookEx(hook_) ? S_OK : nativeError();
            report_.hookDetached = SUCCEEDED(report_.hookRemove);
            if (!report_.hookDetached) {
                hookPoisoned = true;
                context_.transitions.ambiguous = true;
            }
            hook_ = nullptr;
        }
        report_.removedShiftEvents = context_.transitions.count;
        report_.nonremovedShiftNotifications = context_.nonremoved;
        report_.ambiguous = context_.transitions.ambiguous;
        if (!changed_) return FAILED(report_.hookRemove) && report_.hookRemove != E_PENDING ? report_.hookRemove : S_OK;
        FocusKeyboardState fresh{}, requested{}, actual{};
        report_.freshRead = readState(fresh);
        if (FAILED(report_.freshRead)) return report_.freshRead;
        report_.merge = mergeFocusKeyboardState(original_, fresh, context_.transitions, &requested);
        report_.ambiguous = report_.ambiguous || FAILED(report_.merge);
        report_.restoreWrite = writeState(requested);
        if (FAILED(report_.restoreWrite)) return report_.restoreWrite;
        report_.restoreRead = readState(actual);
        if (FAILED(report_.restoreRead)) return report_.restoreRead;
        report_.freshNonShiftPreserved = nonShiftEqual(actual, fresh);
        report_.exactRestoredTable = actual == requested;
        if (!report_.exactRestoredTable || !report_.freshNonShiftPreserved) return E_FAIL;
        changed_ = false;
        if (FAILED(report_.hookRemove)) return report_.hookRemove;
        return report_.merge;
    }
private:
    FocusKeyboardReadback& report_;
    FocusKeyboardState original_{};
    HookContext context_;
    HHOOK hook_ = nullptr;
    bool changed_ = false;
};
} // namespace

void FocusShiftTransitions::observe(UINT side, bool down) noexcept {
    if (count == std::numeric_limits<unsigned>::max()) ambiguous = true;
    else ++count;
    const BYTE bit = static_cast<BYTE>(side == VK_LSHIFT ? Left : side == VK_RSHIFT ? Right : 0);
    if (!bit) { ambiguous = true; return; }
    sidesSeen |= bit;
    if (down) sidesDown |= bit;
    else sidesDown &= static_cast<BYTE>(~bit);
}

UINT focusShiftMessageSide(WPARAM key, LPARAM bits, HKL layout) noexcept {
    if (key != VK_SHIFT && key != VK_LSHIFT && key != VK_RSHIFT) return 0;
    const auto value = static_cast<ULONG_PTR>(bits);
    UINT scan = static_cast<UINT>((value >> 16) & 0xff);
    if (!scan || !layout) return 0;
    if (value & (ULONG_PTR{1} << 24)) scan |= 0xe000;
    const auto side = MapVirtualKeyExW(scan, MAPVK_VSC_TO_VK_EX, layout);
    if (side != VK_LSHIFT && side != VK_RSHIFT) return 0;
    return key == VK_SHIFT || key == side ? side : 0;
}

void FocusShiftTransitions::observeMessage(WPARAM key, LPARAM bits, HKL layout) noexcept {
    observe(focusShiftMessageSide(key, bits, layout),
        (static_cast<ULONG_PTR>(bits) & (ULONG_PTR{1} << 31)) == 0);
}

HRESULT mergeFocusKeyboardState(const FocusKeyboardState& original,
    const FocusKeyboardState& fresh, const FocusShiftTransitions& transitions,
    FocusKeyboardState* restored) noexcept {
    if (!restored) return E_POINTER;
    const auto originalMask = shiftMask(original), freshMask = shiftMask(fresh);
    *restored = fresh;
    if (transitions.ambiguous || (transitions.sidesSeen & ~(Left | Right)) ||
        (transitions.sidesDown & ~transitions.sidesSeen)) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    BYTE mask = originalMask;
    if (transitions.count) {
        // The temporarily neutral table starts with no sides down. Fresh native
        // input must agree with the recorded transitions before we overlay the
        // unchanged original side; unexpected provider writes are ambiguous.
        if (!transitions.sidesSeen || originalMask != withGeneric(originalMask) ||
            freshMask != withGeneric(transitions.sidesDown)) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        mask = withGeneric(static_cast<BYTE>((originalMask & ~transitions.sidesSeen) | transitions.sidesDown));
    } else if (transitions.sidesSeen || transitions.sidesDown || freshMask) {
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
    for (size_t index = 0; index < ShiftKeys.size(); ++index) {
        const auto key = ShiftKeys[index];
        (*restored)[key] = static_cast<BYTE>((fresh[key] & 0x7f) | ((mask & (1 << index)) ? 0x80 : 0));
    }
    return S_OK;
}

HRESULT focusWithNeutralShift(NativeFocusAction action, void* context,
    FocusKeyboardReadback* readback) noexcept {
    FocusKeyboardReadback local;
    auto& report = readback ? *readback : local;
    report = {};
    if (!action) return E_POINTER;
    ShiftGuard guard(report);
    const auto prepared = guard.prepare();
    if (FAILED(prepared)) {
        const auto restored = guard.finish();
        return FAILED(restored) ? restored : prepared;
    }
    try { report.action = action(context); }
    catch (const std::bad_alloc&) { report.action = E_OUTOFMEMORY; }
    catch (...) { report.action = E_FAIL; }
    const auto restored = guard.finish();
    return FAILED(restored) ? restored : report.action;
}

} // namespace explorer
