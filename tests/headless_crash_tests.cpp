#include "explorer/headless_crash.hpp"
#include <dbghelp.h>
#include <objbase.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {
constexpr DWORD ownedException = 0xE0427430;

struct OwnedDirectory {
    std::filesystem::path path;
    ~OwnedDirectory() {
        // Every path is explicit and this unique directory contains only the
        // three files created below. Never recursively remove a computed tree.
        DeleteFileW((path / L"fault.dmp").c_str());
        DeleteFileW((path / L"existing.dmp").c_str());
        RemoveDirectoryW(path.c_str());
    }
};

bool check(bool value, const char* name) {
    if (!value) std::cerr << "FAIL: " << name << '\n';
    return value;
}
}

int wmain(int count, wchar_t** arguments) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (count == 3 && std::wstring_view(arguments[1]) == L"--owned-fault") {
        if (FAILED(explorer::initializeHeadlessCrashDump(true, arguments[2]))) return 2;
        const ULONG_PTR parameters[]{GetCurrentThreadId(), 0x42};
        RaiseException(ownedException, EXCEPTION_NONCONTINUABLE,
            static_cast<DWORD>(_countof(parameters)), parameters);
        return 3;
    }
    WCHAR temporary[MAX_PATH]{};
    WCHAR executable[32768]{};
    if (!GetTempPathW(_countof(temporary), temporary) ||
        !GetModuleFileNameW(nullptr, executable, _countof(executable))) return 1;
    GUID identity{};
    WCHAR guid[40]{};
    if (FAILED(CoCreateGuid(&identity)) || !StringFromGUID2(identity, guid, _countof(guid))) return 1;
    OwnedDirectory owned{std::filesystem::path(temporary) / (std::wstring(L"ExplorerCrashTest-") + guid)};
    if (!CreateDirectoryW(owned.path.c_str(), nullptr)) return 1;
    const auto dump = owned.path / L"fault.dmp";
    const auto existing = owned.path / L"existing.dmp";
    { std::ofstream file(existing, std::ios::binary); file << "owned sentinel"; }
    bool passed = check(explorer::initializeHeadlessCrashDump(false, dump) == E_ACCESSDENIED,
        "interactive option rejected");
    passed &= check(explorer::initializeHeadlessCrashDump(true, L"relative.dmp") == E_INVALIDARG,
        "relative filename rejected");
    passed &= check(explorer::initializeHeadlessCrashDump(true, existing) == HRESULT_FROM_WIN32(ERROR_FILE_EXISTS),
        "existing filename rejected");
    passed &= check(std::filesystem::file_size(existing) == 14 && !std::filesystem::exists(dump),
        "validation creates no dump and preserves existing file");
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --owned-fault \"" + dump.native() + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, owned.path.c_str(), &startup, &child)) return 1;
    const auto wait = WaitForSingleObject(child.hProcess, 30000);
    if (wait != WAIT_OBJECT_0) TerminateProcess(child.hProcess, 124);
    DWORD exitCode = 0;
    GetExitCodeProcess(child.hProcess, &exitCode);
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    passed &= check(wait == WAIT_OBJECT_0 && exitCode == ownedException,
        "original child exception terminates within bound");
    std::ifstream file(dump, std::ios::binary | std::ios::ate);
    if (!file) return 1;
    const auto size = file.tellg();
    if (size <= 0 || size > 32 * 1024 * 1024) return 1;
    std::vector<char> bytes(static_cast<size_t>(size));
    file.seekg(0);
    file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    PMINIDUMP_DIRECTORY directory = nullptr;
    PVOID stream = nullptr;
    ULONG streamSize = 0;
    const bool read = MiniDumpReadDumpStream(bytes.data(), ExceptionStream,
        &directory, &stream, &streamSize) != FALSE;
    passed &= check(read && stream && streamSize >= sizeof(MINIDUMP_EXCEPTION_STREAM),
        "original exception stream exists");
    if (read && stream && streamSize >= sizeof(MINIDUMP_EXCEPTION_STREAM)) {
        const auto& fault = *static_cast<MINIDUMP_EXCEPTION_STREAM*>(stream);
        passed &= check(fault.ExceptionRecord.ExceptionCode == ownedException &&
            fault.ExceptionRecord.NumberParameters == 2 &&
            fault.ExceptionRecord.ExceptionInformation[0] == fault.ThreadId &&
            fault.ExceptionRecord.ExceptionInformation[1] == 0x42 &&
            fault.ExceptionRecord.ExceptionAddress != 0 &&
            fault.ThreadContext.DataSize != 0 && fault.ThreadContext.Rva != 0,
            "original thread record parameters and processor context retained");
    }
    if (passed) std::cout << "PASS: process-local headless crash diagnostics\n";
    return passed ? 0 : 1;
}
