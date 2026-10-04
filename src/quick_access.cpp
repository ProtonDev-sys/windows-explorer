#include "explorer/quick_access.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

namespace explorer {
namespace {
constexpr std::array catalog{
    QuickAccessCommand{Properties, L"Properties", SIID_INFO, "properties"},
    QuickAccessCommand{NewFolder, L"New folder", SIID_FOLDER, "new-folder"},
    QuickAccessCommand{NewWindow, L"New window", SIID_APPLICATION, "new-window"},
    QuickAccessCommand{Refresh, L"Refresh", SIID_FOLDEROPEN, "refresh"},
    QuickAccessCommand{Copy, L"Copy", SIID_DOCNOASSOC, "copy"},
    QuickAccessCommand{Cut, L"Cut", SIID_DOCNOASSOC, "cut"},
    QuickAccessCommand{Paste, L"Paste", SIID_DOCASSOC, "paste"},
    QuickAccessCommand{CopyPath, L"Copy path", SIID_LINK, "copy-path"},
    QuickAccessCommand{CopyTo, L"Copy to", SIID_FOLDER, "copy-to"},
    QuickAccessCommand{MoveTo, L"Move to", SIID_FOLDEROPEN, "move-to"},
    QuickAccessCommand{Delete, L"Delete", SIID_DELETE, "delete"},
    QuickAccessCommand{PermanentDelete, L"Permanently delete", SIID_DELETE, "permanent-delete"},
    QuickAccessCommand{Rename, L"Rename", SIID_RENAME, "rename"},
    QuickAccessCommand{NewText, L"New text document", SIID_DOCNOASSOC, "new-text"},
    QuickAccessCommand{NewShortcut, L"New shortcut", SIID_LINK, "new-shortcut"},
    QuickAccessCommand{PasteShortcut, L"Paste shortcut", SIID_LINK, "paste-shortcut"},
    QuickAccessCommand{Open, L"Open", SIID_FOLDEROPEN, "open"},
    QuickAccessCommand{Edit, L"Edit", SIID_DOCASSOC, "edit"},
    QuickAccessCommand{Pin, L"Pin to Quick access", SIID_FOLDER, "pin"},
    QuickAccessCommand{SelectAll, L"Select all", SIID_DOCASSOC, "select-all"},
    QuickAccessCommand{SelectNone, L"Select none", SIID_DOCNOASSOC, "select-none"},
    QuickAccessCommand{Invert, L"Invert selection", SIID_DOCASSOC, "invert-selection"},
    QuickAccessCommand{Print, L"Print", SIID_PRINTER, "print"},
    QuickAccessCommand{Sharing, L"Share", SIID_SHARE, "sharing"},
    QuickAccessCommand{Security, L"Advanced security", SIID_LOCK, "security"},
    QuickAccessCommand{FolderOptions, L"Folder options", SIID_SETTINGS, "folder-options"},
    QuickAccessCommand{MapDrive, L"Map network drive", SIID_NETWORKCONNECT, "map-drive"},
    QuickAccessCommand{DisconnectDrive, L"Disconnect network drive", SIID_MYNETWORK, "disconnect-drive"},
    QuickAccessCommand{Terminal, L"Open terminal", SIID_APPLICATION, "terminal"},
    QuickAccessCommand{NavigationPane, L"Navigation pane", SIID_FOLDER, "navigation-pane"},
    QuickAccessCommand{PreviewPane, L"Preview pane", SIID_DOCASSOC, "preview-pane"},
    QuickAccessCommand{DetailsPane, L"Details pane", SIID_INFO, "details-pane"},
    QuickAccessCommand{HiddenItems, L"Hidden items", SIID_FOLDER, "hidden-items"},
    QuickAccessCommand{Extensions, L"File name extensions", SIID_DOCASSOC, "extensions"},
    QuickAccessCommand{Collapse, L"Collapse ribbon", SIID_SETTINGS, "collapse-ribbon"},
    QuickAccessCommand{Checkboxes, L"Item check boxes", SIID_DOCASSOC, "checkboxes"},
    QuickAccessCommand{Zip, L"Compress to ZIP", SIID_ZIPFILE, "zip"},
    QuickAccessCommand{Extract, L"Extract all", SIID_FOLDEROPEN, "extract"},
    QuickAccessCommand{FileHistory, L"File history", SIID_FOLDERBACK, "file-history"},
    QuickAccessCommand{HideSelected, L"Hide selected items", SIID_FOLDER, "hide-selected"},
    QuickAccessCommand{SizeColumns, L"Size all columns to fit", SIID_DOCASSOC, "size-columns"},
    QuickAccessCommand{OpenFileLocation, L"Open file location", SIID_FOLDEROPEN, "open-file-location"},
    QuickAccessCommand{CloseSearch, L"Close search", SIID_FIND, "close-search"},
    QuickAccessCommand{SearchSubfolders, L"Search all subfolders", SIID_FIND, "search-subfolders"},
    QuickAccessCommand{SearchCurrent, L"Search current folder", SIID_FIND, "search-current"},
    QuickAccessCommand{SaveSearch, L"Save search", SIID_FIND, "save-search"},
    QuickAccessCommand{Fullscreen, L"Full screen", SIID_APPLICATION, "fullscreen"},
    QuickAccessCommand{SortName, L"Sort by name", SIID_DOCASSOC, "sort-name"},
    QuickAccessCommand{SortDate, L"Sort by date modified", SIID_DOCASSOC, "sort-date"},
    QuickAccessCommand{SortType, L"Sort by type", SIID_DOCASSOC, "sort-type"},
    QuickAccessCommand{SortSize, L"Sort by size", SIID_DOCASSOC, "sort-size"},
    QuickAccessCommand{SortAscending, L"Sort ascending", SIID_DOCASSOC, "sort-ascending"},
    QuickAccessCommand{SortDescending, L"Sort descending", SIID_DOCASSOC, "sort-descending"},
    QuickAccessCommand{GroupNone, L"Group by none", SIID_DOCASSOC, "group-none"},
    QuickAccessCommand{GroupName, L"Group by name", SIID_DOCASSOC, "group-name"},
    QuickAccessCommand{GroupDate, L"Group by date modified", SIID_DOCASSOC, "group-date"},
    QuickAccessCommand{GroupType, L"Group by type", SIID_DOCASSOC, "group-type"},
    QuickAccessCommand{GroupSize, L"Group by size", SIID_DOCASSOC, "group-size"},
    QuickAccessCommand{ViewFirst, L"Extra large icons", SIID_DOCASSOC, "view-extra-large"},
    QuickAccessCommand{static_cast<Command>(ViewFirst + 1), L"Large icons", SIID_DOCASSOC, "view-large"},
    QuickAccessCommand{static_cast<Command>(ViewFirst + 2), L"Medium icons", SIID_DOCASSOC, "view-medium"},
    QuickAccessCommand{static_cast<Command>(ViewFirst + 3), L"Small icons", SIID_DOCASSOC, "view-small"},
    QuickAccessCommand{static_cast<Command>(ViewFirst + 4), L"List", SIID_DOCASSOC, "view-list"},
    QuickAccessCommand{static_cast<Command>(ViewFirst + 5), L"Details", SIID_DOCASSOC, "view-details"},
    QuickAccessCommand{static_cast<Command>(ViewFirst + 6), L"Tiles", SIID_DOCASSOC, "view-tiles"},
    QuickAccessCommand{ViewLast, L"Content", SIID_DOCASSOC, "view-content"}
};
constexpr std::size_t maximumSettingsBytes = 8192;

std::string_view trimAscii(std::string_view text) noexcept {
    const auto whitespace = [](char ch) { return ch == ' ' || ch == '\t' || ch == '\r'; };
    while (!text.empty() && whitespace(text.front())) text.remove_prefix(1);
    while (!text.empty() && whitespace(text.back())) text.remove_suffix(1);
    return text;
}

const QuickAccessCommand* commandByToken(std::string_view token) noexcept {
    const auto found = std::find_if(catalog.begin(), catalog.end(), [token](const auto& command) { return command.token == token; });
    return found == catalog.end() ? nullptr : &*found;
}

bool readCommands(std::string_view value, QuickAccessToolbar& toolbar) {
    value = trimAscii(value);
    QuickAccessToolbar parsed;
    parsed.clear();
    if (value.empty()) { toolbar = std::move(parsed); return true; }
    while (true) {
        const auto comma = value.find(',');
        const auto token = trimAscii(value.substr(0, comma));
        if (token.empty()) return false;
        for (const char ch : token) if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-')) return false;
        if (const auto command = commandByToken(token)) parsed.add(command->command);
        if (comma == std::string_view::npos) break;
        value.remove_prefix(comma + 1);
    }
    if (parsed.commands().empty()) return false;
    toolbar = std::move(parsed);
    return true;
}
} // namespace

std::span<const QuickAccessCommand> quickAccessCommands() noexcept { return catalog; }

const QuickAccessCommand* quickAccessCommand(Command command) noexcept {
    const auto found = std::find_if(catalog.begin(), catalog.end(), [command](const auto& entry) { return entry.command == command; });
    return found == catalog.end() ? nullptr : &*found;
}

QuickAccessToolbar::QuickAccessToolbar() : commands_{Properties, NewFolder} {}
const std::vector<Command>& QuickAccessToolbar::commands() const noexcept { return commands_; }
bool QuickAccessToolbar::contains(Command command) const noexcept { return std::find(commands_.begin(), commands_.end(), command) != commands_.end(); }

bool QuickAccessToolbar::add(Command command, std::size_t index) {
    if (!quickAccessCommand(command) || contains(command) || commands_.size() >= MaximumCommands) return false;
    if (index == Append) index = commands_.size();
    if (index > commands_.size()) return false;
    commands_.insert(commands_.begin() + static_cast<std::ptrdiff_t>(index), command);
    return true;
}

bool QuickAccessToolbar::remove(Command command) noexcept {
    const auto found = std::find(commands_.begin(), commands_.end(), command);
    if (found == commands_.end()) return false;
    commands_.erase(found);
    return true;
}

bool QuickAccessToolbar::move(Command command, std::size_t destinationIndex) noexcept {
    if (destinationIndex >= commands_.size()) return false;
    const auto found = std::find(commands_.begin(), commands_.end(), command);
    if (found == commands_.end()) return false;
    const auto destination = commands_.begin() + static_cast<std::ptrdiff_t>(destinationIndex);
    if (found < destination) std::rotate(found, found + 1, destination + 1);
    else if (destination < found) std::rotate(destination, found, found + 1);
    return true;
}

void QuickAccessToolbar::clear() noexcept { commands_.clear(); }
void QuickAccessToolbar::reset() { commands_ = {Properties, NewFolder}; belowRibbon_ = false; }
bool QuickAccessToolbar::belowRibbon() const noexcept { return belowRibbon_; }
void QuickAccessToolbar::setBelowRibbon(bool below) noexcept { belowRibbon_ = below; }

QuickAccessToolbar loadQuickAccessToolbar(const std::filesystem::path& path) {
    QuickAccessToolbar result;
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > maximumSettingsBytes) return result;
    std::ifstream input(path, std::ios::binary);
    if (!input) return result;
    std::string contents(maximumSettingsBytes + 1, '\0');
    input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    contents.resize(static_cast<std::size_t>(input.gcount()));
    if (input.bad() || contents.size() > maximumSettingsBytes || contents.find('\0') != std::string::npos) return result;
    if (contents.starts_with("\xEF\xBB\xBF")) contents.erase(0, 3);
    if (!contents.empty() && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, contents.data(), static_cast<int>(contents.size()), nullptr, 0)) return result;
    std::istringstream lines(contents);
    std::string line;
    bool supportedVersion = false;
    bool belowRibbon = false;
    while (std::getline(lines, line)) {
        const auto text = trimAscii(line);
        if (text.empty() || text.front() == '#' || text.front() == ';') continue;
        const auto equals = text.find('=');
        if (equals == std::string_view::npos) return {};
        const auto key = trimAscii(text.substr(0, equals));
        const auto value = trimAscii(text.substr(equals + 1));
        if (key == "version") { if (value != "1") return {}; supportedVersion = true; }
        else if (key == "belowRibbon") belowRibbon = value == "true" || value == "1";
        else if (key == "commands") {
            QuickAccessToolbar parsed;
            if (readCommands(value, parsed)) result = std::move(parsed);
            else result.reset();
        }
    }
    if (!supportedVersion) return {};
    result.setBelowRibbon(belowRibbon);
    return result;
}

bool saveQuickAccessToolbar(const std::filesystem::path& path, const QuickAccessToolbar& toolbar) {
    if (path.empty()) return false;
    std::ostringstream output;
    output << "# WindowsExplorer Quick Access Toolbar (UTF-8).\nversion=1\nbelowRibbon="
        << (toolbar.belowRibbon() ? "true" : "false") << "\ncommands=";
    bool separator = false;
    for (const auto command : toolbar.commands()) {
        const auto entry = quickAccessCommand(command);
        if (!entry) return false;
        if (separator) output << ',';
        output << entry->token;
        separator = true;
    }
    output << '\n';
    const auto contents = output.str();
    std::error_code error;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) return false;
    }
    std::filesystem::path temporary;
    HANDLE file = INVALID_HANDLE_VALUE;
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        temporary = path;
        temporary += L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetCurrentThreadId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(attempt);
        file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    }
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool wrote = WriteFile(file, contents.data(), static_cast<DWORD>(contents.size()), &written, nullptr) && written == contents.size() && FlushFileBuffers(file);
    const bool closed = CloseHandle(file) != FALSE;
    if (!wrote || !closed || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

} // namespace explorer
