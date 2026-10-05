#include "explorer/worker_sta.hpp"
#include "explorer/headless_visual.hpp"
#include <objbase.h>
#include <wrl.h>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using Microsoft::WRL::ComPtr;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void succeeded(HRESULT hr, const char* message) { if (FAILED(hr)) throw std::runtime_error(std::string(message) + " HRESULT=" + std::to_string(static_cast<unsigned long>(hr))); }
[[noreturn]] void stopUnfinishedFixture(const char* phase, HRESULT status) noexcept {
    // Only this headless test process is stopped. Native references and desktop
    // connections must not unwind while a provider or creator callback is live.
    std::cerr << "FAIL: unfinished private worker fixture: " << phase
        << "; HRESULT=" << static_cast<unsigned long>(status) << '\n';
    std::cerr.flush();
    if (!TerminateProcess(GetCurrentProcess(), 10)) std::_Exit(10);
    std::_Exit(10);
}
struct Handle { HANDLE value = nullptr; ~Handle() { if (value) CloseHandle(value); } };
std::wstring desktopName() {
    DWORD bytes = 0; const auto desktop = GetThreadDesktop(GetCurrentThreadId());
    GetUserObjectInformationW(desktop, UOI_NAME, nullptr, 0, &bytes);
    require(bytes >= sizeof(wchar_t) && bytes <= 65536, "Desktop name size");
    std::wstring name(bytes / sizeof(wchar_t), L'\0');
    require(GetUserObjectInformationW(desktop, UOI_NAME, name.data(), bytes, &bytes) != FALSE, "Read exact desktop name");
    name.resize(wcslen(name.c_str())); return name;
}
class Callback final : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IPersist> {
public:
    DWORD owner = GetCurrentThreadId();
    std::atomic<unsigned> calls{0};
    HRESULT STDMETHODCALLTYPE GetClassID(CLSID* result) override {
        if (!result) return E_POINTER;
        if (GetCurrentThreadId() != owner) return RPC_E_WRONG_THREAD;
        *result = CLSID_StdGlobalInterfaceTable;
        ++calls; return S_OK;
    }
};
struct CreatorReleaseProbe {
    ComPtr<IGlobalInterfaceTable> git;
    DWORD cookie;
    std::atomic<DWORD>& releasedThread;
    std::atomic<HRESULT>& apartmentStatus;
    std::atomic<HRESULT>& revokeStatus;
    std::atomic<unsigned>& pendingAtRelease;
    CreatorReleaseProbe(IGlobalInterfaceTable* table, DWORD value, std::atomic<DWORD>& thread,
        std::atomic<HRESULT>& apartment, std::atomic<HRESULT>& revoked, std::atomic<unsigned>& pending)
        : git(table), cookie(value), releasedThread(thread), apartmentStatus(apartment),
          revokeStatus(revoked), pendingAtRelease(pending) {}
    ~CreatorReleaseProbe() {
        releasedThread = GetCurrentThreadId();
        APTTYPE apartment{}; APTTYPEQUALIFIER qualifier{};
        const auto hr = CoGetApartmentType(&apartment, &qualifier);
        apartmentStatus = SUCCEEDED(hr) && apartment != APTTYPE_STA && apartment != APTTYPE_MAINSTA ? RPC_E_WRONG_THREAD : hr;
        // Reenter real worker bookkeeping to prove cleanup holds no group lock.
        pendingAtRelease = explorer::pendingStaWorkers();
        revokeStatus = git->RevokeInterfaceFromGlobal(cookie);
    }
};
void privateLifecycle() {
    explorer::PrivateDesktop desktop;
    succeeded(desktop.initialize(), "Attach creator to private desktop before COM");
    succeeded(OleInitialize(nullptr), "Initialize creator STA");
    try {
        const auto clipboard = GetClipboardSequenceNumber();
        require(explorer::pendingStaWorkers() == 0, "Creator started with an unrelated worker");
        require(explorer::StaWorkerLease::prepare(nullptr) == E_POINTER, "Null worker output");
        {
            std::unique_ptr<explorer::StaWorkerLease> abandoned;
            succeeded(explorer::StaWorkerLease::prepare(&abandoned), "Prepare cancelled-before-launch lease");
            require(explorer::pendingStaWorkers() == 1, "Prelaunch lease must keep desktop/shutdown alive");
            require(explorer::drainStaWorkers(0) == RPC_S_CALLPENDING, "Unstarted work must not report a completed drain");
        }
        require(explorer::pendingStaWorkers() == 0, "Unstarted lease leaked its shutdown reservation");
        auto callback = Microsoft::WRL::Make<Callback>();
        require(callback != nullptr, "Create non-agile creator STA callback");
        ComPtr<IGlobalInterfaceTable> git;
        succeeded(CoCreateInstance(CLSID_StdGlobalInterfaceTable, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&git)), "Creator GIT");
        DWORD cookie = 0;
        succeeded(git->RegisterInterfaceInGlobal(callback.Get(), IID_IPersist, &cookie), "Marshal actual creator STA callback");
        DWORD deferredCookie = 0;
        succeeded(git->RegisterInterfaceInGlobal(callback.Get(), IID_IPersist, &deferredCookie), "Register deferred actual STA callback");
        std::atomic<DWORD> releasedThread{0};
        std::atomic<HRESULT> releaseApartment{E_PENDING}, revokeStatus{E_PENDING};
        std::atomic<unsigned> pendingAtRelease{0};
        std::atomic<bool> workerInitializationBalanced{false}, payloadConsumed{false};
        std::atomic<HRESULT> workerApartment{E_PENDING}, deferredStatus{E_PENDING};
        std::atomic<APTTYPEQUALIFIER> workerQualifier{APTTYPEQUALIFIER_NONE};
        std::shared_ptr<void> payload = std::make_shared<CreatorReleaseProbe>(git.Get(), deferredCookie,
            releasedThread, releaseApartment, revokeStatus, pendingAtRelease);
        const std::weak_ptr<void> deferredPayload = payload;
        Handle start{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        Handle finished{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        Handle allowExit{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        require(start.value && finished.value && allowExit.value, "Owned lifecycle events");
        std::unique_ptr<explorer::StaWorkerLease> lease;
        succeeded(explorer::StaWorkerLease::prepare(&lease), "Prepare owned desktop before worker starts");
        require(lease->deferCreatorRelease(payload) == RPC_E_WRONG_THREAD && payload,
            "Creator-side deferral must preserve the input on validation failure");
        const auto creatorThread = GetCurrentThreadId();
        const auto expectedDesktop = desktop.name();
        const auto creatorConnection = GetThreadDesktop(GetCurrentThreadId());
        std::atomic<HRESULT> status{E_PENDING};
        std::atomic<bool> ownVisibleWindow{false};
        const auto startEvent = start.value;
        const auto finishEvent = finished.value, exitEvent = allowExit.value;
        std::thread worker([owned = std::move(lease), payload = std::move(payload), cookie, startEvent, finishEvent, exitEvent,
                           expectedDesktop, creatorConnection, &status, &ownVisibleWindow,
                           &workerInitializationBalanced, &payloadConsumed, &workerApartment, &deferredStatus, &workerQualifier]() mutable {
            APTTYPE initialApartment{}; APTTYPEQUALIFIER initialQualifier{};
            const auto initialApartmentRead = CoGetApartmentType(&initialApartment, &initialQualifier);
            HRESULT final = WaitForSingleObject(startEvent, 2000) == WAIT_OBJECT_0 ? owned->attach() : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            bool initialized = false;
            try {
                if (SUCCEEDED(final)) {
                    require(desktopName() == expectedDesktop, "Worker attached to a different desktop");
                    require(GetThreadDesktop(GetCurrentThreadId()) == creatorConnection,
                        "Worker must use retained creator connection, never its owned duplicate");
                    final = OleInitialize(nullptr); initialized = SUCCEEDED(final);
                }
                if (SUCCEEDED(final)) {
                    ComPtr<IGlobalInterfaceTable> workerGit; ComPtr<IPersist> creator;
                    final = CoCreateInstance(CLSID_StdGlobalInterfaceTable, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&workerGit));
                    if (SUCCEEDED(final)) final = workerGit->GetInterfaceFromGlobal(cookie, IID_PPV_ARGS(&creator));
                    CLSID reply{};
                    if (SUCCEEDED(final)) final = creator->GetClassID(&reply);
                    if (SUCCEEDED(final) && !IsEqualGUID(reply, CLSID_StdGlobalInterfaceTable)) final = E_UNEXPECTED;
                    if (SUCCEEDED(final)) {
                        // Visible only on the never-switched, exact private desktop.
                        const auto window = CreateWindowExW(0, L"STATIC", L"Owned worker fixture", WS_POPUP | WS_VISIBLE,
                            0, 0, 80, 24, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
                        if (!window) final = HRESULT_FROM_WIN32(GetLastError());
                        else { ownVisibleWindow = IsWindowVisible(window) != FALSE; DestroyWindow(window); }
                    }
                }
            } catch (...) { final = E_FAIL; }
            if (initialized) OleUninitialize();
            APTTYPE apartment{}; APTTYPEQUALIFIER qualifier{};
            workerApartment = CoGetApartmentType(&apartment, &qualifier);
            workerQualifier = qualifier;
            // An MTA may appear or disappear elsewhere in the process while
            // native COM runs. Neither permissible implicit-MTA state means
            // this thread retains its own explicit STA initialization.
            const auto noExplicitApartment = [](HRESULT read, APTTYPE type, APTTYPEQUALIFIER kind) {
                return read == CO_E_NOTINITIALIZED ||
                    (SUCCEEDED(read) && type == APTTYPE_MTA && kind == APTTYPEQUALIFIER_IMPLICIT_MTA);
            };
            workerInitializationBalanced = noExplicitApartment(initialApartmentRead, initialApartment, initialQualifier) &&
                noExplicitApartment(workerApartment.load(), apartment, qualifier);
            const auto deferred = owned->deferCreatorRelease(payload);
            deferredStatus = deferred;
            payloadConsumed = !payload;
            if (SUCCEEDED(final) && FAILED(deferred)) final = deferred;
            const auto finished = owned->finish();
            if (SUCCEEDED(final) && FAILED(finished)) final = finished;
            status = final;
            SetEvent(finishEvent);
            // Prove that releasing providers/COM and finish() are distinct
            // from kernel termination. The creator controls this owned wait.
            if (WaitForSingleObject(exitEvent, 5000) != WAIT_OBJECT_0)
                status = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        });
        // Simulate deletion of the task object before its thread enters COM.
        require(lease == nullptr, "Creator still owns worker desktop lease");
        SetEvent(start.value);
        DWORD finishSignal = 0;
        const auto finishRead = CoWaitForMultipleHandles(COWAIT_DISPATCH_CALLS | COWAIT_DISPATCH_WINDOW_MESSAGES,
            5000, 1, &finished.value, &finishSignal);
        const auto beforeExit = explorer::drainStaWorkers(0);
        const auto heldCount = explorer::pendingStaWorkers();
        const bool payloadHeldBeforeExit = !deferredPayload.expired() && releasedThread.load() == 0;
        SetEvent(allowExit.value); // Release even when an assertion below fails.
        const auto drained = explorer::drainStaWorkers(5000);
        const auto workerExited = WaitForSingleObject(worker.native_handle(), 0);
        if (workerExited != WAIT_OBJECT_0)
            stopUnfinishedFixture("worker kernel termination", workerExited == WAIT_FAILED
                ? HRESULT_FROM_WIN32(GetLastError()) : HRESULT_FROM_WIN32(ERROR_TIMEOUT));
        worker.join();
        if (FAILED(drained)) stopUnfinishedFixture("creator cleanup drain", drained);
        succeeded(finishRead, "Dispatch creator callbacks until worker finish");
        require(beforeExit == RPC_S_CALLPENDING && heldCount == 1,
            "Finished worker released shutdown reservation before actual kernel exit");
        std::cout << "Owned creator release: payloadHeld=" << payloadHeldBeforeExit
            << "; apartmentHRESULT=" << static_cast<unsigned long>(workerApartment.load())
            << "; apartmentQualifier=" << static_cast<unsigned>(workerQualifier.load())
            << "; deferHRESULT=" << static_cast<unsigned long>(deferredStatus.load())
            << "; consumed=" << payloadConsumed.load() << "; releasedThread=" << releasedThread.load()
            << "; creatorThread=" << creatorThread << '\n';
        require(payloadHeldBeforeExit && workerInitializationBalanced.load() && payloadConsumed.load(),
            "Worker with balanced initialization must defer its complete native payload until actual exit");
        require(deferredPayload.expired() && releasedThread.load() == creatorThread &&
            SUCCEEDED(releaseApartment.load()) && SUCCEEDED(revokeStatus.load()) && pendingAtRelease.load() == 1,
            "Native registration must release on initialized creator STA outside the group lock before completed drain");
        std::cout << "Owned worker drain: HRESULT=" << static_cast<unsigned long>(drained)
            << "; worker=" << static_cast<unsigned long>(status.load())
            << "; creator callbacks=" << callback->calls.load()
            << "; pending=" << explorer::pendingStaWorkers() << '\n';
        succeeded(drained, "Shutdown drain must dispatch actual creator STA COM call");
        succeeded(status.load(), "Worker attachment, native painting and restoration");
        std::cout << "Owned worker lifecycle: creator callbacks=" << callback->calls.load()
            << "; actual visible private window=" << ownVisibleWindow.load()
            << "; pending=" << explorer::pendingStaWorkers() << '\n';
        require(callback->calls == 1 && ownVisibleWindow && explorer::pendingStaWorkers() == 0,
            "Worker callback/real private window/completion proof");
        succeeded(git->RevokeInterfaceFromGlobal(cookie), "Revoke completed creator callback");
        bool unchanged = false, visible = true;
        succeeded(desktop.verifyIsolation(&unchanged), "Desktop isolation after actual worker shutdown");
        succeeded(desktop.visibleWindowsOnInputDesktop(visible), "Observe process visibility after worker shutdown");
        require(unchanged && !visible && GetClipboardSequenceNumber() == clipboard, "Worker changed desktop/clipboard state");
        require(explorer::drainStaWorkers(0) == S_OK, "Completed shutdown drain");
    } catch (...) {
        const auto drained = explorer::drainStaWorkers(5000);
        if (FAILED(drained)) stopUnfinishedFixture("exception cleanup drain", drained);
        OleUninitialize(); throw;
    }
    OleUninitialize();
}
}
int runStaWorkerTests() {
    // Match the complete suite: the creator is a secondary STA after a main
    // STA already exists. A first-STA-only test misses native teardown timing.
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) {
        std::cerr << "FAIL: initialize worker-test parent STA\n";
        return 1;
    }
    int failures = 0;
    std::thread owner([&] {
        try { privateLifecycle(); }
        catch (const std::exception& error) { failures = 1; std::cerr << "FAIL: owned worker STA lifecycle: " << error.what() << '\n'; }
    });
    // Previous native Shell fixtures can leave the main STA serving process
    // OLE/TSF objects. Pump their real callbacks during secondary-STA teardown;
    // blocking join alone can prevent the desktop's native users from exiting.
    HANDLE ownerThread = owner.native_handle();
    DWORD signaled = 0;
    const auto pumped = CoWaitForMultipleHandles(COWAIT_DISPATCH_CALLS | COWAIT_DISPATCH_WINDOW_MESSAGES,
        15000, 1, &ownerThread, &signaled);
    const auto ownerExited = WaitForSingleObject(ownerThread, 0);
    if (ownerExited != WAIT_OBJECT_0)
        stopUnfinishedFixture("parent STA owner termination", ownerExited == WAIT_FAILED
            ? HRESULT_FROM_WIN32(GetLastError()) : HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    owner.join();
    if (FAILED(pumped)) {
        ++failures;
        std::cerr << "FAIL: worker-test parent STA pump HRESULT="
            << static_cast<unsigned long>(pumped) << '\n';
    }
    CoUninitialize();
    if (!failures) std::cout << "PASS: delayed owned desktop worker and actual STA-dispatching shutdown drain\n";
    return failures;
}
