#pragma once

#include <windows.h>
#include "explorer/ribbon_features.hpp"
#include "explorer/chrome.hpp"
#include <filesystem>
#include <cstdint>
#include <optional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct IUIFramework;
struct IShellItem;

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
    HRESULT verifyEmptyForDiagnostic(DWORD& windows, bool& enumReturned, DWORD& enumError) const;
    struct DiagnosticLabelReadback {
        HRESULT guard = E_PENDING, before = E_PENDING, applied = E_PENDING, after = E_PENDING;
        HRESULT apartmentRead = E_PENDING;
        int apartmentType = -1, apartmentQualifier = -1;
        DWORD windows = 0, beforeLabels = 0, beforeRid = 0, beforeMask = 0;
        DWORD afterLabels = 0, afterRid = 0, afterMask = 0, afterFlags = 0;
        bool enumReturned = false;
        DWORD enumError = 0;
        bool exactOwnedCurrent = false, daclUnchanged = false, ownerUnchanged = false, groupUnchanged = false;
    };
    // Only an empty, exact current owned desktop, before explicit COM/HWND creation.
    // This is a diagnostic comparison; initialize() retains its default SD.
    // Changes LABEL only, reads it back, and never permits desktop switching.
    HRESULT setLowIntegrityLabelForDiagnostic(DiagnosticLabelReadback& readback);
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

// Verifies one complete unresolved native DateModified Today leaf. Equivalent
// native restatement is accepted; absolute dates, other presets and compound
// filters are rejected. Requires caller-initialized COM; no HWND or output.
HRESULT validateRelativeTodayQuery(std::wstring_view query) noexcept;

// A noninheritable read lease for an existing local source descriptor. It
// atomically refuses competing data-write/delete handles and does not recall
// offline data or follow reparses. Attribute-only access is not excluded;
// callers must retain their independent byte/metadata preservation checks.
class VisualSourceReadLease final {
public:
    VisualSourceReadLease() noexcept = default;
    ~VisualSourceReadLease();
    VisualSourceReadLease(const VisualSourceReadLease&) = delete;
    VisualSourceReadLease& operator=(const VisualSourceReadLease&) = delete;
    HRESULT acquire(const PrivateDesktop& desktop, const std::filesystem::path& path);
    bool held() const noexcept { return file_ != INVALID_HANDLE_VALUE; }
private:
    HANDLE file_ = INVALID_HANDLE_VALUE;
};

struct DocumentsLibrarySourceReadback {
    bool requested = false;
    bool unavailable = false;
    bool unsupported = false;
    bool displayUnsupported = false;
    bool writeProtected = false;
    HRESULT leaseRead = E_NOTIMPL;
    HRESULT resolveRead = E_NOTIMPL;
    HRESULT knownFolderRead = E_NOTIMPL;
    HRESULT itemRead = E_NOTIMPL;
    HRESULT pathRead = E_NOTIMPL;
    HRESULT parsingNameRead = E_NOTIMPL;
    HRESULT parsingItemRead = E_NOTIMPL;
    HRESULT parsingIdentityRead = E_NOTIMPL;
    HRESULT loadRead = E_NOTIMPL;
    HRESULT fileRead = E_NOTIMPL;
    HRESULT verifyRead = E_NOTIMPL;
    bool currentMatches = false;
    bool backingFileUnchanged = false;
    bool metadataUnchanged = false;
    HRESULT optionsRead = E_NOTIMPL;
    DWORD options = 0;
    HRESULT typeRead = E_NOTIMPL;
    bool documentsType = false;
    HRESULT foldersRead = E_NOTIMPL;
    DWORD folderCount = 0;
    HRESULT privateSaveRead = E_NOTIMPL;
    HRESULT publicSaveRead = E_NOTIMPL;
    struct Verification {
        UINT stage = 0;
        HRESULT currentRead = E_NOTIMPL;
        HRESULT knownFolderRead = E_NOTIMPL;
        HRESULT knownItemRead = E_NOTIMPL;
        HRESULT knownIdentityRead = E_NOTIMPL;
        HRESULT pathRead = E_NOTIMPL;
        HRESULT metadataRead = E_NOTIMPL;
        HRESULT privateSaveRead = E_NOTIMPL;
        HRESULT publicSaveRead = E_NOTIMPL;
        bool knownMatches = false, pathMatches = false;
        bool optionsUnchanged = false, typeUnchanged = false, folderCountUnchanged = false;
        bool privateSaveStatusUnchanged = false, publicSaveStatusUnchanged = false;
        bool privateSaveMatches = false, publicSaveMatches = false;
        bool fileIdentityUnchanged = false, fileSizeUnchanged = false;
        bool fileCreationUnchanged = false, fileWriteUnchanged = false, fileChangeUnchanged = false;
        bool fileAttributesUnchanged = false, fileBytesUnchanged = false;
    } verification;
};
std::string documentsLibrarySourceJson(const DocumentsLibrarySourceReadback& source);

// A protected current-profile metadata snapshot. It never creates a content
// view: native browsing can rewrite a library descriptor even without Commit.
// Retain the lease through all native interfaces. File bytes/PIDLs/paths remain
// in memory and are never serialized into the numeric provenance report.
class DocumentsLibraryVisualSource final {
public:
    DocumentsLibraryVisualSource() noexcept;
    ~DocumentsLibraryVisualSource();
    DocumentsLibraryVisualSource(const DocumentsLibraryVisualSource&) = delete;
    DocumentsLibraryVisualSource& operator=(const DocumentsLibraryVisualSource&) = delete;
    HRESULT resolve(const PrivateDesktop& desktop, std::wstring& location,
                    DocumentsLibrarySourceReadback& readback);
    HRESULT verify(const PrivateDesktop& desktop, IShellItem* current,
                   DocumentsLibrarySourceReadback& readback) const;
    HRESULT verifyMetadata(const PrivateDesktop& desktop, DocumentsLibrarySourceReadback& readback) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
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

// Exact native popup observed by UI Automation; all bounds are screen pixels.
// Capture accepts only the same owned private HWND and unchanged geometry.
struct NativePopupCapture {
    struct ParentReadback {
        int type = 0;
        bool enabled = false;
        bool offscreen = true;
        RECT bounds{};
        HRESULT elementRead = E_NOTIMPL;
        HRESULT typeRead = E_NOTIMPL;
        HRESULT enabledRead = E_NOTIMPL;
        HRESULT offscreenRead = E_NOTIMPL;
        HRESULT boundsRead = E_NOTIMPL;
        HRESULT patternRead = E_NOTIMPL;
        HRESULT stateRead = E_NOTIMPL;
        int expandState = 3; // ExpandCollapseState_LeafNode; no UIA dependency here
        HRESULT parentRead = E_NOTIMPL;
        int parentType = 0;
        RECT parentBounds{};
        bool parentSameName = false;
        HRESULT ribbonAncestorRead = E_NOTIMPL;
        bool ribbonAncestor = false;
        bool ribbonBoundsContain = false;
        bool toolbarBoundsContain = false;
        bool accepted = false;
        HRESULT automationIdRead = E_NOTIMPL;
        std::wstring automationId;
    };
    UINT stage = 0;
    UINT candidateCount = 0;
    std::vector<ParentReadback> parents;
    UINT_PTR ribbonWindow = 0;
    RECT ribbonBounds{};
    UINT nativeCommand = 0;
    UINT commandType = 0;
    UINT executionAttempts = 0;
    HWND window = nullptr;
    RECT bounds{};
    std::vector<RECT> rowBounds;
};
// Numeric-only readback; does not inspect or operate the window.
std::string nativePopupExpansionJson(const NativePopupCapture& popup);

struct VisualCaptureOptions {
    struct CommandReadinessReadback {
        bool requested = false;
        bool ready = false;
        HRESULT read = E_NOTIMPL;
        ULONGLONG waitMs = 0;
        ULONGLONG namespaceGeneration = 0;
        UINT pendingCapabilities = 0;
        UINT workerTasks = 0;
        UINT statePolls = 0;
        bool selectionBatchPending = false;
    };
    CommandReadinessReadback commandReadiness;
    struct SearchDateMenuReadback {
        bool requested = false;
        HRESULT read = E_NOTIMPL;
        bool expanded = false;
        UINT expectedRows = 0;
        UINT matchedRows = 0;
        bool relativeToday = false;
        HRESULT scopeRead = E_NOTIMPL;
        HRESULT resultRead = E_NOTIMPL;
        UINT resultCount = 0;
        UINT expectedResults = 0;
        UINT matchedIdentities = 0;
        UINT unexpectedPaths = 0;
        UINT duplicateIdentities = 0;
        HRESULT submitRead = E_NOTIMPL;
        UINT submitCount = 0;
        UINT recentCount = 0;
        bool recentMatchesQuery = false;
        HRESULT scopeNavigationRead = E_NOTIMPL;
        bool physicalScopeReady = false;
        bool savedInputUnchanged = false;
        bool nativeViewChanged = false;
        bool scopePreserved = false;
        bool historyCommitted = false;
        bool factoryRetained = false;
        UINT retainedFactoriesBefore = 0;
        UINT retainedFactoriesAfter = 0;
        UINT navigationDelta = 0;
        HRESULT recentEnabledRead = E_NOTIMPL;
        bool recentEnabled = false;
    };
    SearchDateMenuReadback searchDateMenu;
    NativePopupCapture searchDatePopup;
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
        bool cachedChecked = false;
        UINT cachedNativeState = 0;
        bool pending = false;
        bool slowStateCompleted = false;
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
    // Uses documented PW_CLIENTONLY without the compositor full-content flag.
    // For an owned native control fixture; includeFrame must be false.
    bool nativeClientPrint = false;
    // Authentic client pixels cropped from this verified owned root's print.
    // The full source PNG is retained; no scaling or synthetic paint is used.
    HWND nativeClientCropSource = nullptr;
    std::filesystem::path nativeClientCropSourceImage;
    // Diagnostic DC layout for this owned-root crop only; ordinary captures
    // retain their existing native printing policy.
    std::optional<DWORD> nativeClientCropPrintLayout;
    std::optional<UINT> nativeClientCropPrintFlags;
    bool trimInvisibleFrame = false;
    unsigned layoutDpi = 96;
    unsigned minimumUniqueColors = 12;
    double minimumInkFraction = 0.002;
    double maximumUnpaintedFraction = 0.001;
    bool requireVisibleChildren = true;
    RECT pixelInspectionBounds{}; // optional read-only region, in output pixels
    std::optional<COLORREF> pixelInspectionBackground;
};

struct VisualCaptureReport {
    DocumentsLibrarySourceReadback documentsLibrarySource;
    VisualCaptureOptions::CommandReadinessReadback commandReadiness;
    VisualCaptureOptions::SearchDateMenuReadback searchDateMenu;
    NativePopupCapture searchDateExpansion;
    struct PopupReadback {
        UINT_PTR window = 0;
        RECT bounds{}; // output pixels
        HRESULT read = E_NOTIMPL;
        bool ownedPrivate = false;
        bool printed = false;
        UINT physicalRows = 0;
        unsigned uniqueColors = 0;
        double inkFraction = 0;
        double unpaintedFraction = 0;
        unsigned minimumRowUniqueColors = 0;
        double minimumRowInkFraction = 0;
    };
    PopupReadback searchDatePopup;
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
    UINT printWindowFlags = 0;
    bool nativeClientCropped = false;
    UINT_PTR printSourceWindow = 0;
    UINT_PTR printTargetWindow = 0;
    RECT printSourceBounds{}, printTargetClientBounds{}; // physical screen pixels
    std::filesystem::path nativeClientCropSourceImage;
    std::optional<DWORD> nativeClientCropPrintLayout;
    DWORD printSourceDcLayout = GDI_ERROR;
    int printSourceDcMapMode = 0;
    DWORD printMemoryDcInitialLayout = GDI_ERROR;
    DWORD printMemoryDcBeforeLayout = GDI_ERROR;
    DWORD printMemoryDcAfterLayout = GDI_ERROR;
    int printMemoryDcInitialMapMode = 0;
    int printMemoryDcBeforeMapMode = 0;
    int printMemoryDcAfterMapMode = 0;
    bool invisibleFrameTrimmed = false;
    unsigned uniqueColors = 0;
    double inkFraction = 0;
    double unpaintedFraction = 0;
    RECT pixelInspectionBounds{};
    unsigned inspectionUniqueColors = 0;
    double inspectionInkFraction = 0;
    std::optional<COLORREF> pixelInspectionBackground;
    uint64_t inspectionPixelHash = 0;
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
