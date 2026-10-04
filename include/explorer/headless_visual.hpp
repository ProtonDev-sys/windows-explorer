#pragma once

#include <windows.h>
#include "explorer/ribbon_features.hpp"
#include "explorer/chrome.hpp"
#include <filesystem>
#include <string>
#include <vector>

struct IUIFramework;

namespace explorer {

// Attach the calling thread before OleInitialize, registering window classes,
// or creating any HWND. The desktop is never made an input desktop. Destroy all
// windows and uninitialize COM before destroying this guard.
class PrivateDesktop final {
public:
    PrivateDesktop() noexcept = default;
    ~PrivateDesktop();
    PrivateDesktop(const PrivateDesktop&) = delete;
    PrivateDesktop& operator=(const PrivateDesktop&) = delete;
    HRESULT initialize();
    HRESULT verifyIsolation(bool* inputDesktopUnchanged = nullptr) const;
    HRESULT visibleWindowsOnInputDesktop(bool& visible) const;
    // The initialized guard on this UI thread, for bounded native rendering
    // phases in otherwise hidden tests. Null on unguarded/other threads.
    static const PrivateDesktop* current() noexcept;
    bool ready() const noexcept { return desktop_ != nullptr; }
    const std::wstring& name() const noexcept { return name_; }
    const std::wstring& originalInputName() const noexcept { return inputName_; }
private:
    HDESK desktop_ = nullptr;
    HDESK previous_ = nullptr; // borrowed GetThreadDesktop handle
    DWORD thread_ = 0;
    std::wstring name_;
    std::wstring inputName_;
};

struct VisualWidget {
    unsigned depth = 0;
    int id = 0;
    std::wstring className;
    std::wstring text;
    RECT bounds{}; // pixels relative to the outer window; includes frame
    bool visible = false;
    bool enabled = false;
};

struct VisualCaptureOptions {
    struct ContextReadback {
        UINT logicalContext = 0;
        UINT nativeIdentifier = 0;
        UINT availability = 0;
        HRESULT read = E_NOTIMPL;
    };
    struct ProviderReadback {
        UINT command = 0;
        UINT selectedCount = 0;
        HRESULT cachedRead = E_NOTIMPL;
        bool cachedEnabled = false;
        HRESULT nativeRead = E_NOTIMPL;
        UINT nativeState = 0;
    };
    IUIFramework* ribbonFramework = nullptr; // borrowed; optional native state evidence
    IUIFramework* nativeRibbonFramework = nullptr; // borrowed underlying owner-STA framework
    UINT layoutGalleryNativeCommand = 0;
    std::wstring ribbonLayout; // actual loaded layout, supplied by its native host
    HRESULT installedRibbonStatus = E_NOTIMPL;
    bool ribbonFeaturesRead = false;
    RibbonFeatures ribbonFeatures;
    std::vector<ContextReadback> ribbonContexts;
    std::vector<ProviderReadback> ribbonProviders;
    bool includeFrame = true;
    bool trimInvisibleFrame = false;
    unsigned layoutDpi = 96;
    unsigned minimumUniqueColors = 12;
    double minimumInkFraction = 0.002;
    double maximumUnpaintedFraction = 0.001;
    bool requireVisibleChildren = true;
    RECT pixelInspectionBounds{}; // optional read-only region, in output pixels
};

struct VisualCaptureReport {
    struct ToolbarButtonReadback {
        int index = 0;
        int command = 0;
        int width = 0;
        int image = 0;
        BYTE state = 0;
        BYTE style = 0;
        RECT bounds{}; // native toolbar client coordinates, no text or paths
        HRESULT read = E_NOTIMPL;
        HRESULT drawRead = E_NOTIMPL;
        ChromeDrawReadback draw;
    };
    std::vector<ToolbarButtonReadback> breadcrumbButtons;
    std::vector<ToolbarButtonReadback> navigationButtons;
    struct RibbonCommandReadback {
        UINT id = 0;
        HRESULT enabledRead = E_NOTIMPL;
        HRESULT labelRead = E_NOTIMPL;
        HRESULT itemsRead = E_NOTIMPL;
        HRESULT imageRead = E_NOTIMPL;
        bool enabled = false;
        bool firstItemImage = false;
        UINT itemCount = 0;
        std::wstring label; // static command only; never native MRU item labels
    };
    std::vector<RibbonCommandReadback> ribbonCommands;
    std::wstring ribbonLayout;
    HRESULT installedRibbonStatus = E_NOTIMPL;
    bool ribbonFeaturesRead = false;
    RibbonFeatures ribbonFeatures;
    std::vector<VisualCaptureOptions::ContextReadback> ribbonContexts;
    std::vector<VisualCaptureOptions::ProviderReadback> ribbonProviders;
    bool fileExtensionsRead = false;
    bool osFileExtensions = false;
    bool ribbonFileExtensions = false;
    DWORD hideFileExt = 1;
    unsigned width = 0;
    unsigned height = 0;
    unsigned windowDpi = 0;
    unsigned layoutDpi = 0;
    RECT clientBounds{};
    std::wstring desktopName;
    std::wstring inputDesktopName;
    bool inputDesktopUnchanged = false;
    bool visibleInputDesktopWindows = false;
    bool printWindowSucceeded = false;
    bool invisibleFrameTrimmed = false;
    unsigned uniqueColors = 0;
    double inkFraction = 0;
    double unpaintedFraction = 0;
    RECT pixelInspectionBounds{};
    unsigned inspectionUniqueColors = 0;
    double inspectionInkFraction = 0;
    HRESULT layoutGallerySelectedRead = E_NOTIMPL;
    UINT layoutGallerySelected = 0xffffffffu;
    UINT layoutGalleryNativeCommand = 0;
    HRESULT layoutGalleryNativeSelectedRead = E_NOTIMPL;
    UINT layoutGalleryNativeSelected = 0xffffffffu;
    unsigned visibleChildren = 0;
    std::vector<VisualWidget> widgets;
};

// Captures real WM_PRINT/PrintWindow output into a top-down DIB, with no screen
// scraping or alternate synthetic renderer. Requires an owned HWND on this
// thread's private desktop and COM initialized for WIC. Output is create-new;
// failed capture does not replace a previous result.
HRESULT captureWindowPng(const PrivateDesktop& desktop, HWND window,
                         const std::filesystem::path& output,
                         const VisualCaptureOptions& options,
                         VisualCaptureReport& report);

// An inventory accompanies the PNG so geometry/control/text checks do not
// mistake a flat/blank screenshot or mismatched file contents for fidelity.
HRESULT writeVisualCaptureReport(const std::filesystem::path& output,
                                 const VisualCaptureReport& report);

} // namespace explorer
