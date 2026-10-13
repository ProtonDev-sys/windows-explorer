#pragma once

// Source-only draft. Root must review/integrate into its existing owned STA and
// private-desktop test harness before compiling or executing it.
// This is an independent raw framework handler, not production StockFramework.

#include <windows.h>
#include <uiribbon.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <new>
#include <utility>
#include <vector>

namespace installed_image_contract_draft {
using Microsoft::WRL::ComPtr;
using ImageKey = std::pair<UINT, bool>; // physical native command, LargeImage

struct Observation {
    std::uint64_t ordinal = 0;
    bool currentPresent = false;
    VARTYPE currentType = VT_EMPTY;
    HRESULT imageQuery = E_PENDING;
    HRESULT returned = E_PENDING;
    ComPtr<IUIImage> currentImage;
    DWORD creatorThread = 0;
    HWND window = nullptr;
    UINT hwndDpi = 0;
    std::uint64_t windowGeneration = 0;
};

class InstalledImageObserver final : public IUIApplication, public IUICommandHandler {
public:
    explicit InstalledImageObserver(HWND owner = nullptr, std::uint64_t generation = 0)
        : ownerWindow_(owner), ownerGeneration_(generation) {}
    const DWORD creatorThread = GetCurrentThreadId();
    std::map<UINT, UI_COMMANDTYPE> commandTypes;
    std::map<ImageKey, std::vector<Observation>> observations;
    std::uint64_t imageRequests = 0, droppedImageRequests = 0;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (id == IID_IUnknown || id == __uuidof(IUIApplication))
            *output = static_cast<IUIApplication*>(this);
        else if (id == __uuidof(IUICommandHandler))
            *output = static_cast<IUICommandHandler*>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto remaining = --references_;
        if (!remaining) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE OnViewChanged(UINT32, UI_VIEWTYPE, IUnknown*, UI_VIEWVERB verb, INT32 reason) override {
        return verb == UI_VIEWVERB_ERROR ? reason : S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnCreateUICommand(UINT32 command, UI_COMMANDTYPE type, IUICommandHandler** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (GetCurrentThreadId() != creatorThread) return RPC_E_WRONG_THREAD;
        try { commandTypes.insert_or_assign(command, type); }
        catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
        *output = static_cast<IUICommandHandler*>(this);
        AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDestroyUICommand(UINT32, UI_COMMANDTYPE, IUICommandHandler*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Execute(UINT32, UI_EXECUTIONVERB, const PROPERTYKEY*, const PROPVARIANT*, IUISimplePropertySet*) override {
        return E_ACCESSDENIED;
    }
    HRESULT STDMETHODCALLTYPE UpdateProperty(UINT32 command, REFPROPERTYKEY key, const PROPVARIANT* current, PROPVARIANT* output) override {
        if (!output) return E_POINTER;
        PropVariantInit(output);
        if (GetCurrentThreadId() != creatorThread) return RPC_E_WRONG_THREAD;
        const bool large = IsEqualPropertyKey(key, UI_PKEY_LargeImage);
        if (large || IsEqualPropertyKey(key, UI_PKEY_SmallImage)) {
            ++imageRequests;
            try {
                Observation event;
                event.ordinal = ++ordinal_;
                event.creatorThread = creatorThread;
                event.window = ownerWindow_;
                event.hwndDpi = ownerWindow_ ? GetDpiForWindow(ownerWindow_) : 0;
                event.windowGeneration = ownerGeneration_;
                event.currentPresent = current != nullptr;
                if (current) event.currentType = current->vt;
                event.imageQuery = current && current->vt == VT_UNKNOWN && current->punkVal
                    ? current->punkVal->QueryInterface(IID_PPV_ARGS(&event.currentImage)) : E_NOINTERFACE;
                // Own only the actual supplied current object; no image factory,
                // extraction, GetBitmap or IUIFramework reentry in this callback.
                event.returned = SUCCEEDED(event.imageQuery) && event.currentImage ? S_OK : E_NOTIMPL;
                // QI and PropVariantCopy/AddRef can reenter the native handler.
                // Finish external retention on this local event; hold no map or
                // vector reference across it. ordinal was captured at entry, so
                // nested completion/append order cannot redefine first current.
                if (event.returned == S_OK) event.returned = PropVariantCopy(output, current);
                const HRESULT returned = event.returned;
                observations[ImageKey{command, large}].push_back(std::move(event));
                return returned;
            } catch (const std::bad_alloc&) { ++droppedImageRequests; return E_OUTOFMEMORY; }
        }
        // Preserve genuine current text just as the existing independent label
        // reference does. No state/eligibility or missing resources are invented.
        if ((IsEqualPropertyKey(key, UI_PKEY_Label) || IsEqualPropertyKey(key, UI_PKEY_TooltipTitle)) &&
            current && current->vt == VT_LPWSTR && current->pwszVal && *current->pwszVal)
            return PropVariantCopy(output, current);
        return E_NOTIMPL;
    }

private:
    std::atomic<ULONG> references_{1};
    std::uint64_t ordinal_ = 0;
    HWND ownerWindow_ = nullptr;
    std::uint64_t ownerGeneration_ = 0;
};

struct RawDib {
    // Borrowed original handle/storage identity; caller retains IUIImage while
    // using these fields. Never delete/select/write this bitmap.
    HBITMAP bitmap = nullptr;
    const void* sourceBits = nullptr;
    HRESULT bitmapRead = E_PENDING;
    int objectBytes = 0;
    BITMAPINFOHEADER header{};
    LONG width = 0, height = 0, stride = 0;
    WORD planes = 0, bitDepth = 0;
    DWORD channelMasks[3]{};
    std::vector<BYTE> storedBytes; // exact stored scanlines, including alpha/padding
};

// Call only after UpdateProperty and FlushPendingInvalidations have returned,
// on the owning STA, retaining image throughout. Never delete/select/write its
// borrowed HBITMAP. Unsupported format is evidence, not a synthesized fallback.
inline HRESULT copyActualRawDib(IUIImage* image, DWORD creatorThread, RawDib& output) {
    output = {};
    if (GetCurrentThreadId() != creatorThread) return RPC_E_WRONG_THREAD;
    if (!image) return E_POINTER;
    HBITMAP bitmap = nullptr;
    output.bitmapRead = image->GetBitmap(&bitmap);
    if (FAILED(output.bitmapRead)) return output.bitmapRead;
    if (!bitmap) return E_UNEXPECTED;
    output.bitmap = bitmap;
    if (!GdiFlush()) return HRESULT_FROM_WIN32(GetLastError() ? GetLastError() : ERROR_GEN_FAILURE);
    DIBSECTION section{};
    output.objectBytes = GetObjectW(bitmap, sizeof(section), &section);
    if (output.objectBytes != sizeof(section)) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    const auto& native = section.dsBm;
    output.sourceBits = native.bmBits;
    output.header = section.dsBmih;
    output.width = native.bmWidth;
    output.height = native.bmHeight;
    output.stride = native.bmWidthBytes;
    output.planes = native.bmPlanes;
    output.bitDepth = native.bmBitsPixel;
    std::memcpy(output.channelMasks, section.dsBitfields, sizeof(output.channelMasks));
    // Bound raw copies, retain actual dimensions/format, and avoid signed overflow.
    const auto rows = native.bmHeight < 0 ? -static_cast<std::int64_t>(native.bmHeight) : native.bmHeight;
    const auto headerRows = section.dsBmih.biHeight < 0
        ? -static_cast<std::int64_t>(section.dsBmih.biHeight) : section.dsBmih.biHeight;
    if (!native.bmBits || native.bmWidth <= 0 || rows <= 0 || rows > 4096 ||
        native.bmWidth > 4096 || native.bmWidthBytes <= 0 || native.bmPlanes != 1 ||
        native.bmBitsPixel != 32 || section.dsBmih.biBitCount != 32 ||
        section.dsBmih.biPlanes != 1 || section.dsBmih.biCompression != BI_RGB ||
        section.dsBmih.biWidth != native.bmWidth || headerRows != rows ||
        native.bmWidthBytes < native.bmWidth * 4)
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    const auto bytes = static_cast<std::uint64_t>(native.bmWidthBytes) * static_cast<std::uint64_t>(rows);
    if (bytes > 64ULL * 1024 * 1024) return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
    try {
        output.storedBytes.resize(static_cast<std::size_t>(bytes));
        std::memcpy(output.storedBytes.data(), native.bmBits, output.storedBytes.size());
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    return S_OK;
}

// Requery the actual retained image, then require the same original bitmap,
// DIB metadata, backing storage and all four stored bytes. A changed object or
// source storage is unavailable evidence, never another image to compare.
inline HRESULT verifyActualRawDib(IUIImage* image, DWORD creatorThread, const RawDib& original) {
    if (GetCurrentThreadId() != creatorThread) return RPC_E_WRONG_THREAD;
    if (!image || !original.bitmap || !original.sourceBits || original.storedBytes.empty()) return E_UNEXPECTED;
    HBITMAP actual = nullptr;
    const auto result = image->GetBitmap(&actual);
    if (FAILED(result)) return result;
    if (actual != original.bitmap) return HRESULT_FROM_WIN32(ERROR_RETRY);
    if (!GdiFlush()) return HRESULT_FROM_WIN32(GetLastError() ? GetLastError() : ERROR_GEN_FAILURE);
    DIBSECTION section{};
    if (GetObjectW(original.bitmap, sizeof(section), &section) != original.objectBytes)
        return HRESULT_FROM_WIN32(ERROR_RETRY);
    const auto& native = section.dsBm;
    if (native.bmBits != original.sourceBits || native.bmWidth != original.width ||
        native.bmHeight != original.height || native.bmWidthBytes != original.stride ||
        native.bmPlanes != original.planes || native.bmBitsPixel != original.bitDepth ||
        std::memcmp(&section.dsBmih, &original.header, sizeof(original.header)) != 0 ||
        std::memcmp(section.dsBitfields, original.channelMasks, sizeof(original.channelMasks)) != 0)
        return HRESULT_FROM_WIN32(ERROR_RETRY);
    if (std::memcmp(native.bmBits, original.storedBytes.data(), original.storedBytes.size()) != 0)
        return HRESULT_FROM_WIN32(ERROR_RETRY);
    return S_OK;
}

} // namespace installed_image_contract_draft
