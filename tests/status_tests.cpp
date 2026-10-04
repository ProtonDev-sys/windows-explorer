#include "explorer/status.hpp"
#include <shlobj.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void succeeded(HRESULT hr, const char* message) { require(SUCCEEDED(hr), message); }
struct Fixture {
    std::filesystem::path root;
    Fixture() {
        GUID id{}; succeeded(CoCreateGuid(&id), "Create status fixture ID");
        wchar_t name[40]{}; StringFromGUID2(id, name, ARRAYSIZE(name));
        root = std::filesystem::temp_directory_path() / (std::wstring(L"WindowsExplorer-Status-") + name);
        require(std::filesystem::create_directory(root), "Create status fixture");
        std::ofstream(root / L"empty.txt", std::ios::binary);
        std::ofstream(root / L"\u65E5\u672C\u8A9E.bin", std::ios::binary) << std::string(1536, 's');
        std::filesystem::create_directory(root / L"folder");
    }
    ~Fixture() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
    ComPtr<IShellItemArray> items(std::initializer_list<const wchar_t*> names) {
        std::vector<PIDLIST_ABSOLUTE> ids;
        for (const auto name : names) {
            PIDLIST_ABSOLUTE id = nullptr;
            succeeded(SHParseDisplayName((root / name).c_str(), nullptr, &id, 0, nullptr), "Parse status item");
            ids.push_back(id);
        }
        ComPtr<IShellItemArray> result;
        const auto hr = SHCreateShellItemArrayFromIDLists(static_cast<UINT>(ids.size()),
            const_cast<PCIDLIST_ABSOLUTE*>(ids.data()), &result);
        for (const auto id : ids) CoTaskMemFree(id);
        succeeded(hr, "Create status selection");
        return result;
    }
};
}
int runStatusTests() {
    try {
        explorer::SelectionStatus status;
        succeeded(explorer::selectionStatus(nullptr, &status), "Empty status selection");
        require(!status.count && !status.bytes, "No selection must omit size");
        require(explorer::selectionStatus(nullptr, nullptr) == E_POINTER, "Null status output");
        Fixture fixture;
        auto empty = fixture.items({L"empty.txt"});
        succeeded(explorer::selectionStatus(empty.Get(), &status), "Zero-byte status");
        require(status.count == 1 && status.bytes == 0, "Zero-byte file is a known size");
        auto files = fixture.items({L"empty.txt", L"\u65E5\u672C\u8A9E.bin"});
        succeeded(explorer::selectionStatus(files.Get(), &status), "Native fast selected size");
        require(status.count == 2 && status.bytes == 1536, "Native file totals");
        require(explorer::statusText(3, status).find(L"2 items selected") != std::wstring::npos,
                "Windows selected status wording");
        auto mixed = fixture.items({L"empty.txt", L"folder"});
        succeeded(explorer::selectionStatus(mixed.Get(), &status), "Mixed selected status");
        require(status.count == 2 && !status.bytes, "Do not recurse selected folders");
        std::cout << "PASS: native fast status metadata and selected-size formatting\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: status metadata: " << error.what() << '\n'; return 1;
    }
}
