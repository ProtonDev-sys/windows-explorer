#pragma once
#include "explorer/core.hpp"
#include <span>
#include <vector>

namespace explorer {
constexpr size_t maximumTypedAddresses = 20;
bool typedAddressHistoryAllowed();
std::filesystem::path addressHistoryPath();
HRESULT loadAddressHistory(const std::filesystem::path& path, std::vector<std::wstring>* result);
HRESULT saveAddressHistory(const std::filesystem::path& path, std::span<const std::wstring> addresses);
// Preserves the exact typed spelling; case-insensitive ordinal duplicates move
// to the front and take the newest spelling. Invalid input leaves the MRU intact.
bool rememberTypedAddress(std::vector<std::wstring>& addresses, const std::wstring& address);
// Own entries retain priority; valid imported entries follow in their supplied
// order. No expansion, trimming, filesystem probing or shared history writes.
HRESULT mergeTypedAddressHistory(std::vector<std::wstring>& addresses, std::span<const std::wstring> imported);
// Normal startup only. Reads native TypedPaths, retaining REG_EXPAND_SZ text
// without expansion. Tests use readTypedAddressRegistry with an owned key.
HRESULT loadWindowsTypedAddresses(std::vector<std::wstring>* result);
HRESULT readTypedAddressRegistry(HKEY key, std::vector<std::wstring>* result);
} // namespace explorer
