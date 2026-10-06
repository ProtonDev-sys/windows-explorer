#pragma once

#include <windows.h>
#include <atomic>
#include <cstddef>
#include <span>
#include <string>

struct IShellItem;
struct IInitializeWithStream;
struct IStream;

namespace explorer {

class PrivateDesktop;

// Capture on the guarded creator. Borrowed handles and names remain valid
// until the caller's actual worker thread has exited and joined. The helper
// never switches desktops, initializes COM, or transports apartment pointers.
struct PreviewDiagnosticContext {
    HDESK desktop = nullptr;
    HWINSTA station = nullptr;
    std::wstring desktopName;
    std::wstring inputName;
};
HRESULT capturePreviewDiagnosticContext(const PrivateDesktop& desktop,
    PreviewDiagnosticContext* result) noexcept;

// Missing keys/values retain their native FILE/PATH_NOT_FOUND HRESULT. No
// absent, malformed or unreadable value is interpreted as enabled/disabled.
// Registration entries report only type/length/presence; no text is copied.
struct PreviewRegistryReadback {
    HRESULT keyRead = E_PENDING, valueRead = E_PENDING, dataRead = E_PENDING;
    bool keyPresent = false, valuePresent = false, dwordValid = false;
    DWORD type = REG_NONE, bytes = 0, value = 0;
};
struct PreviewPolicySnapshot {
    HRESULT guardBefore = E_PENDING, guardAfter = E_PENDING, budgetStatus = S_OK;
    HRESULT completed = E_PENDING;
    PreviewRegistryReadback showPreviewHandlers;
    PreviewRegistryReadback userNoReadingPane, machineNoReadingPane;
    PreviewRegistryReadback userEnforceShellExtensionSecurity, machineEnforceShellExtensionSecurity;
    PreviewRegistryReadback userPreviewHandlers, machinePreviewHandlers;
    PreviewRegistryReadback userApproved, machineApproved;
};
// associationHandler is the original selected item's already-verified native
// association CLSID. This function never selects or activates a handler.
HRESULT inspectPreviewPolicies(const PreviewDiagnosticContext& context,
    REFCLSID associationHandler, ULONGLONG deadline, PreviewPolicySnapshot* result,
    const std::atomic_bool* cancelled = nullptr) noexcept;

inline constexpr std::size_t ownedPreviewRtfBytes = 83;
struct PreviewStreamReadback {
    HRESULT guardBefore = E_PENDING, guardAfter = E_PENDING, budgetStatus = S_OK;
    HRESULT apartmentRead = E_PENDING;
    HRESULT attributesRead = E_PENDING, pathRead = E_PENDING, sourceOpen = E_PENDING;
    HRESULT sourceBefore = E_PENDING, sourceBytesBefore = E_PENDING;
    HRESULT bindContext = E_PENDING, bindOptions = E_PENDING, bindRead = E_PENDING;
    HRESULT statRead = E_PENDING, seekRead = E_PENDING, streamRead = E_PENDING, rewindRead = E_PENDING;
    HRESULT initialized = E_PENDING;
    HRESULT sourceAfter = E_PENDING, sourceBytesAfter = E_PENDING, pathAfter = E_PENDING;
    HRESULT completed = E_PENDING;
    DWORD shellAttributes = 0, bindMode = 0, streamType = 0, streamMode = 0;
    ULONGLONG streamSize = 0;
    ULONG streamBytes = 0;
    FILE_ID_INFO identityBefore{}, identityAfter{};
    FILE_BASIC_INFO basicBefore{}, basicAfter{};
    bool identityMatchesBefore = false, identityMatchesAfter = false;
    bool fileBytesMatchBefore = false, fileBytesMatchAfter = false, streamBytesMatch = false;
    bool metadataUnchanged = false, pathIdentityUnchanged = false;
    bool originalMetadataMatchesBefore = false, originalMetadataMatchesAfter = false;
    bool initializerAttempted = false;
};
// Synchronous, read-only calls on the correctly initialized caller STA. The
// caller must marshal/recreate actualSelected for this apartment, retain its
// independent selection/attrs/times snapshots, and supply a finite owned-worker
// kernel-exit/join bound. The deadline is checked between calls; BIND_OPTS'
// native deadline is advisory, not cancellation of a blocked provider call.
//
// Accepts exactly the complete 83-byte owned RTF fixture and expected native
// FileID. No Write/Commit/SetWindow/DoPreview call occurs. A fresh initializer
// may be supplied only after its native module/surrogate route was verified by
// the caller. It is initialized once from this exact BHID_Stream at offset 0.
//
// *retainedStream must initially be null. After complete stream/source proof,
// an owned reference is returned even if later Initialize/preservation fails;
// this is diagnostic partial output, never rendering success. Release ALL
// handler interfaces before releasing that stream, then uninitialize COM.
HRESULT inspectOwnedPreviewStream(const PreviewDiagnosticContext& context,
    IShellItem* actualSelected, const FILE_ID_INFO& expectedIdentity,
    std::span<const BYTE> expectedBytes, ULONGLONG deadline,
    IInitializeWithStream* freshApprovedInitializer, IStream** retainedStream,
    PreviewStreamReadback* result, const std::atomic_bool* cancelled = nullptr,
    const FILE_BASIC_INFO* originalBasic = nullptr) noexcept;

} // namespace explorer
