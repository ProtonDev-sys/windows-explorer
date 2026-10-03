#pragma once

#include <windows.h>
#include <filesystem>
#include <vector>

namespace explorer {
// These synchronous operations may be called on a background worker. They never
// display a window or overwrite an existing destination. Extraction accepts
// ordinary single-volume stored/deflated ZIPs, with safe relative paths only.
class ExtraOperations final {
public:
    static HRESULT createZip(const std::vector<std::filesystem::path>& sources,
                             const std::filesystem::path& output);
    static HRESULT extractZip(const std::filesystem::path& archive,
                              const std::filesystem::path& destination);
    static HRESULT createShortcut(const std::filesystem::path& target,
                                  const std::filesystem::path& output);
};
}
