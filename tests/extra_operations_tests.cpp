#include "explorer/extra_operations.hpp"

#include <shobjidl.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <cwctype>

namespace {
namespace fs = std::filesystem;
using explorer::ExtraOperations;
using Microsoft::WRL::ComPtr;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void succeeded(HRESULT hr, const char* message) {
    if (FAILED(hr)) {
        std::cerr << message << " (HRESULT 0x" << std::hex << static_cast<unsigned long>(hr) << std::dec << ")\n";
        throw std::runtime_error(message);
    }
}
void write(const fs::path& path, const std::string& contents) {
    std::ofstream stream(path, std::ios::binary);
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    require(stream.good(), "write fixture");
}
std::string read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    require(stream.good(), "read fixture");
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
struct Fixture {
    fs::path root;
    Fixture() {
        GUID guid{};
        succeeded(CoCreateGuid(&guid), "create fixture id");
        wchar_t identifier[40]{};
        require(StringFromGUID2(guid, identifier, 40) != 0, "format fixture id");
        root = fs::temp_directory_path() / (std::wstring(L"windows-explorer-extra-test-資料-") + identifier);
        require(fs::create_directory(root), "create exclusive fixture directory");
    }
    ~Fixture() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
};

void append16(std::string& output, uint16_t value) {
    output += static_cast<char>(value & 255);
    output += static_cast<char>((value >> 8) & 255);
}
void append32(std::string& output, uint32_t value) {
    append16(output, static_cast<uint16_t>(value & 65535));
    append16(output, static_cast<uint16_t>(value >> 16));
}
struct ArchiveName { std::string name; uint32_t attributes = 0x81a40000; };

// Small stored ZIP fixtures model names and metadata without extracting any
// untrusted payload. Each entry is an empty regular file unless specified.
std::string fixtureZip(const std::vector<ArchiveName>& names) {
    std::string archive, central;
    for (const auto& item : names) {
        const uint32_t offset = static_cast<uint32_t>(archive.size());
        append32(archive, 0x04034b50); append16(archive, 20); append16(archive, 0x0800);
        append16(archive, 0); append16(archive, 0); append16(archive, 0);
        append32(archive, 0); append32(archive, 0); append32(archive, 0);
        append16(archive, static_cast<uint16_t>(item.name.size())); append16(archive, 0); archive += item.name;
        append32(central, 0x02014b50); append16(central, 0x0314); append16(central, 20);
        append16(central, 0x0800); append16(central, 0); append16(central, 0); append16(central, 0);
        append32(central, 0); append32(central, 0); append32(central, 0);
        append16(central, static_cast<uint16_t>(item.name.size())); append16(central, 0); append16(central, 0);
        append16(central, 0); append16(central, 0); append32(central, item.attributes); append32(central, offset);
        central += item.name;
    }
    const uint32_t centralOffset = static_cast<uint32_t>(archive.size());
    archive += central;
    append32(archive, 0x06054b50); append16(archive, 0); append16(archive, 0);
    append16(archive, static_cast<uint16_t>(names.size())); append16(archive, static_cast<uint16_t>(names.size()));
    append32(archive, static_cast<uint32_t>(central.size())); append32(archive, centralOffset); append16(archive, 0);
    return archive;
}

void zipRoundTrip() {
    Fixture fixture;
    const fs::path folder = fixture.root / L"資料 & folder";
    require(fs::create_directories(folder / L"nested" / L"empty"), "create nested source tree");
    const std::string binary("content\0with\nnewlines", 21);
    write(folder / L"nested" / L"日本語.txt", binary);
    write(folder / L"with spaces $(literal).txt", std::string(100000, 'x'));
    write(fixture.root / L"second file.txt", "second source");
    const fs::path archive = fixture.root / L"資料 archive.zip";
    succeeded(ExtraOperations::createZip({folder, fixture.root / L"second file.txt"}, archive), "create Unicode compressed ZIP");
    require(fs::file_size(archive) < 100000, "ZIP creation did not compress repeated data");
    const fs::path extracted = fixture.root / L"extracted 資料";
    succeeded(ExtraOperations::extractZip(archive, extracted), "extract Unicode ZIP");
    require(read(extracted / folder.filename() / L"nested" / L"日本語.txt") == binary, "ZIP changed binary contents");
    require(read(extracted / folder.filename() / L"with spaces $(literal).txt") == std::string(100000, 'x'), "ZIP changed compressed contents");
    require(fs::is_directory(extracted / folder.filename() / L"nested" / L"empty"), "ZIP lost empty nested directory");
    require(read(extracted / L"second file.txt") == "second source", "ZIP lost additional source");
    const std::string before = read(archive);
    require(FAILED(ExtraOperations::createZip({folder}, archive)), "ZIP creation overwrote an archive");
    require(read(archive) == before, "archive changed after rejected overwrite");
    write(extracted / L"sentinel.txt", "keep me");
    require(FAILED(ExtraOperations::extractZip(archive, extracted)), "extraction reused an existing directory");
    require(read(extracted / L"sentinel.txt") == "keep me", "extraction changed an existing destination");
    require(FAILED(ExtraOperations::createZip({folder}, folder / L"self.zip")), "ZIP output allowed inside source tree");
    require(!fs::exists(folder / L"self.zip"), "self-containing archive was created");
    require(SetFileAttributesW(archive.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE, "mark fixture archive read-only");
    succeeded(ExtraOperations::extractZip(archive, fixture.root / L"readonly-result"), "extract read-only source archive");
    require(GetFileAttributesW(archive.c_str()) & FILE_ATTRIBUTE_READONLY, "extraction changed original archive attributes");
    require(SetFileAttributesW(archive.c_str(), FILE_ATTRIBUTE_NORMAL) != FALSE, "restore fixture archive attributes");
    for (const auto& entry : fs::directory_iterator(fixture.root))
        require(!entry.path().filename().native().starts_with(L".windows-explorer-"), "ZIP operation left staging directory behind");
}

void zipFailures() {
    Fixture fixture;
    require(FAILED(ExtraOperations::createZip({}, fixture.root / L"empty.zip")), "empty ZIP sources accepted");
    require(FAILED(ExtraOperations::createZip({fixture.root / L"missing"}, fixture.root / L"missing.zip")), "missing ZIP source accepted");
    require(FAILED(ExtraOperations::extractZip(fixture.root / L"missing.zip", fixture.root / L"missing-result")), "missing archive accepted");
    require(!fs::exists(fixture.root / L"missing-result"), "missing archive left destination");
    write(fixture.root / L"broken.zip", "not a ZIP");
    require(FAILED(ExtraOperations::extractZip(fixture.root / L"broken.zip", fixture.root / L"broken-result")), "invalid archive accepted");
    require(!fs::exists(fixture.root / L"broken-result"), "invalid archive left destination");
    require(fs::create_directory(fixture.root / L"one") && fs::create_directory(fixture.root / L"two"), "duplicate source fixture");
    write(fixture.root / L"one" / L"same.txt", "one"); write(fixture.root / L"two" / L"same.txt", "two");
    require(FAILED(ExtraOperations::createZip({fixture.root / L"one" / L"same.txt", fixture.root / L"two" / L"same.txt"}, fixture.root / L"duplicate.zip")), "ZIP accepted colliding source names");
    require(!fs::exists(fixture.root / L"duplicate.zip"), "colliding archive was committed");
    write(fixture.root / L"empty-archive.zip", fixtureZip({}));
    succeeded(ExtraOperations::extractZip(fixture.root / L"empty-archive.zip", fixture.root / L"empty-result"), "extract empty ZIP");
    require(fs::is_empty(fixture.root / L"empty-result"), "empty ZIP extraction produced files");
}

void unsafeArchives() {
    Fixture fixture;
    const std::vector<std::vector<ArchiveName>> unsafe{
        {{"../outside.txt"}}, {{"folder/../../outside.txt"}}, {{"/absolute.txt"}},
        {{"C:/absolute.txt"}}, {{"\\\\server\\share\\file"}}, {{"file:stream"}},
        {{"folder\\..\\outside"}}, {{"NUL.txt"}}, {{"CON .txt"}}, {{"trailing. "}},
        {{std::string("nul\0name", 8)}}, {{"link", 0xa1ff0000}},
        {{"File.txt"}, {"file.txt"}}, {{"parent"}, {"parent/child"}},
        {{"bad/./child"}}, {{"bad//child"}}, {{"COM1.txt"}}, {{"LPT9"}}
    };
    for (size_t index = 0; index < unsafe.size(); ++index) {
        const fs::path archive = fixture.root / (L"unsafe-" + std::to_wstring(index) + L".zip");
        const fs::path destination = fixture.root / (L"result-" + std::to_wstring(index));
        write(archive, fixtureZip(unsafe[index]));
        require(FAILED(ExtraOperations::extractZip(archive, destination)), "unsafe ZIP path or link accepted");
        require(!fs::exists(destination), "unsafe ZIP left an extraction destination");
    }
    require(!fs::exists(fixture.root / L"outside.txt"), "ZIP traversal wrote outside fixture");
    // A safe stored ZIP from another writer also works, independently of tar's
    // creation format and data-descriptor records used by the round-trip case.
    write(fixture.root / L"safe.zip", fixtureZip({{"folder/safe.txt"}}));
    succeeded(ExtraOperations::extractZip(fixture.root / L"safe.zip", fixture.root / L"safe-result"), "extract stored fixture ZIP");
    require(read(fixture.root / L"safe-result" / L"folder" / L"safe.txt").empty(), "stored ZIP empty content changed");
}

std::string utf8Path(const fs::path& path) {
    const auto bytes = path.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

void requireSameObject(const fs::path& actual, const fs::path& expected, const char* message) {
    std::error_code error;
    const bool same = fs::equivalent(actual, expected, error);
    if (!same || error) {
        std::cerr << message << "\n  expected: " << utf8Path(expected)
                  << "\n  actual:   " << utf8Path(actual)
                  << "\n  filesystem error: " << error.value() << " (" << error.message() << ")\n";
        throw std::runtime_error(message);
    }
}

fs::path verifyShortcut(const fs::path& shortcut, const fs::path& target, const fs::path& workingDirectory) {
    ComPtr<IShellLinkW> link;
    succeeded(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)), "read native shortcut");
    ComPtr<IPersistFile> persist;
    succeeded(link.As(&persist), "query shortcut persistence");
    succeeded(persist->Load(shortcut.c_str(), STGM_READ), "load saved shortcut");
    wchar_t actual[32768]{};
    const HRESULT pathResult = link->GetPath(actual, static_cast<int>(std::size(actual)), nullptr, SLGP_RAWPATH);
    if (pathResult != S_OK || !actual[0]) {
        std::cerr << "shortcut has no persisted target (HRESULT 0x" << std::hex
                  << static_cast<unsigned long>(pathResult) << std::dec << ")\n";
        throw std::runtime_error("shortcut has no persisted target");
    }
    const fs::path actualTarget(actual);
    // Shell links may expand 8.3 names or canonicalize casing. Filesystem
    // identity proves the link references the exact object, including Unicode
    // names, without requiring the Shell to preserve an alias's spelling.
    requireSameObject(actualTarget, target, "shortcut target differs");
    wchar_t actualDirectory[32768]{};
    succeeded(link->GetWorkingDirectory(actualDirectory, static_cast<int>(std::size(actualDirectory))), "read shortcut working directory");
    require(actualDirectory[0] != L'\0', "shortcut has no working directory");
    requireSameObject(fs::path(actualDirectory), workingDirectory, "shortcut working directory differs");
    return actualTarget;
}

void shortcuts() {
    Fixture fixture;
    const fs::path target = fixture.root / L"資料 target.txt";
    const fs::path shortcut = fixture.root / L"資料 shortcut.lnk";
    write(target, "shortcut target");
    succeeded(ExtraOperations::createShortcut(target, shortcut), "create native Unicode shortcut");
    const fs::path actual = verifyShortcut(shortcut, target, target.parent_path());
    const fs::path decoy = fixture.root / L"same contents but different file.txt";
    write(decoy, "shortcut target");
    std::error_code identityError;
    require(!fs::equivalent(actual, decoy, identityError) && !identityError, "shortcut identity check accepted an unrelated same-content file");
    const std::string saved = read(shortcut);
    require(FAILED(ExtraOperations::createShortcut(target, shortcut)), "shortcut overwrote existing output");
    require(read(shortcut) == saved, "shortcut changed after rejected overwrite");
    require(FAILED(ExtraOperations::createShortcut(fixture.root / L"missing", fixture.root / L"missing.lnk")), "shortcut accepted missing target");
    require(!fs::exists(fixture.root / L"missing.lnk"), "missing shortcut target left output");
    require(fs::create_directory(fixture.root / L"folder"), "shortcut directory fixture");
    succeeded(ExtraOperations::createShortcut(fixture.root / L"folder", fixture.root / L"folder.lnk"), "create folder shortcut");
    verifyShortcut(fixture.root / L"folder.lnk", fixture.root / L"folder", fixture.root / L"folder");

    // Reproduce the CI failure mode by passing a valid alternative Windows
    // path spelling. The Shell is allowed to persist the long, on-disk name.
    std::wstring caseAlias = target.native();
    for (auto& character : caseAlias) character = static_cast<wchar_t>(std::towupper(character));
    requireSameObject(fs::path(caseAlias), target, "case-alias fixture does not reference original target");
    succeeded(ExtraOperations::createShortcut(fs::path(caseAlias), fixture.root / L"case alias.lnk"), "create shortcut from case alias");
    const fs::path persistedCase = verifyShortcut(fixture.root / L"case alias.lnk", target, target.parent_path());
    if (persistedCase != fs::path(caseAlias))
        std::cout << "INFO: Shell normalized target casing; exact file identity verified\n";

    const DWORD shortLength = GetShortPathNameW(target.c_str(), nullptr, 0);
    if (shortLength) {
        std::wstring shortAlias(shortLength, L'\0');
        const DWORD written = GetShortPathNameW(target.c_str(), shortAlias.data(), shortLength);
        require(written != 0 && written < shortLength, "read short-path target alias");
        shortAlias.resize(written);
        requireSameObject(fs::path(shortAlias), target, "short-path fixture does not reference original target");
        succeeded(ExtraOperations::createShortcut(fs::path(shortAlias), fixture.root / L"short alias.lnk"), "create shortcut from short-path alias");
        const fs::path persistedShort = verifyShortcut(fixture.root / L"short alias.lnk", target, target.parent_path());
        if (persistedShort != fs::path(shortAlias))
            std::cout << "INFO: Shell expanded 8.3 target spelling; exact file identity verified\n";
    }
}
}

int runExtraOperationTests() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests{
        {"ZIP compressed Unicode round trip, nested folders and no overwrite", zipRoundTrip},
        {"ZIP missing/invalid inputs, self-inclusion and collisions", zipFailures},
        {"ZIP safe paths, link rejection and foreign stored archive", unsafeArchives},
        {"Native Unicode file/folder shortcuts and no overwrite", shortcuts}
    };
    int failures = 0;
    for (const auto& [name, test] : tests) {
        try { test(); std::cout << "PASS: " << name << '\n'; }
        catch (const std::exception& error) { ++failures; std::cerr << "FAIL: " << name << ": " << error.what() << '\n'; }
        catch (...) { ++failures; std::cerr << "FAIL: " << name << ": unknown exception\n"; }
    }
    return failures;
}
