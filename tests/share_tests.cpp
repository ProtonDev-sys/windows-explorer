#include "explorer/share.hpp"

#include <shlobj.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
namespace fs = std::filesystem;
namespace transfer = winrt::Windows::ApplicationModel::DataTransfer;
namespace foundation = winrt::Windows::Foundation;
using explorer::NativeShare;
using explorer::SharePayload;

void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void succeeded(HRESULT hr, const char* message) {
    if (FAILED(hr)) {
        std::cerr << message << " (HRESULT 0x" << std::hex << static_cast<unsigned long>(hr) << std::dec << ")\n";
        throw std::runtime_error(message);
    }
}
struct Fixture {
    fs::path root;
    Fixture() {
        GUID guid{};
        succeeded(CoCreateGuid(&guid), "create Share fixture id");
        wchar_t text[40]{};
        require(StringFromGUID2(guid, text, 40) != 0, "format Share fixture id");
        root = fs::temp_directory_path() / (std::wstring(L"windows-explorer-share-資料-") + text);
        require(fs::create_directory(root), "create exclusive Share fixture directory");
    }
    ~Fixture() { std::error_code ignored; fs::remove_all(root, ignored); }
    fs::path file(const wchar_t* name) {
        const fs::path result = root / name;
        std::ofstream stream(result, std::ios::binary);
        stream << "owned Share fixture";
        require(stream.good(), "write Share fixture");
        return result;
    }
};
struct HiddenWindow {
    HWND value = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"Headless Share interoperability test",
                                 WS_OVERLAPPED, 0, 0, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    HiddenWindow() { require(value != nullptr && !IsWindowVisible(value), "create hidden Share test window"); }
    ~HiddenWindow() { if (value) DestroyWindow(value); }
};
winrt::com_ptr<IShellItemArray> selection(const fs::path& file) {
    winrt::com_ptr<IShellItem> item;
    succeeded(SHCreateItemFromParsingName(file.c_str(), nullptr, __uuidof(IShellItem), item.put_void()), "create Share shell item");
    winrt::com_ptr<IShellItemArray> result;
    succeeded(SHCreateShellItemArrayFromShellItem(item.get(), __uuidof(IShellItemArray), result.put_void()), "create Share shell selection");
    return result;
}
template <typename Async> void waitFor(const Async& operation) {
    const ULONGLONG start = GetTickCount64();
    while (operation.Status() == foundation::AsyncStatus::Started && GetTickCount64() - start < 10000) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    if (operation.Status() == foundation::AsyncStatus::Started) {
        operation.Cancel();
        throw std::runtime_error("headless Share package resolution timed out");
    }
}
transfer::DataPackage package(NativeShare& share, const SharePayload& payload) {
    IInspectable* value = nullptr;
    succeeded(share.makePackage(payload, &value), "create deferred Share package without UI");
    require(value != nullptr, "Share package is null");
    return {value, winrt::take_ownership_from_abi};
}
std::string contents(const fs::path& file) {
    std::ifstream stream(file, std::ios::binary);
    require(stream.good(), "read owned Share fixture");
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void planning() {
    Fixture fixture;
    const fs::path first = fixture.file(L"日本語.txt"), second = fixture.file(L"different.TXT"), third = fixture.file(L"image.png");
    SharePayload payload;
    succeeded(explorer::makeSharePayload({first, second, third}, L"Share 資料", payload), "plan Unicode multi-file Share payload");
    require(payload.title == L"Share 資料", "Share title changed");
    require(payload.files.size() == 3 && payload.files[0] == first && payload.files[1] == second && payload.files[2] == third, "Share file selection or order changed");
    require(payload.fileTypes == std::vector<std::wstring>{L".txt", L".png"}, "Share file type metadata is missing or duplicated");
    const auto one = selection(first);
    succeeded(explorer::makeSharePayload(one.get(), L"One file", payload), "plan native Shell Share selection");
    require(payload.files.size() == 1 && fs::equivalent(payload.files.front(), first), "native Share selection differs");

    // Windows exposes a ZIP as a Shell folder, although it is physically a
    // regular file. Use a valid empty classic ZIP and the native Shell array.
    const fs::path archive = fixture.root / L"資料 archive.zip";
    {
        const unsigned char emptyZip[22]{0x50, 0x4b, 0x05, 0x06};
        std::ofstream stream(archive, std::ios::binary);
        stream.write(reinterpret_cast<const char*>(emptyZip), sizeof(emptyZip));
        require(stream.good(), "write owned ZIP Share fixture");
    }
    const auto zip = selection(archive);
    winrt::com_ptr<IShellItem> zipItem;
    succeeded(zip->GetItemAt(0, zipItem.put()), "read native ZIP Share selection");
    SFGAOF attributes = 0;
    succeeded(zipItem->GetAttributes(SFGAO_FILESYSTEM | SFGAO_FOLDER, &attributes), "read native ZIP Shell attributes");
    require((attributes & SFGAO_FILESYSTEM) != 0 && !fs::is_directory(archive), "ZIP Share fixture is not a filesystem file");
    succeeded(explorer::makeSharePayload(zip.get(), L"Share archive", payload), "plan native Shell ZIP Share selection");
    require(payload.files.size() == 1 && fs::equivalent(payload.files.front(), archive) && payload.fileTypes == std::vector<std::wstring>{L".zip"}, "native ZIP Share payload differs");
    if (attributes & SFGAO_FOLDER)
        std::cout << "INFO: Shell folder semantics on a ZIP preserve regular-file Share support\n";
}

void invalidPlanning() {
    Fixture fixture;
    const fs::path file = fixture.file(L"existing.txt");
    SharePayload output{L"unchanged", {file}, {L"unchanged"}};
    auto fails = [&](const std::vector<fs::path>& paths, const std::wstring& title) {
        require(FAILED(explorer::makeSharePayload(paths, title, output)), "invalid Share selection accepted");
        require(output.title == L"unchanged" && output.files == std::vector<fs::path>{file} && output.fileTypes == std::vector<std::wstring>{L"unchanged"}, "failed Share planning changed output");
    };
    fails({}, L"Share"); fails({file}, L""); fails({file}, L"   "); fails({file}, L"bad\nTitle");
    fails({file}, std::wstring(explorer::maximumShareTitle + 1, L'a'));
    fails({file}, std::wstring(1, static_cast<wchar_t>(0xd800)));
    fails({file, file}, L"Share"); fails({fixture.root}, L"Share");
    fails({fixture.root / L"missing"}, L"Share"); fails({fs::path(file.native() + L":stream")}, L"Share");
    fails(std::vector<fs::path>(explorer::maximumShareFiles + 1, file), L"Share");
    require(explorer::makeSharePayload(static_cast<IShellItemArray*>(nullptr), L"Share", output) == E_INVALIDARG, "null Share selection accepted");
    const auto folder = selection(fixture.root);
    require(explorer::makeSharePayload(folder.get(), L"Share", output) == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), "Share folder selection silently treated as files");
    winrt::com_ptr<IShellItem> virtualItem;
    succeeded(SHGetKnownFolderItem(FOLDERID_ComputerFolder, KF_FLAG_DEFAULT, nullptr, __uuidof(IShellItem), virtualItem.put_void()), "create virtual Share selection");
    winrt::com_ptr<IShellItemArray> virtualSelection;
    succeeded(SHCreateShellItemArrayFromShellItem(virtualItem.get(), __uuidof(IShellItemArray), virtualSelection.put_void()), "create virtual Share array");
    require(explorer::makeSharePayload(virtualSelection.get(), L"Share", output) == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), "Share virtual items accepted");
}

void capability() {
    HiddenWindow hidden;
    NativeShare share;
    require(!share.ready() && share.lastResult() == CO_E_NOTINITIALIZED, "uninitialized Share capability is wrong");
    require(share.initialize(nullptr) == HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE), "Share accepted missing window");
    require(share.initialize(hidden.value, WM_USER + 1) == E_INVALIDARG, "Share accepted reserved completion message");
    succeeded(share.initialize(hidden.value), "discover desktop Share API using hidden HWND");
    require(share.ready() && !IsWindowVisible(hidden.value), "Share initialization displayed a window");
    HRESULT otherThread = S_OK;
    std::thread worker([&] { otherThread = share.initialize(hidden.value); }); worker.join();
    require(otherThread == RPC_E_WRONG_THREAD, "Share initialized from the wrong window thread");
    share.reset();
    require(!share.ready(), "Share reset retained event subscription/API state");
    succeeded(share.initialize(hidden.value), "reinitialize Share after event cleanup");
    share.reset();
}

void deferredFiles() {
    Fixture fixture;
    const fs::path first = fixture.file(L"資料.txt"), second = fixture.file(L"photo.png");
    SharePayload payload;
    succeeded(explorer::makeSharePayload({first, second}, L"Files 資料", payload), "plan deferred Share files");
    HiddenWindow hidden;
    NativeShare share;
    succeeded(share.initialize(hidden.value), "initialize hidden Share source");
    const auto data = package(share, payload);
    const auto view = data.GetView();
    require(view.Properties().Title() == L"Files 資料" && view.RequestedOperation() == transfer::DataPackageOperation::Copy, "Share package title/operation is wrong");
    require(view.Contains(transfer::StandardDataFormats::StorageItems()), "Share package does not advertise storage items");
    const auto files = view.GetStorageItemsAsync();
    waitFor(files);
    const auto items = files.GetResults();
    require(items.Size() == 2, "deferred Share package lost selected files");
    require(fs::equivalent(fs::path(items.GetAt(0).Path().c_str()), first) && fs::equivalent(fs::path(items.GetAt(1).Path().c_str()), second), "resolved Share storage file identity differs");
    require(share.lastResult() == S_OK && !IsWindowVisible(hidden.value), "Share provider failed or displayed UI");
    require(contents(first) == "owned Share fixture" && contents(second) == "owned Share fixture", "Share provider changed file contents");
    share.reset();
}

void providerFailureAndLifetime() {
    Fixture fixture;
    const fs::path file = fixture.file(L"removed after planning.txt");
    SharePayload payload;
    succeeded(explorer::makeSharePayload({file}, L"Missing later", payload), "plan Share race fixture");
    HiddenWindow hidden;
    NativeShare share;
    succeeded(share.initialize(hidden.value), "initialize hidden Share failure source");
    const auto data = package(share, payload);
    require(fs::remove(file), "remove owned Share race fixture");
    const auto request = data.GetView().GetStorageItemsAsync();
    waitFor(request);
    bool failed = false;
    try { static_cast<void>(request.GetResults()); } catch (const winrt::hresult_error&) { failed = true; }
    require(failed, "Share provider silently succeeded after selected file disappeared");
    const HRESULT result = share.lastResult();
    require(result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || result == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND), "Share provider lost original missing-file HRESULT");

    const fs::path retained = fixture.file(L"retained.txt");
    succeeded(explorer::makeSharePayload({retained}, L"Source closes", payload), "plan Share lifetime fixture");
    const auto retainedPackage = package(share, payload);
    share.reset(); // Provider retains safe shared state, never the destroyed app object.
    const auto cancelled = retainedPackage.GetView().GetStorageItemsAsync();
    waitFor(cancelled);
    failed = false;
    try { static_cast<void>(cancelled.GetResults()); } catch (const winrt::hresult_error&) { failed = true; }
    require(failed, "Share provider continued after source shutdown");
    require(fs::exists(retained), "Share cancellation changed source file");
}
}

int runShareTests() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests{
        {"Share Unicode files, extensions and native selection model", planning},
        {"Share invalid, missing, folder, virtual and bounded selections", invalidPlanning},
        {"Share hidden desktop interop, STA ownership and event cleanup", capability},
        {"Share asynchronous real StorageItems payload without recipient UI", deferredFiles},
        {"Share deferred failure HRESULT and source shutdown lifetime", providerFailureAndLifetime}
    };
    int failures = 0;
    for (const auto& [name, test] : tests) {
        try { test(); std::cout << "PASS: " << name << '\n'; }
        catch (const std::exception& error) { ++failures; std::cerr << "FAIL: " << name << ": " << error.what() << '\n'; }
        catch (...) { ++failures; std::cerr << "FAIL: " << name << ": unknown exception\n"; }
    }
    return failures;
}
