#include "explorer/ribbon_features.hpp"
#include "explorer/headless_visual.hpp"

#include <mfapi.h>
#include <mferror.h>
#include <shlobj.h>
#include <array>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void succeeded(HRESULT result, const char* message) {
    if (FAILED(result)) throw std::runtime_error(std::string(message) + " HRESULT=" +
        std::to_string(static_cast<unsigned long>(result)));
}

struct RegistryValue {
    LSTATUS status = ERROR_SUCCESS;
    DWORD type = 0;
    DWORD length = 0;
    std::array<BYTE, 64> bytes{};
    bool operator==(const RegistryValue&) const = default;
};
RegistryValue readValue(HKEY hive, const wchar_t* path, const wchar_t* name) {
    RegistryValue result;
    result.length = static_cast<DWORD>(result.bytes.size());
    result.status = RegGetValueW(hive, path, name, RRF_RT_ANY, &result.type,
                                result.bytes.data(), &result.length);
    return result;
}
auto settings() {
    constexpr auto policies = L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer";
    constexpr auto advanced = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";
    return std::array{
        readValue(HKEY_CURRENT_USER, policies, L"NoCDBurning"),
        readValue(HKEY_LOCAL_MACHINE, policies, L"NoCDBurning"),
        readValue(HKEY_CURRENT_USER, advanced, L"HideFileExt"),
        readValue(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\BitLocker", L"PreventDeviceEncryption")};
}

struct MediaApi {
    HMODULE module = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    using Startup = HRESULT (WINAPI*)(ULONG, DWORD);
    using Shutdown = HRESULT (WINAPI*)();
    using Allocate = HRESULT (WINAPI*)(DWORD*);
    using Unlock = HRESULT (WINAPI*)(DWORD);
    Startup startup = nullptr;
    Shutdown shutdown = nullptr;
    Allocate allocate = nullptr;
    Unlock unlock = nullptr;
    bool started = false;

    template<class T> void resolve(T& function, const char* name) {
        const auto address = module ? GetProcAddress(module, name) : nullptr;
        static_assert(sizeof(function) == sizeof(address));
        std::memcpy(&function, &address, sizeof(function));
    }
    MediaApi() {
        resolve(startup, "MFStartup"); resolve(shutdown, "MFShutdown");
        resolve(allocate, "MFAllocateWorkQueue"); resolve(unlock, "MFUnlockWorkQueue");
    }
    ~MediaApi() {
        if (started) shutdown();
        if (module) FreeLibrary(module);
    }
};

void verifyContract(const explorer::RibbonFeatures& value, HRESULT status) {
    const std::array checks{value.editionStatus, value.mediaFoundationStatus,
                            value.discBurningStatus, value.diskCleanupStatus};
    HRESULT expected = S_OK;
    for (const auto item : checks) {
        require(item != E_PENDING, "Detector left a capability pending");
        if (SUCCEEDED(expected) && FAILED(item)) expected = item;
    }
    require(status == expected, "Overall detector HRESULT lost individual provenance");
    require(value.discBurningStatus == S_OK, "Documented policy query failed");
}

void metadataTests() {
    require(explorer::installedRibbonFeatures(nullptr) == E_POINTER, "Null output was not rejected");
    explorer::RibbonFeatures value;
    const auto status = explorer::installedRibbonFeatures(&value);
    verifyContract(value, status);
    DWORD product = PRODUCT_UNDEFINED;
    require(GetProductInfo(10, 0, 0, 0, &product) != FALSE, "Read-only native SKU query failed");
    if (product == PRODUCT_CORE || product == PRODUCT_CORE_N ||
        product == PRODUCT_CORE_COUNTRYSPECIFIC || product == PRODUCT_CORE_SINGLELANGUAGE)
        require(value.editionStatus == S_OK && !value.bitLocker, "Home advertised ordinary BitLocker Ribbon tools");
    else if (product == PRODUCT_PROFESSIONAL || product == PRODUCT_PROFESSIONAL_N ||
             product == PRODUCT_EDUCATION || product == PRODUCT_EDUCATION_N ||
             product == PRODUCT_ENTERPRISE || product == PRODUCT_ENTERPRISE_N)
        require(value.editionStatus == S_OK && value.bitLocker, "Supported client edition lost BitLocker tools");
    else if (FAILED(value.editionStatus))
        require(value.bitLocker, "Unclassified SKU hid a feature without installed-component proof");

    require(value.discBurning == (SHRestricted(REST_NOCDBURNING) == 0), "Ribbon ignored native administrator burning policy");
    std::array<wchar_t, MAX_PATH + 1> system{};
    const auto length = GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
    require(length > 0 && length < system.size(), "Native system directory query failed");
    const auto cleanup = std::filesystem::path(system.data()) / L"cleanmgr.exe";
    std::error_code error;
    const bool cleanupInstalled = std::filesystem::is_regular_file(cleanup, error);
    if (!error) require(value.diskCleanup == cleanupInstalled, "Cleanup availability differs from the actual system executable");
    std::cout << "PASS: null contract, filled provenance, installed SKU, Shell policy and actual cleanup metadata\n"
              << "FEATURES product=" << product << " bitLocker=" << value.bitLocker
              << " mediaFoundation=" << value.mediaFoundation << " discBurning=" << value.discBurning
              << " diskCleanup=" << value.diskCleanup << '\n'
              << "STATUS edition=" << static_cast<unsigned long>(value.editionStatus)
              << " media=" << static_cast<unsigned long>(value.mediaFoundationStatus)
              << " burning=" << static_cast<unsigned long>(value.discBurningStatus)
              << " cleanup=" << static_cast<unsigned long>(value.diskCleanupStatus) << '\n';
}

void mediaLifetimeTests() {
    MediaApi api;
    if (!api.module || !api.startup || !api.shutdown || !api.allocate || !api.unlock) {
        explorer::RibbonFeatures value;
        explorer::installedRibbonFeatures(&value);
        require(FAILED(value.mediaFoundationStatus), "Absent public MF component advertised a successful check");
        std::cout << "PASS: unavailable Media Foundation retains diagnostic HRESULT\n";
        return;
    }
    const auto startup = api.startup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(startup)) {
        explorer::RibbonFeatures value;
        explorer::installedRibbonFeatures(&value);
        require(value.mediaFoundationStatus == startup, "Media startup error was replaced with a guess");
        if (startup == E_NOTIMPL || startup == MF_E_DISABLED_IN_SAFEMODE)
            require(!value.mediaFoundation, "Unavailable Media Foundation advertised media tools");
        std::cout << "PASS: native Media Foundation startup absence/error provenance\n";
        return;
    }
    api.started = true;
    for (int index = 0; index != 3; ++index) {
        explorer::RibbonFeatures value;
        explorer::installedRibbonFeatures(&value);
        require(value.mediaFoundation && value.mediaFoundationStatus == S_OK, "Installed public media initialization failed");
        DWORD queue = 0;
        succeeded(api.allocate(&queue), "Detector shut down the caller's existing Media Foundation platform");
        succeeded(api.unlock(queue), "Release owned empty Media Foundation work queue");
    }
    succeeded(api.shutdown(), "Release caller's Media Foundation startup");
    api.started = false;
    DWORD queue = 0;
    const auto afterShutdown = api.allocate(&queue);
    if (SUCCEEDED(afterShutdown)) api.unlock(queue);
    require(afterShutdown == MF_E_SHUTDOWN, "Detector leaked a Media Foundation startup reference");
    std::cout << "PASS: repeated detector checks preserve caller lifetime and leave no Media Foundation startup references\n";
}

} // namespace

int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    explorer::PrivateDesktop desktop;
    int result = 0;
    try {
        succeeded(desktop.initialize(), "Initialize private test desktop");
        const auto before = settings();
        const auto clipboard = GetClipboardSequenceNumber();
        metadataTests();
        mediaLifetimeTests();
        require(settings() == before, "Read-only detector changed user/system settings");
        require(GetClipboardSequenceNumber() == clipboard, "Capability detection changed clipboard state");
        bool inputUnchanged = false, inputVisible = true;
        succeeded(desktop.verifyIsolation(&inputUnchanged), "Verify private desktop isolation");
        succeeded(desktop.visibleWindowsOnInputDesktop(inputVisible), "Verify no process windows on input desktop");
        require(inputUnchanged && !inputVisible, "Capability detection exposed input desktop UI");
        std::cout << "PASS: settings and clipboard unchanged, no input desktop windows\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        result = 1;
    }
    return result;
}
