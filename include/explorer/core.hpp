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
    bool showHidden = false;
    bool showExtensions = true;
    bool ribbonCollapsed = false;
    ViewMode view = ViewMode::Details;
    int windowWidth = 1200;
    int windowHeight = 800;
    std::wstring startupLocation = L"shell:MyComputerFolder";
};

std::filesystem::path preferencesPath();
Preferences loadPreferences(const std::filesystem::path& path);
bool savePreferences(const std::filesystem::path& path, const Preferences& preferences);
std::wstring trim(const std::wstring& text);
std::wstring expandEnvironment(const std::wstring& text);
std::wstring searchUri(const std::wstring& query, const std::wstring& scope);
std::wstring formatBytes(std::uint64_t bytes);
bool validLeafName(const std::wstring& name);
std::wstring hresultMessage(HRESULT result);

} // namespace explorer
