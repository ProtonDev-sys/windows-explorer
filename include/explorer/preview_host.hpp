#pragma once
#include <windows.h>
#include <shtypes.h>
#include <cstdint>
#include <functional>
#include <memory>

namespace explorer {

enum class PreviewEmptyReason { None, MultipleSelection, Folder, NoAssociation, Disabled };
enum class PreviewHostStage {
    Idle, Queued, Starting, Association, Activation, Site, Source, Initialization,
    Window, Rendering, Ready, Resizing, Focusing, Unloading, Retired, Failed, Stopped
};
enum class PreviewInitialization { None, Stream, Item, File };

// Copied values only; no HFONT or apartment-owned interface crosses threads.
// An absent field withholds that suggestion; it does not reset handler styling.
struct PreviewVisualSuggestions {
    COLORREF background = 0, text = 0;
    LOGFONTW font{};
    bool backgroundPresent = false, textPresent = false, fontPresent = false;
};

struct PreviewVisualStatus {
    std::uint64_t requestedRevision = 0, attemptedRevision = 0, attemptedEpoch = 0;
    PreviewVisualSuggestions requested, attempted;
    // Raw optional HRESULTs belong to attemptedRevision/attemptedEpoch. A
    // newer requestedRevision can remain pending without changing those facts.
    HRESULT result = S_FALSE, queryResult = E_PENDING;
    HRESULT backgroundResult = E_PENDING, textResult = E_PENDING, fontResult = E_PENDING;
    bool queryAttempted = false, backgroundAttempted = false, textAttempted = false, fontAttempted = false;
    bool pending = false;
};

// Teardown belongs to its retired source epoch and never replaces the primary
// rendering stage/result. Raw method results remain available on clean exits.
struct PreviewCleanupStatus {
    std::uint64_t epoch = 0;
    PreviewHostStage stage = PreviewHostStage::Idle;
    HRESULT result = S_OK, unloadResult = E_PENDING, siteClearResult = E_PENDING, windowDestroyResult = E_PENDING;
    bool unloadAttempted = false, siteClearAttempted = false, windowDestroyAttempted = false, pending = false;
};

// A queued request is not proof of native focus. Raw method results, the
// returned HWND and worker Shift sample belong to completedSequence/epoch;
// a newer requestedSequence can remain pending while those facts are retained.
struct PreviewFocusStatus {
    std::uint64_t requestedSequence = 0, requestedEpoch = 0, completedSequence = 0, completedEpoch = 0;
    bool requestedReverse = false, completedReverse = false;
    HRESULT result = E_PENDING, setResult = E_PENDING, queryResult = E_PENDING;
    HWND queriedWindow = nullptr;
    bool workerShiftRead = false, workerShiftDown = false, setAttempted = false, queryAttempted = false, pending = false;
};

struct PreviewHostStatus {
    std::uint64_t requestedEpoch = 0, activeEpoch = 0;
    PreviewHostStage stage = PreviewHostStage::Idle;
    PreviewHostStage nativeStage = PreviewHostStage::Idle;
    PreviewEmptyReason emptyReason = PreviewEmptyReason::None;
    PreviewInitialization initialization = PreviewInitialization::None;
    HRESULT result = S_OK, nativeResult = S_OK, cleanupResult = S_OK, focusResult = E_PENDING;
    CLSID handler{};
    HWND sessionWindow = nullptr;
    DWORD workerThread = 0, sessionThread = 0, sessionProcess = 0;
    bool workerStarted = false, workerExited = false, ready = false, pending = false;
    bool desktopMatchesCreator = false;
    wchar_t desktopName[256]{};
    // Optional native visual suggestions never replace the rendering result.
    PreviewVisualStatus visuals;
    PreviewCleanupStatus cleanup;
    PreviewFocusStatus focus;
};

struct PreviewHostCallbacks {
    // Invoked on the creator STA through the per-ticket standard-marshaled
    // IPreviewHandlerFrame. Validate the epoch again before changing App state.
    // Return S_OK only for a shortcut actually accepted by the host, S_FALSE
    // otherwise. Never call IPreviewHandler::TranslateAccelerator here.
    std::function<HRESULT(std::uint64_t, const MSG&)> translateAccelerator;
    // Optional creator-owned wake window/message. Worker posts only an epoch
    // split into two DWORD values; no pointers, COM objects or borrowed strings.
    HWND notifyWindow = nullptr;
    UINT notifyMessage = 0;
};

// Explicit native host: registered CLSCTX_LOCAL_SERVER, actual Shell-item
// association, read-only Stream/Item/File initialization and native rendering.
// https://learn.microsoft.com/windows/win32/shell/preview-handlers
// https://learn.microsoft.com/windows/win32/api/propsys/nf-propsys-iinitializewithstream-initialize
//
// All methods except the native worker run on the creating STA. The render
// container must be a dedicated creator-owned child inside the App's pane;
// status/empty text belongs outside it. update/clear hide that container on its
// own thread immediately. Only showCurrent may expose a fresh Ready ticket.
// Every worker-owned session child is destroyed before its successor is built.
//
// One worker and one coalesced latest request: cancellation invalidates a
// ticket, not an arbitrary synchronous COM call. A blocked native provider
// leaves later work pending while the main UI remains responsive. No provider
// retries, registry changes, isolation bypass, detached worker or shared-server
// termination. drain must succeed BEFORE parent HWND/App/COM/desktop teardown;
// on failure preserve those resources and follow the App's existing fail-closed
// shutdown policy. StaWorkerLease also retains creator cleanup through actual
// kernel-thread exit. A new object does not imply a new prevhost process.
class NativePreviewHost final {
public:
    static HRESULT create(HWND renderContainer, PreviewHostCallbacks callbacks,
                          std::unique_ptr<NativePreviewHost>* result) noexcept;
    ~NativePreviewHost();
    NativePreviewHost(const NativePreviewHost&) = delete;
    NativePreviewHost& operator=(const NativePreviewHost&) = delete;

    // Caller supplies the absolute PIDL of its exact, currently accepted SINGLE
    // native selected item. It is cloned; no filesystem path is inferred.
    // Epochs must be nonzero and strictly increase for update/clear.
    HRESULT update(PCIDLIST_ABSOLUTE actualSingleItem, std::uint64_t epoch) noexcept;
    HRESULT clear(std::uint64_t epoch, PreviewEmptyReason reason = PreviewEmptyReason::None) noexcept;
    // Client coordinates within the immutable renderContainer; bounded positive
    // sizes. Native SetRect calls are coalesced and do not restart DoPreview.
    HRESULT resize(HWND renderContainer, const RECT& clientBounds) noexcept;
    // Coalesces distinct copied values independently of the source epoch.
    // S_OK queues a revision; S_FALSE means the same values were already requested.
    // Raw optional-interface/method results are reported separately in status.
    // Native rejection never fails a current rendered preview. A handler is
    // queried at most once per session, only when a present field is requested.
    // https://learn.microsoft.com/windows/win32/api/shobjidl_core/nn-shobjidl_core-ipreviewhandlervisuals
    HRESULT suggestVisuals(const PreviewVisualSuggestions& suggestions) noexcept;
    // S_OK acknowledges a coalesced asynchronous request, not actual focus.
    // status().focus reports one SetFocus and, on its S_OK, one QueryFocus;
    // callers must verify the completed sequence/epoch and actual native HWND.
    // These receipts never replace the primary rendering result. focusResult
    // remains the direction-checked SetFocus result for existing callers.
    // https://learn.microsoft.com/windows/win32/api/shobjidl_core/nf-shobjidl_core-ipreviewhandler-queryfocus
    HRESULT focus(bool reverse) noexcept;
    HRESULT showCurrent(std::uint64_t epoch) noexcept;
    PreviewHostStatus status() const noexcept;
    // Stops acceptance, hides the owned container and pumps the creator STA
    // while waiting for this exact retained kernel thread. A timeout is failure,
    // never permission to destroy the parent under a live handler/callback.
    HRESULT drain(DWORD timeoutMilliseconds) noexcept;
private:
    struct Impl;
    explicit NativePreviewHost(std::shared_ptr<Impl> implementation) noexcept;
    std::shared_ptr<Impl> impl_;
};

} // namespace explorer
