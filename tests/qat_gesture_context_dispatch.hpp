#pragma once

// Test-only creator dispatch for the opt-in native QAT gesture fixture.
// This delivers one right-button down/up gesture to the proven native child.
// The native control opens its own context menu; this bridge neither creates a
// menu nor maps a menu row to an application command. A separate windowless MTA
// must discover/invoke a real popup row while the creator is in the native loop.
#include "explorer/headless_visual.hpp"
#include <windows.h>
#include <commctrl.h>
#include <array>
#include <atomic>
#include <climits>
#include <cstdlib>
#include <cwchar>

namespace qat_gesture_protocol {
inline constexpr UINT dispatchMessage = WM_APP + 0x4ab;
inline constexpr UINT cancelMessage = WM_APP + 0x4ac;
inline constexpr UINT_PTR subclassId = 0x51415447;

struct WindowIdentity {
    HWND window = nullptr, root = nullptr, parent = nullptr;
    DWORD process = 0, thread = 0;
    RECT bounds{};
    std::array<wchar_t, 256> windowClass{};
};
inline bool equalRect(const RECT& left, const RECT& right) noexcept {
    return left.left == right.left && left.top == right.top &&
        left.right == right.right && left.bottom == right.bottom;
}
inline HRESULT nativeError() noexcept {
    const auto error = GetLastError();
    return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
}
inline HRESULT readOwnedWindow(HWND window, HWND owner, DWORD creator,
                               WindowIdentity& output) noexcept {
    if (!window || !owner || !IsWindow(window) || !IsWindow(owner))
        return HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
    WindowIdentity actual; actual.window = window;
    actual.thread = GetWindowThreadProcessId(window, &actual.process);
    if (actual.thread != creator || actual.process != GetCurrentProcessId())
        return E_ACCESSDENIED;
    actual.root = GetAncestor(window, GA_ROOT);
    actual.parent = GetAncestor(window, GA_PARENT);
    if (actual.root != owner || (window != owner && !IsChild(owner, window)))
        return E_ACCESSDENIED;
    SetLastError(ERROR_SUCCESS);
    const auto classSize = GetClassNameW(window, actual.windowClass.data(),
        static_cast<int>(actual.windowClass.size()));
    if (!classSize) return nativeError();
    if (classSize == static_cast<int>(actual.windowClass.size()) - 1)
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    SetLastError(ERROR_SUCCESS);
    if (!GetWindowRect(window, &actual.bounds)) return nativeError();
    if (actual.bounds.left >= actual.bounds.right || actual.bounds.top >= actual.bounds.bottom)
        return E_UNEXPECTED;
    // Every HWND here belongs to the known creator TID, whose exact private
    // desktop is checked by the caller. No denied foreign HDESK inference.
    output = actual; return S_OK;
}
inline HRESULT sameOwnedWindow(const WindowIdentity& expected, HWND owner,
                               DWORD creator) noexcept {
    if (expected.windowClass.back() != L'\0') return E_INVALIDARG;
    WindowIdentity actual;
    const auto read = readOwnedWindow(expected.window, owner, creator, actual);
    if (read != S_OK) return read;
    return actual.root == expected.root && actual.parent == expected.parent &&
        actual.process == expected.process && actual.thread == expected.thread &&
        equalRect(actual.bounds, expected.bounds) &&
        std::wcscmp(actual.windowClass.data(), expected.windowClass.data()) == 0
        ? S_OK : HRESULT_FROM_WIN32(ERROR_RETRY);
}

// Filled only after MTA discovery has finished and its UIA target/pattern refs
// are retained on that MTA. Publishing this immutable plain ticket precedes
// PostMessage(owner, dispatchMessage, sequence, 0); no pointer crosses in MSG.
struct ContextTicket {
    UINT_PTR sequence = 0;
    UINT applicationCommand=0,physicalCommand=0; // Creator-pinned actual native inventory authority.
    ULONGLONG deadline = 0;
    ULONGLONG drainDeadline = 0; // <= original fixture deadline, <= phase+500ms.
    DWORD creator = 0;
    HWND owner = nullptr;
    WindowIdentity ownerIdentity{}, receiverIdentity{};
    RECT elementBounds{}; // Actual UIA physical rectangle, under PMv2.
    POINT screenPoint{};
};
struct ContextReceipt {
    std::atomic<bool> consumed{false}, entered{false}, returned{false};
    std::atomic<HRESULT> before{E_PENDING}, after{E_PENDING};
    std::atomic<HRESULT> receiverAfter{E_PENDING}; // Removal may retire its HWND.
    std::atomic<LRESULT> nativeReturn{0}; // Passive native button-up result, not success proof.
    std::atomic<bool> downEntered{false}, downReturned{false}, upEntered{false}, upReturned{false};
    std::atomic<HRESULT> clientBefore{E_PENDING}, beforeUp{E_PENDING};
    std::atomic<LRESULT> downReturn{0}; // Passive result; actual menu/native rows prove success.
    std::atomic<LONG> clientX{0}, clientY{0};
    std::atomic<HWND> captureBefore{nullptr}, captureAfterDown{nullptr};
    std::atomic<bool> cancellationRequested{false};
    std::atomic<BOOL> endMenuResult{FALSE};
};
// Creator-only, allocation-free retained-state check: actual framework pointer,
// reset epoch and immutable fixture authority. No COM/provider call or message
// dispatch here; the original/final whole-row oracle is outside this callback.
using CreatorSourceCurrent = bool (*)(void*, const ContextTicket&) noexcept;

class CreatorContextDispatch {
public:
    // The immutable ticket, source state and receipt outlive this bridge and
    // its worker's actual kernel exit. Install/use/remove on creator only.
    CreatorContextDispatch(const ContextTicket& ticket, ContextReceipt& receipt,
                           CreatorSourceCurrent current, void* source) noexcept
        : ticket_(ticket), receipt_(receipt), current_(current), source_(source) {}
    CreatorContextDispatch(const CreatorContextDispatch&) = delete;
    CreatorContextDispatch& operator=(const CreatorContextDispatch&) = delete;
    HRESULT install() noexcept {
        if (GetCurrentThreadId() != ticket_.creator || installed_ || !current_)
            return E_INVALIDARG;
        const auto verified = admission();
        if (verified != S_OK) return verified;
        SetLastError(ERROR_SUCCESS);
        if (!SetWindowSubclass(ticket_.owner, procedure, subclassId,
                reinterpret_cast<DWORD_PTR>(this))) return nativeError();
        installed_ = true; return S_OK;
    }
    ~CreatorContextDispatch() {
        // Never return into a callback using a destroyed stack object. No
        // killing/detaching a shared server; this is only the test's process.
        if (active_ || GetCurrentThreadId() != ticket_.creator) std::_Exit(9);
        if (installed_ && IsWindow(ticket_.owner) &&
            !RemoveWindowSubclass(ticket_.owner, procedure, subclassId)) std::_Exit(9);
    }
private:
    HRESULT admission() const noexcept {
        if (GetCurrentThreadId() != ticket_.creator) return RPC_E_WRONG_THREAD;
        if (!ticket_.sequence || !ticket_.applicationCommand || !ticket_.physicalCommand ||
            ticket_.ownerIdentity.window != ticket_.owner ||
            GetTickCount64() >= ticket_.deadline)
            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        const auto* desktop = explorer::PrivateDesktop::current();
        if (!desktop) return E_ACCESSDENIED;
        const auto isolated = desktop->verifyIsolation();
        if (isolated != S_OK) return isolated;
        auto result = sameOwnedWindow(ticket_.ownerIdentity, ticket_.owner, ticket_.creator);
        if (result != S_OK) return result;
        result = sameOwnedWindow(ticket_.receiverIdentity, ticket_.owner, ticket_.creator);
        if (result != S_OK) return result;
        if (!IsWindowVisible(ticket_.owner) || !IsWindowVisible(ticket_.receiverIdentity.window))
            return E_ACCESSDENIED;
        const auto point = ticket_.screenPoint;
        if (!PtInRect(&ticket_.elementBounds, point) ||
            !PtInRect(&ticket_.receiverIdentity.bounds, point) ||
            point.x < SHRT_MIN || point.x > SHRT_MAX ||
            point.y < SHRT_MIN || point.y > SHRT_MAX) return E_INVALIDARG;
        if (!current_(source_, ticket_)) return E_ABORT;
        return GetTickCount64() < ticket_.deadline ? S_OK : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
    // Map the already proven physical element point into this exact native
    // receiver's client coordinates. One-point MapWindowPoints preserves RTL
    // semantics; zero displacement is valid only with unchanged last error.
    HRESULT clientPoint(POINT& output) const noexcept {
        POINT point = ticket_.screenPoint;
        SetLastError(ERROR_SUCCESS);
        const auto mapped = MapWindowPoints(nullptr, ticket_.receiverIdentity.window, &point, 1);
        if (!mapped && GetLastError() != ERROR_SUCCESS) return nativeError();
        RECT client{};
        SetLastError(ERROR_SUCCESS);
        if (!GetClientRect(ticket_.receiverIdentity.window, &client)) return nativeError();
        if (client.left >= client.right || client.top >= client.bottom || !PtInRect(&client, point) ||
            point.x < SHRT_MIN || point.x > SHRT_MAX || point.y < SHRT_MIN || point.y > SHRT_MAX)
            return E_INVALIDARG;
        POINT roundTrip = point;
        SetLastError(ERROR_SUCCESS);
        const auto restored = MapWindowPoints(ticket_.receiverIdentity.window, nullptr, &roundTrip, 1);
        if (!restored && GetLastError() != ERROR_SUCCESS) return nativeError();
        if (roundTrip.x != ticket_.screenPoint.x || roundTrip.y != ticket_.screenPoint.y) return E_ABORT;
        output = point;
        return S_OK;
    }
    HRESULT completionAdmission() const noexcept {
        if (GetCurrentThreadId() != ticket_.creator) return RPC_E_WRONG_THREAD;
        if (GetTickCount64() >= ticket_.deadline) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        const auto* desktop = explorer::PrivateDesktop::current();
        if (!desktop) return E_ACCESSDENIED;
        const auto isolated = desktop->verifyIsolation();
        if (isolated != S_OK) return isolated;
        const auto owner = sameOwnedWindow(ticket_.ownerIdentity, ticket_.owner, ticket_.creator);
        if (owner != S_OK) return owner;
        if (!IsWindowVisible(ticket_.owner)) return E_ACCESSDENIED;
        // A successful native Remove may legitimately destroy the receiver or
        // resize its ancestor. This is reported separately, never used to fake
        // a successful gesture; the creator's whole native row oracle decides.
        if (!current_(source_, ticket_)) return E_ABORT;
        return GetTickCount64() < ticket_.deadline ? S_OK : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
    HRESULT cancellationAdmission() const noexcept {
        if (GetCurrentThreadId() != ticket_.creator || !ticket_.drainDeadline ||
            ticket_.drainDeadline < ticket_.deadline || ticket_.drainDeadline - ticket_.deadline > 500 ||
            GetTickCount64() >= ticket_.drainDeadline) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        const auto* desktop = explorer::PrivateDesktop::current();
        if (!desktop) return E_ACCESSDENIED;
        const auto isolated = desktop->verifyIsolation();
        if (isolated != S_OK) return isolated;
        const auto owner = sameOwnedWindow(ticket_.ownerIdentity, ticket_.owner, ticket_.creator);
        if (owner != S_OK) return owner;
        return current_(source_, ticket_) ? S_OK : E_ABORT;
    }
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wParam,
                                      LPARAM lParam, UINT_PTR, DWORD_PTR reference) noexcept {
        auto* bridge = reinterpret_cast<CreatorContextDispatch*>(reference);
        if (!bridge || (message != dispatchMessage && message != cancelMessage))
            return DefSubclassProc(window, message, wParam, lParam);
        if (window != bridge->ticket_.owner || wParam != bridge->ticket_.sequence || lParam)
            return 0; // Ignore stale/plain unmatched trigger; no native gesture.
        if (message == cancelMessage) {
            if (GetCurrentThreadId() != bridge->ticket_.creator ||
                !bridge->receipt_.entered.load(std::memory_order_acquire) ||
                bridge->cancellationAdmission() != S_OK) return 0;
            bridge->receipt_.cancellationRequested.store(true);
            // Only the exact current receiver is sent this native cancellation.
            // EndMenu is used only when this creator actually owns a live menu.
            const auto currentReceiver = sameOwnedWindow(bridge->ticket_.receiverIdentity,
                bridge->ticket_.owner, bridge->ticket_.creator);
            if (currentReceiver == S_OK)
                SendMessageW(bridge->ticket_.receiverIdentity.window, WM_CANCELMODE, 0, 0);
            GUITHREADINFO info{sizeof(info)};
            if (GetGUIThreadInfo(bridge->ticket_.creator, &info) &&
                (info.flags & GUI_INMENUMODE) && info.hwndMenuOwner) {
                DWORD process = 0;
                if (GetWindowThreadProcessId(info.hwndMenuOwner, &process) == bridge->ticket_.creator &&
                    process == GetCurrentProcessId() &&
                    (info.hwndMenuOwner == bridge->ticket_.owner || IsChild(bridge->ticket_.owner, info.hwndMenuOwner)))
                    bridge->receipt_.endMenuResult.store(EndMenu());
            }
            return 0;
        }
        bool unused = false;
        if (!bridge->receipt_.consumed.compare_exchange_strong(unused, true)) return 0;
        ++bridge->active_;
        const auto before = bridge->admission(); bridge->receipt_.before.store(before);
        if (before == S_OK) {
            bridge->receipt_.entered.store(true, std::memory_order_release);
            POINT point{};
            auto result = bridge->clientPoint(point);
            const auto capture = GetCapture();
            bridge->receipt_.captureBefore.store(capture);
            // Do not steal an existing creator capture or redirect the gesture.
            if (result == S_OK && capture) result = E_ABORT;
            if (result == S_OK) result = bridge->admission();
            bridge->receipt_.clientBefore.store(result);
            if (result == S_OK) {
                bridge->receipt_.clientX.store(point.x); bridge->receipt_.clientY.store(point.y);
                const auto coordinates = MAKELPARAM(static_cast<WORD>(point.x), static_cast<WORD>(point.y));
                bridge->receipt_.downEntered.store(true, std::memory_order_release);
                bridge->receipt_.downReturn.store(SendMessageW(bridge->ticket_.receiverIdentity.window,
                    WM_RBUTTONDOWN, MK_RBUTTON, coordinates));
                bridge->receipt_.downReturned.store(true, std::memory_order_release);
                // Down may pump providers, reset the framework, or destroy the
                // original receiver. Never send Up using healed/stale authority.
                result = bridge->admission();
                const auto captureAfter = GetCapture();
                bridge->receipt_.captureAfterDown.store(captureAfter);
                if (result == S_OK && captureAfter && captureAfter != bridge->ticket_.receiverIdentity.window)
                    result = E_ABORT;
                POINT currentPoint{};
                if (result == S_OK) result = bridge->clientPoint(currentPoint);
                if (result == S_OK && (currentPoint.x != point.x || currentPoint.y != point.y)) result = E_ABORT;
                if (result == S_OK) result = bridge->admission();
                bridge->receipt_.beforeUp.store(result);
                if (result == S_OK) {
                    bridge->receipt_.upEntered.store(true, std::memory_order_release);
                    // Exactly one Up completes the native control's right-click
                    // path. No explicit WM_CONTEXTMENU or alternative retry.
                    bridge->receipt_.nativeReturn.store(SendMessageW(bridge->ticket_.receiverIdentity.window,
                        WM_RBUTTONUP, 0, coordinates));
                    bridge->receipt_.upReturned.store(true, std::memory_order_release);
                }
            }
            bridge->receipt_.receiverAfter.store(sameOwnedWindow(
                bridge->ticket_.receiverIdentity, bridge->ticket_.owner, bridge->ticket_.creator));
            bridge->receipt_.after.store(result == S_OK ? bridge->completionAdmission() : result);
        } else bridge->receipt_.after.store(before);
        bridge->receipt_.returned.store(true, std::memory_order_release);
        --bridge->active_; return 0;
    }
    const ContextTicket& ticket_;
    ContextReceipt& receipt_;
    CreatorSourceCurrent current_ = nullptr;
    void* source_ = nullptr;
    unsigned active_ = 0;
    bool installed_ = false;
};
} // namespace qat_gesture_protocol
