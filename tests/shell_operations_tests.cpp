#include "explorer/shell_operations.hpp"

#include <shlobj.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
using explorer::ShellOperations;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void succeeded(HRESULT hr, const char* message) {
    if (FAILED(hr)) {
        std::cerr << message << " (HRESULT 0x" << std::hex
                  << static_cast<unsigned long>(hr) << std::dec << ")\n";
        throw std::runtime_error(message);
    }
}

ComPtr<IShellItem> item(const fs::path& path) {
    ComPtr<IShellItem> result;
    succeeded(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&result)),
              "create shell item");
    return result;
}

ComPtr<IShellItemArray> selection(const fs::path& path) {
    const auto source = item(path);
    ComPtr<IShellItemArray> result;
    succeeded(SHCreateShellItemArrayFromShellItem(source.Get(), IID_PPV_ARGS(&result)),
              "create shell selection");
    return result;
}

void write(const fs::path& path, const char* contents) {
    std::ofstream stream(path, std::ios::binary);
    stream << contents;
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
        require(StringFromGUID2(guid, identifier, 40) > 0, "format fixture id");
        root = fs::temp_directory_path() / (std::wstring(L"windows-explorer-test-") + identifier);
        require(fs::create_directories(root / L"source"), "create fixture source");
        require(fs::create_directories(root / L"destination"), "create fixture destination");
    }
    ~Fixture() {
        std::error_code error;
        fs::remove_all(root, error);
    }
};
} // namespace

int runShellOperationTests() {
    // Every fixture mutation is silent: no Shell undo records. Clipboard tests
    // stop at argument validation, so the user's clipboard is never replaced.
    const HRESULT initialized = OleInitialize(nullptr);
    if (FAILED(initialized)) {
        std::cerr << "Shell tests require an STA/OLE apartment\n";
        return 1;
    }
    int failures = 0;
    try {
        require(ShellOperations::copyOrMove(nullptr, nullptr, nullptr, false, true) == E_INVALIDARG,
                "copy rejects null selection");
        require(ShellOperations::remove(nullptr, nullptr, true, true) == E_INVALIDARG,
                "delete rejects null selection");
        require(ShellOperations::copyToClipboard(nullptr, nullptr, false) == E_INVALIDARG,
                "clipboard rejects null selection");
        require(ShellOperations::copyPaths(nullptr, nullptr) == E_INVALIDARG,
                "paths reject null selection");
        require(ShellOperations::undo(nullptr) == E_NOTIMPL && !ShellOperations::canUndo(),
                "unsupported undo is honest");
        require(ShellOperations::redo(nullptr) == E_NOTIMPL && !ShellOperations::canRedo(),
                "unsupported redo is honest");

        Fixture fixture;
        const fs::path source = fixture.root / L"source";
        const fs::path destination = fixture.root / L"destination";
        auto target = item(destination);
        write(source / L"copy.txt", "original contents");
        auto selected = selection(source / L"copy.txt");
        succeeded(ShellOperations::copyOrMove(nullptr, selected.Get(), target.Get(), false, true),
                  "silent copy");
        require(read(source / L"copy.txt") == "original contents", "copy preserves source");
        require(read(destination / L"copy.txt") == "original contents", "copy preserves contents");

        write(destination / L"copy.txt", "existing destination");
        succeeded(ShellOperations::copyOrMove(nullptr, selected.Get(), target.Get(), false, true),
                  "silent copy collision");
        require(read(destination / L"copy.txt") == "existing destination",
                "collision preserves existing destination");
        bool renamedCopy = false;
        for (const auto& entry : fs::directory_iterator(destination)) {
            if (entry.path().filename() != L"copy.txt" && entry.is_regular_file() &&
                read(entry.path()) == "original contents") renamedCopy = true;
        }
        require(renamedCopy, "collision creates a separate copy");

        write(source / L"move.txt", "moved contents");
        selected = selection(source / L"move.txt");
        succeeded(ShellOperations::copyOrMove(nullptr, selected.Get(), target.Get(), true, true),
                  "silent move");
        require(!fs::exists(source / L"move.txt"), "move removes source");
        require(read(destination / L"move.txt") == "moved contents", "move preserves contents");

        auto moved = item(destination / L"move.txt");
        require(ShellOperations::rename(nullptr, moved.Get(), L"..", true) == E_INVALIDARG,
                "rename rejects parent traversal");
        require(ShellOperations::rename(nullptr, moved.Get(), L"CON.txt", true) == E_INVALIDARG,
                "rename rejects reserved Windows devices");
        require(FAILED(ShellOperations::rename(nullptr, moved.Get(), L"copy.txt", true)),
                "rename rejects collision");
        require(read(destination / L"copy.txt") == "existing destination", "rename preserves collision");
        succeeded(ShellOperations::rename(nullptr, moved.Get(), L"renamed-\u03bb.txt", true), "silent Unicode rename");
        require(!fs::exists(destination / L"move.txt"), "rename removes old name");
        require(read(destination / L"renamed-\u03bb.txt") == "moved contents", "Unicode rename preserves contents");

        succeeded(ShellOperations::newFolder(nullptr, target.Get(), L"New folder \u03bb", true),
                  "silent Unicode folder creation");
        require(fs::is_directory(destination / L"New folder \u03bb"), "folder created");
        require(FAILED(ShellOperations::newFolder(nullptr, target.Get(), L"New folder \u03bb", true)),
                "new folder rejects collision");
        require(ShellOperations::newFolder(nullptr, target.Get(), L"LPT1", true) == E_INVALIDARG,
                "new folder rejects reserved Windows devices");

        selected = selection(destination / L"renamed-\u03bb.txt");
        succeeded(ShellOperations::remove(nullptr, selected.Get(), true, true), "silent permanent delete");
        require(!fs::exists(destination / L"renamed-\u03bb.txt"), "permanent delete removes fixture");
        require(read(source / L"copy.txt") == "original contents", "delete preserves unrelated source");
        std::cout << "Shell operations: headless copy, collision, move, Unicode rename, new-folder and permanent delete passed\n";
    } catch (const std::exception& error) {
        std::cerr << "Shell operations failed: " << error.what() << '\n';
        ++failures;
    }
    ShellOperations::flushClipboardIfOwned();
    OleUninitialize();
    return failures;
}
