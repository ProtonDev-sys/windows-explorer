#include "explorer/ribbon_features.hpp"

#include <mfapi.h>
#include <mferror.h>
#include <shlobj.h>
#include <array>
#include <cstring>

namespace explorer {
namespace {

HRESULT editionFeature(bool& available) noexcept {
    DWORD product = PRODUCT_UNDEFINED;
    // A current maximum target avoids mapping a newer client SKU to an older
    // product family. GetProductInfo does not depend on a version manifest.
    if (!GetProductInfo(10, 0, 0, 0, &product)) return E_FAIL;
    switch (product) {
    case PRODUCT_CORE:
    case PRODUCT_CORE_N:
    case PRODUCT_CORE_COUNTRYSPECIFIC:
    case PRODUCT_CORE_SINGLELANGUAGE:
        available = false;
        return S_OK;
    case PRODUCT_PROFESSIONAL:
    case PRODUCT_PROFESSIONAL_N:
    case PRODUCT_PRO_WORKSTATION:
    case PRODUCT_PRO_WORKSTATION_N:
    case PRODUCT_PRO_FOR_EDUCATION:
    case PRODUCT_PRO_FOR_EDUCATION_N:
    case PRODUCT_EDUCATION:
    case PRODUCT_EDUCATION_N:
    case PRODUCT_ENTERPRISE:
    case PRODUCT_ENTERPRISE_N:
    case PRODUCT_ENTERPRISE_E:
    case PRODUCT_ENTERPRISE_EVALUATION:
    case PRODUCT_ENTERPRISE_N_EVALUATION:
    case PRODUCT_ENTERPRISE_S:
    case PRODUCT_ENTERPRISE_S_N:
    case PRODUCT_ENTERPRISE_S_EVALUATION:
    case PRODUCT_ENTERPRISE_S_N_EVALUATION:
    case PRODUCT_ENTERPRISE_SUBSCRIPTION:
    case PRODUCT_ENTERPRISE_SUBSCRIPTION_N:
    case PRODUCT_SERVERRDSH:
        available = true;
        return S_OK;
    default:
        // Server BitLocker is an optional component; the client edition table
        // does not establish whether it is installed. Neither do unknown SKUs.
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    }
}

bool missingFile(DWORD error) noexcept {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ||
           error == ERROR_MOD_NOT_FOUND;
}

HRESULT mediaFeature(bool& available) noexcept {
    const auto module = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) {
        const auto error = GetLastError();
        if (missingFile(error)) available = false;
        return HRESULT_FROM_WIN32(error ? error : ERROR_DLL_INIT_FAILED);
    }
    using Startup = HRESULT (WINAPI*)(ULONG, DWORD);
    using Shutdown = HRESULT (WINAPI*)();
    Startup startup = nullptr;
    Shutdown shutdown = nullptr;
    const auto startupAddress = GetProcAddress(module, "MFStartup");
    const auto shutdownAddress = GetProcAddress(module, "MFShutdown");
    static_assert(sizeof(startup) == sizeof(startupAddress));
    static_assert(sizeof(shutdown) == sizeof(shutdownAddress));
    std::memcpy(&startup, &startupAddress, sizeof(startup));
    std::memcpy(&shutdown, &shutdownAddress, sizeof(shutdown));
    HRESULT status = HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    if (startup && shutdown) {
        // This does not start sockets, create media sessions, or inspect devices.
        status = startup(MF_VERSION, MFSTARTUP_LITE);
        if (SUCCEEDED(status)) status = shutdown();
        else if (status == E_NOTIMPL || status == MF_E_DISABLED_IN_SAFEMODE)
            available = false;
    }
    FreeLibrary(module);
    return status;
}

HRESULT cleanupFeature(bool& available) noexcept {
    std::array<wchar_t, MAX_PATH + 32> path{};
    const auto length = GetSystemDirectoryW(path.data(), static_cast<UINT>(path.size()));
    if (!length) {
        const auto error = GetLastError();
        return HRESULT_FROM_WIN32(error ? error : ERROR_PATH_NOT_FOUND);
    }
    constexpr wchar_t executable[] = L"\\cleanmgr.exe";
    if (length >= path.size() || length + std::size(executable) > path.size())
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    std::memcpy(path.data() + length, executable, sizeof(executable));
    const auto attributes = GetFileAttributesW(path.data());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const auto error = GetLastError();
        if (missingFile(error)) available = false;
        return HRESULT_FROM_WIN32(error ? error : ERROR_FILE_NOT_FOUND);
    }
    available = (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    return available ? S_OK : HRESULT_FROM_WIN32(ERROR_DIRECTORY);
}

} // namespace

HRESULT installedRibbonFeatures(RibbonFeatures* result) noexcept {
    if (!result) return E_POINTER;
    *result = {};
    result->editionStatus = editionFeature(result->bitLocker);
    result->mediaFoundationStatus = mediaFeature(result->mediaFoundation);
    result->discBurning = SHRestricted(REST_NOCDBURNING) == 0;
    result->discBurningStatus = S_OK;
    result->diskCleanupStatus = cleanupFeature(result->diskCleanup);
    for (const auto status : {result->editionStatus, result->mediaFoundationStatus,
                              result->discBurningStatus, result->diskCleanupStatus})
        if (FAILED(status)) return status;
    return S_OK;
}

} // namespace explorer
