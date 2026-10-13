#include "explorer/native_apartment.hpp"
// Source-only packet: merge the reviewed production diagnostic patch first.
#include "explorer/ribbon.hpp"
#include "explorer/commands.hpp"
#include "explorer/namespace_actions.hpp"
#include "explorer/headless_crash.hpp"
#include "explorer/worker_sta.hpp"
#include <shlobj.h>
#include <shlwapi.h>
#include "explorer/headless_visual.hpp"
#include "stock_image_observer.hpp"
#include "native_icon_reference.hpp"
#include <UIRibbonPropertyHelpers.h>
#include <commctrl.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {
using Microsoft::WRL::ComPtr;
using namespace installed_image_contract_draft;
constexpr wchar_t hostClass[] = L"WindowsExplorerOwnedImageProvenance";
ULONGLONG admissionDeadline = 0;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void exact(HRESULT result, const char* message) {
    if (result != S_OK) throw std::runtime_error(std::string(message) + " HRESULT=" + std::to_string(static_cast<ULONG>(result)));
}
struct Skip { std::string reason; };
struct Window {
    HWND handle = nullptr;
    Window() {
        handle = CreateWindowExW(0, hostClass, L"Owned image provenance", WS_OVERLAPPEDWINDOW,
            0, 0, 1000, 700, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        require(handle != nullptr, "Create exclusively owned private HWND");
    }
    ~Window() { if (handle && !DestroyWindow(handle)) std::terminate(); }
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
};
void guard(HWND owner) {
    require(GetTickCount64() < admissionDeadline, "Image fixture exceeded its 65-second admission bound");
    const auto desktop = explorer::PrivateDesktop::current();
    require(desktop != nullptr, "Image fixture lost its private desktop guard");
    exact(desktop->verifyIsolation(), "Verify native private desktop identity");
    DWORD process = 0;
    require(IsWindow(owner) && GetWindowThreadProcessId(owner, &process) == GetCurrentThreadId() &&
        process == GetCurrentProcessId(), "Image fixture refuses an unowned HWND");
    bool visible = true;
    exact(desktop->visibleWindowsOnInputDesktop(visible), "Observe private fixture input-desktop isolation");
    require(!visible, "Owned image fixture exposed an input-desktop window");
    const UINT dpi = GetDpiForWindow(owner);
    if (dpi != 96) throw Skip{"actual owned HWND DPI=" + std::to_string(dpi) + "; required 96"};
}
void realize(HWND owner) {
    guard(owner);
    // First ShowWindow can be overridden by STARTUPINFO. This exact owned HWND
    // is shown without activation through flags, with actual native readback.
    require(SetWindowPos(owner, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW) != FALSE,
        "Show only the owned HWND on its private desktop");
    const auto style = GetWindowLongPtrW(owner, GWL_STYLE);
    WINDOWPLACEMENT placement{}; placement.length = sizeof(placement);
    RECT client{};
    require((style & WS_VISIBLE) && IsWindowVisible(owner) && GetWindowPlacement(owner, &placement) &&
        GetClientRect(owner, &client) && client.right > client.left && client.bottom > client.top,
        "Owned HWND style/show/client readback failed");
    std::cout << "OWNED_SHOW window=" << reinterpret_cast<UINT_PTR>(owner) << " style=" << style
        << " showCmd=" << placement.showCmd << " clientWidth=" << client.right - client.left
        << " clientHeight=" << client.bottom - client.top << '\n';
    UpdateWindow(owner);
    const auto until = GetTickCount64() + 35;
    do {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        MsgWaitForMultipleObjectsEx(0, nullptr, 2, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    } while (GetTickCount64() < until);
    guard(owner);
}
struct ProductionRow {
    explorer::RibbonImageObservation metadata;
    ComPtr<IUIImage> current, returned;
};
struct ProductionCollector {
    std::vector<ProductionRow> rows;
    bool allocationFailure = false;
    bool armed = false;
    explorer::NativeRibbon* owner = nullptr;
    HRESULT reentryLabel = E_PENDING, reentryInvalidation = E_PENDING;
    void observe(const explorer::RibbonImageObservation& event) {
        // No bitmap reads, framework calls, image extraction or state queries.
        try {
            ProductionRow row;
            row.metadata = event;
            row.current = event.currentImage;
            row.returned = event.returnedImage;
            rows.push_back(std::move(row));
        } catch (...) { allocationFailure = true; }
        // These admission-control calls are required to stop at the diagnostic
        // delivery guard; neither is allowed to reach IUIFramework.
        if (armed && owner) {
            armed = false;
            std::wstring ignored;
            reentryLabel = owner->commandLabel(explorer::Copy, ignored);
            reentryInvalidation = owner->invalidateState(explorer::Copy);
        }
    }
};
struct IndependentStockFramework {
    Window window;
    HMODULE module = nullptr;
    ComPtr<InstalledImageObserver> handler;
    ComPtr<IUIFramework> framework;
    ~IndependentStockFramework() {
        if (framework) framework->Destroy();
        framework.Reset(); handler.Reset();
        if (module) FreeLibrary(module);
    }
    void initialize(std::uint64_t generation, UINT32 modes) {
        guard(window.handle);
        module = LoadLibraryExW(L"ExplorerFrame.dll", nullptr,
            LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        if (!module) throw Skip{"installed ExplorerFrame.dll unavailable; HRESULT=" +
            std::to_string(static_cast<ULONG>(HRESULT_FROM_WIN32(GetLastError())))};
        handler.Attach(new InstalledImageObserver(window.handle, generation));
        exact(CoCreateInstance(CLSID_UIRibbonFramework, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&framework)),
            "Create fresh independent real native framework");
        exact(framework->Initialize(window.handle, handler.Get()), "Initialize independent owned native framework");
        exact(framework->LoadUI(module, L"EXPLORER_RIBBON"), "Load actual unmodified installed EXPLORER_RIBBON");
        exact(framework->SetModes(modes), "Select independent installed feature mode");
        // Match NativeRibbon's initial context suppression, using only actual
        // registered context IDs. No command Enabled property is changed.
        for (const auto& [id, type] : handler->commandTypes) if (type == UI_COMMANDTYPE_CONTEXT) {
            PROPVARIANT value{};
            exact(InitPropVariantFromUInt32(UI_CONTEXTAVAILABILITY_NOTAVAILABLE, &value), "Native context baseline value");
            const auto result = framework->SetUICommandProperty(id, UI_PKEY_ContextAvailable, value);
            PropVariantClear(&value);
            exact(result, "Match actual initial hidden contexts");
        }
        realize(window.handle);
    }
};

struct PixelRead {
    RawDib raw;
    HRESULT rawStatus = E_PENDING, rgbRead = E_PENDING, orientation = E_PENDING;
    HRESULT identityBeforeRgb = E_PENDING, identityAfterRgb = E_PENDING;
    bool storedTopDown = false;
    std::vector<BYTE> topDownStoredPixels;
};
PixelRead readActual(IUIImage* image, HWND owner, DWORD creatorThread) {
    guard(owner);
    PixelRead read;
    // AddRef may pump native work; fence ownership/DPI/deadline after it, and
    // retain this exact IUIImage across raw/RGB/identity checks and DC release.
    ComPtr<IUIImage> retained = image;
    guard(owner);
    const auto finish = [&]() -> PixelRead {
        retained.Reset(); // external Release precedes the final owner fence
        guard(owner);
        return std::move(read);
    };
    read.rawStatus = copyActualRawDib(retained.Get(), creatorThread, read.raw);
    guard(owner);
    if (FAILED(read.rawStatus)) return finish();
    read.identityBeforeRgb = verifyActualRawDib(retained.Get(), creatorThread, read.raw);
    guard(owner);
    if (FAILED(read.identityBeforeRgb)) return finish();
    const HBITMAP bitmap = read.raw.bitmap;
    const auto width = static_cast<UINT>(read.raw.width);
    const auto rows = static_cast<UINT>(read.raw.height < 0 ? -static_cast<std::int64_t>(read.raw.height) : read.raw.height);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = static_cast<LONG>(width);
    info.bmiHeader.biHeight = -static_cast<LONG>(rows); info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    std::vector<BYTE> rgb(static_cast<std::size_t>(width) * rows * 4);
    const HDC dc = CreateCompatibleDC(nullptr);
    struct OwnedDc {HDC value;~OwnedDc(){if(value)DeleteDC(value);}} ownedDc{dc};
    guard(owner);
    if (!dc) { read.rgbRead = E_OUTOFMEMORY; return finish(); }
    // Never select the framework's bitmap into this or any other DC.
    const int copied = GetDIBits(dc, bitmap, 0, rows, rgb.data(), &info, DIB_RGB_COLORS);
    const bool releasedDc = DeleteDC(dc) != FALSE;
    if(releasedDc)ownedDc.value=nullptr;
    guard(owner);
    read.rgbRead = copied == static_cast<int>(rows) && releasedDc ? S_OK : E_FAIL;
    // Validate against the original handle/storage after GetDIBits and its
    // temporary DC release, before using its RGB to orient the raw snapshot.
    read.identityAfterRgb = verifyActualRawDib(retained.Get(), creatorThread, read.raw);
    guard(owner);
    if (FAILED(read.rgbRead) || FAILED(read.identityAfterRgb)) return finish();
    bool direct = true, reverse = true, rawSymmetric = true;
    for (UINT row = 0; row < rows; ++row) for (UINT column = 0; column < width; ++column) {
        const auto top = rgb.data() + (static_cast<std::size_t>(row) * width + column) * 4;
        const auto stored = read.raw.storedBytes.data() + static_cast<std::size_t>(row) * read.raw.stride + column * 4;
        const auto reversed = read.raw.storedBytes.data() + static_cast<std::size_t>(rows - row - 1) * read.raw.stride + column * 4;
        direct = direct && std::memcmp(top, stored, 3) == 0;
        reverse = reverse && std::memcmp(top, reversed, 3) == 0;
        rawSymmetric = rawSymmetric && std::memcmp(stored, reversed, 4) == 0;
    }
    if ((!direct && !reverse) || (direct && reverse && !rawSymmetric)) {
        read.orientation = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED); return finish();
    }
    read.storedTopDown = direct;
    read.topDownStoredPixels.resize(rgb.size());
    for (UINT row = 0; row < rows; ++row) {
        const auto storedRow = direct ? row : rows - row - 1;
        std::memcpy(read.topDownStoredPixels.data() + static_cast<std::size_t>(row) * width * 4,
            read.raw.storedBytes.data() + static_cast<std::size_t>(storedRow) * read.raw.stride, width * 4);
    }
    // All four normalized bytes came from source DIB storage. GetDIBits is used
    // solely to validate RGB row orientation; none of its alpha bytes are used.
    read.orientation = S_OK;
    return finish();
}
std::uint64_t hashBytes(const std::vector<BYTE>& bytes) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto byte : bytes) { hash ^= byte; hash *= 1099511628211ULL; }
    return hash;
}
void diagnosePixels(const char* source, const PixelRead& read) {
    std::cout << " " << source << "Raw=" << static_cast<ULONG>(read.rawStatus)
        << " " << source << "Bitmap=" << static_cast<ULONG>(read.raw.bitmapRead)
        << " " << source << "originalHbitmap=" << reinterpret_cast<UINT_PTR>(read.raw.bitmap)
        << " " << source << "identityBeforeRgb=" << static_cast<ULONG>(read.identityBeforeRgb)
        << " " << source << "identityAfterRgb=" << static_cast<ULONG>(read.identityAfterRgb)
        << " " << source << "DibBytes=" << read.raw.objectBytes
        << " " << source << "W/H/stride=" << read.raw.width << '/' << read.raw.height << '/' << read.raw.stride
        << " " << source << "signedHeaderHeight=" << read.raw.header.biHeight
        << " " << source << "bpp/compression=" << read.raw.bitDepth << '/' << read.raw.header.biCompression
        << " " << source << "planes=" << read.raw.planes
        << " " << source << "channelMasks=" << read.raw.channelMasks[0] << '/' << read.raw.channelMasks[1] << '/' << read.raw.channelMasks[2]
        << " " << source << "storedBytes=" << read.raw.storedBytes.size()
        << " " << source << "storedHash=" << hashBytes(read.raw.storedBytes)
        << " " << source << "orientation=" << static_cast<ULONG>(read.orientation)
        << " " << source << "rgbRead=" << static_cast<ULONG>(read.rgbRead)
        << " " << source << "storedTopDown=" << read.storedTopDown
        << " " << source << "topDownHash=" << hashBytes(read.topDownStoredPixels);
}

enum class Scene { Home, Computer, Network, Compressed, Recycle, Drive };
const char* sceneName(Scene scene) {
    switch (scene) { case Scene::Home: return "Home"; case Scene::Computer: return "Computer";
    case Scene::Network: return "Network"; case Scene::Compressed: return "Compressed";
    case Scene::Recycle: return "Recycle"; case Scene::Drive: return "Drive"; }
    return "Unknown";
}
void context(IndependentStockFramework& independent, explorer::NativeRibbon& production, Scene scene) {
    explorer::RibbonContext selected = explorer::RibbonContext::None;
    if (scene == Scene::Compressed) selected = explorer::RibbonContext::Compressed;
    if (scene == Scene::Recycle) selected = explorer::RibbonContext::Recycle;
    if (scene == Scene::Drive) selected = explorer::RibbonContext::Drive;
    if (selected == explorer::RibbonContext::None) return;
    exact(production.setContexts(selected, true), "Activate production owned contextual resource scene");
    UINT native = 0, availability = 0;
    exact(production.contextAvailable(selected, native, availability), "Read actual production context ID");
    require(availability == UI_CONTEXTAVAILABILITY_ACTIVE && independent.handler->commandTypes.contains(native),
        "Independent framework lacks selected actual context variant");
    PROPVARIANT active{};
    exact(InitPropVariantFromUInt32(UI_CONTEXTAVAILABILITY_ACTIVE, &active), "Native contextual resource value");
    const auto status = independent.framework->SetUICommandProperty(native, UI_PKEY_ContextAvailable, active);
    PropVariantClear(&active);
    exact(status, "Activate matching independent context variant");
}

struct Totals { std::size_t actualProductionRequests = 0, actualIndependentRequests = 0, productionRows = 0;
    std::size_t physicalKeys = 0, genuineFirstCurrents = 0, comparable = 0, identical = 0, different = 0, unsupported = 0;
    std::size_t cachedNativeRepeatRows=0,nullFirstLaterHostImageRows=0; };
void run(Scene scene, bool largeFirst, std::uint64_t generation, Totals& totals) {
    Window productionWindow;
    guard(productionWindow.handle);
    ProductionCollector collector;
    explorer::NativeRibbon production;
    // On failure, retire native observer delivery before releasing any late
    // live rows: their Release cannot append into a vector being cleared.
    struct RetainedRows {ProductionCollector& collector;explorer::NativeRibbon& production;
        ~RetainedRows(){production.reset();collector.rows.clear();}} retainedRows{collector,production};
    collector.owner = &production;
    explorer::RibbonCallbacks callbacks;
    callbacks.observeImageRequest = [&](const explorer::RibbonImageObservation& event) { collector.observe(event); };
    exact(production.initialize(productionWindow.handle, GetModuleHandleW(nullptr), std::move(callbacks),
        explorer::RibbonLayout::InstalledWindows10), "Initialize actual production native handler");
    if (production.layout() != explorer::RibbonLayout::InstalledWindows10)
        throw Skip{"installed 19045 Ribbon unavailable; status=" + std::to_string(static_cast<ULONG>(production.installedLayoutStatus()))};
    const auto features = production.features();
    UINT32 modes = 0xa1 | (features.discBurning ? 0x20000 : 0x40000);
    if (scene == Scene::Computer) {
        exact(production.setComputerMode(true), "Production Computer resource mode");
        modes = 4 | (features.mediaFoundation ? 0x2000 : 0x4000);
    } else if (scene == Scene::Network) {
        exact(production.setNetworkMode(true, true), "Production Network resource mode"); modes = 0x202;
    }
    IndependentStockFramework independent;
    independent.initialize(generation, modes);
    context(independent, production, scene);
    realize(productionWindow.handle); realize(independent.window.handle);
    auto native = production.nativeFramework();
    require(native != nullptr, "Actual production framework missing");
    // This opt-in callback verifies reentry cannot enter native APIs. It never
    // calls public commandImage and never extracts an unrequested image.
    collector.armed = true;
    std::set<UINT> targeted;
    for (const auto& [command, type] : independent.handler->commandTypes) {
        if (type == UI_COMMANDTYPE_CONTEXT || type == UI_COMMANDTYPE_UNKNOWN) continue;
        targeted.insert(command);
    }
    for (const bool large : std::array{largeFirst, !largeFirst}) {
        const auto& key = large ? UI_PKEY_LargeImage : UI_PKEY_SmallImage;
        for (const UINT command : targeted) {
            guard(productionWindow.handle); guard(independent.window.handle);
            const auto referenceInvalidation = independent.framework->InvalidateUICommand(command, UI_INVALIDATIONS_PROPERTY, &key);
            const auto productionInvalidation = native->InvalidateUICommand(command, UI_INVALIDATIONS_PROPERTY, &key);
            // Failed unsupported keys are recorded, never converted to success.
            std::cout << "REQUEST scene=" << sceneName(scene) << " generation=" << generation << " native=" << command
                << " large=" << large << " independentInvalidation=" << static_cast<ULONG>(referenceInvalidation)
                << " productionInvalidation=" << static_cast<ULONG>(productionInvalidation) << '\n';
        }
        exact(independent.framework->FlushPendingInvalidations(), "Complete real independent image requests");
        exact(production.flush(), "Complete real production image requests");
        realize(independent.window.handle); realize(productionWindow.handle);
    }
    require(!collector.allocationFailure, "Production collector dropped actual requests");
    if (!collector.armed) require(collector.reentryLabel == E_PENDING && collector.reentryInvalidation == E_PENDING,
        "Diagnostic observer reentered creator-STA native properties");
    explorer::RibbonImageObservationStats requestStats;
    exact(production.imageObservationStats(requestStats), "Read actual opt-in image callback counters after return");
    require(requestStats.dropped == 0 && requestStats.reentrant == 0 && requestStats.delivered == collector.rows.size() &&
        requestStats.requests == collector.rows.size(), "Diagnostic coverage dropped or duplicated an actual image request");
    std::size_t independentRowCount = 0;
    for (const auto& [key, events] : independent.handler->observations) independentRowCount += events.size();
    const auto independentRequestCount = independent.handler->imageRequests;
    require(independent.handler->droppedImageRequests == 0 && independentRequestCount == independentRowCount,
        "Independent observer dropped an actual native resource request");
    const auto measurementCurrent = [&] {
        guard(productionWindow.handle); guard(independent.window.handle);
        require(production.nativeFramework() == native && independent.framework,
            "Actual image framework binding changed during immutable comparison");
        explorer::RibbonImageObservationStats after;
        exact(production.imageObservationStats(after), "Read actual image counters after external pixel/retention work");
        guard(productionWindow.handle); guard(independent.window.handle);
        std::size_t lateIndependentRows = 0;
        for (const auto& [key, events] : independent.handler->observations) lateIndependentRows += events.size();
        const bool stable = !collector.allocationFailure && collector.rows.empty() && lateIndependentRows == 0 &&
            after.requests == requestStats.requests && after.delivered == requestStats.delivered &&
            after.dropped == 0 && after.reentrant == 0 && independent.handler->droppedImageRequests == 0 &&
            independent.handler->imageRequests == independentRequestCount;
        if (!stable) std::cout << "LATE_CALLBACKS scene=" << sceneName(scene) << " generation=" << generation
            << " productionRequestsBefore/After=" << requestStats.requests << '/' << after.requests
            << " productionDeliveredBefore/After=" << requestStats.delivered << '/' << after.delivered
            << " productionDropped/reentrant=" << after.dropped << '/' << after.reentrant
            << " liveProductionRows=" << collector.rows.size()
            << " independentRequestsBefore/After=" << independentRequestCount << '/' << independent.handler->imageRequests
            << " liveIndependentRows=" << lateIndependentRows << " independentDropped=" << independent.handler->droppedImageRequests
            << " evidence=NOT_ADMITTED\n";
        require(stable, "Actual image callbacks changed resource coverage during immutable comparison; no comparison admitted");
    };
    {
    // Swap into immutable buffers without IUIImage AddRef/Release. Native
    // callbacks retain their ordinary live collectors, now empty. No row or
    // reference used below can be invalidated by an external image call.
    const auto productionRows = [&] {
        std::vector<ProductionRow> saved; saved.swap(collector.rows); return saved;
    }();
    const auto independentRows = [&] {
        decltype(independent.handler->observations) saved;
        saved.swap(independent.handler->observations); return saved;
    }();
    std::map<ImageKey, const ProductionRow*> firstProduction;
    for (const auto& row : productionRows) {
        require(row.metadata.creatorThread == GetCurrentThreadId() && row.metadata.window == productionWindow.handle &&
            row.metadata.hwndDpi == 96 && row.metadata.windowGeneration != 0 && row.metadata.installedLayout,
            "Production observation lost its actual HWND/DPI/creator provenance");
        require(row.metadata.completion && row.metadata.completion->bindingStable && !row.metadata.completion->observerThrew &&
            row.metadata.completion->result == row.metadata.normalUpdateResult,
            "Production diagnostic row did not describe a stable actual returned property");
        std::cout << "PRODUCTION_EVENT scene=" << sceneName(scene) << " hostGeneration=" << generation
            << " bindingGeneration=" << row.metadata.windowGeneration << " ordinal=" << row.metadata.ordinal
            << " callbackEpoch=" << row.metadata.callbackEpoch
            << " native=" << row.metadata.nativeCommand << " application=" << row.metadata.applicationCommand
            << " nativeType=" << row.metadata.nativeType << " large=" << row.metadata.large
            << " path=" << static_cast<UINT>(row.metadata.path) << " nativeFirstObservation=" << row.metadata.nativeFirstObservation
            << " nativeImageCached=" << row.metadata.nativeImageCached << " nativeFirstAdmissionFailed=" << row.metadata.nativeFirstAdmissionFailed
            << " currentPresent=" << row.metadata.currentPresent
            << " currentVT=" << row.metadata.currentType << " currentQI=" << static_cast<ULONG>(row.metadata.currentImageQuery)
            << " returnedVT=" << row.metadata.returnedType << " returnedQI=" << static_cast<ULONG>(row.metadata.returnedImageQuery)
            << " normalHRESULT=" << static_cast<ULONG>(row.metadata.normalUpdateResult)
            << " returnedHRESULT=" << static_cast<ULONG>(row.metadata.completion->result)
            << " creator=" << row.metadata.creatorThread << " window=" << reinterpret_cast<UINT_PTR>(row.metadata.window)
            << " dpi=" << row.metadata.hwndDpi << '\n';
        auto [first, inserted] = firstProduction.try_emplace(ImageKey{row.metadata.nativeCommand, row.metadata.large}, &row);
        if (!inserted && row.metadata.ordinal < first->second->metadata.ordinal) first->second = &row;
    }
    totals.actualProductionRequests += static_cast<std::size_t>(requestStats.requests);
    totals.productionRows += productionRows.size();
    totals.physicalKeys += firstProduction.size();
    for(const auto& row:productionRows) {
        require(!row.metadata.nativeFirstAdmissionFailed,"Native first-current capture was poisoned by a lost reservation");
        const auto first=firstProduction.at(ImageKey{row.metadata.nativeCommand,row.metadata.large});
        if(!first->metadata.nativeFirstObservation)continue; // Dynamic and unmapped keep their existing path.
        if(first->metadata.nativeImageCached) {
            require(row.metadata.nativeImageCached&&row.returned.Get()==first->returned.Get()&&
                row.metadata.completion->result==S_OK&&
                (row.metadata.path==explorer::RibbonObservedImagePath::InstalledNativeFirstCurrent||
                 row.metadata.path==explorer::RibbonObservedImagePath::InstalledNativeCached),
                "Actual installed image invalidation replaced the authentic first-current IUIImage");
            if(row.metadata.ordinal!=first->metadata.ordinal)++totals.cachedNativeRepeatRows;
        } else {
            require(!row.metadata.nativeImageCached&&row.metadata.path==explorer::RibbonObservedImagePath::CommandMetadata,
                "A null/unsupported first current was promoted from a later host fallback");
            if(row.metadata.ordinal!=first->metadata.ordinal&&first->metadata.currentType!=VT_UNKNOWN&&row.current)
                ++totals.nullFirstLaterHostImageRows;
        }
    }
    totals.actualIndependentRequests += static_cast<std::size_t>(independentRequestCount);
    measurementCurrent();
    for (const auto& [key, events] : independentRows) {
        if (events.empty()) continue;
        // Appends can complete in nested callback order. The smallest actual
        // entry ordinal, not vector position, defines the first native current.
        const auto& reference = *std::min_element(events.begin(), events.end(),
            [](const auto& left, const auto& right) { return left.ordinal < right.ordinal; });
        require(reference.creatorThread == GetCurrentThreadId() && reference.window == independent.window.handle &&
            reference.hwndDpi == 96 && reference.windowGeneration == generation,
            "Independent first-current lost its actual HWND/DPI/creator/generation provenance");
        const auto found = firstProduction.find(key);
        std::cout << "OBSERVED scene=" << sceneName(scene) << " generation=" << generation
            << " native=" << key.first << " large=" << key.second << " independentOrdinal=" << reference.ordinal
            << " independentCurrentPresent=" << reference.currentPresent << " independentVT=" << reference.currentType
            << " independentQI=" << static_cast<ULONG>(reference.imageQuery)
            << " independentReturned=" << static_cast<ULONG>(reference.returned)
            << " creator=" << reference.creatorThread << " dpi=" << reference.hwndDpi
            << " independentWindow=" << reinterpret_cast<UINT_PTR>(reference.window)
            << " independentGeneration=" << reference.windowGeneration;
        if (found == firstProduction.end()) { std::cout << " production=UNREQUESTED\n"; continue; }
        const auto& actual = *found->second;
        std::cout << " productionOrdinal=" << actual.metadata.ordinal << " productionVT=" << actual.metadata.currentType
            << " productionWindow=" << reinterpret_cast<UINT_PTR>(actual.metadata.window)
            << " productionGeneration=" << actual.metadata.windowGeneration
            << " productionCurrentQI=" << static_cast<ULONG>(actual.metadata.currentImageQuery)
            << " productionReturnedVT=" << actual.metadata.returnedType
            << " productionReturnedQI=" << static_cast<ULONG>(actual.metadata.returnedImageQuery)
            << " productionHRESULT=" << static_cast<ULONG>(actual.metadata.completion->result);
        if (!reference.currentImage) { std::cout << " evidence=UNAVAILABLE_CURRENT_IMAGE\n"; continue; }
        ++totals.genuineFirstCurrents;
        const auto referencePixels = readActual(reference.currentImage.Get(), independent.window.handle, reference.creatorThread);
        measurementCurrent();
        const auto actualCurrent = readActual(actual.current.Get(), productionWindow.handle, actual.metadata.creatorThread);
        measurementCurrent();
        const auto actualReturned = readActual(actual.returned.Get(), productionWindow.handle, actual.metadata.creatorThread);
        measurementCurrent();
        diagnosePixels("independent", referencePixels); diagnosePixels("current", actualCurrent); diagnosePixels("returned", actualReturned);
        if (FAILED(referencePixels.orientation) || FAILED(actualCurrent.orientation) || FAILED(actualReturned.orientation)) {
            ++totals.unsupported; std::cout << " evidence=UNAVAILABLE_RAW_COMPARATOR\n"; continue;
        }
        // A different first current would confound the source claim. Restrict
        // comparable evidence to matching actual pre-host current pixels.
        if (referencePixels.raw.width != actualCurrent.raw.width || referencePixels.raw.height != actualCurrent.raw.height ||
            referencePixels.topDownStoredPixels != actualCurrent.topDownStoredPixels) {
            ++totals.unsupported; std::cout << " evidence=INDEPENDENT_CURRENT_DIFFERS\n"; continue;
        }
        ++totals.comparable;
        const bool identical = referencePixels.raw.width == actualReturned.raw.width &&
            referencePixels.raw.height == actualReturned.raw.height && referencePixels.topDownStoredPixels == actualReturned.topDownStoredPixels;
        if (identical) ++totals.identical; else ++totals.different;
        std::cout << " exactFourBytePixels=" << identical << " evidence=" << (identical ? "SAME" : "DIFFERENT") << '\n';
    }
    for (const auto& [key, actual] : firstProduction) if (!independentRows.contains(key)) {
        std::cout << "OBSERVED scene=" << sceneName(scene) << " generation=" << generation << " native=" << key.first
            << " large=" << key.second << " independent=UNREQUESTED productionOrdinal=" << actual->metadata.ordinal
            << " evidence=UNAVAILABLE_INDEPENDENT_REQUEST\n";
    }
    measurementCurrent();
    } // Immutable snapshots release their IUIImage references on this STA.
    // Snapshot Release can pump too. Admit only after all immutable image rows
    // released and both live callback streams/counters remain unchanged.
    measurementCurrent();
    guard(productionWindow.handle); guard(independent.window.handle);
    // All retained interface rows are released on this STA, before the owning
    // framework and HWND destruction. No normal profile/settings are opened.
    collector.rows.clear(); production.reset();
}
// Actual filesystem/provider/icon fixtures. These files never become synthetic
// currentValue inputs: only real native invalidations reach UpdateProperty.
struct OwnedIconFiles {
    std::wstring root,text,icon;
    bool rootCreated=false,textCreated=false,iconCreated=false;
    void cleanup()noexcept {
        if(iconCreated)DeleteFileW(icon.c_str());
        if(textCreated)DeleteFileW(text.c_str());
        if(rootCreated)RemoveDirectoryW(root.c_str());
    }
    void write(const std::wstring& path,const std::vector<BYTE>& bytes,bool& created) {
        HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        require(file!=INVALID_HANDLE_VALUE,"Exclusively create owned image fixture file");
        created=true;DWORD written=0;
        const bool complete=WriteFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)&&written==bytes.size();
        const bool closed=CloseHandle(file)!=FALSE;
        require(complete&&closed,"Write/close owned native icon fixture file");
    }
    OwnedIconFiles() {
        try {
            std::array<wchar_t,MAX_PATH> temporary{};
            const auto length=GetTempPathW(static_cast<DWORD>(temporary.size()),temporary.data());
            require(length&&length<temporary.size(),"Read bounded fixture temporary root");
            root=std::wstring(temporary.data())+L"Explorer-Owned-Image-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
            require(root.size()+24<MAX_PATH&&std::filesystem::path(root).is_absolute(),"Owned image paths must be bounded and absolute");
            require(CreateDirectoryW(root.c_str(),nullptr)!=FALSE,"Exclusively create owned native image directory");rootCreated=true;
            text=root+L"\\selection.txt";icon=root+L"\\explicit.ico";
            write(text,std::vector<BYTE>{'o','w','n','e','d','\r','\n'},textCreated);
            // One valid 32-bit 32x32 ICO file, with its own opaque artwork. Index
            // zero is the ICO file's only image, not a guessed DLL resource ID.
            std::vector<BYTE> bytes(22+40+32*32*4+32*4,0);
            const auto word=[&](size_t at,UINT value){bytes[at]=static_cast<BYTE>(value);bytes[at+1]=static_cast<BYTE>(value>>8);};
            const auto dword=[&](size_t at,UINT value){word(at,value);word(at+2,value>>16);};
            word(2,1);word(4,1);bytes[6]=32;bytes[7]=32;word(10,1);word(12,32);
            dword(14,static_cast<UINT>(bytes.size()-22));dword(18,22);
            dword(22,40);dword(26,32);dword(30,64);word(34,1);word(36,32);dword(42,32*32*4);
            for(size_t at=62;at<62+32*32*4;at+=4){bytes[at]=31;bytes[at+1]=73;bytes[at+2]=211;bytes[at+3]=255;}
            write(icon,bytes,iconCreated);
        }catch(...){cleanup();throw;}
    }
    ~OwnedIconFiles(){cleanup();}
};

void runOwnedOverrides() {
    OwnedIconFiles files;
    Window window;guard(window.handle);
    ComPtr<IShellItem> selected;ComPtr<IShellItemArray> selection;
    exact(SHCreateItemFromParsingName(files.text.c_str(),nullptr,IID_PPV_ARGS(&selected)),"Create actual owned association item");
    exact(SHCreateShellItemArrayFromShellItem(selected.Get(),IID_PPV_ARGS(&selection)),"Create actual owned association selection");
    explorer::NamespaceCommandMetadata association;
    exact(explorer::namespaceCommandMetadata(L"Windows.open",&association,selection.Get()),"Read actual native owned-item Open icon metadata");
    if(association.icon.empty())throw Skip{"actual owned-item Open provider supplied no icon; override coverage unavailable"};
    explorer::NamespaceCommandMetadata copyMetadata;
    exact(explorer::namespaceCommandMetadata(explorer::ribbonCommandStoreName(explorer::Copy),&copyMetadata),
        "Read real initial Copy provider icon specification");
    require(!copyMetadata.icon.empty(),"Actual default Copy provider has no icon specification");
    const auto explicitIcon=L"\""+files.icon+L"\",0";
    ProductionCollector collector;explorer::NativeRibbon production;
    struct RetainedRows {ProductionCollector& collector;explorer::NativeRibbon& production;
        ~RetainedRows(){production.reset();collector.rows.clear();}} retainedRows{collector,production};
    collector.owner=&production;
    explorer::RibbonCallbacks callbacks;
    callbacks.observeImageRequest=[&](const explorer::RibbonImageObservation& event){collector.observe(event);};
    callbacks.items=[&](UINT command) {
        if(command!=explorer::RibbonExtractToGallery)return std::vector<explorer::RibbonItem>{};
        explorer::RibbonItem item;item.label=L"Owned association destination";item.image=association.icon;
        return std::vector<explorer::RibbonItem>{std::move(item)};
    };
    exact(production.initialize(window.handle,GetModuleHandleW(nullptr),std::move(callbacks),explorer::RibbonLayout::InstalledWindows10),
        "Initialize actual native override host");
    if(production.layout()!=explorer::RibbonLayout::InstalledWindows10)throw Skip{"actual installed override host unavailable"};
    auto native=production.nativeFramework();require(native!=nullptr,"Native override framework missing");
    try {
        // Match the admitted Home hosts' real resource-request setup. Initial
        // presentation alone need not request both images for every group.
        // Discover only commands registered by a fresh unmodified native BML;
        // request their real properties, never manufacture currentValue.
        const auto features=production.features();
        const UINT32 modes=0xa1|(features.discBurning?0x20000:0x40000);
        IndependentStockFramework discovery;
        discovery.initialize(13,modes);
        realize(window.handle);
        std::set<UINT> targeted;
        for(const auto& [command,type]:discovery.handler->commandTypes)
            if(type!=UI_COMMANDTYPE_CONTEXT&&type!=UI_COMMANDTYPE_UNKNOWN)targeted.insert(command);
        for(const bool large:{true,false}) {
            const auto& key=large?UI_PKEY_LargeImage:UI_PKEY_SmallImage;
            for(const UINT command:targeted) {
                guard(window.handle);guard(discovery.window.handle);
                const auto status=native->InvalidateUICommand(command,UI_INVALIDATIONS_PROPERTY,&key);
                std::cout<<"OWNED_REQUEST native="<<command<<" large="<<large
                    <<" productionInvalidation="<<static_cast<ULONG>(status)<<'\n';
            }
            exact(production.flush(),"Complete actual owned-host initial native image requests");
            realize(window.handle);
        }
        // Discovery holds independent actual images only as its normal native
        // observer; none are supplied to production or used as its witness.
    } catch(const Skip& unavailable) {
        // The preceding twelve hosts already established native availability.
        // Missing additional discovery cannot waive strict override coverage.
        throw std::runtime_error("Owned native registration discovery unavailable: "+unavailable.reason);
    } // Native discovery release precedes the production immutable snapshot.
    guard(window.handle);
    const auto originalRows=[&]{std::vector<ProductionRow> saved;saved.swap(collector.rows);return saved;}();
    std::map<ImageKey,const ProductionRow*> original;
    for(const auto& row:originalRows) {
        auto [at,inserted]=original.try_emplace(ImageKey{row.metadata.nativeCommand,row.metadata.large},&row);
        if(!inserted&&row.metadata.ordinal<at->second->metadata.ordinal)at->second=&row;
    }
    explorer::RibbonImageObservationStats previous;
    exact(production.imageObservationStats(previous),"Read original actual override-host request count");
    require(!collector.allocationFailure&&previous.requests==originalRows.size()&&previous.delivered==originalRows.size()&&
        previous.dropped==0&&previous.reentrant==0&&collector.rows.empty()&&production.nativeFramework()==native,
        "Original override-host callback coverage incomplete");
    std::cout<<"OWNED_COVERAGE actualProductionRequests="<<previous.requests<<" productionRows="<<originalRows.size()
        <<" physicalKeys="<<original.size()<<" dropped="<<previous.dropped<<" reentrant="<<previous.reentrant<<'\n';
    // Dump immutable copied first rows before any Copy/witness requirement.
    // Missing pairs, null-first currents, alias mappings or metadata failure
    // must remain discriminating observations even when admission fails.
    for(const auto& [key,row]:original) {
        const auto paired=original.find({key.first,!key.second});
        std::cout<<"OWNED_FIRST native="<<key.first<<" application="<<row->metadata.applicationCommand<<" large="<<key.second
            <<" ordinal="<<row->metadata.ordinal<<" nativeType="<<row->metadata.nativeType
            <<" pairedSizeObserved="<<(paired!=original.end())
            <<" mappedRoundTrip="<<(row->metadata.applicationCommand&&production.nativeCommandId(row->metadata.applicationCommand)==key.first)
            <<" currentPresent="<<row->metadata.currentPresent<<" currentVT="<<row->metadata.currentType
            <<" currentQI="<<static_cast<ULONG>(row->metadata.currentImageQuery)<<" currentImage="<<static_cast<bool>(row->current)
            <<" returnedVT="<<row->metadata.returnedType<<" returnedQI="<<static_cast<ULONG>(row->metadata.returnedImageQuery)
            <<" returnedImage="<<static_cast<bool>(row->returned)<<" HRESULT="<<static_cast<ULONG>(row->metadata.normalUpdateResult)
            <<" nativeFirstObservation="<<row->metadata.nativeFirstObservation<<" nativeImageCached="<<row->metadata.nativeImageCached
            <<" nativeFirstAdmissionFailed="<<row->metadata.nativeFirstAdmissionFailed
            <<" path="<<static_cast<UINT>(row->metadata.path)<<" creator="<<row->metadata.creatorThread
            <<" window="<<reinterpret_cast<UINT_PTR>(row->metadata.window)<<" dpi="<<row->metadata.hwndDpi
            <<" generation="<<row->metadata.windowGeneration<<" callbackEpoch="<<row->metadata.callbackEpoch
            <<" completionPresent="<<static_cast<bool>(row->metadata.completion)
            <<" bindingStable="<<(row->metadata.completion&&row->metadata.completion->bindingStable)
            <<" observerThrew="<<(row->metadata.completion&&row->metadata.completion->observerThrew)
            <<" completionHRESULT="<<static_cast<ULONG>(row->metadata.completion?row->metadata.completion->result:E_PENDING)<<'\n';
    }
    const auto copy=production.nativeCommandId(explorer::Copy),open=production.nativeCommandId(explorer::Open),
        openMenu=production.nativeCommandId(explorer::RibbonOpenMenu);
    // Copy can legitimately be null-first in both sizes. Save its actual
    // original route/output, then select a separate positive cache witness from
    // this same host's real first callbacks, never from a guessed command ID.
    for(const bool large:{false,true}) {
        const auto at=original.find({copy,large});
        require(at!=original.end()&&at->second->metadata.nativeFirstObservation&&at->second->returned&&
            at->second->metadata.completion&&at->second->metadata.completion->bindingStable&&
            at->second->metadata.completion->result==S_OK,
            "Actual original Copy image route/output required for restoration");
    }
    UINT nativeRestore=0,applicationRestore=0,authenticRestoreSizes=0;
    for(const auto& [key,smallRow]:original) {
        if(key.second||key.first==copy||key.first==open||key.first==openMenu)continue;
        const auto large=original.find({key.first,true});if(large==original.end())continue;
        const auto application=smallRow->metadata.applicationCommand;
        if(!application||large->second->metadata.applicationCommand!=application||
            production.nativeCommandId(application)!=key.first)continue;
        if(!smallRow->metadata.nativeFirstObservation||!large->second->metadata.nativeFirstObservation)continue;
        const UINT cached=static_cast<UINT>(smallRow->metadata.nativeImageCached)+static_cast<UINT>(large->second->metadata.nativeImageCached);
        if(!cached||cached<=authenticRestoreSizes)continue;
        std::wstring registeredLabel;
        const auto labelStatus=production.commandLabel(application,registeredLabel);
        std::cout<<"OWNED_WITNESS_CANDIDATE native="<<key.first<<" application="<<application<<" authenticSizes="<<cached
            <<" ownedMetadataLabelHRESULT="<<static_cast<ULONG>(labelStatus)<<'\n';
        if(labelStatus==HRESULT_FROM_WIN32(ERROR_NOT_FOUND))continue;
        exact(labelStatus,"Read actual mapped restoration command metadata");
        nativeRestore=key.first;applicationRestore=application;authenticRestoreSizes=cached;
        if(cached==2)break; // Prefer both actual native sizes; a real one-size witness remains honest.
    }
    require(nativeRestore&&applicationRestore&&authenticRestoreSizes,
        "Actual mapped native first-current cache restoration witness unavailable");
    for(const UINT id:{copy,nativeRestore})for(const bool large:{false,true}) {
        const auto& row=*original.at({id,large});
        require(row.metadata.creatorThread==GetCurrentThreadId()&&row.metadata.window==window.handle&&
            row.metadata.hwndDpi==96&&!row.metadata.nativeFirstAdmissionFailed&&row.metadata.completion&&
            row.metadata.completion->bindingStable&&!row.metadata.completion->observerThrew&&
            row.metadata.completion->result==row.metadata.normalUpdateResult,
            "Actual restoration baseline lost owned creator/source/completion provenance");
        if(row.metadata.nativeImageCached)require(row.current&&row.returned&&row.metadata.normalUpdateResult==S_OK&&
            row.metadata.path==explorer::RibbonObservedImagePath::InstalledNativeFirstCurrent,
            "Selected authentic restoration witness lacks a genuine first native image");
        else require(row.metadata.path==explorer::RibbonObservedImagePath::CommandMetadata,
            "Selected fallback restoration witness has the wrong original path");
        std::cout<<"OWNED_INITIAL native="<<id<<" application="<<row.metadata.applicationCommand<<" large="<<large
            <<" ordinal="<<row.metadata.ordinal<<" currentVT="<<row.metadata.currentType
            <<" currentQI="<<static_cast<ULONG>(row.metadata.currentImageQuery)
            <<" returnedVT="<<row.metadata.returnedType<<" returnedQI="<<static_cast<ULONG>(row.metadata.returnedImageQuery)
            <<" HRESULT="<<static_cast<ULONG>(row.metadata.normalUpdateResult)
            <<" nativeFirstObservation="<<row.metadata.nativeFirstObservation<<" nativeImageCached="<<row.metadata.nativeImageCached
            <<" path="<<static_cast<UINT>(row.metadata.path)<<" creator="<<row.metadata.creatorThread
            <<" window="<<reinterpret_cast<UINT_PTR>(row.metadata.window)<<" dpi="<<row.metadata.hwndDpi<<'\n';
    }
    const auto verify=[&](const char* phase,const std::map<UINT,std::wstring>& specifications,
                          explorer::RibbonObservedImagePath path,bool restore=false,
                          const std::set<ImageKey>& requiredImageKeys={}) {
        const auto rows=[&]{std::vector<ProductionRow> saved;saved.swap(collector.rows);return saved;}();
        explorer::RibbonImageObservationStats before;
        exact(production.imageObservationStats(before),"Read actual override phase request coverage");
        require(!collector.allocationFailure&&before.requests-previous.requests==rows.size()&&before.delivered-previous.delivered==rows.size()&&
            before.dropped==0&&before.reentrant==0,"Actual override phase lost native callback rows");
        const auto stable=[&] {
            guard(window.handle);explorer::RibbonImageObservationStats after;
            exact(production.imageObservationStats(after),"Fence actual override pixel/retention work");
            require(production.nativeFramework()==native&&collector.rows.empty()&&!collector.allocationFailure&&
                after.requests==before.requests&&after.delivered==before.delivered&&after.dropped==0&&after.reentrant==0,
                "Native override callback stream changed during immutable comparison");
        };
        {
            std::map<ImageKey,ComPtr<IUIImage>> expected;
            if(!restore)for(const auto& [id,specification]:specifications)for(const bool large:{false,true}) {
                ComPtr<IUIImage> image;
                // Read only an actually requested override's existing item
                // cache after the native phase. This is not the stock oracle.
                exact(production.itemImage(specification,large,&image),"Read requested real override image");stable();
                expected.emplace(ImageKey{id,large},std::move(image));
            }
            std::set<ImageKey> covered;
            for(const auto& row:rows) {
                if(!specifications.contains(row.metadata.nativeCommand))continue;
                const ImageKey key{row.metadata.nativeCommand,row.metadata.large};
                const auto* initial=restore?original.at(key):nullptr;
                const bool authentic=restore&&initial->metadata.nativeImageCached;
                const auto expectedPath=restore?(authentic?explorer::RibbonObservedImagePath::InstalledNativeCached:
                    explorer::RibbonObservedImagePath::CommandMetadata):path;
                const auto expectedResult=restore?initial->metadata.normalUpdateResult:S_OK;
                require(row.metadata.creatorThread==GetCurrentThreadId()&&row.metadata.window==window.handle&&row.metadata.hwndDpi==96&&
                    row.metadata.completion&&row.metadata.completion->bindingStable&&!row.metadata.completion->observerThrew&&
                    row.metadata.completion->result==expectedResult&&row.metadata.normalUpdateResult==expectedResult&&row.metadata.path==expectedPath,
                    "Actual native override returned the wrong source/HRESULT");
                if(restore)require(row.metadata.nativeImageCached==initial->metadata.nativeImageCached&&!row.metadata.nativeFirstObservation&&
                    row.metadata.returnedType==initial->metadata.returnedType,
                    "Restoration changed its actual first-cache/fallback classification");
                auto* reference=restore?initial->returned.Get():expected.at(key).Get();
                if(authentic)require(row.returned.Get()==reference,"Actual restored native cache changed its authentic IUIImage");
                if(FAILED(expectedResult)) {
                    require(restore&&!authentic&&!reference&&!row.returned,
                        "Failed original fallback restoration manufactured a returned image");
                    covered.insert(key);std::cout<<"OWNED_OVERRIDE phase="<<phase<<" native="<<key.first<<" large="<<key.second
                        <<" path="<<static_cast<UINT>(expectedPath)<<" originalHRESULT="<<static_cast<ULONG>(expectedResult)
                        <<" evidence=ORIGINAL_FALLBACK_UNAVAILABLE\n";continue;
                }
                require(reference&&row.returned,"Actual successful override/restoration has no IUIImage");
                const bool firstProof=covered.insert(key).second;
                if(firstProof||!authentic) {
                    const auto actualPixels=readActual(row.returned.Get(),window.handle,row.metadata.creatorThread);stable();
                    const auto expectedPixels=readActual(reference,window.handle,GetCurrentThreadId());stable();
                    require(SUCCEEDED(actualPixels.orientation)&&SUCCEEDED(expectedPixels.orientation)&&
                        actualPixels.raw.width==expectedPixels.raw.width&&actualPixels.raw.height==expectedPixels.raw.height&&
                        actualPixels.topDownStoredPixels==expectedPixels.topDownStoredPixels,"Actual override raw four-byte pixels differ");
                    std::cout<<"OWNED_OVERRIDE phase="<<phase<<" native="<<key.first<<" large="<<key.second
                        <<" path="<<static_cast<UINT>(expectedPath)<<" nativeImageCached="<<row.metadata.nativeImageCached
                        <<" authenticIdentityRequired="<<authentic<<" exactFourBytePixels=1\n";
                }
            }
            if(requiredImageKeys.empty()) {
                for(const auto& [id,specification]:specifications)for(const bool large:{false,true})
                    require(covered.contains({id,large}),"Required actual native override image callback was not observed");
            } else for(const auto& key:requiredImageKeys) {
                require(specifications.contains(key.first)&&covered.contains(key),
                    "Required actual native gallery image callback was not observed");
            }
            std::cout<<"OWNED_OVERRIDE_COVERAGE phase="<<phase<<" actualSmall=";
            size_t smallCount=0,largeCount=0;
            for(const auto& key:covered)if(key.second)++largeCount;else ++smallCount;
            std::cout<<smallCount<<" actualLarge="<<largeCount<<" nativeCallbacksOnly=1\n";
            stable();
        } // Expected images released before the final native binding fence.
        stable();previous=before;
        // rows release after this lambda; its caller checks late live rows too.
    };
    exact(production.setCommandImageSpec(applicationRestore,explicitIcon),"Override actually observed native-cache command with owned ICO source");
    exact(production.setCommandImageSpec(explorer::Copy,explicitIcon),"Set explicit exclusively owned ICO source");
    exact(production.flush(),"Complete actual Home explicit ICO native image requests");realize(window.handle);
    verify("explicit-home-ico",{{nativeRestore,explicitIcon},{copy,explicitIcon}},
        explorer::RibbonObservedImagePath::CommandMetadata);
    require(collector.rows.empty(),"Override row Release produced a late native callback");
    exact(production.setCommandImageSpec(explorer::Copy,L""),"Restore actual original Copy native-or-fallback image route");
    exact(production.setCommandImageSpec(applicationRestore,L""),"Restore actually observed native cache artwork");
    exact(production.flush(),"Complete actual native and fallback artwork restoration");realize(window.handle);
    verify("restore-original-routes",{{copy,L""},{nativeRestore,L""}},explorer::RibbonObservedImagePath::Unmapped,true);
    require(collector.rows.empty(),"Restoration row Release produced a late native callback");
    exact(production.setCommandImageSpec(explorer::Copy,copyMetadata.icon),"Explicit default metadata must retain its actual override source semantics");
    require(production.setCommandImageSpec(explorer::Copy,copyMetadata.icon)==S_FALSE,"Repeated explicit image source changed its admission state");
    exact(production.flush(),"Complete actual equal-default explicit override");realize(window.handle);
    verify("explicit-default-metadata",{{copy,copyMetadata.icon}},explorer::RibbonObservedImagePath::CommandMetadata);
    require(collector.rows.empty(),"Equal-default explicit row Release produced a late native callback");
    // Home's genuine first-current witness and Copy restoration are complete.
    // Open's physical control belongs to the real Computer resource mode on
    // the measured stock BML; a Home metadata setter cannot request that row.
    const auto computerEpoch=production.callbackEntryEpoch();
    const auto hostGeneration=original.at({copy,false})->metadata.windowGeneration;
    const auto computerCurrent=[&] {
        guard(window.handle);
        require(production.nativeFramework()==native&&production.callbackEntryEpoch()==computerEpoch&&
            production.layout()==explorer::RibbonLayout::InstalledWindows10,
            "Computer association phase changed its original native host/source");
    };
    exact(production.setComputerMode(true),"Select actual production Computer mode for Open association override");
    computerCurrent();
    const auto computerOpen=production.nativeCommandId(explorer::Open),
        computerOpenMenu=production.nativeCommandId(explorer::RibbonOpenMenu);
    require(computerOpen&&computerOpenMenu,"Actual Computer Open physical aliases are unavailable");
    const std::set<UINT> computerTargets{computerOpen,computerOpenMenu};
    std::map<UINT,UI_COMMANDTYPE> registeredComputerTargets;
    try {
        IndependentStockFramework discovery;
        const auto features=production.features();
        const UINT32 modes=4|(features.mediaFoundation?0x2000:0x4000);
        discovery.initialize(14,modes);computerCurrent();
        for(const UINT command:computerTargets) {
            const auto at=discovery.handler->commandTypes.find(command);
            std::cout<<"OWNED_COMPUTER_REGISTRATION native="<<command<<" modes="<<modes
                <<" registered="<<(at!=discovery.handler->commandTypes.end())
                <<" nativeType="<<(at==discovery.handler->commandTypes.end()?UI_COMMANDTYPE_UNKNOWN:at->second)<<'\n';
            require(at!=discovery.handler->commandTypes.end()&&at->second!=UI_COMMANDTYPE_UNKNOWN&&
                at->second!=UI_COMMANDTYPE_CONTEXT,
                "Actual Computer BML did not register the requested Open physical control");
            registeredComputerTargets.emplace(command,at->second);
        }
        // Only the independently registered actual Open aliases are requested.
        // No Enabled value or synthetic currentValue/physical ID is supplied.
        for(const bool large:{true,false}) {
            const auto& key=large?UI_PKEY_LargeImage:UI_PKEY_SmallImage;
            for(const UINT command:computerTargets) {
                computerCurrent();guard(discovery.window.handle);
                const auto status=native->InvalidateUICommand(command,UI_INVALIDATIONS_PROPERTY,&key);
                std::cout<<"OWNED_COMPUTER_REQUEST native="<<command<<" large="<<large
                    <<" productionInvalidation="<<static_cast<ULONG>(status)<<'\n';
                computerCurrent();exact(status,"Invalidate actual registered Computer Open image");
            }
            exact(production.flush(),"Complete actual Computer Open baseline image requests");
            realize(window.handle);computerCurrent();
        }
    } catch(const Skip& unavailable) {
        throw std::runtime_error("Computer native registration discovery unavailable: "+unavailable.reason);
    } // Independent native discovery release precedes source/row admission.
    computerCurrent();
    explorer::RibbonImageObservationStats computerBaseline;
    {
        const auto rows=[&]{std::vector<ProductionRow> saved;saved.swap(collector.rows);return saved;}();
        exact(production.imageObservationStats(computerBaseline),"Read actual Computer baseline callback coverage");
        require(!collector.allocationFailure&&computerBaseline.requests-previous.requests==rows.size()&&
            computerBaseline.delivered-previous.delivered==rows.size()&&computerBaseline.dropped==0&&
            computerBaseline.reentrant==0,"Actual Computer baseline lost native callback rows");
        std::map<ImageKey,const ProductionRow*> first;
        for(const auto& row:rows)if(computerTargets.contains(row.metadata.nativeCommand)) {
            const ImageKey key{row.metadata.nativeCommand,row.metadata.large};
            auto [at,inserted]=first.try_emplace(key,&row);
            if(!inserted&&row.metadata.ordinal<at->second->metadata.ordinal)at->second=&row;
        }
        for(const UINT command:computerTargets)for(const bool large:{false,true}) {
            const auto at=first.find({command,large});
            require(at!=first.end(),"Actual registered Computer Open image callback was not requested");
            const auto& row=*at->second;
            std::cout<<"OWNED_COMPUTER_FIRST native="<<command<<" application="<<row.metadata.applicationCommand
                <<" large="<<large<<" ordinal="<<row.metadata.ordinal<<" nativeType="<<row.metadata.nativeType
                <<" currentVT="<<row.metadata.currentType<<" currentQI="<<static_cast<ULONG>(row.metadata.currentImageQuery)
                <<" nativeFirstObservation="<<row.metadata.nativeFirstObservation<<" nativeImageCached="<<row.metadata.nativeImageCached
                <<" path="<<static_cast<UINT>(row.metadata.path)<<" HRESULT="<<static_cast<ULONG>(row.metadata.normalUpdateResult)<<'\n';
            require(row.metadata.creatorThread==GetCurrentThreadId()&&row.metadata.window==window.handle&&
                row.metadata.hwndDpi==96&&row.metadata.windowGeneration==hostGeneration&&row.metadata.callbackEpoch==computerEpoch&&
                row.metadata.nativeType==static_cast<UINT>(registeredComputerTargets.at(command))&&row.metadata.nativeFirstObservation&&
                !row.metadata.nativeFirstAdmissionFailed&&row.metadata.completion&&row.metadata.completion->bindingStable&&
                !row.metadata.completion->observerThrew&&row.metadata.completion->result==row.metadata.normalUpdateResult&&
                (row.metadata.applicationCommand==explorer::Open||row.metadata.applicationCommand==explorer::RibbonOpenMenu)&&
                production.nativeCommandId(row.metadata.applicationCommand)==command,
                "Computer Open first row lost actual registered/source/first-current authority");
            if(row.metadata.nativeImageCached)require(row.current&&row.returned&&row.metadata.normalUpdateResult==S_OK&&
                row.metadata.path==explorer::RibbonObservedImagePath::InstalledNativeFirstCurrent,
                "Computer Open authentic first image lacks native provenance");
            else require(row.metadata.path==explorer::RibbonObservedImagePath::CommandMetadata,
                "Computer Open null first current was not kept on its actual metadata path");
            for(const auto& later:rows)if(later.metadata.nativeCommand==command&&later.metadata.large==large) {
                require(later.metadata.creatorThread==GetCurrentThreadId()&&later.metadata.window==window.handle&&
                    later.metadata.hwndDpi==96&&later.metadata.windowGeneration==hostGeneration&&later.metadata.callbackEpoch==computerEpoch&&
                    later.metadata.nativeType==static_cast<UINT>(registeredComputerTargets.at(command))&&!later.metadata.nativeFirstAdmissionFailed&&
                    later.metadata.completion&&later.metadata.completion->bindingStable&&!later.metadata.completion->observerThrew&&
                    later.metadata.completion->result==later.metadata.normalUpdateResult&&
                    later.metadata.nativeImageCached==row.metadata.nativeImageCached,
                    "Computer Open null first current was promoted by a later image");
                const auto expectedPath=row.metadata.nativeImageCached?
                    (later.metadata.nativeFirstObservation?explorer::RibbonObservedImagePath::InstalledNativeFirstCurrent:
                        explorer::RibbonObservedImagePath::InstalledNativeCached):explorer::RibbonObservedImagePath::CommandMetadata;
                require(later.metadata.path==expectedPath,"Computer baseline changed its actual first-cache/metadata route");
                if(row.metadata.nativeImageCached)require(later.returned.Get()==row.returned.Get(),
                    "Computer Open repeated native baseline changed its authentic cache identity");
            }
        }
        computerCurrent();require(collector.rows.empty(),"Computer baseline comparison produced late native callbacks");
    } // Release every Computer baseline image before the final native fence.
    computerCurrent();
    explorer::RibbonImageObservationStats afterComputerBaseline;
    exact(production.imageObservationStats(afterComputerBaseline),"Fence Computer baseline image Release");
    require(collector.rows.empty()&&!collector.allocationFailure&&afterComputerBaseline.requests==computerBaseline.requests&&
        afterComputerBaseline.delivered==computerBaseline.delivered&&afterComputerBaseline.dropped==0&&afterComputerBaseline.reentrant==0,
        "Computer baseline image Release changed its native callback stream");
    previous=computerBaseline;
    exact(production.setCommandImageSpec(explorer::Open,association.icon),"Set actual Computer Open association icon");
    computerCurrent();
    exact(production.setCommandImageSpec(explorer::RibbonOpenMenu,association.icon),"Set actual Computer Open menu association icon");
    computerCurrent();
    exact(production.flush(),"Complete actual Computer association image requests");realize(window.handle);computerCurrent();
    verify("association-computer",{{computerOpen,association.icon},{computerOpenMenu,association.icon}},
        explorer::RibbonObservedImagePath::CommandMetadata);
    computerCurrent();require(collector.rows.empty(),"Computer association row Release produced late native callbacks");
    exact(production.setComputerMode(false),"Return real Home mode before dynamic association context");
    exact(production.flush(),"Complete real Home mode return");realize(window.handle);computerCurrent();
    verify("return-home-before-dynamic",{},explorer::RibbonObservedImagePath::Unmapped);
    require(collector.rows.empty(),"Home mode return row Release produced late native callbacks");

    exact(production.setContexts(explorer::RibbonContext::Compressed,true),"Realize actual dynamic association-item gallery");
    exact(production.flush(),"Complete actual dynamic native gallery activation");realize(window.handle);
    PROPVARIANT source{};
    exact(production.framework()->GetUICommandProperty(explorer::RibbonExtractToGallery,UI_PKEY_ItemsSource,&source),"Read actual native association-item source");
    ComPtr<IUICollection> collection;
    const auto sourceQuery=source.vt==VT_UNKNOWN&&source.punkVal?source.punkVal->QueryInterface(IID_PPV_ARGS(&collection)):E_NOINTERFACE;
    PropVariantClear(&source);exact(sourceQuery,"Query actual native association-item collection");
    UINT count=0;exact(collection->GetCount(&count),"Read actual native dynamic item count");require(count==1,"Actual dynamic association collection changed");
    ComPtr<IUnknown> raw;ComPtr<IUISimplePropertySet> item;
    exact(collection->GetItem(0,&raw),"Read actual owned dynamic native item");exact(raw.As(&item),"Query actual dynamic native row");
    PROPVARIANT identity{};exact(item->GetValue(UI_PKEY_CommandId,&identity),"Read actual native-assigned dynamic physical ID");
    ULONG dynamic=0;const auto identityResult=PropVariantToUInt32(identity,&dynamic);PropVariantClear(&identity);exact(identityResult,"Read actual dynamic ID type");
    for(const auto* key:{&UI_PKEY_SmallImage,&UI_PKEY_LargeImage})
        exact(native->InvalidateUICommand(dynamic,UI_INVALIDATIONS_PROPERTY,key),"Invalidate actual native-assigned dynamic image");
    exact(production.flush(),"Complete actual dynamic association-icon image callbacks");realize(window.handle);
    // Record the actual framework's public image-property contract rather
    // than directly invoking a command handler to manufacture a request.
    for(const bool large:{false,true}) {
        guard(window.handle);
        const auto& key=large?UI_PKEY_LargeImage:UI_PKEY_SmallImage;
        PROPVARIANT actual{};
        const auto read=native->GetUICommandProperty(dynamic,key,&actual);
        std::cout<<"OWNED_DYNAMIC_PROPERTY native="<<dynamic<<" large="<<large
            <<" HRESULT="<<static_cast<ULONG>(read)<<" returnedVT="<<actual.vt
            <<" actualUnknown="<<(actual.vt==VT_UNKNOWN&&actual.punkVal!=nullptr)<<'\n';
        PropVariantClear(&actual);
        guard(window.handle);
    }
    // Native menu rendering requires this dynamic SmallImage. Invalidation
    // does not promise a callback for the unused large presentation; every
    // size actually supplied by the framework is still checked by verify.
    verify("dynamic-association",{{dynamic,association.icon}},explorer::RibbonObservedImagePath::DynamicItemMetadata,
        false,{{dynamic,false}});
    require(collector.rows.empty(),"Dynamic callback Release produced a late native image callback");
    const auto dynamicBaseline=previous;
    const auto dynamicStable=[&] {
        guard(window.handle);explorer::RibbonImageObservationStats current;
        exact(production.imageObservationStats(current),"Fence independent dynamic image cache contract");
        require(production.nativeFramework()==native&&collector.rows.empty()&&!collector.allocationFailure&&
            current.requests==dynamicBaseline.requests&&current.delivered==dynamicBaseline.delivered&&
            current.dropped==0&&current.reentrant==0,
            "Independent dynamic cache proof changed its actual native callback source");
    };
    {
        auto path=association.icon;
        const int resource=PathParseIconLocationW(path.data());path.resize(wcslen(path.c_str()));
        std::array<wchar_t,32768> expanded{};
        const auto length=ExpandEnvironmentStringsW(path.c_str(),expanded.data(),static_cast<DWORD>(expanded.size()));
        require(length&&length<=expanded.size(),"Expand actual native association icon specification");
        for(const bool large:{false,true}) {
            const UINT pixels=static_cast<UINT>(MulDiv(large?32:16,GetDpiForWindow(window.handle),96));
            dynamicStable();
            const auto reference=native_icon_reference::singleIconReference(expanded.data(),resource,pixels);dynamicStable();
            ComPtr<IUIImage> first,repeated;
            const auto status=production.itemImage(association.icon,large,&first);dynamicStable();
            const auto repeat=production.itemImage(association.icon,large,&repeated);dynamicStable();
            require(status==repeat&&first.Get()==repeated.Get(),"Dynamic association cache changed repeated output/status");
            require(reference.imageStatus()==S_OK,"Actual association native icon raster is unavailable");
            const auto bytes=native_icon_reference::requireImageContract(status,first.Get(),reference,pixels);dynamicStable();
            std::cout<<"OWNED_DYNAMIC_PUBLIC_CACHE native="<<dynamic<<" large="<<large<<" requestedPixels="<<pixels
                <<" exactFourByteBytes="<<bytes<<" independentSingleExtraction=1 nativeCallbackProof=0\n";
        }
    } // Release both actual cache references before the original native fence.
    dynamicStable();
    require(collector.rows.empty(),"Dynamic row Release produced a late native callback");
    item.Reset();raw.Reset();collection.Reset();selection.Reset();selected.Reset();
    require(collector.rows.empty(),"Owned provider release produced a late native image callback");
    // Retire native delivery before originalRows/other retained image releases.
    production.reset();guard(window.handle);
}
} // namespace

int wmain(int argc,wchar_t** argv) {
    bool crashDiagnostics=false;
    std::wstring_view dumpPath;
    // Parse every option before desktop/COM setup. No duplicate, unknown or
    // missing operand can partially configure the diagnostic.
    if(argc<1||!argv) {std::cerr<<"FAIL: invalid fixture arguments\n";return 2;}
    for(int index=1;index<argc;++index) {
        if(!argv[index]){std::cerr<<"FAIL: null fixture argument\n";return 2;}
        const std::wstring_view option(argv[index]);
        if(option==L"--crash-dump") {
            if(crashDiagnostics||index+1>=argc||!argv[index+1]) {
                std::cerr<<"FAIL: duplicate/missing crash dump option\n";return 2;
            }
            crashDiagnostics=true;dumpPath=argv[++index];
            if(dumpPath.empty()||dumpPath.size()>=32768||dumpPath.starts_with(L"--")) {
                std::cerr<<"FAIL: crash dump path is missing or exceeds its bound\n";return 2;
            }
        } else {
            std::cerr<<"FAIL: usage: ribbon_image_provenance_tests [--crash-dump <absolutePath>]\n";return 2;
        }
    }
    if(crashDiagnostics) {
        try {
            const auto configured=explorer::initializeHeadlessCrashDump(true,std::filesystem::path(dumpPath));
            if(configured!=S_OK) {
                std::cerr<<"FAIL: crash dump configuration HRESULT="<<static_cast<ULONG>(configured)<<'\n';return 2;
            }
        }catch(const std::exception& error) {
            std::cerr<<"FAIL: crash dump configuration: "<<error.what()<<'\n';return 2;
        }
        std::cerr<<"OWNED_TEARDOWN phase=crashCaptureConfigured creator="<<GetCurrentThreadId()<<'\n';
    }
    const bool teardownDiagnostics=crashDiagnostics;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    // PM-aware context ensures GetDpiForWindow is actual HWND DPI, not unaware
    // virtualization to 96. No display or DPI setting is changed by this call.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (GetAwarenessFromDpiAwarenessContext(GetThreadDpiAwarenessContext()) != DPI_AWARENESS_PER_MONITOR_AWARE) {
        std::cout << "UNAVAILABLE: fixture process is not per-monitor DPI aware\n"; return 77;
    }
    explorer::PrivateDesktop desktop;
    const auto isolated = desktop.initialize();
    if (FAILED(isolated)) { std::cerr << "FAIL: private desktop HRESULT=" << static_cast<ULONG>(isolated) << '\n'; return 2; }
    explorer::NativeApartmentOwner nativeApartment;
    const auto initialized = nativeApartment.initializeOle();
    if (FAILED(initialized)) { std::cerr << "FAIL: owned STA HRESULT=" << static_cast<ULONG>(initialized) << '\n'; return 3; }
    int result = 0;bool admitted=false;
    try {
        APTTYPE apartment{}; APTTYPEQUALIFIER qualifier{};
        exact(CoGetApartmentType(&apartment, &qualifier), "Read actual creator COM apartment");
        require(apartment == APTTYPE_STA || apartment == APTTYPE_MAINSTA, "Image fixture creator is not STA");
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES};
        require(InitCommonControlsEx(&controls) != FALSE, "Initialize actual native common controls");
        WNDCLASSW type{}; type.lpfnWndProc = DefWindowProcW; type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = hostClass; type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        require(RegisterClassW(&type) != 0, "Register only the owned provenance HWND class");
        admissionDeadline = GetTickCount64() + 65000;
        Totals totals; std::uint64_t generation = 0;
        for (const auto scene : {Scene::Home, Scene::Computer, Scene::Network, Scene::Compressed, Scene::Recycle, Scene::Drive})
            for (const bool largeFirst : {true, false}) run(scene, largeFirst, ++generation, totals);
        std::cout << "COVERAGE actualProductionRequests=" << totals.actualProductionRequests
            << " productionRows=" << totals.productionRows
            << " actualIndependentRequests=" << totals.actualIndependentRequests << " sumPhysicalKeysPerFreshHost=" << totals.physicalKeys
            << " genuineFirstCurrents=" << totals.genuineFirstCurrents << " comparable=" << totals.comparable
            << " same=" << totals.identical << " different=" << totals.different << " unsupported=" << totals.unsupported
            << " cachedNativeRepeatRows=" << totals.cachedNativeRepeatRows << " nullFirstLaterHostImageRows=" << totals.nullFirstLaterHostImageRows << '\n';
        if (!totals.actualProductionRequests || !totals.genuineFirstCurrents || !totals.comparable) {
            std::cout << "UNAVAILABLE: actual native first-current artwork supplied zero comparable rows; no displaced-icon proof\n";
            result = 77;
        } else {
            require(totals.comparable>=182&&totals.identical==totals.comparable&&totals.different==0,
                "Installed first-current artwork differs or measured 19045 comparable coverage fell below 182");
            require(totals.cachedNativeRepeatRows>0&&totals.nullFirstLaterHostImageRows>0,
                "Actual cached-native repeat and null-first/later-host-image scenarios were not observed");
            runOwnedOverrides();
            admitted=true;
        }
    } catch (const Skip& skip) { std::cout << "UNAVAILABLE: " << skip.reason << '\n'; result = 77; }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; result = 1; }
    // All test-local COM/image/framework/HWND owners have left scope. Drain
    // only this creator's existing tracked native STA workers while its exact
    // private desktop and initialized apartment remain alive. Untracked Shell
    // helper threads are not claimed to be covered by this repository API.
    const auto beforeDrain=explorer::pendingStaWorkers();
    const auto now=GetTickCount64();
    const DWORD drainBudget=now<admissionDeadline?
        static_cast<DWORD>(std::min<ULONGLONG>(2000,admissionDeadline-now)):0;
    if(teardownDiagnostics)std::cerr<<"OWNED_TEARDOWN phase=beforeStaWorkerDrain pending="<<beforeDrain
        <<" budgetMilliseconds="<<drainBudget<<" fixtureResult="<<result<<'\n';
    const auto drained=explorer::drainStaWorkers(drainBudget);
    const auto afterDrain=explorer::pendingStaWorkers();
    if(teardownDiagnostics)std::cerr<<"OWNED_TEARDOWN phase=afterStaWorkerDrain pending="<<afterDrain
        <<" HRESULT="<<static_cast<ULONG>(drained)<<" fixtureResult="<<result<<'\n';
    if(drained!=S_OK||afterDrain) {
        std::cerr<<"FAIL: final owned STA worker drain HRESULT="<<static_cast<ULONG>(drained)
            <<" pending="<<afterDrain<<'\n';result=1;
    }
    if(teardownDiagnostics)std::cerr<<"OWNED_TEARDOWN phase=beforeOleUninitialize fixtureResult="<<result<<'\n';
    nativeApartment.finishOrTerminate();
    if(teardownDiagnostics)std::cerr<<"OWNED_TEARDOWN phase=afterOleUninitialize fixtureResult="<<result<<'\n';
    if(!result&&admitted) {
        bool visible=true;const auto isolation=desktop.verifyIsolation();
        const auto visibility=desktop.visibleWindowsOnInputDesktop(visible);
        if(FAILED(isolation)||FAILED(visibility)||visible||GetTickCount64()>=admissionDeadline) {
            std::cerr<<"FAIL: image policy teardown crossed deadline/isolation boundary\n";result=1;
        } else std::cout<<"PASS: all actual comparable first-current artwork retained; real repeated, null-first and explicit association/dynamic overrides; no UI parity claim\n";
    }
    return result;
}
