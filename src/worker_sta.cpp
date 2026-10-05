#include "explorer/worker_sta.hpp"
#include <objbase.h>
#include <algorithm>
#include <mutex>
#include <new>
#include <tlhelp32.h>
#include <vector>

namespace explorer {
namespace {
HRESULT lastFailure() noexcept {
    const DWORD error = GetLastError();
    return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
}
struct DeferredDesktop;
struct WorkerGroup {
    HANDLE idle = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    HDESK creatorConnection = nullptr; // Borrowed for the group's entire lifetime.
    unsigned active = 0;
    HRESULT cleanup = S_OK;
    DeferredDesktop* completed = nullptr;
    ~WorkerGroup() { if (idle) CloseHandle(idle); }
};
std::mutex groupsMutex;
// Windows recycles thread IDs. A finished fixture's deferred cleanup status
// must never become the shutdown result of an unrelated, later creator STA.
// Workers retain the group explicitly, so it survives its creator's TLS.
thread_local std::shared_ptr<WorkerGroup> creatorGroup;
std::shared_ptr<WorkerGroup> currentGroup() {
    return creatorGroup;
}
struct DeferredDesktop {
    HDESK desktop = nullptr;
    HANDLE thread = nullptr, wait = nullptr;
    std::shared_ptr<WorkerGroup> group;
    std::shared_ptr<void> creatorRelease;
    bool retainOnFailure = false;
    DeferredDesktop* next = nullptr;
};
void CALLBACK releaseTerminatedDesktop(void* context, BOOLEAN) {
    std::unique_ptr<DeferredDesktop> owned(static_cast<DeferredDesktop*>(context));
    // A pool callback must never enter USER32: its first desktop connection
    // could itself retain a handle being reaped. Publish termination to the
    // original creator STA, which owns an independent desktop connection.
    UnregisterWaitEx(owned->wait, nullptr);
    CloseHandle(owned->thread);
    owned->thread = owned->wait = nullptr;
    std::lock_guard lock(groupsMutex);
    owned->next = owned->group->completed;
    owned->group->completed = owned.get();
    SetEvent(owned->group->idle);
    owned.release();
}
HRESULT reapCompleted(const std::shared_ptr<WorkerGroup>& group) noexcept {
    DeferredDesktop* completed = nullptr;
    {
        std::lock_guard lock(groupsMutex);
        completed = group->completed;
        group->completed = nullptr;
        if (group->active) ResetEvent(group->idle);
    }
    while (completed) {
        std::unique_ptr<DeferredDesktop> owned(completed);
        completed = owned->next;
        owned->next = nullptr;
        if (owned->retainOnFailure) {
            std::lock_guard lock(groupsMutex);
            owned->next = group->completed;
            group->completed = owned.release();
            continue;
        }
        // Release native values on this initialized creator STA, outside the
        // group mutex: their destructors may reenter COM or worker bookkeeping.
        owned->creatorRelease.reset();
        const auto closed = CloseDesktop(owned->desktop) ? S_OK : lastFailure();
        std::lock_guard lock(groupsMutex);
        if (SUCCEEDED(closed)) {
            owned->desktop = nullptr;
            if (--group->active == 0) SetEvent(group->idle);
        } else {
            // Native desktop users can finish after the kernel thread signal.
            // Retry BUSY while the creator pumps COM, retaining the exact
            // owned handle; other errors remain explicit shutdown failures.
            if (closed != HRESULT_FROM_WIN32(ERROR_BUSY) && SUCCEEDED(group->cleanup)) group->cleanup = closed;
            owned->next = group->completed;
            group->completed = owned.release();
        }
    }
    std::lock_guard lock(groupsMutex);
    return group->cleanup;
}
}

struct StaWorkerLease::Impl {
    DWORD creatorThread = GetCurrentThreadId();
    HDESK desktop = nullptr;
    HDESK creatorConnection = nullptr; // Borrowed; creator retains it through drain.
    HDESK previous = nullptr;
    DWORD attachedThread = 0;
    HANDLE thread = nullptr;
    std::shared_ptr<WorkerGroup> group;
    std::unique_ptr<DeferredDesktop> completion;
    bool counted = false;
    bool retainOnFailure = false;
    bool terminationRequired = false;
    ~Impl() {
        if (counted && (terminationRequired || retainOnFailure)) {
            // A failed termination registration must not turn into a completed
            // shutdown reservation during destruction. Retain the keepalive
            // and creator connection, and expose the failure to the final drain.
            std::lock_guard lock(groupsMutex);
            if (SUCCEEDED(group->cleanup)) group->cleanup = E_UNEXPECTED;
            if (completion && completion->creatorRelease) {
                completion->retainOnFailure = true;
                completion->desktop = desktop;
                completion->thread = thread;
                completion->group = group;
                completion->next = group->completed;
                group->completed = completion.release();
                desktop = nullptr;
                thread = nullptr;
            }
            counted = false;
        }
        if (desktop && !terminationRequired && !retainOnFailure) CloseDesktop(desktop);
        if (thread) CloseHandle(thread);
        if (counted) {
            std::lock_guard lock(groupsMutex);
            if (--group->active == 0) SetEvent(group->idle);
        }
    }
};

StaWorkerLease::StaWorkerLease(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
StaWorkerLease::~StaWorkerLease() { finish(); }

HRESULT StaWorkerLease::prepare(std::unique_ptr<StaWorkerLease>* result) noexcept {
    if (!result) return E_POINTER;
    try {
        APTTYPE apartment{}; APTTYPEQUALIFIER qualifier{};
        auto hr = CoGetApartmentType(&apartment, &qualifier);
        if (FAILED(hr)) return hr;
        if (apartment != APTTYPE_STA && apartment != APTTYPE_MAINSTA) return RPC_E_WRONG_THREAD;
        if (const auto group = currentGroup()) {
            hr = reapCompleted(group);
            if (FAILED(hr)) return hr;
        }
        const auto borrowed = GetThreadDesktop(GetCurrentThreadId());
        if (!borrowed) return lastFailure();
        if (const auto group = currentGroup(); group && group->creatorConnection != borrowed)
            return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
        auto impl = std::make_unique<Impl>();
        // Reserve termination bookkeeping before launching a worker. Finish
        // must remain allocation-free after providers and COM have torn down.
        impl->completion = std::make_unique<DeferredDesktop>();
        HANDLE duplicate = nullptr;
        // DuplicateHandle explicitly supports GetThreadDesktop handles. The
        // duplicate is independently owned, unlike the borrowed connection,
        // and needs no desktop-name lookup or switch permission.
        if (!DuplicateHandle(GetCurrentProcess(), borrowed, GetCurrentProcess(),
            &duplicate, DESKTOP_CREATEWINDOW | DESKTOP_CREATEMENU | DESKTOP_ENUMERATE |
            DESKTOP_HOOKCONTROL | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS, FALSE, 0)) return lastFailure();
        impl->desktop = reinterpret_cast<HDESK>(duplicate);
        impl->creatorConnection = borrowed;
        {
            std::lock_guard lock(groupsMutex);
            impl->group = currentGroup();
            if (!impl->group) {
                impl->group = std::make_shared<WorkerGroup>();
                if (!impl->group->idle) return lastFailure();
                impl->group->creatorConnection = borrowed;
                creatorGroup = impl->group;
            }
            if (!ResetEvent(impl->group->idle)) return lastFailure();
            ++impl->group->active;
            impl->counted = true;
        }
        *result = std::unique_ptr<StaWorkerLease>(new StaWorkerLease(std::move(impl)));
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
      catch (...) { return E_FAIL; }
}

HRESULT StaWorkerLease::attach() noexcept {
    if (!impl_ || !impl_->desktop) return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
    if (impl_->attachedThread) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
    const auto original = GetThreadDesktop(GetCurrentThreadId());
    if (!original) return lastFailure();
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
        &impl_->thread, SYNCHRONIZE, FALSE, 0)) return lastFailure();
    // Shell/COM can create helper threads which inherit this exact connection
    // and outlive our worker. Use the creator's retained connection, never the
    // independently owned keepalive: that duplicate must remain closeable.
    if (!SetThreadDesktop(impl_->creatorConnection)) return lastFailure();
    impl_->previous = original;
    impl_->attachedThread = GetCurrentThreadId();
    impl_->terminationRequired = true;
    return S_OK;
}

HRESULT StaWorkerLease::deferCreatorRelease(std::shared_ptr<void>& keepalive) noexcept {
    if (!impl_ || !impl_->completion || !keepalive) return E_INVALIDARG;
    if (GetCurrentThreadId() == impl_->creatorThread ||
        (impl_->attachedThread && impl_->attachedThread != GetCurrentThreadId())) return RPC_E_WRONG_THREAD;
    if (impl_->completion->creatorRelease) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
    // Reserve ownership before any fallible kernel operation. A failure must
    // retain this value, never destroy native registrations on a non-COM worker.
    impl_->completion->creatorRelease = std::move(keepalive);
    if (!impl_->thread && !DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
        &impl_->thread, SYNCHRONIZE, FALSE, 0)) {
        const auto failed = lastFailure();
        impl_->retainOnFailure = true;
        std::lock_guard lock(groupsMutex);
        if (SUCCEEDED(impl_->group->cleanup)) impl_->group->cleanup = failed;
        return failed;
    }
    impl_->terminationRequired = true;
    return S_OK;
}

HRESULT StaWorkerLease::finish() noexcept {
    if (!impl_) return S_FALSE;
    if (impl_->retainOnFailure && impl_->completion && impl_->completion->creatorRelease) {
        HRESULT failed;
        { std::lock_guard lock(groupsMutex); failed = impl_->group->cleanup; }
        impl_.reset(); // Retains the complete payload in the failed creator group.
        return FAILED(failed) ? failed : E_UNEXPECTED;
    }
    const auto deferUntilThreadExit = [&](HRESULT cause) noexcept -> HRESULT {
        auto deferred = std::move(impl_->completion);
        if (!deferred) return E_UNEXPECTED;
        deferred->desktop = impl_->desktop; deferred->thread = impl_->thread;
        deferred->group = impl_->group;
        if (!RegisterWaitForSingleObject(&deferred->wait, deferred->thread,
            releaseTerminatedDesktop, deferred.get(), INFINITE, WT_EXECUTEONLYONCE)) {
            const auto failed = lastFailure();
            impl_->completion = std::move(deferred);
            impl_->retainOnFailure = true;
            std::lock_guard lock(groupsMutex);
            if (SUCCEEDED(impl_->group->cleanup)) impl_->group->cleanup = failed;
            return failed;
        }
        impl_->desktop = nullptr; impl_->thread = nullptr;
        impl_->attachedThread = 0; impl_->counted = false;
        deferred.release(); impl_.reset();
        return cause;
    };
    if (impl_->attachedThread) {
        if (impl_->attachedThread != GetCurrentThreadId()) return RPC_E_WRONG_THREAD;
        if (!SetThreadDesktop(impl_->previous)) {
            const auto restore = lastFailure();
            return deferUntilThreadExit(restore);
        }
        impl_->attachedThread = 0;
        // Finishing COM is not kernel-thread termination. Keep the shutdown
        // reservation until the exact worker has exited, then reap our unused
        // duplicate on the creator STA. Borrowed connections are never closed.
        return deferUntilThreadExit(S_OK);
    }
    if (impl_->terminationRequired) return deferUntilThreadExit(S_OK);
    HRESULT hr = S_OK;
    if (impl_->desktop) {
        if (!CloseDesktop(impl_->desktop)) {
            hr = lastFailure();
        }
        else impl_->desktop = nullptr;
    }
    impl_.reset();
    return hr;
}

unsigned pendingStaWorkers() noexcept {
    std::lock_guard lock(groupsMutex);
    const auto group = currentGroup();
    return group ? group->active : 0;
}
HRESULT staWorkerDiagnostics(StaWorkerDiagnostics* result) noexcept {
    if (!result) return E_POINTER;
    try {
        StaWorkerDiagnostics value;
        std::vector<HDESK> desktops;
        {
            std::lock_guard lock(groupsMutex);
            const auto group = currentGroup();
            if (!group) { *result = value; return S_OK; }
            value.pending = group->active;
            for (auto node = group->completed; node; node = node->next) desktops.push_back(node->desktop);
            value.completedThreads = static_cast<unsigned>(desktops.size());
        }
        const auto creator = GetCurrentThreadId();
        const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return lastFailure();
        THREADENTRY32 entry{sizeof(entry)};
        if (Thread32First(snapshot, &entry)) do {
            if (entry.th32OwnerProcessID != GetCurrentProcessId()) continue;
            const auto borrowed = GetThreadDesktop(entry.th32ThreadID);
            if (std::find(desktops.begin(), desktops.end(), borrowed) == desktops.end()) continue;
            if (entry.th32ThreadID == creator) ++value.creatorHandleUsers; else ++value.otherHandleUsers;
        } while (Thread32Next(snapshot, &entry));
        CloseHandle(snapshot);
        struct WindowCounts { StaWorkerDiagnostics* value; DWORD creator; } counts{&value, creator};
        for (const auto desktop : desktops) EnumDesktopWindows(desktop, [](HWND window, LPARAM context) -> BOOL {
            auto& counts = *reinterpret_cast<WindowCounts*>(context);
            DWORD process = 0; const auto thread = GetWindowThreadProcessId(window, &process);
            if (process != GetCurrentProcessId()) return TRUE;
            ++counts.value->desktopWindows;
            if (thread == counts.creator) ++counts.value->creatorWindows; else ++counts.value->otherWindows;
            return TRUE;
        }, reinterpret_cast<LPARAM>(&counts));
        *result = value;
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
      catch (...) { return E_FAIL; }
}

HRESULT drainStaWorkers(DWORD timeoutMilliseconds) noexcept {
    APTTYPE apartment{}; APTTYPEQUALIFIER qualifier{};
    auto hr = CoGetApartmentType(&apartment, &qualifier);
    if (FAILED(hr)) return hr;
    if (apartment != APTTYPE_STA && apartment != APTTYPE_MAINSTA) return RPC_E_WRONG_THREAD;
    std::shared_ptr<WorkerGroup> group;
    {
        std::lock_guard lock(groupsMutex);
        group = currentGroup();
        if (!group) return S_OK;
        if (group->creatorConnection != GetThreadDesktop(GetCurrentThreadId()))
            return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
    }
    const auto deadline = GetTickCount64() + timeoutMilliseconds;
    for (;;) {
        hr = reapCompleted(group);
        if (FAILED(hr)) return hr;
        {
            std::lock_guard lock(groupsMutex);
            if (!group->active) {
                creatorGroup.reset();
                return group->cleanup;
            }
        }
        const auto now = GetTickCount64();
        if (now >= deadline) return RPC_S_CALLPENDING;
        DWORD signaled = 0;
        const auto remaining = static_cast<DWORD>(std::min<ULONGLONG>(deadline - now, 10));
        hr = CoWaitForMultipleHandles(COWAIT_DISPATCH_CALLS | COWAIT_DISPATCH_WINDOW_MESSAGES,
            remaining, 1, &group->idle, &signaled);
        if (FAILED(hr) && hr != RPC_S_CALLPENDING) return hr;
    }
}
} // namespace explorer
