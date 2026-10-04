#include "explorer/item_actions.hpp"

#include <shlobj.h>
#include <winioctl.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <algorithm>
#include <cstddef>
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

ComPtr<IShellItemArray> selectPaths(const std::vector<fs::path>& paths) {
    std::vector<ComPtr<IShellItem>> items;
    for (const auto& path : paths) items.push_back(shellItem(path));
    return selectItems(items);
}

void writeFile(const fs::path& path) {
    std::ofstream stream(path, std::ios::binary);
    stream << "headless item-action fixture";
    require(stream.good(), "write owned fixture");
}

DWORD attributes(const fs::path& path) {
    const DWORD result = GetFileAttributesW(path.c_str());
    require(result != INVALID_FILE_ATTRIBUTES, "read fixture attributes");
    return result;
}

void setAttributes(const fs::path& path, DWORD value) {
    require(SetFileAttributesW(path.c_str(), value) != FALSE, "set fixture attributes");
}

struct Fixture {
    fs::path root;
    fs::path junction;
    Fixture() {
        GUID guid{};
        succeeded(CoCreateGuid(&guid), "create fixture identifier");
        wchar_t identifier[40]{};
        require(StringFromGUID2(guid, identifier, 40) > 0, "format fixture identifier");
        root = fs::temp_directory_path() / (std::wstring(L"windows-explorer-attributes-") + identifier);
        require(fs::create_directory(root), "create owned fixture directory");
    }
    ~Fixture() {
        // Remove our junction itself before recursive cleanup; never traverse
        // a reparse target. Normalize read-only fixtures so cleanup succeeds.
        if (!junction.empty()) RemoveDirectoryW(junction.c_str());
        std::error_code error;
        for (fs::recursive_directory_iterator it(root, error), end; it != end && !error;
             it.increment(error)) {
            const DWORD value = GetFileAttributesW(it->path().c_str());
            if (value != INVALID_FILE_ATTRIBUTES && !(value & FILE_ATTRIBUTE_REPARSE_POINT)) {
                SetFileAttributesW(it->path().c_str(), value & ~FILE_ATTRIBUTE_READONLY);
            }
        }
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
    bool hide = true;
    require(ItemActions::canToggleHidden(array.Get(), hide) == array->countResult,
            "hidden capability preserves count error");
    require(hide, "failed capability preserves direction output");
    array->countResult = S_OK;
    array->itemResult = E_PENDING;
    require(ItemActions::quotedPaths(array.Get(), result) == E_PENDING, "item error preserved");
    require(result == unchanged, "all Shell failures preserve output");
    require(ItemActions::quotedPaths(nullptr, result) == E_INVALIDARG, "null path selection rejected");
    require(ItemActions::toggleHidden(nullptr) == E_INVALIDARG, "null hidden selection rejected");
    array->itemResult = S_OK;
    array->items.clear();
    require(ItemActions::quotedPaths(array.Get(), result) == E_INVALIDARG, "empty path selection rejected");
    require(ItemActions::canToggleHidden(array.Get(), hide) == E_INVALIDARG, "empty hidden selection rejected");

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

void mixedHiddenSelection() {
    Fixture fixture;
    const auto first = fixture.root / L"first \u03bb.txt";
    const auto second = fixture.root / L"second.txt";
    writeFile(first);
    writeFile(second);
    setAttributes(first, FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED);
    setAttributes(second, FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_TEMPORARY);
    const DWORD firstOriginal = attributes(first);
    const DWORD secondOriginal = attributes(second);
    const auto selected = selectPaths({first, second});
    bool hide = false;
    succeeded(ItemActions::canToggleHidden(selected.Get(), hide), "mixed-selection capability");
    require(hide, "mixed selection hides all items");
    require(attributes(first) == firstOriginal && attributes(second) == secondOriginal,
            "capability check does not mutate attributes");
    HRESULT rollback = E_FAIL;
    succeeded(ItemActions::toggleHidden(selected.Get(), &rollback), "hide mixed selection");
    require(rollback == S_OK, "successful operation has no rollback failure");
    require(attributes(first) == (firstOriginal | FILE_ATTRIBUTE_HIDDEN), "hide preserves read-only and indexing attributes");
    require(attributes(second) == secondOriginal, "already-hidden item preserves attributes");
    succeeded(ItemActions::canToggleHidden(selected.Get(), hide), "all-hidden capability");
    require(!hide, "all hidden selection becomes unhide");
    succeeded(ItemActions::toggleHidden(selected.Get()), "unhide all selected items");
    require(attributes(first) == firstOriginal, "unhide restores original unrelated attributes");
    require(attributes(second) == (secondOriginal & ~FILE_ATTRIBUTE_HIDDEN), "unhide preserves second unrelated attributes");

    const auto normal = fixture.root / L"normal.txt";
    writeFile(normal);
    setAttributes(normal, FILE_ATTRIBUTE_NORMAL);
    const auto normalSelection = selectPaths({normal});
    succeeded(ItemActions::toggleHidden(normalSelection.Get()), "hide normal file");
    require(attributes(normal) == FILE_ATTRIBUTE_HIDDEN, "NORMAL is not combined with other attributes");
    succeeded(ItemActions::toggleHidden(normalSelection.Get()), "unhide normal file");
    require(attributes(normal) == FILE_ATTRIBUTE_NORMAL, "clearing final attribute restores NORMAL");
}

void foldersAndRejections() {
    Fixture fixture;
    const auto folder = fixture.root / L"selected folder";
    require(fs::create_directory(folder), "create owned folder");
    const auto child = folder / L"child.txt";
    writeFile(child);
    const DWORD folderOriginal = attributes(folder);
    const DWORD childOriginal = attributes(child);
    const auto selected = selectPaths({folder});
    succeeded(ItemActions::toggleHidden(selected.Get()), "hide selected folder");
    require(attributes(folder) == (folderOriginal | FILE_ATTRIBUTE_HIDDEN), "folder keeps directory attribute");
    require(attributes(child) == childOriginal, "hiding folder does not recurse");
    succeeded(ItemActions::toggleHidden(selected.Get()), "unhide selected folder");
    require(attributes(folder) == folderOriginal && attributes(child) == childOriginal, "unhiding folder does not recurse");

    const auto protectedFile = fixture.root / L"protected.txt";
    writeFile(protectedFile);
    setAttributes(protectedFile, FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE);
    const DWORD protectedOriginal = attributes(protectedFile);
    auto batch = selectPaths({child, protectedFile});
    bool hide = false;
    require(ItemActions::canToggleHidden(batch.Get(), hide) == E_ACCESSDENIED, "protected capability rejected");
    require(ItemActions::toggleHidden(batch.Get()) == E_ACCESSDENIED, "protected batch rejected");
    require(attributes(child) == childOriginal && attributes(protectedFile) == protectedOriginal,
            "protected selection rejection changes no earlier item");

    const auto missing = fixture.root / L"now missing.txt";
    writeFile(missing);
    batch = selectPaths({child, missing});
    require(DeleteFileW(missing.c_str()) != FALSE, "remove own fixture after Shell selection");
    const HRESULT missingHr = ItemActions::toggleHidden(batch.Get());
    require(missingHr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
            missingHr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND), "missing item retains native failure");
    require(attributes(child) == childOriginal, "missing selection rejection changes no earlier item");

    ComPtr<IShellItem> thisPc;
    succeeded(SHCreateItemInKnownFolder(FOLDERID_ComputerFolder, 0, nullptr, IID_PPV_ARGS(&thisPc)),
              "create virtual selection");
    batch = selectItems({shellItem(child), thisPc});
    require(ItemActions::toggleHidden(batch.Get()) == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),
            "virtual selection rejected");
    require(attributes(child) == childOriginal, "virtual selection rejection changes no earlier item");
}

void storageAttributePreservation() {
    Fixture fixture;
    for (const bool compressed : {false, true}) {
        const auto path = fixture.root / (compressed ? L"compressed.txt" : L"sparse.txt");
        writeFile(path);
        const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        require(file != INVALID_HANDLE_VALUE, "open owned storage-attribute fixture");
        DWORD returned = 0;
        USHORT format = COMPRESSION_FORMAT_DEFAULT;
        const BOOL changed = DeviceIoControl(file, compressed ? FSCTL_SET_COMPRESSION : FSCTL_SET_SPARSE,
            compressed ? &format : nullptr, compressed ? sizeof(format) : 0,
            nullptr, 0, &returned, nullptr);
        const DWORD error = changed ? ERROR_SUCCESS : GetLastError();
        CloseHandle(file);
        if (!changed) succeeded(HRESULT_FROM_WIN32(error), "set owned storage attribute");
        const DWORD original = attributes(path);
        require((original & (compressed ? FILE_ATTRIBUTE_COMPRESSED : FILE_ATTRIBUTE_SPARSE_FILE)) != 0,
                "native storage attribute was set");
        const auto selected = selectPaths({path});
        succeeded(ItemActions::toggleHidden(selected.Get()), "hide native storage-attribute fixture");
        require(attributes(path) == (original | FILE_ATTRIBUTE_HIDDEN), "hidden mutation preserves native storage attribute");
        succeeded(ItemActions::toggleHidden(selected.Get()), "unhide native storage-attribute fixture");
        require(attributes(path) == original, "unhide preserves native storage attribute");
    }
}

// A junction can be created in an owned directory without the symbolic-link
// privilege. Its target remains inside the same fixture and is never traversed.
void reparseRejection() {
    Fixture fixture;
    const auto target = fixture.root / L"target";
    require(fs::create_directory(target), "create owned junction target");
    const auto file = target / L"child.txt";
    writeFile(file);
    const DWORD targetOriginal = attributes(target);
    const DWORD childOriginal = attributes(file);
    fixture.junction = fixture.root / L"junction";
    require(fs::create_directory(fixture.junction), "create owned junction directory");
    const HANDLE directory = CreateFileW(fixture.junction.c_str(), GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    require(directory != INVALID_HANDLE_VALUE, "open owned junction");
    struct MountPoint {
        DWORD tag;
        WORD dataLength;
        WORD reserved;
        WORD substituteOffset;
        WORD substituteLength;
        WORD printOffset;
        WORD printLength;
        wchar_t names[1];
    };
    const std::wstring substitute = L"\\??\\" + target.wstring();
    const std::wstring print = target.wstring();
    const auto nameBytes = (substitute.size() + print.size() + 2) * sizeof(wchar_t);
    std::vector<unsigned char> buffer(offsetof(MountPoint, names) + nameBytes);
    auto* data = reinterpret_cast<MountPoint*>(buffer.data());
    data->tag = IO_REPARSE_TAG_MOUNT_POINT;
    data->dataLength = static_cast<WORD>(8 + nameBytes);
    data->substituteLength = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    data->printOffset = static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t));
    data->printLength = static_cast<WORD>(print.size() * sizeof(wchar_t));
    std::copy(substitute.begin(), substitute.end(), data->names);
    std::copy(print.begin(), print.end(), data->names + substitute.size() + 1);
    DWORD returned = 0;
    const BOOL created = DeviceIoControl(directory, FSCTL_SET_REPARSE_POINT, buffer.data(),
        static_cast<DWORD>(buffer.size()), nullptr, 0, &returned, nullptr);
    const DWORD error = created ? ERROR_SUCCESS : GetLastError();
    CloseHandle(directory);
    if (!created) succeeded(HRESULT_FROM_WIN32(error), "create owned junction reparse point");
    require((attributes(fixture.junction) & FILE_ATTRIBUTE_REPARSE_POINT) != 0, "fixture is a real junction");
    const auto batch = selectPaths({file, fixture.junction});
    require(ItemActions::toggleHidden(batch.Get()) == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),
            "reparse item rejected before target or prior selection mutation");
    require(attributes(target) == targetOriginal && attributes(file) == childOriginal,
            "junction rejection leaves target and first selection unchanged");
    require(RemoveDirectoryW(fixture.junction.c_str()) != FALSE, "remove own junction itself");
    fixture.junction.clear();
}
} // namespace

int runItemActionTests() {
    // COM is initialized by the shared headless runner. No clipboard publishing,
    // Shell dialogs, or operations on user files are performed by these tests.
    const std::vector<std::pair<const char*, std::function<void()>>> tests{
        {"quoted Unicode path formatting", formatting},
        {"Shell path resolution and HRESULT preservation", shellPathResolution},
        {"mixed hidden selection and unrelated attribute preservation", mixedHiddenSelection},
        {"nonrecursive folder attributes and batch preflight rejection", foldersAndRejections},
        {"compressed and sparse attribute preservation", storageAttributePreservation},
        {"real junction rejection without target mutation", reparseRejection}
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
