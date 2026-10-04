#include "explorer/status.hpp"
#include <propkey.h>
#include <propsys.h>
#include <propvarutil.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <limits>

namespace explorer {
namespace {
std::wstring countText(unsigned count) {
    const auto text = std::to_wstring(count);
    wchar_t separator[16]{}, decimal[16]{};
    wchar_t output[64]{};
    GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_STHOUSAND, separator, ARRAYSIZE(separator));
    GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SDECIMAL, decimal, ARRAYSIZE(decimal));
    NUMBERFMTW format{};
    format.LeadingZero = 1;
    format.Grouping = 3;
    format.lpDecimalSep = decimal;
    format.lpThousandSep = separator;
    format.NegativeOrder = 1;
    if (GetNumberFormatEx(LOCALE_NAME_USER_DEFAULT, 0, text.c_str(), &format,
                          output, ARRAYSIZE(output))) return output;
    return text;
}
}
HRESULT selectionStatus(IShellItemArray* selection, SelectionStatus* output) {
    if (!output) return E_POINTER;
    SelectionStatus result;
    if (!selection) { *output = result; return S_OK; }
    auto hr = selection->GetCount(&result.count);
    if (FAILED(hr)) return hr;
    // A huge selection must not turn a status update into eager metadata work.
    // Counts remain exact, while size is omitted when unavailable or bounded.
    if (!result.count || result.count > 4096) { *output = result; return S_OK; }
    std::uint64_t total = 0;
    for (DWORD index = 0; index < result.count; ++index) {
        Microsoft::WRL::ComPtr<IShellItem> item;
        hr = selection->GetItemAt(index, &item);
        if (FAILED(hr)) return hr;
        SFGAOF attributes = 0;
        if (FAILED(item->GetAttributes(SFGAO_FOLDER | SFGAO_FILESYSTEM, &attributes))) {
            *output = result; return S_OK;
        }
        if (attributes & SFGAO_FOLDER) {
            // ZIP files can expose SFGAO_FOLDER. Only a physical directory
            // suppresses the selected-size display for that filesystem item.
            bool directory = true;
            if (attributes & SFGAO_FILESYSTEM) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    const auto fileAttributes = GetFileAttributesW(path);
                    directory = fileAttributes == INVALID_FILE_ATTRIBUTES ||
                        (fileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                    CoTaskMemFree(path);
                }
            }
            if (directory) { *output = result; return S_OK; }
        }
        Microsoft::WRL::ComPtr<IShellItem2> item2;
        Microsoft::WRL::ComPtr<IPropertyStore> store;
        if (FAILED(item.As(&item2)) || FAILED(item2->GetPropertyStore(
            static_cast<GETPROPERTYSTOREFLAGS>(GPS_FASTPROPERTIESONLY | GPS_BESTEFFORT),
            IID_PPV_ARGS(&store)))) { *output = result; return S_OK; }
        PROPVARIANT value{};
        PropVariantInit(&value);
        hr = store->GetValue(PKEY_Size, &value);
        ULONGLONG size = 0;
        const auto converted = SUCCEEDED(hr) && value.vt != VT_EMPTY && value.vt != VT_NULL
            ? PropVariantToUInt64(value, &size) : E_FAIL;
        PropVariantClear(&value);
        if (FAILED(converted) || size > std::numeric_limits<std::uint64_t>::max() - total) {
            *output = result; return S_OK;
        }
        total += size;
    }
    result.bytes = total;
    *output = result;
    return S_OK;
}
std::wstring statusText(unsigned itemCount, const SelectionStatus& selected) {
    auto text = countText(itemCount) + (itemCount == 1 ? L" item" : L" items");
    if (!selected.count) return text;
    text += L"    " + countText(selected.count) +
        (selected.count == 1 ? L" item selected" : L" items selected");
    if (selected.bytes) {
        wchar_t formatted[64]{};
        if (*selected.bytes <= static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max()) &&
            StrFormatByteSizeW(static_cast<LONGLONG>(*selected.bytes), formatted, ARRAYSIZE(formatted)))
            text += L"  " + std::wstring(formatted);
    }
    return text;
}
}
