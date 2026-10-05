#pragma once
#include <windows.h>
#include <memory>

namespace explorer {

// Prepare on the owning STA before launching a worker. The lease owns a handle
// to that exact desktop, without switch access, and survives task cancellation.
// The creator must retain its exact desktop connection through the final drain.
// Workers attach to that borrowed connection; the owned keepalive is never
// attached or inherited by native helper threads, and closes after worker exit.
// Attach on the worker before COM/windows; finish after releasing providers and
// uninitializing COM. Never close GetThreadDesktop's borrowed handle.
class StaWorkerLease final {
public:
    static HRESULT prepare(std::unique_ptr<StaWorkerLease>* result) noexcept;
    ~StaWorkerLease();
    StaWorkerLease(const StaWorkerLease&) = delete;
    StaWorkerLease& operator=(const StaWorkerLease&) = delete;
    HRESULT attach() noexcept;
    // Move one retained value into preallocated creator-STA cleanup. The
    // worker must finish/exit; the creator releases it while pumping COM.
    // Validation failure preserves keepalive. Native bookkeeping failure
    // consumes/retains it and makes the final drain fail. No allocation occurs.
    HRESULT deferCreatorRelease(std::shared_ptr<void>& keepalive) noexcept;
    HRESULT finish() noexcept;
private:
    struct Impl;
    explicit StaWorkerLease(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

// Only for final shutdown, while the owning STA and its sites are still alive.
// CoWait dispatches marshaled COM calls and window messages; normal navigation
// cancellation remains nonblocking. A timeout is retained as an HRESULT.
HRESULT drainStaWorkers(DWORD timeoutMilliseconds) noexcept;
unsigned pendingStaWorkers() noexcept;
struct StaWorkerDiagnostics {
    unsigned pending = 0, completedThreads = 0;
    unsigned creatorHandleUsers = 0, otherHandleUsers = 0;
    unsigned desktopWindows = 0, creatorWindows = 0, otherWindows = 0;
};
// Numeric-only, read-only diagnostics for this creator's owned desktops.
HRESULT staWorkerDiagnostics(StaWorkerDiagnostics* result) noexcept;

} // namespace explorer
