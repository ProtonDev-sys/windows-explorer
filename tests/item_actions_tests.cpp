#include "explorer/item_actions.hpp"

#include <shlobj.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::ClassicCom;
using explorer::ItemActions;
namespace fs = std::filesystem;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void succeeded(HRESULT hr, const char* message) {
    if (FAILED(hr)) {
        std::cerr << message << " (HRESULT 0x" << std::hex
                  << static_cast<unsigned long>(hr) << std::dec << ")\n";
        throw std::runtime_error(message);
    }
}

std::string diagnosticUtf8(const std::wstring& text) {
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!bytes) return text.empty() ? "" : "<invalid UTF-16>";
    std::string utf8(static_cast<size_t>(bytes), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), utf8.data(), bytes, nullptr, nullptr)) return "<UTF-8 conversion failed>";
    std::string escaped;
    for (const char character : utf8) {
        if (character == '\r') escaped += "\\r";
        else if (character == '\n') escaped += "\\n";
        else if (character == '\t') escaped += "\\t";
        else escaped += character;
    }
    return escaped;
}

void equalText(const std::wstring& actual, const std::wstring& expected, const char* message) {
    if (actual == expected) return;
    std::cerr << message << "\n  expected (UTF-8): " << diagnosticUtf8(expected)
              << "\n  actual (UTF-8):   " << diagnosticUtf8(actual) << '\n';
    throw std::runtime_error(message);
}

std::wstring displayName(IShellItem* item, SIGDN kind) {
    PWSTR raw = nullptr;
    const HRESULT hr = item->GetDisplayName(kind, &raw);
    struct StringDeleter { void operator()(wchar_t* value) const { CoTaskMemFree(value); } };
    const std::unique_ptr<wchar_t, StringDeleter> owned(raw);
    succeeded(hr, "read expected native Shell display name");
    require(owned && *owned, "expected native Shell display name is nonempty");
    return owned.get();
}

struct TaskDeleter {
    using pointer = LPITEMIDLIST;
    void operator()(pointer value) const noexcept { CoTaskMemFree(value); }
};
using Pidl = std::unique_ptr<ITEMIDLIST, TaskDeleter>;

ComPtr<IShellItem> shellItem(const fs::path& path) {
    ComPtr<IShellItem> result;
    succeeded(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&result)),
              "create fixture shell item");
    return result;
}

ComPtr<IShellItemArray> selectItems(const std::vector<ComPtr<IShellItem>>& items) {
    std::vector<Pidl> pidls;
    std::vector<PCIDLIST_ABSOLUTE> rawPidls;
    for (const auto& item : items) {
        PIDLIST_ABSOLUTE raw = nullptr;
        succeeded(SHGetIDListFromObject(item.Get(), &raw), "read fixture PIDL");
        pidls.emplace_back(raw);
        rawPidls.push_back(raw);
    }
    ComPtr<IShellItemArray> result;
    succeeded(SHCreateShellItemArrayFromIDLists(static_cast<UINT>(rawPidls.size()),
              rawPidls.data(), &result), "create fixture selection");
    return result;
}

void writeFile(const fs::path& path) {
    std::ofstream stream(path, std::ios::binary);
    stream << "headless item-action fixture";
    require(stream.good(), "write owned fixture");
}

struct Fixture {
    fs::path root;
    Fixture() {
        GUID guid{};
        succeeded(CoCreateGuid(&guid), "create fixture identifier");
        wchar_t identifier[40]{};
        require(StringFromGUID2(guid, identifier, 40) > 0, "format fixture identifier");
        root = fs::temp_directory_path() / (std::wstring(L"windows-explorer-attributes-") + identifier);
        require(fs::create_directory(root), "create owned fixture directory");
    }
    ~Fixture() {
        std::error_code error;
        fs::remove_all(root, error);
    }
};

class StubItem final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IShellItem> {
public:
    SFGAOF attributes = SFGAO_FILESYSTEM;
    HRESULT attributeResult = S_OK;
    HRESULT pathResult = S_OK;
    HRESULT parsingResult = S_OK;
    std::wstring path = L"C:\\fixture path\\\u65e5\u672c.txt";
    std::wstring parsing = L"::{fixture-virtual-item}";
    IFACEMETHODIMP BindToHandler(IBindCtx*, REFGUID, REFIID, void**) override { return E_NOTIMPL; }
    IFACEMETHODIMP GetParent(IShellItem**) override { return E_NOTIMPL; }
    IFACEMETHODIMP GetDisplayName(SIGDN name, LPWSTR* result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        const bool filesystem = name == SIGDN_FILESYSPATH;
        const HRESULT hr = filesystem ? pathResult : parsingResult;
        if (FAILED(hr)) return hr;
        const auto& content = filesystem ? path : parsing;
        auto* copy = static_cast<wchar_t*>(CoTaskMemAlloc((content.size() + 1) * sizeof(wchar_t)));
        if (!copy) return E_OUTOFMEMORY;
        std::copy(content.begin(), content.end(), copy);
        copy[content.size()] = L'\0';
        *result = copy;
        return hr;
    }
    IFACEMETHODIMP GetAttributes(SFGAOF mask, SFGAOF* result) override {
        if (!result) return E_POINTER;
        if (FAILED(attributeResult)) return attributeResult;
        *result = attributes & mask;
        return attributeResult;
    }
    IFACEMETHODIMP Compare(IShellItem*, SICHINTF, int*) override { return E_NOTIMPL; }
};

class StubArray final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IShellItemArray> {
public:
    std::vector<ComPtr<IShellItem>> items;
    HRESULT countResult = S_OK;
    HRESULT itemResult = S_OK;
    IFACEMETHODIMP BindToHandler(IBindCtx*, REFGUID, REFIID, void**) override { return E_NOTIMPL; }
    IFACEMETHODIMP GetPropertyStore(GETPROPERTYSTOREFLAGS, REFIID, void**) override { return E_NOTIMPL; }
    IFACEMETHODIMP GetPropertyDescriptionList(REFPROPERTYKEY, REFIID, void**) override { return E_NOTIMPL; }
    IFACEMETHODIMP GetAttributes(SIATTRIBFLAGS, SFGAOF, SFGAOF*) override { return E_NOTIMPL; }
    IFACEMETHODIMP GetCount(DWORD* count) override {
        if (!count) return E_POINTER;
        if (FAILED(countResult)) return countResult;
        *count = static_cast<DWORD>(items.size());
        return countResult;
    }
    IFACEMETHODIMP GetItemAt(DWORD index, IShellItem** item) override {
        if (!item) return E_POINTER;
        *item = nullptr;
        if (FAILED(itemResult)) return itemResult;
        return index < items.size() ? items[index].CopyTo(item) : E_INVALIDARG;
    }
    IFACEMETHODIMP EnumItems(IEnumShellItems**) override { return E_NOTIMPL; }
};

void formatting() {
    std::wstring result = L"unchanged";
    succeeded(explorer::formatQuotedPaths({L"C:\\space here\\\u65e5\u672c\U0001f4c1.txt",
                                          L"\\\\server\\share\\second.txt"}, result),
              "format Unicode and UNC paths");
    require(result == L"\"C:\\space here\\\u65e5\u672c\U0001f4c1.txt\"\r\n"
                      L"\"\\\\server\\share\\second.txt\"", "every path quoted with CRLF");
    succeeded(explorer::formatQuotedPaths({L"::{virtual parsing name}"}, result),
              "format virtual path");
    require(result == L"\"::{virtual parsing name}\"", "single path has no trailing line");
    succeeded(explorer::formatQuotedPaths({L"C:\\TEMP~1\\FILE~1.TXT", L"C:\\directory\\.\\file.txt"}, result),
              "format literal path aliases");
    equalText(result, L"\"C:\\TEMP~1\\FILE~1.TXT\"\r\n\"C:\\directory\\.\\file.txt\"",
              "pure formatter preserves provider path spelling verbatim");
    const auto unchanged = result;
    require(explorer::formatQuotedPaths({}, result) == E_INVALIDARG, "empty formatter selection rejected");
    require(result == unchanged, "empty formatter preserves output");
    for (const auto& invalid : std::vector<std::wstring>{L"", L"a\rb", L"a\nb", std::wstring(L"a\0b", 3)}) {
        require(explorer::formatQuotedPaths({L"valid", invalid}, result) == HRESULT_FROM_WIN32(ERROR_INVALID_NAME),
                "invalid path line rejected");
        require(result == unchanged, "formatter failure preserves complete output");
    }
}

void shellPathResolution() {
    auto array = Make<StubArray>();
    auto filesystem = Make<StubItem>();
    auto virtualItem = Make<StubItem>();
    virtualItem->attributes = 0;
    virtualItem->pathResult = E_FAIL;
    array->items = {filesystem, virtualItem};
    std::wstring result;
    succeeded(ItemActions::quotedPaths(array.Get(), result), "resolve filesystem and virtual paths");
    require(result == L"\"C:\\fixture path\\\u65e5\u672c.txt\"\r\n\"::{fixture-virtual-item}\"",
            "filesystem preferred and virtual parser fallback");
    const auto unchanged = result;
    filesystem->pathResult = HRESULT_FROM_WIN32(ERROR_BAD_NETPATH);
    require(ItemActions::quotedPaths(array.Get(), result) == filesystem->pathResult,
            "filesystem path failure must not become parsing-name success");
    require(result == unchanged, "partial paths never replace output");
    filesystem->pathResult = S_OK;
    virtualItem->parsingResult = E_ACCESSDENIED;
    require(ItemActions::quotedPaths(array.Get(), result) == E_ACCESSDENIED,
            "virtual parsing error preserved");
    virtualItem->parsingResult = S_OK;
    filesystem->attributeResult = E_ABORT;
    require(ItemActions::quotedPaths(array.Get(), result) == E_ABORT, "attribute error preserved");
    filesystem->attributeResult = S_OK;
    array->countResult = HRESULT_FROM_WIN32(ERROR_NOT_READY);
    require(ItemActions::quotedPaths(array.Get(), result) == array->countResult, "selection count error preserved");
    array->countResult = S_OK;
    array->itemResult = E_PENDING;
    require(ItemActions::quotedPaths(array.Get(), result) == E_PENDING, "item error preserved");
    require(result == unchanged, "all Shell failures preserve output");
    require(ItemActions::quotedPaths(nullptr, result) == E_INVALIDARG, "null path selection rejected");
    array->itemResult = S_OK;
    array->items.clear();
    require(ItemActions::quotedPaths(array.Get(), result) == E_INVALIDARG, "empty path selection rejected");

    Fixture fixture;
    const auto path = fixture.root / L"native \u03bb.txt";
    writeFile(path);
    ComPtr<IShellItem> thisPc;
    succeeded(SHCreateItemInKnownFolder(FOLDERID_ComputerFolder, 0, nullptr, IID_PPV_ARGS(&thisPc)),
              "create virtual This PC item");
    const auto checkNative = [&](const fs::path& input) {
        const auto actual = selectItems({shellItem(input), thisPc});
        ComPtr<IShellItem> nativeFile;
        ComPtr<IShellItem> nativeVirtual;
        succeeded(actual->GetItemAt(0, &nativeFile), "read native filesystem selection");
        succeeded(actual->GetItemAt(1, &nativeVirtual), "read native virtual selection");
        // Shell parsing may expand short names, remove dot segments and choose
        // its own virtual identifier spelling. Assert the native display names
        // instead of the path used to create the fixture or a hardcoded GUID.
        const auto filesystemName = displayName(nativeFile.Get(), SIGDN_FILESYSPATH);
        const auto parsingName = displayName(nativeVirtual.Get(), SIGDN_DESKTOPABSOLUTEPARSING);
        succeeded(ItemActions::quotedPaths(actual.Get(), result), "resolve actual native filesystem and virtual items");
        equalText(result, L'"' + filesystemName + L"\"\r\n\"" + parsingName + L'"',
                  "native filesystem and virtual item names are quoted exactly");
        return filesystemName;
    };
    checkNative(path);

    const auto caseAlias = path.parent_path() / L"NATIVE \u03bb.TXT";
    if (GetFileAttributesW(caseAlias.c_str()) != INVALID_FILE_ATTRIBUTES) {
        checkNative(caseAlias);
        std::cout << "Native path aliases: differently cased fixture selection passed\n";
    }

    const DWORD needed = GetShortPathNameW(path.c_str(), nullptr, 0);
    if (needed) {
        std::wstring shortName(needed, L'\0');
        const DWORD length = GetShortPathNameW(path.c_str(), shortName.data(), needed);
        require(length && length < needed, "read owned fixture short path");
        shortName.resize(length);
        if (shortName != path.wstring()) {
            checkNative(fs::path(shortName));
            std::cout << "Native path aliases: 8.3 fixture selection passed\n";
        } else {
            std::cout << "Native path aliases: filesystem supplied no distinct 8.3 alias\n";
        }
    } else {
        std::cout << "Native path aliases: 8.3 path unavailable (Win32 "
                  << GetLastError() << ")\n";
    }
}

} // namespace

int runItemActionTests() {
    // COM is initialized by the shared headless runner. No clipboard publishing,
    // Shell dialogs, or operations on user files are performed by these tests.
    const std::vector<std::pair<const char*, std::function<void()>>> tests{
        {"quoted Unicode path formatting", formatting},
        {"Shell path resolution and HRESULT preservation", shellPathResolution}
    };
    int failures = 0;
    for (const auto& [name, test] : tests) {
        try { test(); std::cout << "PASS: " << name << '\n'; }
        catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
        }
        catch (...) { ++failures; std::cerr << "FAIL: " << name << ": unknown exception\n"; }
    }
    std::cout << tests.size() - static_cast<size_t>(failures) << '/' << tests.size()
              << " headless item-action groups passed\n";
    return failures;
}
