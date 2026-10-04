#pragma once

#include <windows.h>
#include <inspectable.h>
#include <shobjidl.h>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace explorer {
struct SharePayload {
    std::wstring title;
    std::vector<std::filesystem::path> files;
    std::vector<std::wstring> fileTypes;
};

inline constexpr size_t maximumShareFiles = 256;
inline constexpr size_t maximumShareTitle = 256;

// Bounded, read-only planning: existing filesystem files only. Folders, virtual
// items, device/ADS paths, duplicates and reparse points are rejected. Output is
// unchanged on failure. No clipboard, Share UI, recipient or content reads.
HRESULT makeSharePayload(const std::vector<std::filesystem::path>& files,
                         const std::wstring& title, SharePayload& output);
HRESULT makeSharePayload(IShellItemArray* selection, const std::wstring& title,
                         SharePayload& output);

// One object per app window. Call instance methods on the owning STA and keep
// its message pump running. Reset before destroying the HWND
// or uninitializing COM. Data providers retain immutable payloads, never `this`.
class NativeShare final {
public:
    NativeShare();
    ~NativeShare();
    NativeShare(const NativeShare&) = delete;
    NativeShare& operator=(const NativeShare&) = delete;

    // Acquires desktop WinRT interop and subscribes events, without showing UI.
    // Optional WM_APP..0xBFFF message reports async HRESULT in wParam.
    HRESULT initialize(HWND owner, UINT resultMessage = 0);
    // The only method that displays system Share UI. Never call in headless mode.
    HRESULT show(const SharePayload& payload);
    // Creates an actual deferred StorageItems DataPackage, without displaying
    // UI. Caller owns the returned IInspectable reference. Useful for headless
    // tests, and requires successful initialize on the owning STA.
    HRESULT makePackage(const SharePayload& payload, IInspectable** package);
    void reset() noexcept;
    bool ready() const noexcept;
    // Share UI dispatch success is distinct from recipient delivery. This is
    // the latest package-resolution HRESULT (E_PENDING while resolving).
    HRESULT lastResult() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
