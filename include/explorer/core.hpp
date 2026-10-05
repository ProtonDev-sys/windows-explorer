#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <string>

namespace explorer {

enum class ViewMode { ExtraLargeIcons, LargeIcons, MediumIcons, SmallIcons, List, Details, Tiles, Content };

struct Preferences {
    bool navigationPane = true;
    bool previewPane = false;
    bool detailsPane = false;
    bool expandToCurrent = false;
    bool showAllFolders = false;
    bool showLibraries = false;
    bool showHidden = false;
    bool showExtensions = true;
    bool ribbonCollapsed = false;
    ViewMode view = ViewMode::Details;
    int windowWidth = 1200;
    int windowHeight = 800;
    int searchWidth = 146;
    bool useWindowsStartup = true;
    std::wstring startupLocation = L"shell:::{679f85cb-0220-4080-b29b-5540cc05aab6}";
};

std::filesystem::path preferencesPath();
// Read-only Windows 10 Folder Options "Open File Explorer to" preference.
std::wstring windowsDefaultStartupLocation();
Preferences loadPreferences(const std::filesystem::path& path);
bool savePreferences(const std::filesystem::path& path, const Preferences& preferences);
std::wstring trim(const std::wstring& text);
std::wstring expandEnvironment(const std::wstring& text);
bool validLeafName(const std::wstring& name);
std::wstring hresultMessage(HRESULT result);

} // namespace explorer
