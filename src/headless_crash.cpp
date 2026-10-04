#include "explorer/headless_crash.hpp"

#include <dbghelp.h>
#include <cwchar>

namespace explorer {
namespace {

// No C++ allocation, COM, or loader call occurs in the exception filter.
// DbgHelp stays loaded until process termination, including an early crash.
WCHAR dumpFilename[32768]{};
using WriteDump = BOOL (WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
    PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION,
    PMINIDUMP_CALLBACK_INFORMATION);
WriteDump writeDump = nullptr;
volatile LONG dumpAttempted = 0;
volatile LONG configured = 0;

LONG WINAPI captureUnhandledException(EXCEPTION_POINTERS* exception) noexcept {
    if (exception && writeDump && dumpFilename[0] &&
        InterlockedCompareExchange(&dumpAttempted, 1, 0) == 0) {
        const auto file = CreateFileW(dumpFilename, GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION context{};
            context.ThreadId = GetCurrentThreadId();
            context.ExceptionPointers = exception;
            context.ClientPointers = FALSE;
            writeDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                MiniDumpNormal, &context, nullptr, nullptr);
            CloseHandle(file);
        }
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace

HRESULT initializeHeadlessCrashDump(bool headless,
    const std::filesystem::path& filename) noexcept {
    if (!headless) return E_ACCESSDENIED;
    try {
        if (filename.empty() || !filename.is_absolute() ||
            !filename.has_filename() || filename.filename() == L"." ||
            filename.filename() == L"..") return E_INVALIDARG;
        // A stream name is not a new standalone dump file.
        if (filename.filename().native().find(L':') != std::wstring::npos)
            return E_INVALIDARG;
        const auto& value = filename.native();
        if (value.size() >= _countof(dumpFilename) ||
            value.find(L'\0') != std::wstring::npos) return E_INVALIDARG;
        // Directory and target checks are read-only. CREATE_NEW repeats the
        // exclusive target check atomically when a fault actually occurs.
        const auto targetAttributes = GetFileAttributesW(value.c_str());
        if (targetAttributes != INVALID_FILE_ATTRIBUTES)
            return HRESULT_FROM_WIN32(ERROR_FILE_EXISTS);
        const auto targetError = GetLastError();
        if (targetError != ERROR_FILE_NOT_FOUND)
            return HRESULT_FROM_WIN32(targetError);
        const auto parent = filename.parent_path().native();
        const auto parentAttributes = GetFileAttributesW(parent.c_str());
        if (parentAttributes == INVALID_FILE_ATTRIBUTES)
            return HRESULT_FROM_WIN32(GetLastError());
        if (!(parentAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (parentAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return E_INVALIDARG;
        if (InterlockedCompareExchange(&configured, 1, 0) != 0)
            return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        const auto library = LoadLibraryExW(L"dbghelp.dll", nullptr,
            LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!library) {
            const auto error = GetLastError();
            InterlockedExchange(&configured, 0);
            return HRESULT_FROM_WIN32(error);
        }
        const auto function = GetProcAddress(library, "MiniDumpWriteDump");
        if (!function) {
            const auto error = GetLastError();
            FreeLibrary(library);
            InterlockedExchange(&configured, 0);
            return HRESULT_FROM_WIN32(error);
        }
        // DbgHelp can defer loading its dump writer. Preload that system
        // dependency too, while the process is healthy and before COM.
        const auto dumpCore = LoadLibraryExW(L"dbgcore.dll", nullptr,
            LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!dumpCore) {
            const auto error = GetLastError();
            FreeLibrary(library);
            InterlockedExchange(&configured, 0);
            return HRESULT_FROM_WIN32(error);
        }
        writeDump = reinterpret_cast<WriteDump>(function);
        std::wmemcpy(dumpFilename, value.c_str(), value.size() + 1);
        SetUnhandledExceptionFilter(captureUnhandledException);
        return S_OK;
    } catch (...) {
        return E_OUTOFMEMORY;
    }
}

} // namespace explorer
