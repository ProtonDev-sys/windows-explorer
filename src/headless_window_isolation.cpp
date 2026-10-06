#include "explorer/headless_window_isolation.hpp"
#include "explorer/headless_visual.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <string_view>
namespace explorer {
namespace {
constexpr ULONG_PTR nullResultSentinel=static_cast<ULONG_PTR>(0xA5B6C7D8E9FA1023ULL);
using NullReceipt=WindowMessageReceipt;
using ControlPacket=WindowControlPacket;
struct Failure {HRESULT value;};
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    ~Handle(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    Handle()=default;Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
};
HRESULT win32() noexcept {const auto error=GetLastError();return HRESULT_FROM_WIN32(error?error:ERROR_GEN_FAILURE);}
std::wstring desktopName(HDESK desktop) {
    std::array<wchar_t,256> name{};DWORD bytes=0;
    if(!desktop||!GetUserObjectInformationW(desktop,UOI_NAME,name.data(),static_cast<DWORD>(sizeof(name)),&bytes)||
       !name.front()||name.back())return {};
    return name.data();
}
bool sameDesktop(HDESK desktop,const std::wstring& expected){return desktopName(desktop)==expected;}
DWORD stageCode(const char* name) noexcept {
    DWORD value=2166136261u;
    for(;*name;++name){value^=static_cast<unsigned char>(*name);value*=16777619u;}
    return value;
}
struct Report {
    WindowIsolationControlReport& output;
    ULONGLONG started=GetTickCount64(),deadline=started+4500;
    unsigned index=0;
    explicit Report(WindowIsolationControlReport& value):output(value){}
    WindowControlReadback& group() noexcept{return output.controls[index];}
    void check() const {if(GetTickCount64()>=deadline)throw Failure{HRESULT_FROM_WIN32(ERROR_TIMEOUT)};}
    void phase(const char* name){check();index=std::strcmp(name,"owned-different-private-process-control")==0?1u:0u;}
    HRESULT record(const char* name,HRESULT hr,std::uint64_t fact=0) noexcept {
        if(output.stepCount>=output.steps.size()){output.overflow=true;return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);}
        output.steps[output.stepCount++]={stageCode(name),hr,fact,GetTickCount64()-started};
        return hr;
    }
    void require(const char* name,HRESULT hr,std::uint64_t fact=0) {
        const auto recorded=record(name,hr,fact);
        if(FAILED(recorded))throw Failure{recorded};check();
    }
    HRESULT finish(HRESULT hr) noexcept {
        output.result=hr;
        std::fprintf(stderr,"headless-window-isolation terminal HR=0x%08lX steps=%lu overflow=%u\n",
            static_cast<unsigned long>(hr),static_cast<unsigned long>(output.stepCount),static_cast<unsigned>(output.overflow));
        std::fflush(stderr);return S_OK;
    }
};
HRESULT cleanupReceipt(Report* report,const char* stage,HRESULT hr) noexcept {
    return report?report->record(stage,hr):hr;
}
thread_local std::uint64_t lastCalibration=0;
thread_local HDESK calibratedDesktop=nullptr;
constexpr DWORD controlMagic = 0x50564D36;
constexpr wchar_t controlClass[] = L"WindowsExplorer.PreviewMessageControl";
NullReceipt sendNull(HWND window, ULONGLONG deadline) noexcept {
    NullReceipt result;
    const auto now = GetTickCount64();
    if (now >= deadline) { result.error = ERROR_TIMEOUT; return result; }
    result.timeout = static_cast<UINT>(std::min<ULONGLONG>(100, deadline - now));
    SetLastError(ERROR_SUCCESS);
    result.call = SendMessageTimeoutW(window, WM_NULL, 0, 0,
        SMTO_BLOCK | SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, result.timeout, &result.result);
    result.error = GetLastError();
    return result;
}
void traceNull(const char* phase, HWND window, const NullReceipt& receipt) noexcept {
    std::fprintf(stderr, "headless-window-isolation message-channel phase=%s HWND=%p call=%lld error=%lu "
        "result=%llu sentinelChanged=%u timeoutMs=%u delivered=%u\n", phase, static_cast<void*>(window),
        static_cast<long long>(receipt.call), static_cast<unsigned long>(receipt.error),
        static_cast<unsigned long long>(receipt.result), static_cast<unsigned>(receipt.result != nullResultSentinel),
        receipt.timeout, static_cast<unsigned>(receipt.delivered())); std::fflush(stderr);
}
struct ControlWindowContext { DWORD nullCount = 0; };
LRESULT CALLBACK controlWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    if (message == WM_NCCREATE) {
        const auto creation = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(creation->lpCreateParams));
    }
    auto context = reinterpret_cast<ControlWindowContext*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NULL && context) { ++context->nullCount; return 0; }
    return DefWindowProcW(window, message, wparam, lparam);
}
bool decimal(const wchar_t* text, uint64_t& value) noexcept {
    value = 0; if (!text || !*text) return false;
    for (; *text; ++text) {
        if (*text < L'0' || *text > L'9') return false;
        const auto digit = static_cast<unsigned>(*text - L'0');
        if (value > (UINT64_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    return true;
}
int messageControlChild(wchar_t* const* arguments) noexcept {
    try {
        const bool different = std::wstring(arguments[2]) == L"different";
        if (!different && std::wstring(arguments[2]) != L"same") return 2;
        uint64_t parentValue = 0, pipeValue = 0, stopValue = 0, exitValue = 0;
        if (!decimal(arguments[3], parentValue) || !parentValue || parentValue > MAXDWORD ||
            !decimal(arguments[4], pipeValue) || !pipeValue || pipeValue > UINTPTR_MAX ||
            !decimal(arguments[5], stopValue) || !stopValue || stopValue > UINTPTR_MAX ||
            !decimal(arguments[6], exitValue) || !exitValue || exitValue > UINTPTR_MAX) return 2;
        const auto parentProcess = static_cast<DWORD>(parentValue);
        const std::wstring expected(arguments[7]);
        const auto prefix = L"WindowsExplorer.Visual." + std::to_wstring(parentProcess) + L".{";
        if (!expected.starts_with(prefix) || expected.back() != L'}' || expected.size() >= 256 ||
            expected.find_first_of(L"\\/\"\r\n") != std::wstring::npos || parentProcess == GetCurrentProcessId()) return 2;
        Handle pipe, stop, release, parent;
        pipe.value = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(pipeValue));
        stop.value = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(stopValue));
        release.value = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(exitValue));
        if (pipe.value == INVALID_HANDLE_VALUE || stop.value == INVALID_HANDLE_VALUE || release.value == INVALID_HANDLE_VALUE ||
            pipe.value == stop.value || pipe.value == release.value || stop.value == release.value) return 2;
        parent.value = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parentProcess);
        if (!parent.value || GetProcessId(parent.value) != parentProcess || WaitForSingleObject(parent.value, 0) != WAIT_TIMEOUT ||
            !sameDesktop(GetThreadDesktop(GetCurrentThreadId()), expected)) return 3;
        // The parent explicitly places both children on its owned private
        // startup desktop. Only the negative control creates a new private one.
        explorer::PrivateDesktop separate;
        if (different && (FAILED(separate.initialize()) || FAILED(separate.verifyIsolation()) || separate.name() == expected)) return 3;
        const auto actualDesktop = desktopName(GetThreadDesktop(GetCurrentThreadId()));
        if (actualDesktop.empty() || (different ? actualDesktop == expected : actualDesktop != expected)) return 3;
        ControlWindowContext context;
        WNDCLASSW type{}; type.lpfnWndProc = controlWindowProc; type.hInstance = GetModuleHandleW(nullptr); type.lpszClassName = controlClass;
        if (!RegisterClassW(&type)) return 4;
        struct WindowLifetime {
            HWND window = nullptr; HINSTANCE instance; bool registered = true;
            ~WindowLifetime() { if (window) DestroyWindow(window); if (registered) UnregisterClassW(controlClass, instance); }
        } lifetime{nullptr, type.hInstance, true};
        HWND window = CreateWindowExW(0, controlClass, L"", WS_OVERLAPPED, 0, 0, 32, 32,
            nullptr, nullptr, type.hInstance, &context);
        if (!window) return 4;
        lifetime.window = window;
        const auto deadline = GetTickCount64() + 4500;
        const auto packet = [&](DWORD stage) {
            ControlPacket result; result.stage = stage; result.different = different ? 1 : 0;
            result.parentProcess = parentProcess; result.process = GetCurrentProcessId(); result.thread = GetCurrentThreadId();
            result.window = reinterpret_cast<ULONG_PTR>(window); result.observedBeforeLocal = context.nullCount;
            DWORD process = 0; const auto thread = GetWindowThreadProcessId(window, &process);
            const auto name = desktopName(GetThreadDesktop(result.thread));
            const auto classRead = GetClassNameW(window, result.type.data(), static_cast<int>(result.type.size()));
            if (name.size() < result.desktop.size()) std::copy(name.begin(), name.end(), result.desktop.begin());
            result.local = sendNull(window, deadline); result.nullCount = context.nullCount;
            result.status = process == result.process && thread == result.thread && IsWindow(window) &&
                classRead > 0 && std::wstring_view(result.type.data()) == controlClass && name == actualDesktop &&
                result.local.delivered() && result.nullCount == result.observedBeforeLocal + 1 &&
                (!different || SUCCEEDED(separate.verifyIsolation())) ? S_OK : E_FAIL;
            return result;
        };
        const auto write = [&](const ControlPacket& value) noexcept {
            DWORD written = 0;
            return WriteFile(pipe.value, &value, static_cast<DWORD>(sizeof(value)), &written, nullptr) && written == sizeof(value);
        };
        const auto initial = packet(1); bool okay = initial.status == S_OK && write(initial);
        bool stopped = false;
        const std::array<HANDLE, 2> waits{stop.value, parent.value};
        while (okay && GetTickCount64() < deadline) {
            const auto result = MsgWaitForMultipleObjectsEx(static_cast<DWORD>(waits.size()), waits.data(), 20,
                QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (result == WAIT_OBJECT_0) { stopped = true; break; }
            if (result == WAIT_OBJECT_0 + 1 || result == WAIT_FAILED) { okay = false; break; }
            MSG message{}; unsigned count = 0;
            while (count++ < 64 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message); DispatchMessageW(&message);
            }
        }
        const auto final = packet(2);
        okay = okay && stopped && final.status == S_OK && write(final);
        // Keep the actual HWND responsive and unchanged until the parent has
        // checked the final native identity and explicitly permits teardown.
        bool released = false;
        const std::array<HANDLE, 2> finalWaits{release.value, parent.value};
        while (okay && GetTickCount64() < deadline) {
            const auto result = MsgWaitForMultipleObjectsEx(static_cast<DWORD>(finalWaits.size()), finalWaits.data(), 20,
                QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (result == WAIT_OBJECT_0) { released = true; break; }
            if (result == WAIT_OBJECT_0 + 1 || result == WAIT_FAILED) { okay = false; break; }
            MSG message{}; unsigned count = 0;
            while (count++ < 64 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message); DispatchMessageW(&message);
            }
        }
        okay = okay && released;
        if (!DestroyWindow(window)) okay = false;
        else lifetime.window = nullptr;
        if (!UnregisterClassW(controlClass, type.hInstance)) okay = false;
        else lifetime.registered = false;
        if (different && FAILED(separate.verifyIsolation())) okay = false;
        return okay ? 0 : 5;
    } catch (...) { return 6; }
}

struct OwnedControlProcess {
    Handle process, thread, readPipe, writePipe, stop, release;
    DWORD processId = 0, primaryThreadId = 0;
    Report* report = nullptr;
    bool joined = false;
    ~OwnedControlProcess() { close(); }
    bool live() const noexcept {
        return process.value && thread.value && processId && primaryThreadId &&
            GetProcessId(process.value) == processId && GetThreadId(thread.value) == primaryThreadId &&
            GetProcessIdOfThread(thread.value) == processId && WaitForSingleObject(process.value, 0) == WAIT_TIMEOUT &&
            WaitForSingleObject(thread.value, 0) == WAIT_TIMEOUT;
    }
    bool matchesLive(const ControlPacket& packet) const noexcept {
        return packet.process == processId && packet.thread == primaryThreadId && live();
    }
    void requireLive(const char* stage, const ControlPacket& packet, Report& output) const {
        const auto actualProcess = GetProcessId(process.value), actualThread = GetThreadId(thread.value);
        const auto threadProcess = GetProcessIdOfThread(thread.value);
        const auto processWait = WaitForSingleObject(process.value, 0), threadWait = WaitForSingleObject(thread.value, 0);
        const bool exact = actualProcess == processId && actualThread == primaryThreadId && threadProcess == processId &&
            packet.process == actualProcess && packet.thread == actualThread && processWait == WAIT_TIMEOUT && threadWait == WAIT_TIMEOUT;
        auto& group=output.group();
        if(group.kernelCount>=group.kernel.size())throw Failure{HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)};
        group.kernel[group.kernelCount++]={actualProcess,actualThread,threadProcess,processWait,threadWait,exact};
        std::fprintf(stderr, "headless-window-isolation retained-control stage=%s createdPID=%lu createdTID=%lu "
            "kernelPID=%lu kernelTID=%lu kernelThreadPID=%lu processWait=%lu threadWait=%lu packetPID=%lu packetTID=%lu exact=%u\n",
            stage, static_cast<unsigned long>(processId), static_cast<unsigned long>(primaryThreadId),
            static_cast<unsigned long>(actualProcess), static_cast<unsigned long>(actualThread), static_cast<unsigned long>(threadProcess),
            static_cast<unsigned long>(processWait), static_cast<unsigned long>(threadWait), static_cast<unsigned long>(packet.process),
            static_cast<unsigned long>(packet.thread), static_cast<unsigned>(exact)); std::fflush(stderr);
        output.require(stage, exact ? S_OK : E_ACCESSDENIED);
    }
    HRESULT close() noexcept {
        if (!process.value || process.value == INVALID_HANDLE_VALUE || joined) return S_OK;
        HRESULT result = S_OK;
        if (stop.value && stop.value != INVALID_HANDLE_VALUE && !SetEvent(stop.value)) result = win32();
        if (release.value && release.value != INVALID_HANDLE_VALUE && !SetEvent(release.value)) result = win32();
        auto wait = WaitForSingleObject(process.value, 1000);
        if (wait != WAIT_OBJECT_0) {
            result = wait == WAIT_FAILED ? win32() : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            // Only the retained process directly created by this headless test is
            // eligible for failed-drain termination. Shared servers are untouched.
            if (GetProcessId(process.value) == processId) {
                const BOOL terminated = TerminateProcess(process.value, 9);
                const auto terminatedStatus=terminated?S_OK:win32();
                if(report)report->group().termination=terminatedStatus;
                cleanupReceipt(report, "owned-control-failed-drain-termination", terminatedStatus);
                if (terminated) wait = WaitForSingleObject(process.value, 1000);
            }
        }
        joined = wait == WAIT_OBJECT_0;
        if(report){report->group().kernelExited=joined;report->group().drain=joined?result:HRESULT_FROM_WIN32(ERROR_TIMEOUT);}
        const auto receipt = cleanupReceipt(report, "owned-control-process-kernel-drain", joined ? result : HRESULT_FROM_WIN32(ERROR_TIMEOUT));
        if (!joined) {
            // No parent stack/desktop teardown with a live retained child.
            // End only this owned test process; the raw failed drain is preserved.
            if (report) report->finish(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
            std::fflush(stderr); ExitProcess(9);
        }
        return FAILED(receipt) ? receipt : joined ? result : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
    HRESULT read(ControlPacket& packet, ULONGLONG deadline) noexcept {
        while (GetTickCount64() < deadline) {
            // Buffered bytes cannot stand in for a still-live owned process
            // and the exact primary thread that created this control HWND.
            if (!live()) return HRESULT_FROM_WIN32(ERROR_PROCESS_ABORTED);
            DWORD available = 0;
            if (!PeekNamedPipe(readPipe.value, nullptr, 0, nullptr, &available, nullptr)) return win32();
            if (available >= sizeof(packet)) {
                DWORD received = 0;
                return ReadFile(readPipe.value, &packet, static_cast<DWORD>(sizeof(packet)), &received, nullptr) && received == sizeof(packet) ? S_OK : win32();
            }
            const auto live = WaitForSingleObject(process.value, 0);
            if (live != WAIT_TIMEOUT) return live == WAIT_FAILED ? win32() : HRESULT_FROM_WIN32(ERROR_PROCESS_ABORTED);
            Sleep(1);
        }
        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
    void start(bool different, const std::wstring& desktop, Report& output) {
        report = &output;
        SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
        HANDLE read = nullptr, write = nullptr;
        const BOOL pipeCreated = CreatePipe(&read, &write, &inherit, 4096);
        readPipe.value = read; writePipe.value = write;
        output.require("owned-control-pipe-create", pipeCreated ? S_OK : win32());
        output.require("owned-control-read-pipe-not-inherited", SetHandleInformation(readPipe.value, HANDLE_FLAG_INHERIT, 0) ? S_OK : win32());
        stop.value = CreateEventW(&inherit, TRUE, FALSE, nullptr);
        output.require("owned-control-stop-event-create", stop.value ? S_OK : win32());
        release.value = CreateEventW(&inherit, TRUE, FALSE, nullptr);
        output.require("owned-control-release-event-create", release.value ? S_OK : win32());
        SIZE_T bytes = 0; InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        if (!bytes) throw Failure{win32()};
        std::unique_ptr<BYTE[]> storage(new BYTE[bytes]);
        auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.get());
        output.require("owned-control-handle-list-initialize", InitializeProcThreadAttributeList(attributes, 1, 0, &bytes) ? S_OK : win32());
        struct DeleteAttributes { LPPROC_THREAD_ATTRIBUTE_LIST value; ~DeleteAttributes() { DeleteProcThreadAttributeList(value); } } cleanup{attributes};
        std::array<HANDLE, 3> handles{writePipe.value, stop.value, release.value};
        output.require("owned-control-exact-inherited-handles", UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            handles.data(), sizeof(handles), nullptr, nullptr) ? S_OK : win32());
        std::array<wchar_t, 32768> executable{};
        const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        output.require("owned-control-executable", length && length < executable.size() ? S_OK : win32());
        std::wstring command = L"\"" + std::wstring(executable.data()) + L"\" --headless-window-isolation-control " +
            (different ? L"different " : L"same ") + std::to_wstring(GetCurrentProcessId()) + L" " +
            std::to_wstring(reinterpret_cast<uintptr_t>(writePipe.value)) + L" " +
            std::to_wstring(reinterpret_cast<uintptr_t>(stop.value)) + L" " +
            std::to_wstring(reinterpret_cast<uintptr_t>(release.value)) + L" \"" + desktop + L"\"";
        std::array<wchar_t, 256> station{}; DWORD stationBytes = 0;
        output.require("owned-control-current-window-station", GetUserObjectInformationW(GetProcessWindowStation(), UOI_NAME,
            station.data(), static_cast<DWORD>(sizeof(station)), &stationBytes) && station.front() ? S_OK : win32());
        std::wstring startupDesktop = std::wstring(station.data()) + L"\\" + desktop;
        STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.StartupInfo.lpDesktop = startupDesktop.data();
        startup.lpAttributeList = attributes;
        PROCESS_INFORMATION created{};
        const BOOL launched = CreateProcessW(executable.data(), command.data(), nullptr, nullptr, TRUE,
            EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, nullptr, &startup.StartupInfo, &created);
        const auto launchStatus = launched ? S_OK : win32();
        process.value = created.hProcess; thread.value = created.hThread;
        processId = created.dwProcessId; primaryThreadId = created.dwThreadId;
        output.group().createdProcess=processId;output.group().createdThread=primaryThreadId;
        output.require("owned-control-explicit-private-process-launch", launchStatus, processId);
        if (!live() || processId == GetCurrentProcessId())
            throw Failure{E_ACCESSDENIED};
        CloseHandle(writePipe.value); writePipe.value = nullptr;
    }
};
bool exactControlPacket(const ControlPacket& value, DWORD stage, bool different, DWORD parent, DWORD child) noexcept {
    return value.magic == controlMagic && value.version == 1 && value.stage == stage && value.different == (different ? 1U : 0U) &&
        value.parentProcess == parent && value.process == child && value.thread && value.window && value.status == S_OK &&
        value.desktop.back() == L'\0' && value.type.back() == L'\0' && std::wstring_view(value.type.data()) == controlClass;
}
bool verifyMessageControls(const explorer::PrivateDesktop& desktop, Report& report) {
    bool admitted = true;
    for (bool different : {false, true}) {
        report.phase(different ? "owned-different-private-process-control" : "owned-same-private-process-control");
        report.require("before-owned-control-parent-isolation", desktop.verifyIsolation());
        OwnedControlProcess child; child.start(different, desktop.name(), report);
        ControlPacket initial, final;
        report.require("owned-control-bounded-ready-packet", child.read(initial, std::min(report.deadline, GetTickCount64() + 2000)));
        report.group().initial=initial;
        child.requireLive("owned-control-ready-retained-kernel-identity", initial, report);
        HWND window = reinterpret_cast<HWND>(initial.window);
        DWORD process = 0;
        SetLastError(ERROR_SUCCESS);
        const auto thread = GetWindowThreadProcessId(window, &process); const auto threadError = GetLastError();
        std::array<wchar_t, 128> type{};
        SetLastError(ERROR_SUCCESS);
        const auto classRead = GetClassNameW(window, type.data(), static_cast<int>(type.size())); const auto classError = GetLastError();
        auto& group=report.group();group.readyProcess=process;group.readyThread=thread;
        group.readyThreadError=threadError;group.readyClassRead=classRead;group.readyClassError=classError;
        const bool packetExact = exactControlPacket(initial, 1, different, GetCurrentProcessId(), child.processId);
        const bool nativeProcessExact = process == child.processId, nativeThreadExact = thread == initial.thread;
        const bool nativeClassExact = classRead > 0 && std::wstring_view(type.data()) == controlClass;
        const bool relationExact = packetExact && (different ? std::wstring_view(initial.desktop.data()) != desktop.name() :
            std::wstring_view(initial.desktop.data()) == desktop.name());
        const bool localExact = initial.local.delivered() && initial.observedBeforeLocal == 0 && initial.nullCount == 1;
        std::fprintf(stderr, "headless-window-isolation owned-control-ready different=%u retainedPID=%lu HWND=%p "
            "packetMagic=%lu version=%lu stage=%lu parentPID=%lu childPID=%lu childTID=%lu childHR=0x%08lX "
            "childClass=%.*ls childDesktop=%.*ls localCall=%lld localError=%lu localResult=%llu beforeLocal=%lu localCount=%lu "
            "nativePID=%lu nativeTID=%lu threadError=%lu classRead=%d classError=%lu nativeClass=%ls "
            "packetExact=%u processExact=%u threadExact=%u classExact=%u relationExact=%u localExact=%u\n",
            static_cast<unsigned>(different), static_cast<unsigned long>(child.processId), static_cast<void*>(window),
            static_cast<unsigned long>(initial.magic), static_cast<unsigned long>(initial.version), static_cast<unsigned long>(initial.stage),
            static_cast<unsigned long>(initial.parentProcess), static_cast<unsigned long>(initial.process), static_cast<unsigned long>(initial.thread),
            static_cast<unsigned long>(initial.status), static_cast<int>(initial.type.size()), initial.type.data(),
            static_cast<int>(initial.desktop.size()), initial.desktop.data(), static_cast<long long>(initial.local.call),
            static_cast<unsigned long>(initial.local.error), static_cast<unsigned long long>(initial.local.result),
            static_cast<unsigned long>(initial.observedBeforeLocal), static_cast<unsigned long>(initial.nullCount),
            static_cast<unsigned long>(process), static_cast<unsigned long>(thread), static_cast<unsigned long>(threadError),
            classRead, static_cast<unsigned long>(classError), type.data(), static_cast<unsigned>(packetExact),
            static_cast<unsigned>(nativeProcessExact), static_cast<unsigned>(nativeThreadExact), static_cast<unsigned>(nativeClassExact),
            static_cast<unsigned>(relationExact), static_cast<unsigned>(localExact)); std::fflush(stderr);
        // Only this directly launched negative control may use its exclusive
        // pipe's cooperative native self-attestation across desktop namespaces.
        // Successful contradictory HWND queries remain fatal. The positive
        // control and every shared renderer keep their original native gates.
        const bool nativeCompatible = (thread == 0 && process == 0) || (nativeProcessExact && nativeThreadExact);
        const bool classCompatible = classRead == 0 || nativeClassExact;
        const bool exactInitial = packetExact && child.matchesLive(initial) && relationExact && localExact &&
            (different ? nativeCompatible && classCompatible : nativeProcessExact && nativeThreadExact && nativeClassExact);
        group.initialExact=exactInitial;
        report.require("owned-control-exact-native-ready-identity", exactInitial ? S_OK : E_ACCESSDENIED);
        report.require("before-owned-control-message-isolation", desktop.verifyIsolation());
        child.requireLive("before-owned-control-message-retained-kernel-identity", initial, report);
        const auto receipt = sendNull(window, report.deadline);group.message=receipt;
        traceNull(different ? "different-private-process" : "same-private-process", window, receipt);
        report.require("after-owned-control-message-isolation", desktop.verifyIsolation());
        child.requireLive("after-owned-control-message-retained-kernel-identity", initial, report);
        report.record("owned-control-message-call", S_OK, static_cast<uint64_t>(receipt.call));
        report.record("owned-control-message-immediate-error", S_OK, receipt.error);
        report.record("owned-control-message-native-result", S_OK, receipt.result);
        const bool expected = different ? receipt.call == 0 : receipt.delivered();group.expectedDelivery=expected;
        report.record(different ? "owned-other-private-message-rejected" : "owned-same-private-message-delivered", expected ? S_OK : E_FAIL);
        report.require("owned-control-stop-handshake", SetEvent(child.stop.value) ? S_OK : win32());
        report.require("owned-control-bounded-final-packet", child.read(final, std::min(report.deadline, GetTickCount64() + 1000)));
        group.final=final;
        child.requireLive("owned-control-final-retained-kernel-identity", final, report);
        DWORD finalProcess = 0;
        SetLastError(ERROR_SUCCESS);
        const auto finalThread = GetWindowThreadProcessId(window, &finalProcess); const auto finalThreadError = GetLastError();
        std::array<wchar_t, 128> finalType{};
        SetLastError(ERROR_SUCCESS);
        const auto finalClassRead = GetClassNameW(window, finalType.data(), static_cast<int>(finalType.size())); const auto finalClassError = GetLastError();
        std::fprintf(stderr, "headless-window-isolation owned-control-final different=%u HWND=%p nativePID=%lu nativeTID=%lu "
            "threadError=%lu classRead=%d classError=%lu nativeClass=%ls childHR=0x%08lX childTID=%lu "
            "beforeLocal=%lu localCount=%lu localCall=%lld localError=%lu localResult=%llu\n", static_cast<unsigned>(different),
            static_cast<void*>(window), static_cast<unsigned long>(finalProcess), static_cast<unsigned long>(finalThread),
            static_cast<unsigned long>(finalThreadError), finalClassRead, static_cast<unsigned long>(finalClassError), finalType.data(),
            static_cast<unsigned long>(final.status), static_cast<unsigned long>(final.thread),
            static_cast<unsigned long>(final.observedBeforeLocal), static_cast<unsigned long>(final.nullCount),
            static_cast<long long>(final.local.call), static_cast<unsigned long>(final.local.error),
            static_cast<unsigned long long>(final.local.result)); std::fflush(stderr);
        group.finalProcess=finalProcess;group.finalThread=finalThread;group.finalThreadError=finalThreadError;
        group.finalClassRead=finalClassRead;group.finalClassError=finalClassError;
        const bool finalNativeExact = finalProcess == initial.process && finalThread == initial.thread;
        const bool finalClassExact = finalClassRead > 0 && finalType == initial.type;
        const bool stillExact = different ? ((finalThread == 0 && finalProcess == 0) || finalNativeExact) &&
            (finalClassRead == 0 || finalClassExact) : finalNativeExact && finalClassExact;
        const bool finalExact = exactControlPacket(final, 2, different, GetCurrentProcessId(), child.processId) &&
            final.window == initial.window && final.thread == initial.thread && final.desktop == initial.desktop && final.type == initial.type &&
            final.local.delivered() && final.observedBeforeLocal == (different ? 1U : 2U) &&
            final.nullCount == final.observedBeforeLocal + 1 && stillExact && child.matchesLive(final);
        group.finalExact=finalExact;
        report.require("owned-control-exact-responsive-final-identity-counter", finalExact ? S_OK : E_FAIL);
        child.requireLive("before-owned-control-release-retained-kernel-identity", final, report);
        report.require("owned-control-process-stop-drain", child.close());
        DWORD exit = STILL_ACTIVE; const BOOL exitRead = GetExitCodeProcess(child.process.value, &exit);
        group.exitCode=exit;group.exitRead=exitRead?S_OK:win32();
        report.require("owned-control-clean-exit", exitRead && exit == 0 ? S_OK : group.exitRead==S_OK ? E_FAIL : group.exitRead, exit);
        std::fprintf(stderr, "headless-window-isolation owned-control different=%u pid=%lu tid=%lu HWND=%p class=%ls desktop=%ls "
            "initialCounter=%lu beforeFinalLocal=%lu finalCounter=%lu exact=%u exit=%lu\n", static_cast<unsigned>(different),
            static_cast<unsigned long>(initial.process), static_cast<unsigned long>(initial.thread), static_cast<void*>(window),
            initial.type.data(), initial.desktop.data(), static_cast<unsigned long>(initial.nullCount),
            static_cast<unsigned long>(final.observedBeforeLocal), static_cast<unsigned long>(final.nullCount),
            static_cast<unsigned>(finalExact), static_cast<unsigned long>(exit)); std::fflush(stderr);
        report.require("after-owned-control-parent-isolation", desktop.verifyIsolation());
        admitted = admitted && expected;
    }
    report.require("owned-cross-process-desktop-message-controls", admitted ? S_OK : E_ACCESSDENIED);
    return admitted;
}

HRESULT snapshot(HWND window,PrivateWindowSnapshot& value) noexcept {
    value={};value.window=window;SetLastError(ERROR_SUCCESS);
    value.thread=GetWindowThreadProcessId(window,&value.process);value.threadError=GetLastError();
    if(!window||!value.thread||!value.process||!IsWindow(window))return E_ACCESSDENIED;
    value.parent=GetParent(window);value.root=GetAncestor(window,GA_ROOT);
    SetLastError(ERROR_SUCCESS);
    value.classRead=GetClassNameW(window,value.type.data(),static_cast<int>(value.type.size()));value.classError=GetLastError();
    if(value.classRead<=0||value.classRead>=static_cast<int>(value.type.size()-1)||
       value.type.back()||!value.root)return E_ACCESSDENIED;
    SetLastError(ERROR_SUCCESS);
    const auto geometry=GetWindowRect(window,&value.rectangle);const auto geometryError=GetLastError();
    value.geometry=geometry?S_OK:HRESULT_FROM_WIN32(geometryError?geometryError:ERROR_GEN_FAILURE);
    if(!geometry)return value.geometry;
    SetLastError(ERROR_SUCCESS);
    value.borrowedDesktop=GetThreadDesktop(value.thread);value.desktopError=GetLastError();
    if(!value.borrowedDesktop) {
        value.desktopRead=HRESULT_FROM_WIN32(value.desktopError?value.desktopError:ERROR_GEN_FAILURE);
        return S_OK; // Unknown query is retained; it grants no admission itself.
    }
    DWORD bytes=0;SetLastError(ERROR_SUCCESS);
    const auto name=GetUserObjectInformationW(value.borrowedDesktop,UOI_NAME,value.desktop.data(),
        static_cast<DWORD>(sizeof(value.desktop)),&bytes);value.desktopError=GetLastError();
    value.desktopRead=name&&value.desktop.front()&&!value.desktop.back()?S_OK:
        HRESULT_FROM_WIN32(value.desktopError?value.desktopError:ERROR_INVALID_DATA);
    return S_OK;
}
bool sameSnapshot(const PrivateWindowSnapshot& before,const PrivateWindowSnapshot& after) noexcept {
    return before.window==after.window&&before.parent==after.parent&&before.root==after.root&&
        before.process==after.process&&before.thread==after.thread&&before.type==after.type&&
        before.classRead==after.classRead&&before.geometry==S_OK&&after.geometry==S_OK&&
        EqualRect(&before.rectangle,&after.rectangle)&&before.desktopRead==after.desktopRead&&
        before.borrowedDesktop==after.borrowedDesktop&&before.desktop==after.desktop;
}
bool knownMismatch(const PrivateWindowSnapshot& value,const PrivateDesktop& desktop) noexcept {
    return value.desktopRead==S_OK&&std::wstring_view(value.desktop.data())!=desktop.name();
}
bool ownedPrivate(const PrivateWindowSnapshot& value,const PrivateDesktop& desktop) noexcept {
    return value.process==GetCurrentProcessId()&&value.desktopRead==S_OK&&
        std::wstring_view(value.desktop.data())==desktop.name();
}
thread_local std::uint64_t calibrationSequence=0;
} // namespace

bool WindowMessageReceipt::delivered() const noexcept {
    return call!=0&&result!=nullResultSentinel&&result==0;
}
bool runHeadlessWindowControl(int argc,wchar_t* const* argv,int* exitCode) noexcept {
    if(argc<2||!argv||!argv[1]||std::wstring_view(argv[1])!=L"--headless-window-isolation-control")return false;
    if(!exitCode)return true;
    *exitCode=2;
    if(argc!=8)return true;
    for(int index=2;index<8;++index)if(!argv[index])return true;
    *exitCode=messageControlChild(argv);return true;
}
HRESULT runPrivateDesktopMessageControls(const PrivateDesktop& desktop,WindowIsolationControlReport* output) noexcept {
    if(!output)return E_POINTER;
    *output={};lastCalibration=0;calibratedDesktop=nullptr;
    try {
        if(PrivateDesktop::current()!=&desktop)return output->result=E_ACCESSDENIED;
        const auto guard=desktop.verifyIsolation();if(FAILED(guard))return output->result=guard;
        if(desktop.name().empty()||desktop.name().size()>=output->desktop.size())return output->result=E_INVALIDARG;
        output->creatorProcess=GetCurrentProcessId();output->creatorThread=GetCurrentThreadId();
        output->borrowedDesktop=GetThreadDesktop(GetCurrentThreadId());
        if(!output->borrowedDesktop)return output->result=win32();
        std::copy(desktop.name().begin(),desktop.name().end(),output->desktop.begin());
        Report report(*output);
        if(!verifyMessageControls(desktop,report)||output->overflow)return output->result=E_ACCESSDENIED;
        const auto finalGuard=desktop.verifyIsolation();if(FAILED(finalGuard))return output->result=finalGuard;
        if(++calibrationSequence==0)++calibrationSequence;
        lastCalibration=calibrationSequence;calibratedDesktop=output->borrowedDesktop;
        output->calibration=lastCalibration;output->calibrated=true;return output->result=S_OK;
    }catch(const Failure& failure){return output->result=failure.value;}
     catch(const std::bad_alloc&){return output->result=E_OUTOFMEMORY;}
     catch(...){return output->result=E_FAIL;}
}
HRESULT admitForeignPrivateWindow(const PrivateDesktop& desktop,HWND parent,HWND target,
    const WindowIsolationControlReport& controls,ULONGLONG deadline,PrivateWindowAdmissionReport* output) noexcept {
    if(!output)return E_POINTER;
    *output={};
    try {
        if(PrivateDesktop::current()!=&desktop)return output->result=E_ACCESSDENIED;
        output->guardBefore=desktop.verifyIsolation();if(FAILED(output->guardBefore))return output->result=output->guardBefore;
        if(GetTickCount64()>=deadline)return output->result=HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        if(controls.result!=S_OK||!controls.calibrated||controls.overflow||!controls.calibration||controls.desktop.back()!=0||
           controls.calibration!=lastCalibration||controls.creatorProcess!=GetCurrentProcessId()||
           controls.creatorThread!=GetCurrentThreadId()||controls.borrowedDesktop!=calibratedDesktop||
           controls.borrowedDesktop!=GetThreadDesktop(GetCurrentThreadId())||
           std::wstring_view(controls.desktop.data())!=desktop.name())return output->result=E_ACCESSDENIED;
        for(const auto& control:controls.controls)if(!control.initialExact||!control.finalExact||!control.expectedDelivery||
            !control.kernelExited||control.drain!=S_OK||control.exitRead!=S_OK||control.exitCode!=0)return output->result=E_ACCESSDENIED;
        auto hr=snapshot(parent,output->parentBefore);if(FAILED(hr))return output->result=hr;
        hr=snapshot(output->parentBefore.root,output->rootBefore);if(FAILED(hr))return output->result=hr;
        hr=snapshot(target,output->targetBefore);if(FAILED(hr))return output->result=hr;
        output->ancestryBefore=parent!=target&&IsChild(parent,target)&&
            output->targetBefore.root==output->parentBefore.root;
        if(!ownedPrivate(output->parentBefore,desktop)||!ownedPrivate(output->rootBefore,desktop)||
           output->rootBefore.thread!=GetCurrentThreadId()||output->targetBefore.process==GetCurrentProcessId()||
           !output->ancestryBefore||knownMismatch(output->targetBefore,desktop))return output->result=E_ACCESSDENIED;
        output->message=sendNull(target,deadline);
        output->guardAfter=desktop.verifyIsolation();
        hr=snapshot(parent,output->parentAfter);if(FAILED(hr))return output->result=hr;
        hr=snapshot(output->parentAfter.root,output->rootAfter);if(FAILED(hr))return output->result=hr;
        hr=snapshot(target,output->targetAfter);if(FAILED(hr))return output->result=hr;
        output->ancestryAfter=parent!=target&&IsChild(parent,target)&&
            output->targetAfter.root==output->parentAfter.root;
        output->exactSnapshot=sameSnapshot(output->parentBefore,output->parentAfter)&&
            sameSnapshot(output->rootBefore,output->rootAfter)&&sameSnapshot(output->targetBefore,output->targetAfter);
        if(FAILED(output->guardAfter))return output->result=output->guardAfter;
        if(GetTickCount64()>=deadline)return output->result=HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        if(!output->message.delivered()||!output->ancestryAfter||!output->exactSnapshot||
           !ownedPrivate(output->parentAfter,desktop)||!ownedPrivate(output->rootAfter,desktop)||
           knownMismatch(output->targetAfter,desktop))return output->result=E_ACCESSDENIED;
        output->admission=output->targetBefore.desktopRead==S_OK&&output->targetAfter.desktopRead==S_OK?
            PrivateWindowAdmission::ExactDesktopQuery:PrivateWindowAdmission::MessageChannelInference;
        return output->result=S_OK;
    }catch(const std::bad_alloc&){return output->result=E_OUTOFMEMORY;}catch(...){return output->result=E_FAIL;}
}
} // namespace explorer
