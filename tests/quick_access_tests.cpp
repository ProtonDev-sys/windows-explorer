#include "explorer/quick_access.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
using explorer::QuickAccessToolbar;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Fixture {
    std::filesystem::path root;
    Fixture() {
        const auto temporary = std::filesystem::temp_directory_path();
        for (unsigned attempt = 0; attempt < 16; ++attempt) {
            root = temporary / (L"WindowsExplorer-QAT-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(root, error)) return;
            if (error) throw std::runtime_error("Cannot create QAT fixture directory");
        }
        throw std::runtime_error("Cannot allocate QAT fixture directory");
    }
    ~Fixture() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
};

void write(const std::filesystem::path& path, const std::string& contents) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    require(stream.good(), "Cannot write QAT fixture");
}

void defaultsAndCustomization() {
    QuickAccessToolbar toolbar;
    require(toolbar.commands() == std::vector<explorer::Command>{explorer::Properties, explorer::NewFolder} && !toolbar.belowRibbon(), "QAT defaults are wrong");
    require(toolbar.add(explorer::Copy) && toolbar.add(explorer::Paste, 0), "Cannot customize QAT");
    require(toolbar.commands() == std::vector<explorer::Command>{explorer::Paste, explorer::Properties, explorer::NewFolder, explorer::Copy}, "QAT insertion lost order");
    require(toolbar.move(explorer::Paste, 3), "Cannot move QAT command to end");
    require(toolbar.commands() == std::vector<explorer::Command>{explorer::Properties, explorer::NewFolder, explorer::Copy, explorer::Paste}, "QAT rightward move lost order");
    require(toolbar.move(explorer::Copy, 0), "Cannot move QAT command to start");
    require(toolbar.commands() == std::vector<explorer::Command>{explorer::Copy, explorer::Properties, explorer::NewFolder, explorer::Paste}, "QAT leftward move lost order");
    require(toolbar.move(explorer::Copy, 0), "Moving to current position must succeed");
    require(toolbar.remove(explorer::Properties) && !toolbar.contains(explorer::Properties), "Cannot remove QAT command");
    toolbar.setBelowRibbon(true);
    const auto unchanged = toolbar.commands();
    require(!toolbar.add(explorer::Copy) && !toolbar.add(explorer::Rename, 99) && !toolbar.move(explorer::Paste, 99) && !toolbar.move(explorer::Properties, 0) && !toolbar.remove(explorer::Properties), "Invalid QAT customization was accepted");
    require(toolbar.commands() == unchanged && toolbar.belowRibbon(), "Invalid customization mutated QAT");
    toolbar.clear();
    require(toolbar.commands().empty() && toolbar.belowRibbon() && !toolbar.move(explorer::Copy, 0), "Empty QAT or placement is wrong");
    toolbar.reset();
    require(toolbar.commands() == std::vector<explorer::Command>{explorer::Properties, explorer::NewFolder} && !toolbar.belowRibbon(), "Reset did not restore defaults");
}

void eligibleCommandsAndBounds() {
    std::set<UINT> identifiers;
    std::set<std::string_view> tokens;
    for (const auto& entry : explorer::quickAccessCommands()) {
        require(identifiers.insert(entry.command).second && tokens.insert(entry.token).second, "QAT catalog has duplicate commands or tokens");
        require(!entry.label.empty() && !entry.token.empty() && entry.icon >= SIID_DOCNOASSOC && entry.icon < SIID_MAX_ICONS, "QAT catalog lacks native label/icon metadata");
        require(explorer::quickAccessCommand(entry.command) == &entry, "QAT catalog lookup is inconsistent");
    }
    constexpr std::array ineligible{explorer::Undo, explorer::Redo, explorer::FileMenu, explorer::ViewMenu, explorer::SortMenu, explorer::GroupMenu, explorer::HistoryMenu, explorer::ColumnsMenu, explorer::SearchKindMenu, explorer::SearchDateMenu, explorer::SearchSizeMenu, explorer::RecentSearches, explorer::BreadcrumbFirst, static_cast<explorer::Command>(0), static_cast<explorer::Command>(999999)};
    QuickAccessToolbar toolbar;
    toolbar.clear();
    for (const auto command : ineligible) require(!explorer::quickAccessCommand(command) && !toolbar.add(command), "Unsupported or menu command admitted to QAT");
    const auto catalog = explorer::quickAccessCommands();
    require(catalog.size() > QuickAccessToolbar::MaximumCommands, "Catalog must support meaningful bounded customization");
    for (std::size_t index = 0; index < QuickAccessToolbar::MaximumCommands; ++index) require(toolbar.add(catalog[index].command), "QAT rejected a command below capacity");
    const auto full = toolbar.commands();
    require(!toolbar.add(catalog[QuickAccessToolbar::MaximumCommands].command) && toolbar.commands() == full, "QAT exceeded its capacity");
    require(toolbar.remove(full.front()) && toolbar.add(catalog[QuickAccessToolbar::MaximumCommands].command), "QAT capacity was not released after removal");
}

void unicodePersistenceAndReplacement() {
    Fixture fixture;
    const auto path = fixture.root / L"\u8A2D\u5B9A \U0001F4C1" / L"qat.ini";
    const auto missing = explorer::loadQuickAccessToolbar(path);
    require(missing.commands() == QuickAccessToolbar{}.commands() && !missing.belowRibbon(), "Missing QAT settings must use defaults");
    QuickAccessToolbar toolbar;
    toolbar.clear();
    toolbar.add(explorer::Rename);
    toolbar.add(explorer::NewFolder);
    toolbar.add(explorer::Copy);
    toolbar.setBelowRibbon(true);
    require(explorer::saveQuickAccessToolbar(path, toolbar), "Cannot save QAT on a Unicode path");
    const auto loaded = explorer::loadQuickAccessToolbar(path);
    require(loaded.commands() == toolbar.commands() && loaded.belowRibbon(), "QAT order or placement did not round-trip");
    toolbar.move(explorer::Copy, 0);
    toolbar.setBelowRibbon(false);
    require(explorer::saveQuickAccessToolbar(path, toolbar), "Cannot replace QAT settings");
    require(explorer::loadQuickAccessToolbar(path).commands() == toolbar.commands() && !explorer::loadQuickAccessToolbar(path).belowRibbon(), "Replacement did not persist updated QAT");
    toolbar.clear();
    require(explorer::saveQuickAccessToolbar(path, toolbar) && explorer::loadQuickAccessToolbar(path).commands().empty(), "Intentionally empty QAT did not round-trip");
    for (const auto& file : std::filesystem::directory_iterator(path.parent_path())) require(file.path() == path, "QAT save leaked temporary files");
    require(!explorer::saveQuickAccessToolbar({}, toolbar) && !explorer::saveQuickAccessToolbar(fixture.root, toolbar), "Invalid QAT settings target was accepted");
    const auto blocked = fixture.root / L"blocked.ini";
    write(blocked, "version=1\ncommands=properties,new-folder\n");
    const HANDLE locked = CreateFileW(blocked.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(locked != INVALID_HANDLE_VALUE, "Cannot lock QAT fixture");
    const bool saved = explorer::saveQuickAccessToolbar(blocked, toolbar);
    CloseHandle(locked);
    require(!saved && explorer::loadQuickAccessToolbar(blocked).commands() == QuickAccessToolbar{}.commands(), "Failed atomic QAT save damaged previous settings");
    for (const auto& file : std::filesystem::directory_iterator(fixture.root)) require(file.path().filename().wstring().find(L".tmp-") == std::wstring::npos, "Failed QAT save leaked temporary files");
}

void corruptAndFutureSettings() {
    Fixture fixture;
    const auto path = fixture.root / L"qat.ini";
    write(path, "\xEF\xBB\xBF# Unicode comment: \xE6\x97\xA5\xE6\x9C\xAC\r\nversion=1\r\nbelowRibbon=true\r\ncommands=copy,unknown-future,properties,copy,new-folder,paste,properties\r\nignored=future\r\n");
    auto loaded = explorer::loadQuickAccessToolbar(path);
    require(loaded.commands() == std::vector<explorer::Command>{explorer::Copy, explorer::Properties, explorer::NewFolder, explorer::Paste} && loaded.belowRibbon(), "QAT unknown/duplicate tokens did not preserve supported order");
    write(path, "version=1\nbelowRibbon=not-a-bool\ncommands=unknown,undo,redo,file-menu\n");
    loaded = explorer::loadQuickAccessToolbar(path);
    require(loaded.commands() == QuickAccessToolbar{}.commands() && !loaded.belowRibbon(), "Unrecognized QAT values must fall back to defaults");
    const std::array<std::string, 7> corrupt{
        "version=2\ncommands=copy\nbelowRibbon=true\n",
        "commands=copy\nbelowRibbon=true\n",
        "version=1\ncommands=copy,,paste\n",
        "version=1\ncommands=copy,\n",
        "version=1\ncommands=copy;paste\n",
        std::string("version=1\ncommands=copy\n") + static_cast<char>(0xFF),
        std::string("version=1\ncommands=copy\n") + std::string(1, '\0')
    };
    for (const auto& contents : corrupt) {
        write(path, contents);
        loaded = explorer::loadQuickAccessToolbar(path);
        require(loaded.commands() == QuickAccessToolbar{}.commands() && !loaded.belowRibbon(), "Corrupt QAT settings did not use defaults");
    }
    write(path, std::string(8193, 'x'));
    require(explorer::loadQuickAccessToolbar(path).commands() == QuickAccessToolbar{}.commands(), "Oversized QAT file did not fall back");
    std::string contents = "version=1\ncommands=";
    for (const auto& entry : explorer::quickAccessCommands()) { if (contents.back() != '=') contents += ','; contents += entry.token; }
    write(path, contents + "\nbelowRibbon=1\n");
    loaded = explorer::loadQuickAccessToolbar(path);
    require(loaded.commands().size() == QuickAccessToolbar::MaximumCommands && loaded.belowRibbon(), "Persisted QAT exceeded bounds or lost placement");
    const auto catalog = explorer::quickAccessCommands();
    for (std::size_t index = 0; index < loaded.commands().size(); ++index) require(loaded.commands()[index] == catalog[index].command, "Bounded QAT load lost command order");
}
} // namespace

int runQuickAccessTests() {
    int failures = 0;
    const std::array tests{
        std::pair{"QAT customization and order", defaultsAndCustomization},
        std::pair{"QAT eligible commands and bounds", eligibleCommandsAndBounds},
        std::pair{"QAT Unicode persistence and atomic replacement", unicodePersistenceAndReplacement},
        std::pair{"QAT corrupt, unknown and duplicate settings", corruptAndFutureSettings}
    };
    for (const auto& [name, test] : tests) {
        try { test(); std::cout << "PASS: " << name << '\n'; }
        catch (const std::exception& error) { ++failures; std::cerr << "FAIL: " << name << ": " << error.what() << '\n'; }
        catch (...) { ++failures; std::cerr << "FAIL: " << name << ": unknown exception\n"; }
    }
    return failures;
}
