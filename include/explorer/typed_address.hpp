#pragma once
#include "explorer/core.hpp"
#include <functional>
#include <string_view>

namespace explorer {
struct TypedAddressLaunch {
    std::wstring target;
    std::wstring parameters;
    std::wstring directory;
};
// Splits explicit executable input while retaining its parameter quoting.
// Protocol addresses stay whole. This function performs no resolution or IO.
HRESULT parseTypedAddressLaunch(std::wstring_view input, TypedAddressLaunch* result);
using TypedAddressLauncher = std::function<HRESULT(HWND, const TypedAddressLaunch&)>;
// The headless guard precedes parsing, the optional owned test callback and
// ShellExecuteEx. Normal application callers use the native default launcher.
HRESULT launchTypedAddress(HWND owner, std::wstring_view input, bool headless,
                           const TypedAddressLauncher& testLauncher = {}, std::wstring_view directory = {});
}
