#include "explorer/startup_desktop.hpp"

#include <objbase.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <cwchar>
#include <limits>
#include <type_traits>
#include <vector>

namespace explorer {
namespace {
constexpr ULONGLONG PacketMagic = 0x5758455853544450ULL;
constexpr DWORD PacketVersion = 1;
constexpr size_t NameCapacity = 256, QualifiedCapacity = 512, ReportCapacity = 2048;
constexpr DWORD ChildTimeoutMs = 180000, StopTimeoutMs = 5000;
constexpr wchar_t ChildOption[] = L"--headless-startup-desktop-child";

struct Packet {
    ULONGLONG magic = PacketMagic;
    DWORD version = PacketVersion, bytes = sizeof(Packet);
    StartupDesktopArm arm = StartupDesktopArm::InputThenPrivate;
    DWORD installedRibbon = 0;
    GUID nonce{};
    ULONGLONG mapping = 0, parentHandle = 0;
    ULONGLONG stdoutHandle = 0, stderrHandle = 0, stdinHandle = 0;
    DWORD parentPid = 0, parentTid = 0, childPid = 0, childTid = 0, session = 0;
    FILETIME parentCreation{}, parentThreadCreation{}, childCreation{}, childThreadCreation{};
    FILE_ID_INFO executable{};
    FILE_BASIC_INFO executableBasic{};
    LARGE_INTEGER executableSize{};
    std::array<wchar_t, NameCapacity> station{}, target{}, privateDesktop{}, input{};
    std::array<wchar_t, QualifiedCapacity> qualified{};
    std::array<wchar_t, ReportCapacity> report{};
    std::array<DWORD, 8> reserved{};
};
static_assert(std::is_trivially_copyable_v<Packet>);
static_assert(sizeof(Packet) < 16384);

HRESULT failure(DWORD error = GetLastError()) noexcept {
    return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
}
bool sameTime(const FILETIME& left, const FILETIME& right) noexcept {
    return left.dwLowDateTime == right.dwLowDateTime && left.dwHighDateTime == right.dwHighDateTime;
}
bool sameName(const std::wstring& left, const std::wstring& right) noexcept {
    return left.size() == right.size() && left.size() <= static_cast<size_t>((std::numeric_limits<int>::max)()) &&
        CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()), right.c_str(),
            static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}
bool sameFile(const FILE_ID_INFO& left, const FILE_ID_INFO& right) noexcept {
    return left.VolumeSerialNumber == right.VolumeSerialNumber &&
        std::memcmp(left.FileId.Identifier, right.FileId.Identifier, sizeof(left.FileId.Identifier)) == 0;
}
bool sameBasic(const FILE_BASIC_INFO& left, const FILE_BASIC_INFO& right) noexcept {
    return left.CreationTime.QuadPart == right.CreationTime.QuadPart &&
        left.LastWriteTime.QuadPart == right.LastWriteTime.QuadPart &&
        left.ChangeTime.QuadPart == right.ChangeTime.QuadPart && left.FileAttributes == right.FileAttributes;
}
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    HANDLE release() noexcept { const auto result = value; value = nullptr; return result; }
};
struct Desktop {
    HDESK value = nullptr;
    ~Desktop() { if (value) CloseDesktop(value); }
};

HRESULT nameOf(HANDLE object, std::wstring& output) {
    DWORD bytes = 0;
    SetLastError(ERROR_SUCCESS);
    GetUserObjectInformationW(object, UOI_NAME, nullptr, 0, &bytes);
    if (!bytes || bytes % sizeof(wchar_t) || bytes > NameCapacity * sizeof(wchar_t))
        return failure();
    std::array<wchar_t, NameCapacity> text{};
    if (!GetUserObjectInformationW(object, UOI_NAME, text.data(), bytes, &bytes)) return failure();
    const auto length = wcsnlen_s(text.data(), text.size());
    if (!length || length == text.size()) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    output.assign(text.data(), length);
    return S_OK;
}
HRESULT currentInput(std::wstring& output) {
    Desktop input{OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS)};
    return input.value ? nameOf(input.value, output) : failure();
}
HRESULT uninitializedCaller(HRESULT& result, int* type = nullptr, int* qualifierValue = nullptr) noexcept {
    APTTYPE apartment{}; APTTYPEQUALIFIER qualifier{};
    result = CoGetApartmentType(&apartment, &qualifier);
    if (SUCCEEDED(result)) {
        if (type) *type = static_cast<int>(apartment);
        if (qualifierValue) *qualifierValue = static_cast<int>(qualifier);
    }
    if (result != CO_E_NOTINITIALIZED && !(result == S_OK && apartment == APTTYPE_MTA &&
        qualifier == APTTYPEQUALIFIER_IMPLICIT_MTA)) return E_ACCESSDENIED;
    Handle token;
    if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token.value)) return E_ACCESSDENIED;
    const auto error = GetLastError();
    return error == ERROR_NO_TOKEN ? S_OK : failure(error);
}
HRESULT creationOf(HANDLE process, FILETIME& creation) noexcept {
    FILETIME exit{}, kernel{}, user{};
    return GetProcessTimes(process, &creation, &exit, &kernel, &user) ? S_OK : failure();
}
HRESULT threadCreationOf(HANDLE thread, FILETIME& creation) noexcept {
    FILETIME exit{}, kernel{}, user{};
    return GetThreadTimes(thread, &creation, &exit, &kernel, &user) ? S_OK : failure();
}
HRESULT executablePath(std::wstring& path) {
    std::vector<wchar_t> text(32768);
    const auto length = GetModuleFileNameW(nullptr, text.data(), static_cast<DWORD>(text.size()));
    if (!length) return failure();
    if (length >= text.size()) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    path.assign(text.data(), length);
    return S_OK;
}
HRESULT readFileFacts(HANDLE file, FILE_ID_INFO& identity, FILE_BASIC_INFO& basic, LARGE_INTEGER& size) noexcept {
    return GetFileInformationByHandleEx(file, FileIdInfo, &identity, sizeof(identity)) &&
        GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic)) &&
        GetFileSizeEx(file, &size) ? S_OK : failure();
}
HRESULT currentExecutable(const Packet& packet) {
    std::wstring path;
    auto hr = executablePath(path);
    if (FAILED(hr)) return hr;
    Handle file{CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE) return failure();
    FILE_ID_INFO identity{}; FILE_BASIC_INFO basic{}; LARGE_INTEGER size{};
    hr = readFileFacts(file.value, identity, basic, size);
    if (FAILED(hr)) return hr;
    return sameFile(identity, packet.executable) && sameBasic(basic, packet.executableBasic) &&
        size.QuadPart == packet.executableSize.QuadPart ? S_OK : E_ACCESSDENIED;
}
template<size_t Capacity> bool copyText(const std::wstring& value, std::array<wchar_t, Capacity>& output) noexcept {
    if (value.empty() || value.size() >= Capacity || value.find(L'\0') != std::wstring::npos) return false;
    std::copy(value.begin(), value.end(), output.begin());
    return true; // the value-initialized packet retains its terminating/tail NULs
}
template<size_t Capacity> bool validText(const std::array<wchar_t, Capacity>& value) noexcept {
    const auto length = wcsnlen_s(value.data(), value.size());
    return length && length < value.size() &&
        std::all_of(value.begin() + static_cast<std::ptrdiff_t>(length), value.end(),
            [](wchar_t ch) { return ch == L'\0'; });
}
bool supportedArm(StartupDesktopArm arm) noexcept {
    return arm == StartupDesktopArm::InputThenPrivate || arm == StartupDesktopArm::InitialPrivate;
}
bool validPacket(const Packet& packet, HANDLE mapping, HANDLE parent, StartupDesktopArm arm) noexcept {
    const std::array<ULONGLONG, 5> handles{packet.mapping, packet.parentHandle,
        packet.stdoutHandle, packet.stderrHandle, packet.stdinHandle};
    for (size_t index = 0; index < handles.size(); ++index) {
        if (!handles[index] || handles[index] >= static_cast<ULONGLONG>((std::numeric_limits<ULONG_PTR>::max)()))
            return false;
        for (size_t earlier = 0; earlier < index; ++earlier)
            if (handles[earlier] == handles[index]) return false;
    }
    return packet.magic == PacketMagic && packet.version == PacketVersion && packet.bytes == sizeof(Packet) &&
        supportedArm(arm) && packet.arm == arm && packet.installedRibbon <= 1 &&
        !IsEqualGUID(packet.nonce, GUID_NULL) &&
        packet.mapping == static_cast<ULONGLONG>(reinterpret_cast<ULONG_PTR>(mapping)) &&
        packet.parentHandle == static_cast<ULONGLONG>(reinterpret_cast<ULONG_PTR>(parent)) &&
        packet.parentPid && packet.parentTid && packet.childPid && packet.childTid &&
        validText(packet.station) && validText(packet.target) && validText(packet.privateDesktop) &&
        validText(packet.input) && validText(packet.qualified) && validText(packet.report) &&
        std::all_of(packet.reserved.begin(), packet.reserved.end(), [](DWORD value) { return value == 0; });
}

HRESULT verifyParent(const Packet& packet, HANDLE parent) noexcept {
    if (GetProcessId(parent) != packet.parentPid || packet.parentPid == GetCurrentProcessId()) return E_ACCESSDENIED;
    FILETIME creation{};
    auto hr = creationOf(parent, creation);
    if (FAILED(hr)) return hr;
    DWORD session = 0;
    if (!ProcessIdToSessionId(packet.parentPid, &session)) return failure();
    if (session != packet.session || !sameTime(creation, packet.parentCreation)) return E_ACCESSDENIED;
    const auto alive = WaitForSingleObject(parent, 0);
    if (alive != WAIT_TIMEOUT) return alive == WAIT_FAILED ? failure() : E_ACCESSDENIED;
    Handle thread{OpenThread(THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, packet.parentTid)};
    if (!thread.value) return failure();
    if (GetProcessIdOfThread(thread.value) != packet.parentPid) return E_ACCESSDENIED;
    hr = threadCreationOf(thread.value, creation);
    if (FAILED(hr)) return hr;
    if (!sameTime(creation, packet.parentThreadCreation)) return E_ACCESSDENIED;
    const auto threadAlive = WaitForSingleObject(thread.value, 0);
    return threadAlive == WAIT_TIMEOUT ? S_OK : threadAlive == WAIT_FAILED ? failure() : E_ACCESSDENIED;
}
HRESULT verifyChildIdentity(const Packet& packet) noexcept {
    if (packet.childPid != GetCurrentProcessId() || packet.childTid != GetCurrentThreadId()) return E_ACCESSDENIED;
    DWORD session = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) return failure();
    if (session != packet.session) return E_ACCESSDENIED;
    FILETIME creation{};
    auto hr = creationOf(GetCurrentProcess(), creation);
    if (FAILED(hr)) return hr;
    if (!sameTime(creation, packet.childCreation)) return E_ACCESSDENIED;
    hr = threadCreationOf(GetCurrentThread(), creation);
    return FAILED(hr) ? hr : sameTime(creation, packet.childThreadCreation) ? S_OK : E_ACCESSDENIED;
}
std::wstring quoted(const std::wstring& argument) {
    // Windows argv quoting, including backslash runs immediately before a
    // quote or the closing quote. No command interpreter processes this text.
    std::wstring value(1, L'"');
    size_t slashes = 0;
    for (const auto ch : argument) {
        if (ch == L'\\') { ++slashes; continue; }
        value.append(ch == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0; value += ch;
    }
    value.append(slashes * 2, L'\\'); value += L'"';
    return value;
}
struct AttributeList {
    std::vector<BYTE> storage;
    LPPROC_THREAD_ATTRIBUTE_LIST value = nullptr;
    ~AttributeList() { if (value) DeleteProcThreadAttributeList(value); }
    HRESULT initialize(const std::array<HANDLE, 5>& handles) {
        SIZE_T bytes = 0;
        SetLastError(ERROR_SUCCESS);
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        if (!bytes || GetLastError() != ERROR_INSUFFICIENT_BUFFER) return failure();
        storage.resize(bytes);
        auto list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(list, 1, 0, &bytes)) return failure();
        value = list;
        return UpdateProcThreadAttribute(value, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            const_cast<HANDLE*>(handles.data()), sizeof(handles), nullptr, nullptr) ? S_OK : failure();
    }
};
[[noreturn]] void stopUnsafeParent() noexcept {
    std::fprintf(stderr, "Startup desktop diagnostic could not establish owned child kernel exit; retaining desktop/resources.\n");
    std::fflush(stderr);
    if (!TerminateProcess(GetCurrentProcess(), 10)) std::_Exit(10);
    std::_Exit(10);
}
struct ChildLease {
    PROCESS_INFORMATION value{};
    StartupDesktopArmReadback* result = nullptr;
    bool exited = false;
    void observeExit() noexcept {
        exited = true;
        if (!result) return;
        result->kernelExited = true;
        if (!result->kernelExitTick) result->kernelExitTick = GetTickCount64();
        result->exitRead = GetExitCodeProcess(value.hProcess, &result->exitCode) ? S_OK : failure();
    }
    HRESULT stop() noexcept {
        if (!value.hProcess || exited) return S_OK;
        const auto ready = WaitForSingleObject(value.hProcess, 0);
        if (ready == WAIT_OBJECT_0) { observeExit(); return S_OK; }
        if (ready == WAIT_FAILED || GetProcessId(value.hProcess) != value.dwProcessId) stopUnsafeParent();
        const auto killed = TerminateProcess(value.hProcess, 10);
        const auto error = killed ? ERROR_SUCCESS : GetLastError();
        if (result) result->termination = killed ? S_OK : failure(error);
        if (WaitForSingleObject(value.hProcess, StopTimeoutMs) != WAIT_OBJECT_0) stopUnsafeParent();
        observeExit();
        return killed ? S_OK : failure(error);
    }
    ~ChildLease() {
        stop();
        if (value.hThread) CloseHandle(value.hThread);
        if (value.hProcess) CloseHandle(value.hProcess);
    }
};

HRESULT runArm(Packet packet, const std::wstring& application, const std::wstring& directory,
    const std::wstring& qualifiedDesktop, HANDLE executableLease, StartupDesktopArmReadback& result) {
    result.arm = packet.arm;
    Handle section{CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
        static_cast<DWORD>(sizeof(Packet)), nullptr)};
    if (!section.value) return result.packet = failure();
    Handle inheritedSection, inheritedParent;
    if (!DuplicateHandle(GetCurrentProcess(), section.value, GetCurrentProcess(), &inheritedSection.value,
            FILE_MAP_READ, TRUE, 0) ||
        !DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &inheritedParent.value,
            SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, TRUE, 0)) return result.packet = failure();
    packet.mapping = static_cast<ULONGLONG>(reinterpret_cast<ULONG_PTR>(inheritedSection.value));
    packet.parentHandle = static_cast<ULONGLONG>(reinterpret_cast<ULONG_PTR>(inheritedParent.value));
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    Handle stdoutFile{CreateFileW(result.stdoutLog.c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ, &inherit, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (stdoutFile.value == INVALID_HANDLE_VALUE) return result.packet = failure();
    Handle stderrFile{CreateFileW(result.stderrLog.c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ, &inherit, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (stderrFile.value == INVALID_HANDLE_VALUE) return result.packet = failure();
    Handle inputFile{CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &inherit, OPEN_EXISTING, 0, nullptr)};
    if (inputFile.value == INVALID_HANDLE_VALUE) return result.packet = failure();
    packet.stdoutHandle = static_cast<ULONGLONG>(reinterpret_cast<ULONG_PTR>(stdoutFile.value));
    packet.stderrHandle = static_cast<ULONGLONG>(reinterpret_cast<ULONG_PTR>(stderrFile.value));
    packet.stdinHandle = static_cast<ULONGLONG>(reinterpret_cast<ULONG_PTR>(inputFile.value));
    const std::array<HANDLE, 5> handles{inheritedSection.value, inheritedParent.value,
        stdoutFile.value, stderrFile.value, inputFile.value};
    AttributeList attributes;
    auto hr = attributes.initialize(handles);
    if (FAILED(hr)) return result.packet = hr;
    auto command = quoted(application) + L" --headless-smoke ";
    if (packet.installedRibbon) command += L"--installed-ribbon ";
    command += std::wstring(ChildOption) + L" " + std::to_wstring(packet.mapping) + L" " +
        std::to_wstring(packet.parentHandle) + L" " + std::to_wstring(static_cast<DWORD>(packet.arm)) +
        L" --report " + quoted(packet.report.data());
    if (command.size() >= 32767) return result.packet = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.lpDesktop = const_cast<wchar_t*>(qualifiedDesktop.c_str());
    startup.StartupInfo.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    startup.StartupInfo.wShowWindow = SW_HIDE;
    startup.StartupInfo.hStdOutput = stdoutFile.value;
    startup.StartupInfo.hStdError = stderrFile.value;
    startup.StartupInfo.hStdInput = inputFile.value;
    startup.lpAttributeList = attributes.value;
    ChildLease child;
    child.result = &result;
    const auto created = CreateProcessW(application.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, directory.c_str(),
        &startup.StartupInfo, &child.value);
    result.create = created ? S_OK : failure();
    if (!created) return result.create;
    result.childPid = packet.childPid = child.value.dwProcessId;
    result.childTid = packet.childTid = child.value.dwThreadId;
    hr = creationOf(child.value.hProcess, packet.childCreation);
    if (SUCCEEDED(hr)) hr = threadCreationOf(child.value.hThread, packet.childThreadCreation);
    DWORD childSession = 0;
    if (SUCCEEDED(hr) && !ProcessIdToSessionId(packet.childPid, &childSession)) hr = failure();
    if (SUCCEEDED(hr) && childSession != packet.session) hr = E_ACCESSDENIED;
    if (FAILED(hr)) return result.packet = hr;
    auto view = MapViewOfFile(section.value, FILE_MAP_WRITE, 0, 0, sizeof(Packet));
    if (!view) return result.packet = failure();
    std::memcpy(view, &packet, sizeof(packet));
    if (!UnmapViewOfFile(view)) return result.packet = failure();
    // No parent writable view or writable section handle remains when the
    // suspended child starts. This is a controlled handshake, not a claim
    // that FILE_MAP_READ alone authenticates the packet against other writers.
    const auto writable = section.release();
    if (!CloseHandle(writable)) return result.packet = failure();
    result.packet = S_OK;
    result.resumeTick = GetTickCount64();
    const auto priorSuspend = ResumeThread(child.value.hThread);
    result.resume = priorSuspend == 1 ? S_OK : priorSuspend == MAXDWORD ? failure() : E_UNEXPECTED;
    if (FAILED(result.resume)) return result.resume;
    const auto wait = WaitForSingleObject(child.value.hProcess, ChildTimeoutMs);
    const auto waitError = wait == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
    result.wait = wait == WAIT_OBJECT_0 ? S_OK : wait == WAIT_TIMEOUT ?
        HRESULT_FROM_WIN32(ERROR_TIMEOUT) : failure(waitError);
    result.timedOut = wait == WAIT_TIMEOUT;
    if (wait != WAIT_OBJECT_0) child.stop();
    else child.observeExit();
    FILE_ID_INFO identity{}; FILE_BASIC_INFO basic{}; LARGE_INTEGER size{};
    result.preservation = readFileFacts(executableLease, identity, basic, size);
    if (SUCCEEDED(result.preservation) && (!sameFile(identity, packet.executable) ||
        !sameBasic(basic, packet.executableBasic) || size.QuadPart != packet.executableSize.QuadPart))
        result.preservation = E_ACCESSDENIED;
    if (FAILED(result.wait)) return result.wait;
    if (FAILED(result.exitRead)) return result.exitRead;
    return result.preservation;
}
const std::wstring Empty;
} // namespace

struct StartupDesktopChild::Impl {
    Packet packet;
    HANDLE mapping = nullptr, parent = nullptr;
    HDESK initial = nullptr; // assigned by Windows; never close this handle
    std::wstring station, target, input, report;
    ~Impl() {
        if (mapping) CloseHandle(mapping);
        if (parent) CloseHandle(parent);
    }
    HRESULT parentAndInput() const {
        auto hr = verifyParent(packet, parent);
        if (SUCCEEDED(hr)) hr = verifyChildIdentity(packet);
        if (FAILED(hr)) return hr;
        std::wstring actualStation, actualInput;
        hr = nameOf(GetProcessWindowStation(), actualStation);
        if (SUCCEEDED(hr)) hr = currentInput(actualInput);
        return FAILED(hr) ? hr : sameName(actualStation, station) && sameName(actualInput, input) ? S_OK : E_ACCESSDENIED;
    }
    HRESULT initialState() const {
        const auto hr = parentAndInput();
        if (FAILED(hr)) return hr;
        if (GetThreadDesktop(GetCurrentThreadId()) != initial) return E_ACCESSDENIED;
        std::wstring actual;
        const auto read = nameOf(initial, actual);
        return FAILED(read) ? read : sameName(actual, target) ? S_OK : E_ACCESSDENIED;
    }
};

StartupDesktopChild::StartupDesktopChild() noexcept = default;
StartupDesktopChild::~StartupDesktopChild() = default;
bool StartupDesktopChild::ready() const noexcept { return impl_ != nullptr; }
StartupDesktopArm StartupDesktopChild::arm() const noexcept {
    return impl_ ? impl_->packet.arm : static_cast<StartupDesktopArm>(0);
}
HDESK StartupDesktopChild::initialDesktop() const noexcept { return impl_ ? impl_->initial : nullptr; }
const std::wstring& StartupDesktopChild::desktopName() const noexcept { return impl_ ? impl_->target : Empty; }
const std::wstring& StartupDesktopChild::inputName() const noexcept { return impl_ ? impl_->input : Empty; }
const std::wstring& StartupDesktopChild::reportPath() const noexcept { return impl_ ? impl_->report : Empty; }
bool StartupDesktopChild::installedRibbon() const noexcept { return impl_ && impl_->packet.installedRibbon != 0; }
HRESULT StartupDesktopChild::verifyInitial() const noexcept {
    try { return impl_ ? impl_->initialState() : E_ACCESSDENIED; }
    catch (...) { return E_OUTOFMEMORY; }
}
HRESULT StartupDesktopChild::verifyParentAndInput() const noexcept {
    try { return impl_ ? impl_->parentAndInput() : E_ACCESSDENIED; }
    catch (...) { return E_OUTOFMEMORY; }
}

HRESULT StartupDesktopChild::claim(HANDLE mapping, HANDLE parent, StartupDesktopArm expected,
    StartupDesktopChildReadback& readback) noexcept {
    readback = {};
    try {
        if (impl_) return readback.packet = HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        if (!mapping || !parent || mapping == INVALID_HANDLE_VALUE || parent == INVALID_HANDLE_VALUE ||
            mapping == parent || !supportedArm(expected)) return readback.packet = E_INVALIDARG;
        DWORD mapFlags = 0, parentFlags = 0;
        if (!GetHandleInformation(mapping, &mapFlags) || !GetHandleInformation(parent, &parentFlags))
            return readback.packet = failure();
        if (!(mapFlags & HANDLE_FLAG_INHERIT) || !(parentFlags & HANDLE_FLAG_INHERIT))
            return readback.packet = E_ACCESSDENIED;
        const auto view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(Packet));
        if (!view) return readback.packet = failure();
        Packet packet;
        std::memcpy(&packet, view, sizeof(packet));
        if (!UnmapViewOfFile(view)) return readback.packet = failure();
        if (!validPacket(packet, mapping, parent, expected)) return readback.packet = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        readback.packet = S_OK;
        auto candidate = std::make_unique<Impl>();
        candidate->packet = packet;
        candidate->station = packet.station.data(); candidate->target = packet.target.data();
        candidate->input = packet.input.data(); candidate->report = packet.report.data();
        const std::wstring qualified = candidate->station + L"\\" + candidate->target;
        if (qualified != packet.qualified.data() || !std::filesystem::path(candidate->report).is_absolute() ||
            sameName(packet.privateDesktop.data(), candidate->input) ||
            (expected == StartupDesktopArm::InputThenPrivate && !sameName(candidate->target, candidate->input)) ||
            (expected == StartupDesktopArm::InitialPrivate && !sameName(candidate->target, packet.privateDesktop.data())))
            return readback.packet = E_ACCESSDENIED;
        auto hr = uninitializedCaller(readback.apartment, &readback.apartmentType, &readback.apartmentQualifier);
        if (FAILED(hr)) return hr;
        readback.parentPid = packet.parentPid; readback.parentTid = packet.parentTid;
        readback.childPid = packet.childPid; readback.childTid = packet.childTid; readback.session = packet.session;
        readback.parent = verifyParent(packet, parent);
        if (FAILED(readback.parent)) return readback.parent;
        readback.identity = verifyChildIdentity(packet);
        if (FAILED(readback.identity)) return readback.identity;
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        GetStartupInfoW(&startup);
        const auto length = startup.lpDesktop ? wcsnlen_s(startup.lpDesktop, QualifiedCapacity) : 0;
        readback.startup = length && length < QualifiedCapacity &&
            qualified == startup.lpDesktop ? S_OK : E_ACCESSDENIED;
        const auto sameHandle = [](HANDLE handle, ULONGLONG expectedHandle) {
            return handle && handle != INVALID_HANDLE_VALUE &&
                static_cast<ULONGLONG>(reinterpret_cast<ULONG_PTR>(handle)) == expectedHandle;
        };
        if (SUCCEEDED(readback.startup) && (!(startup.dwFlags & STARTF_USESTDHANDLES) ||
            !sameHandle(startup.hStdOutput, packet.stdoutHandle) || !sameHandle(startup.hStdError, packet.stderrHandle) ||
            !sameHandle(startup.hStdInput, packet.stdinHandle) ||
            !sameHandle(GetStdHandle(STD_OUTPUT_HANDLE), packet.stdoutHandle) ||
            !sameHandle(GetStdHandle(STD_ERROR_HANDLE), packet.stderrHandle) ||
            !sameHandle(GetStdHandle(STD_INPUT_HANDLE), packet.stdinHandle) ||
            GetFileType(startup.hStdOutput) != FILE_TYPE_DISK || GetFileType(startup.hStdError) != FILE_TYPE_DISK ||
            GetFileType(startup.hStdInput) != FILE_TYPE_CHAR)) readback.startup = E_ACCESSDENIED;
        if (FAILED(readback.startup)) return readback.startup;
        candidate->initial = GetThreadDesktop(GetCurrentThreadId());
        if (!candidate->initial) return readback.attachment = failure();
        std::wstring station, desktop, input;
        hr = nameOf(GetProcessWindowStation(), station);
        if (SUCCEEDED(hr)) hr = nameOf(candidate->initial, desktop);
        readback.attachment = FAILED(hr) ? hr : sameName(station, candidate->station) &&
            sameName(desktop, candidate->target) ? S_OK : E_ACCESSDENIED;
        if (FAILED(readback.attachment)) return readback.attachment;
        readback.input = currentInput(input);
        if (SUCCEEDED(readback.input) && !sameName(input, candidate->input)) readback.input = E_ACCESSDENIED;
        readback.inputUnchanged = readback.input == S_OK;
        if (FAILED(readback.input)) return readback.input;
        readback.executable = currentExecutable(packet);
        if (FAILED(readback.executable)) return readback.executable;
        // Stop these exact designated handles from leaking into any later
        // child. Failed validation never closes an arbitrary numeric handle.
        if (!SetHandleInformation(mapping, HANDLE_FLAG_INHERIT, 0) ||
            !SetHandleInformation(parent, HANDLE_FLAG_INHERIT, 0) ||
            !SetHandleInformation(startup.hStdOutput, HANDLE_FLAG_INHERIT, 0) ||
            !SetHandleInformation(startup.hStdError, HANDLE_FLAG_INHERIT, 0) ||
            !SetHandleInformation(startup.hStdInput, HANDLE_FLAG_INHERIT, 0)) return failure();
        candidate->mapping = mapping; candidate->parent = parent;
        readback.initialHandleBorrowed = true;
        impl_ = std::move(candidate);
        return S_OK;
    } catch (...) { return E_OUTOFMEMORY; }
}

HRESULT runStartupDesktopComparison(const std::filesystem::path& outputDirectory, bool installedRibbon,
    StartupDesktopComparisonReadback& readback) noexcept {
    readback = {};
    try {
        HRESULT apartment = E_PENDING;
        readback.guard = uninitializedCaller(apartment);
        if (FAILED(readback.guard)) return readback.guard;
        if (outputDirectory.empty() || !outputDirectory.is_absolute() ||
            outputDirectory.wstring().size() >= ReportCapacity - 64) return readback.directory = E_INVALIDARG;
        Packet packet;
        packet.installedRibbon = installedRibbon ? 1 : 0;
        packet.parentPid = GetCurrentProcessId(); packet.parentTid = GetCurrentThreadId();
        auto hr = creationOf(GetCurrentProcess(), packet.parentCreation);
        if (SUCCEEDED(hr)) hr = threadCreationOf(GetCurrentThread(), packet.parentThreadCreation);
        if (FAILED(hr)) return readback.guard = hr;
        if (!ProcessIdToSessionId(packet.parentPid, &packet.session)) return readback.guard = failure();
        std::wstring station, originalInput, application;
        const auto stationHandle = GetProcessWindowStation();
        if (!stationHandle) return readback.guard = failure();
        hr = nameOf(stationHandle, station);
        if (SUCCEEDED(hr)) hr = currentInput(originalInput);
        if (FAILED(hr)) return readback.guard = hr;
        hr = executablePath(application);
        if (FAILED(hr)) return readback.executable = hr;
        Handle executable{CreateFileW(application.c_str(), FILE_READ_DATA | FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL, nullptr)};
        if (executable.value == INVALID_HANDLE_VALUE) return readback.executable = failure();
        readback.executable = readFileFacts(executable.value, packet.executable, packet.executableBasic, packet.executableSize);
        if (FAILED(readback.executable)) return readback.executable;
        if (packet.executableBasic.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT |
            FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS))
            return readback.executable = E_ACCESSDENIED;
        hr = CoCreateGuid(&packet.nonce);
        if (FAILED(hr)) return readback.guard = hr;
        wchar_t guid[40]{};
        if (!StringFromGUID2(packet.nonce, guid, static_cast<int>(std::size(guid)))) return readback.guard = E_FAIL;
        const auto privateName = L"WindowsExplorer.Startup." + std::to_wstring(packet.parentPid) + L"." + guid;
        if (!copyText(station, packet.station) || !copyText(originalInput, packet.input) ||
            !copyText(privateName, packet.privateDesktop)) return readback.guard = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        if (!CreateDirectoryW(outputDirectory.c_str(), nullptr)) return readback.directory = failure();
        readback.directory = S_OK; // only this new directory receives reports
        constexpr ACCESS_MASK rights = DESKTOP_CREATEWINDOW | DESKTOP_CREATEMENU | DESKTOP_ENUMERATE |
            DESKTOP_HOOKCONTROL | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS;
        SetLastError(ERROR_SUCCESS);
        Desktop privateDesktop{CreateDesktopW(privateName.c_str(), nullptr, nullptr, 0, rights, nullptr)};
        const auto createError = GetLastError();
        readback.desktop = privateDesktop.value ? S_OK : failure(createError);
        if (FAILED(readback.desktop)) return readback.desktop;
        if (createError == ERROR_ALREADY_EXISTS) return readback.desktop = E_ACCESSDENIED;
        USEROBJECTFLAGS flags{}; DWORD bytes = 0;
        if (!GetUserObjectInformationW(privateDesktop.value, UOI_FLAGS, &flags, sizeof(flags), &bytes))
            return readback.desktop = failure();
        readback.noninheritableDesktop = flags.fInherit == FALSE;
        std::wstring createdName;
        hr = nameOf(privateDesktop.value, createdName);
        if (FAILED(hr)) return readback.desktop = hr;
        if (!readback.noninheritableDesktop || !sameName(createdName, privateName) ||
            sameName(privateName, originalInput) || GetProcessWindowStation() != stationHandle)
            return readback.desktop = E_ACCESSDENIED;
        HRESULT overall = S_OK;
        for (size_t index = 0; index < readback.arms.size(); ++index) {
            auto armPacket = packet;
            armPacket.arm = index == 0 ? StartupDesktopArm::InputThenPrivate : StartupDesktopArm::InitialPrivate;
            const auto target = index == 0 ? originalInput : privateName;
            const auto qualified = station + L"\\" + target;
            const auto report = outputDirectory / (index == 0 ? L"input-then-private.json" : L"initial-private.json");
            auto& arm = readback.arms[index]; arm.arm = armPacket.arm; arm.report = report;
            arm.stdoutLog = outputDirectory / (index == 0 ? L"input-then-private.stdout.log" : L"initial-private.stdout.log");
            arm.stderrLog = outputDirectory / (index == 0 ? L"input-then-private.stderr.log" : L"initial-private.stderr.log");
            if (!copyText(target, armPacket.target) || !copyText(qualified, armPacket.qualified) ||
                !copyText(report.wstring(), armPacket.report)) return readback.guard = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
            std::wstring input;
            hr = currentInput(input);
            if (FAILED(hr) || !sameName(input, originalInput) || GetProcessWindowStation() != stationHandle)
                return readback.guard = FAILED(hr) ? hr : E_ACCESSDENIED;
            const auto run = runArm(armPacket, application, outputDirectory.wstring(), qualified, executable.value, arm);
            if (FAILED(run) && SUCCEEDED(overall)) overall = run;
            // A strict Smoke failure still has a real kernel exit and must not
            // prevent the serialized B control. Failed launch/wait also stops
            // its exact owned child before this loop can continue.
        }
        readback.sameExecutable = std::all_of(readback.arms.begin(), readback.arms.end(),
            [](const StartupDesktopArmReadback& arm) { return arm.preservation == S_OK; });
        std::wstring finalInput;
        hr = currentInput(finalInput);
        readback.inputUnchanged = SUCCEEDED(hr) && sameName(finalInput, originalInput) &&
            GetProcessWindowStation() == stationHandle;
        if (FAILED(hr) && SUCCEEDED(overall)) overall = hr;
        if (!readback.inputUnchanged && SUCCEEDED(overall)) overall = E_ACCESSDENIED;
        const auto ownedDesktop = privateDesktop.value;
        if (CloseDesktop(ownedDesktop)) { privateDesktop.value = nullptr; readback.finish = S_OK; }
        else readback.finish = failure();
        if (FAILED(readback.finish) && SUCCEEDED(overall)) overall = readback.finish;
        return overall;
    } catch (...) { return E_OUTOFMEMORY; }
}

} // namespace explorer
