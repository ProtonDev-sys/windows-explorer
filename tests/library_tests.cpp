#include "explorer/library.hpp"

#include <array>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;
using explorer::LibraryKind;
using explorer::ShellLibrary;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void succeeded(HRESULT result, const char* message) {
    if (FAILED(result)) {
        std::cerr << message << " (HRESULT 0x" << std::hex << static_cast<unsigned long>(result) << std::dec << ")\n";
        throw std::runtime_error(message);
    }
}

struct Fixture {
    fs::path root;
    fs::path first;
    fs::path second;
    fs::path third;
    Fixture() {
        GUID guid{};
        succeeded(CoCreateGuid(&guid), "Create library fixture GUID");
        wchar_t text[40]{};
        require(StringFromGUID2(guid, text, 40) != 0, "Format library fixture GUID");
        root = fs::temp_directory_path() / (std::wstring(L"WindowsExplorer-Library-") + text);
        first = root / L"first \u03BB";
        second = root / L"\u65E5\u672C\u8A9E \U0001F4C1";
        third = root / L"third";
        require(fs::create_directories(first) && fs::create_directory(second) && fs::create_directory(third), "Create owned library fixture folders");
        std::ofstream(first / L"marker.txt") << "included directory contents stay intact";
    }
    ~Fixture() { std::error_code ignored; fs::remove_all(root, ignored); }
};

ComPtr<IShellItem> item(const fs::path& path) {
    ComPtr<IShellItem> result;
    succeeded(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&result)), "Create library fixture shell item");
    return result;
}

struct TaskMemoryDelete { void operator()(wchar_t* value) const noexcept { CoTaskMemFree(value); } };

fs::path itemPath(IShellItem* value) {
    PWSTR raw = nullptr;
    succeeded(value->GetDisplayName(SIGDN_FILESYSPATH, &raw), "Read saved library item path");
    std::unique_ptr<wchar_t, TaskMemoryDelete> owned(raw);
    require(raw != nullptr, "Saved library has no path");
    return fs::path(raw);
}

std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "Read owned library fixture");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

bool contains(const std::vector<explorer::LibraryFolder>& folders, const fs::path& path) {
    for (const auto& folder : folders) {
        if (!folder.path.empty() && fs::equivalent(folder.path, path)) return true;
    }
    return false;
}

void persistenceAndUnicode() {
    Fixture fixture;
    ShellLibrary library;
    succeeded(ShellLibrary::create(library), "Create native shell library");
    require(library.valid() && library.writable() && library.native(), "Library helper does not own native object");
    std::vector<explorer::LibraryFolder> folders;
    succeeded(library.folders(folders), "Read empty library folders");
    require(folders.empty(), "New native library must start empty");
    const HRESULT nativeCommit = library.native()->Commit();
    require(FAILED(nativeCommit) && library.commit() == nativeCommit, "Commit without backing file did not preserve native error");
    succeeded(library.addFolder(fixture.first), "Add first library folder");
    succeeded(library.addFolder(fixture.second), "Add Unicode library folder");
    succeeded(library.setDefaultSaveFolder(fixture.second), "Choose Unicode default save folder");
    succeeded(library.optimize(LibraryKind::Documents), "Optimize library for documents");
    ComPtr<IShellItem> saved;
    const std::wstring name = L"Library \u732B \U0001F4C1";
    succeeded(library.save(fixture.root, name, saved), "Save native library in owned fixture");
    require(fs::equivalent(itemPath(saved.Get()), fixture.root / (name + L".library-ms")), "Native Save escaped its explicit owned directory or corrupted Unicode name");
    // Save retains a native backing-file writer. Release it before a fresh
    // read/write load; this exercises persisted state rather than that owner.
    library = ShellLibrary{};

    ShellLibrary loaded;
    succeeded(ShellLibrary::load(saved.Get(), true, loaded), "Load saved native library read/write");
    succeeded(loaded.folders(folders), "Enumerate saved included folders");
    require(folders.size() == 2 && contains(folders, fixture.first) && contains(folders, fixture.second), "Included library folders did not round-trip");
    for (const auto& folder : folders) require(folder.item && !folder.displayName.empty() && !folder.path.empty(), "Included folder lost owning item, Unicode label, or path");
    fs::path defaultPath;
    succeeded(loaded.defaultSavePath(defaultPath, DSFT_PRIVATE), "Read private default save path");
    require(fs::equivalent(defaultPath, fixture.second), "Library default save folder did not round-trip");
    GUID type{};
    succeeded(loaded.folderType(type), "Read library folder type");
    require(explorer::libraryKindForType(type) == LibraryKind::Documents, "Library optimization did not round-trip");

    succeeded(loaded.setDefaultSaveFolder(fixture.first), "Change default save folder");
    succeeded(loaded.optimize(LibraryKind::Pictures), "Change library optimization");
    succeeded(loaded.commit(), "Commit library changes to existing file");
    ShellLibrary committed;
    succeeded(ShellLibrary::load(saved.Get(), false, committed), "Reload committed library");
    succeeded(committed.defaultSavePath(defaultPath, DSFT_PRIVATE), "Read committed save folder");
    succeeded(committed.folderType(type), "Read committed folder type");
    require(fs::equivalent(defaultPath, fixture.first) && explorer::libraryKindForType(type) == LibraryKind::Pictures, "Commit lost library changes");

    succeeded(loaded.setDefaultSaveFolder(fixture.second), "Move default before removing folder");
    succeeded(loaded.removeFolder(fixture.first), "Remove included folder");
    succeeded(loaded.addFolder(fixture.third), "Add another library folder");
    succeeded(loaded.commit(), "Commit changed included folders");
    succeeded(ShellLibrary::load(saved.Get(), false, committed), "Reload changed included folders");
    succeeded(committed.folders(folders), "Enumerate changed folders");
    require(folders.size() == 2 && !contains(folders, fixture.first) && contains(folders, fixture.second) && contains(folders, fixture.third), "Remove/add library changes did not persist");
    require(read(fixture.first / L"marker.txt") == "included directory contents stay intact", "Removing a library location changed its contents");
    for (const auto& kind : explorer::libraryKinds()) {
        succeeded(loaded.optimize(kind.kind), "Set supported library optimization");
        succeeded(loaded.commit(), "Commit supported library optimization");
        succeeded(ShellLibrary::load(saved.Get(), false, committed), "Reload supported library optimization");
        succeeded(committed.folderType(type), "Read supported library optimization");
        require(IsEqualGUID(type, *kind.folderType) && explorer::libraryKindForType(type) == kind.kind, "Supported library type differs from documented GUID");
    }
}

void validationDuplicatesAndNoOverwrite() {
    Fixture fixture;
    ShellLibrary library;
    succeeded(ShellLibrary::create(library), "Create validation library");
    succeeded(library.addFolder(fixture.first), "Add validation library folder");
    require(library.addFolder(fixture.first) == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), "Duplicate library folder was accepted");
    require(library.addFolder(fixture.first / L".") == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), "Canonical duplicate library folder was accepted");
    require(library.removeFolder(fixture.third) == HRESULT_FROM_WIN32(ERROR_NOT_FOUND), "Removing absent library folder was accepted");
    require(library.setDefaultSaveFolder(fixture.third) == HRESULT_FROM_WIN32(ERROR_NOT_FOUND), "Default folder outside library was accepted");
    require(library.removeFolder(static_cast<IShellItem*>(nullptr)) == E_INVALIDARG && library.setDefaultSaveFolder(static_cast<IShellItem*>(nullptr)) == E_INVALIDARG, "Null included folder was accepted");
    require(library.setDefaultSaveFolder(fixture.first, static_cast<DEFAULTSAVEFOLDERTYPE>(999)) == E_INVALIDARG, "Invalid default save type was accepted");
    require(library.optimize(static_cast<LibraryKind>(999)) == E_INVALIDARG, "Unknown library optimization was accepted");

    const std::array invalidPaths{
        fs::path{}, fs::path(L"relative"), fs::path(L"C:relative"),
        fs::path(L"C:\\" + std::wstring(ShellLibrary::MaximumPathUnits, L'x')),
        fs::path(fixture.first.native() + std::wstring(1, L'\0') + L"suffix"),
        fs::path(fixture.root.native() + L"\\" + std::wstring(1, static_cast<wchar_t>(0xD800))),
        fs::path(L"\\\\.\\C:\\"), fs::path(L"\\\\?\\globalroot\\Device\\")
    };
    for (const auto& path : invalidPaths) require(library.addFolder(path) == E_INVALIDARG, "Invalid directory input was accepted by library helper");
    require(library.addFolder(fixture.first / L"marker.txt") == HRESULT_FROM_WIN32(ERROR_DIRECTORY), "Regular file was accepted as library folder");
    require(FAILED(library.addFolder(fixture.root / L"missing")), "Missing folder was accepted");

    ComPtr<IShellItem> saved;
    const std::array invalidNames{L"", L"..", L"a/b", L"NUL", L"already.library-ms", L"already.LIBRARY-MS"};
    for (const auto name : invalidNames) require(library.save(fixture.root, name, saved) == E_INVALIDARG && !saved, "Invalid library basename was accepted");
    require(library.save(fixture.root, std::wstring(ShellLibrary::MaximumNameUnits + 1, L'a'), saved) == E_INVALIDARG, "Oversized library basename was accepted");
    require(library.save(fs::path{}, L"test", saved) == E_INVALIDARG && library.save(fixture.first / L"marker.txt", L"test", saved) == HRESULT_FROM_WIN32(ERROR_DIRECTORY), "Library save target was not a validated directory");
    succeeded(library.save(fixture.root, L"existing", saved), "Save existing validation library");
    const auto path = itemPath(saved.Get());
    const auto contents = read(path);
    const auto originalItem = saved.Get();
    ShellLibrary another;
    succeeded(ShellLibrary::create(another), "Create colliding library");
    succeeded(another.addFolder(fixture.second), "Add colliding library folder");
    require(FAILED(another.save(fixture.root, L"existing", saved)), "Library save overwrote an existing description");
    require(read(path) == contents && saved.Get() == originalItem, "Failed library save changed existing file or output");
    ShellLibrary invalid;
    const auto ordinary = item(fixture.first / L"marker.txt");
    require(FAILED(ShellLibrary::load(ordinary.Get(), true, invalid)) && !invalid.valid(), "Non-library item loaded as a library");
    require(ShellLibrary::load(nullptr, true, invalid) == E_INVALIDARG && !invalid.valid(), "Null library item was accepted");
    const auto included = item(fixture.first);
    succeeded(library.setDefaultSaveFolder(included.Get()), "Choose included native item as default folder");
    succeeded(library.removeFolder(included.Get()), "Remove included native item");
    std::vector<explorer::LibraryFolder> remaining;
    succeeded(library.folders(remaining), "Read library after native item removal");
    require(remaining.empty() && fs::exists(fixture.first / L"marker.txt"), "Native item removal changed filesystem content or failed to remove membership");
}

void readonlyAndFailureOutputs() {
    ShellLibrary empty;
    std::vector<explorer::LibraryFolder> unchanged(1);
    unchanged.front().displayName = L"sentinel";
    require(empty.folders(unchanged) == E_UNEXPECTED && unchanged.size() == 1 && unchanged.front().displayName == L"sentinel", "Uninitialized library changed folder output");
    fs::path unchangedPath(L"sentinel");
    require(empty.defaultSavePath(unchangedPath) == E_UNEXPECTED && unchangedPath == L"sentinel", "Failed library getter changed path output");
    GUID unchangedType = *explorer::libraryKinds().front().folderType;
    const auto originalType = unchangedType;
    require(empty.folderType(unchangedType) == E_UNEXPECTED && IsEqualGUID(unchangedType, originalType), "Failed library getter changed GUID output");
    require(empty.commit() == E_UNEXPECTED && !empty.valid() && !empty.writable(), "Uninitialized library mutation was accepted");
    require(explorer::libraryKindForType(GUID_NULL) == std::nullopt, "Unknown library folder type was guessed");

    Fixture fixture;
    ShellLibrary created;
    succeeded(ShellLibrary::create(created), "Create readonly test library");
    succeeded(created.addFolder(fixture.first), "Add readonly test folder");
    succeeded(created.setDefaultSaveFolder(fixture.first), "Set readonly test default folder");
    ComPtr<IShellItem> saved;
    succeeded(created.save(fixture.root, L"readonly", saved), "Save readonly test library");
    const auto path = itemPath(saved.Get());
    const auto before = read(path);
    ShellLibrary readonly;
    succeeded(ShellLibrary::load(saved.Get(), false, readonly), "Load library read-only");
    require(readonly.valid() && !readonly.writable(), "Read-only library access was not retained");
    require(readonly.addFolder(fixture.second) == E_ACCESSDENIED && readonly.removeFolder(fixture.first) == E_ACCESSDENIED && readonly.setDefaultSaveFolder(fixture.first) == E_ACCESSDENIED && readonly.optimize(LibraryKind::Videos) == E_ACCESSDENIED && readonly.commit() == E_ACCESSDENIED && readonly.save(fixture.root, L"should-not-exist", saved) == E_ACCESSDENIED, "Read-only library mutation was accepted");
    require(read(path) == before && !fs::exists(fixture.root / L"should-not-exist.library-ms"), "Read-only library test modified its description");
    ComPtr<IShellItem> defaultItem;
    succeeded(readonly.defaultSaveFolder(defaultItem, DSFT_PRIVATE), "Read readonly default folder");
    const auto sentinelItem = defaultItem.Get();
    require(readonly.defaultSaveFolder(defaultItem, static_cast<DEFAULTSAVEFOLDERTYPE>(0)) == E_INVALIDARG && defaultItem.Get() == sentinelItem, "Invalid default query changed output");
    require(ShellLibrary::load(nullptr, true, readonly) == E_INVALIDARG && readonly.valid() && !readonly.writable(), "Failed load discarded existing owned library");
}
} // namespace

int runLibraryTests() {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) { std::cerr << "FAIL: Library tests require COM STA initialization\n"; return 1; }
    int failures = 0;
    const std::array tests{
        std::pair{"Native library Unicode persistence, locations, defaults and types", persistenceAndUnicode},
        std::pair{"Native library input validation, canonical duplicates and no overwrite", validationDuplicatesAndNoOverwrite},
        std::pair{"Native library read-only access, HRESULTs and unchanged failure outputs", readonlyAndFailureOutputs}
    };
    for (const auto& [name, test] : tests) {
        try { test(); std::cout << "PASS: " << name << '\n'; }
        catch (const std::exception& error) { ++failures; std::cerr << "FAIL: " << name << ": " << error.what() << '\n'; }
        catch (...) { ++failures; std::cerr << "FAIL: " << name << ": unknown exception\n"; }
    }
    CoUninitialize();
    return failures;
}
