#include "explorer/search_window.hpp"
#include <shlobj.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <utility>

namespace explorer {
namespace {
using Microsoft::WRL::ComPtr;
constexpr DWORD packetMagic = 0x43535745; // EWSC
constexpr DWORD packetVersion = 1;
constexpr size_t maximumPacket = 32 * 1024 * 1024;
constexpr DWORD maximumText = 32768, maximumLocations = 256, maximumPidl = 65536;
constexpr size_t headerBytes = 4 * sizeof(DWORD);
constexpr HRESULT invalidPacket = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);

struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value) CloseHandle(value); }
};
struct View {
    void* value = nullptr;
    ~View() { if (value) UnmapViewOfFile(value); }
};
struct PidlFree {
    using pointer = PIDLIST_ABSOLUTE;
    void operator()(pointer value) const noexcept { CoTaskMemFree(value); }
};
using OwnedPidl = std::unique_ptr<ITEMIDLIST, PidlFree>;
bool validText(std::wstring_view text) {
    return text.size() <= maximumText && text.find(L'\0') == std::wstring_view::npos &&
        (text.empty() || WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
            static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr) != 0);
}
bool validPidl(std::span<const BYTE> bytes) {
    if (bytes.size() < sizeof(USHORT) || bytes.size() > maximumPidl) return false;
    size_t offset = 0;
    for (;;) {
        if (bytes.size() - offset < sizeof(USHORT)) return false;
        USHORT size = 0; std::memcpy(&size, bytes.data() + offset, sizeof(size));
        if (!size) return offset + sizeof(size) == bytes.size();
        if (size < sizeof(size) || size > bytes.size() - offset) return false;
        offset += size;
    }
}
struct Writer {
    std::vector<BYTE> bytes;
    HRESULT append(const void* value, size_t size) {
        if (size > maximumPacket - bytes.size()) return invalidPacket;
        if (!size) return S_OK;
        const auto data = static_cast<const BYTE*>(value);
        bytes.insert(bytes.end(), data, data + size); return S_OK;
    }
    HRESULT number(DWORD value) { return append(&value, sizeof(value)); }
    HRESULT flag(bool value) { return number(value ? 1 : 0); }
    HRESULT text(std::wstring_view value) {
        if (!validText(value)) return E_INVALIDARG;
        auto hr = number(static_cast<DWORD>(value.size()));
        return FAILED(hr) ? hr : append(value.data(), value.size() * sizeof(wchar_t));
    }
    HRESULT optionalText(const std::optional<std::wstring>& value) {
        auto hr = flag(value.has_value()); return FAILED(hr) || !value ? hr : text(*value);
    }
    HRESULT item(IShellItem* item) {
        if (!item) return E_INVALIDARG;
        SFGAOF attributes = 0;
        auto hr = item->GetAttributes(SFGAO_FOLDER, &attributes);
        if (FAILED(hr)) return hr;
        if (!(attributes & SFGAO_FOLDER)) return HRESULT_FROM_WIN32(ERROR_DIRECTORY);
        PIDLIST_ABSOLUTE raw = nullptr;
        hr = SHGetIDListFromObject(item, &raw); OwnedPidl pidl(raw);
        if (FAILED(hr)) return hr;
        if (!raw) return E_UNEXPECTED;
        const auto size = ILGetSize(raw);
        if (!validPidl({reinterpret_cast<const BYTE*>(raw), size})) return invalidPacket;
        hr = number(size); return FAILED(hr) ? hr : append(raw, size);
    }
};
struct Reader {
    std::span<const BYTE> bytes;
    size_t offset = 0;
    HRESULT take(void* value, size_t size) {
        if (size > bytes.size() - offset) return invalidPacket;
        std::memcpy(value, bytes.data() + offset, size); offset += size; return S_OK;
    }
    HRESULT number(DWORD& value) { return take(&value, sizeof(value)); }
    HRESULT flag(bool& value) {
        DWORD raw = 0; const auto hr = number(raw);
        if (FAILED(hr)) return hr;
        if (raw > 1) return invalidPacket;
        value = raw != 0; return S_OK;
    }
    HRESULT text(std::wstring& value) {
        DWORD count = 0; auto hr = number(count);
        if (FAILED(hr)) return hr;
        if (count > maximumText || count > (bytes.size() - offset) / sizeof(wchar_t)) return invalidPacket;
        std::wstring candidate(count, L'\0');
        hr = take(candidate.data(), static_cast<size_t>(count) * sizeof(wchar_t));
        if (FAILED(hr)) return hr;
        if (!validText(candidate)) return invalidPacket;
        value = std::move(candidate); return S_OK;
    }
    HRESULT optionalText(std::optional<std::wstring>& value) {
        bool present = false; auto hr = flag(present);
        if (FAILED(hr) || !present) return hr;
        std::wstring textValue; hr = text(textValue);
        if (SUCCEEDED(hr)) value = std::move(textValue);
        return hr;
    }
    HRESULT item(ComPtr<IShellItem>& value) {
        DWORD count = 0; auto hr = number(count);
        if (FAILED(hr)) return hr;
        if (count > bytes.size() - offset || !validPidl(bytes.subspan(offset, count))) return invalidPacket;
        OwnedPidl pidl(static_cast<PIDLIST_ABSOLUTE>(CoTaskMemAlloc(count)));
        if (!pidl) return E_OUTOFMEMORY;
        std::memcpy(pidl.get(), bytes.data() + offset, count); offset += count;
        ComPtr<IShellItem> itemValue;
        hr = SHCreateItemFromIDList(pidl.get(), IID_PPV_ARGS(&itemValue));
        SFGAOF attributes = 0;
        if (SUCCEEDED(hr)) hr = itemValue->GetAttributes(SFGAO_FOLDER, &attributes);
        if (FAILED(hr)) return hr;
        if (!(attributes & SFGAO_FOLDER)) return invalidPacket;
        value = std::move(itemValue); return S_OK;
    }
};

HRESULT writePresentation(Writer& output, const SearchViewPresentation& value) {
    auto hr = validateSearchViewPresentation(value); if (FAILED(hr)) return hr;
    hr = output.flag(value.mode.has_value());
    if (SUCCEEDED(hr) && value.mode) hr = output.number(static_cast<DWORD>(*value.mode));
    if (SUCCEEDED(hr)) hr = output.flag(value.iconSize.has_value());
    if (SUCCEEDED(hr) && value.iconSize) hr = output.number(static_cast<DWORD>(*value.iconSize));
    if (SUCCEEDED(hr)) hr = output.flag(value.visibleColumns.has_value());
    if (SUCCEEDED(hr) && value.visibleColumns) {
        if (value.visibleColumns->size() > maximumLocations) return E_INVALIDARG;
        hr = output.number(static_cast<DWORD>(value.visibleColumns->size()));
        for (const auto& column : *value.visibleColumns) if (SUCCEEDED(hr)) hr = output.text(column);
    }
    const auto order = [&](const SearchViewOrder& item) {
        auto status = output.text(item.property);
        return FAILED(status) ? status : output.number(item.direction == SORT_ASCENDING ? 1 : 0);
    };
    if (SUCCEEDED(hr)) hr = output.flag(value.groupBy.has_value());
    if (SUCCEEDED(hr) && value.groupBy) hr = order(*value.groupBy);
    if (SUCCEEDED(hr)) hr = output.flag(value.sort.has_value());
    if (SUCCEEDED(hr) && value.sort) {
        if (value.sort->size() > maximumLocations) return E_INVALIDARG;
        hr = output.number(static_cast<DWORD>(value.sort->size()));
        for (const auto& sort : *value.sort) if (SUCCEEDED(hr)) hr = order(sort);
    }
    return hr;
}
HRESULT readPresentation(Reader& input, SearchViewPresentation& value) {
    bool present = false; DWORD raw = 0;
    auto hr = input.flag(present);
    if (SUCCEEDED(hr) && present) {
        hr = input.number(raw);
        if (SUCCEEDED(hr) && raw > static_cast<DWORD>(SearchViewMode::Content)) return invalidPacket;
        if (SUCCEEDED(hr)) value.mode = static_cast<SearchViewMode>(raw);
    }
    if (SUCCEEDED(hr)) hr = input.flag(present);
    if (SUCCEEDED(hr) && present) {
        hr = input.number(raw);
        if (SUCCEEDED(hr) && raw > static_cast<DWORD>((std::numeric_limits<int>::max)())) return invalidPacket;
        if (SUCCEEDED(hr)) value.iconSize = static_cast<int>(raw);
    }
    if (SUCCEEDED(hr)) hr = input.flag(present);
    if (SUCCEEDED(hr) && present) {
        hr = input.number(raw); if (FAILED(hr)) return hr;
        if (raw > maximumLocations) return invalidPacket;
        std::vector<std::wstring> columns;
        for (DWORD i = 0; i < raw; ++i) { std::wstring column; if (FAILED(hr = input.text(column))) return hr; columns.push_back(std::move(column)); }
        value.visibleColumns = std::move(columns);
    }
    const auto order = [&](SearchViewOrder& item) {
        auto status = input.text(item.property); bool ascending = false;
        if (SUCCEEDED(status)) status = input.flag(ascending);
        if (SUCCEEDED(status)) item.direction = ascending ? SORT_ASCENDING : SORT_DESCENDING;
        return status;
    };
    if (SUCCEEDED(hr)) hr = input.flag(present);
    if (SUCCEEDED(hr) && present) {
        SearchViewOrder group; if (FAILED(hr = order(group))) return hr; value.groupBy = std::move(group);
    }
    if (SUCCEEDED(hr)) hr = input.flag(present);
    if (SUCCEEDED(hr) && present) {
        hr = input.number(raw); if (FAILED(hr)) return hr;
        if (raw > maximumLocations) return invalidPacket;
        std::vector<SearchViewOrder> sorts;
        for (DWORD i = 0; i < raw; ++i) { SearchViewOrder sort; if (FAILED(hr = order(sort))) return hr; sorts.push_back(std::move(sort)); }
        value.sort = std::move(sorts);
    }
    return FAILED(hr) ? hr : validateSearchViewPresentation(value);
}
HRESULT readMapping(HANDLE mapping, SearchWindowContext* result) {
    if (!result) return E_POINTER;
    View header{MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, headerBytes)};
    if (!header.value) return HRESULT_FROM_WIN32(GetLastError());
    std::array<DWORD, 4> fields{}; std::memcpy(fields.data(), header.value, headerBytes);
    if (fields[0] != packetMagic || fields[1] != packetVersion || fields[2] < headerBytes || fields[2] > maximumPacket || fields[3]) return invalidPacket;
    View packet{MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, fields[2])};
    if (!packet.value) return HRESULT_FROM_WIN32(GetLastError());
    try {
        // A read-only handle does not prove another process has no writable
        // handle to the section. Validate only our immutable owned snapshot.
        const auto first = static_cast<const BYTE*>(packet.value);
        const std::vector<BYTE> snapshot(first, first + fields[2]);
        return decodeSearchWindowContext(snapshot, result);
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}
std::wstring quote(const std::wstring& argument) {
    std::wstring result = L"\""; size_t slashes = 0;
    for (auto code : argument) {
        if (code == L'\\') { ++slashes; continue; }
        if (code == L'\"') result.append(slashes * 2 + 1, L'\\');
        else result.append(slashes, L'\\');
        result += code; slashes = 0;
    }
    result.append(slashes * 2, L'\\'); result += L'\"'; return result;
}
} // namespace

HRESULT encodeSearchWindowContext(const SearchWindowContext& context, std::vector<BYTE>* result) {
    if (!result) return E_POINTER;
    try {
        if (context.query.empty() || !context.primaryScope || !context.scopes || context.rules.size() > maximumLocations) return E_INVALIDARG;
        Writer output; const std::array<DWORD, 4> header{packetMagic, packetVersion, 0, 0};
        auto hr = output.append(header.data(), headerBytes);
        if (SUCCEEDED(hr)) hr = output.text(context.query);
        if (SUCCEEDED(hr)) hr = output.item(context.primaryScope.Get());
        if (SUCCEEDED(hr)) hr = output.flag(context.closeOrigin != nullptr);
        if (SUCCEEDED(hr) && context.closeOrigin) hr = output.item(context.closeOrigin.Get());
        if (SUCCEEDED(hr)) hr = output.flag(context.recursive);
        DWORD count = 0; if (SUCCEEDED(hr)) hr = context.scopes->GetCount(&count);
        if (FAILED(hr)) return hr;
        if (!count || count > maximumLocations) return E_INVALIDARG;
        hr = output.number(count);
        for (DWORD i = 0; SUCCEEDED(hr) && i < count; ++i) {
            ComPtr<IShellItem> scope; hr = context.scopes->GetItemAt(i, &scope);
            if (SUCCEEDED(hr)) hr = output.item(scope.Get());
        }
        if (SUCCEEDED(hr)) hr = output.number(static_cast<DWORD>(context.rules.size()));
        bool included = context.rules.empty();
        for (const auto& rule : context.rules) {
            included = included || !rule.excluded;
            if (SUCCEEDED(hr)) hr = output.item(rule.folder.Get());
            if (SUCCEEDED(hr)) hr = output.flag(rule.recursive);
            if (SUCCEEDED(hr)) hr = output.flag(rule.excluded);
        }
        if (!included) return E_INVALIDARG;
        if (SUCCEEDED(hr)) hr = output.flag(context.presentation.has_value());
        if (SUCCEEDED(hr) && context.presentation) hr = writePresentation(output, *context.presentation);
        if (SUCCEEDED(hr)) hr = output.flag(context.fileProperties.has_value());
        if (SUCCEEDED(hr) && context.fileProperties) for (const auto* field : {
            &context.fileProperties->author, &context.fileProperties->kind, &context.fileProperties->description, &context.fileProperties->tags})
            if (SUCCEEDED(hr)) hr = output.optionalText(*field);
        if (FAILED(hr)) return hr;
        const auto size = static_cast<DWORD>(output.bytes.size()); std::memcpy(output.bytes.data() + 2 * sizeof(DWORD), &size, sizeof(size));
        *result = std::move(output.bytes); return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}
HRESULT decodeSearchWindowContext(std::span<const BYTE> packet, SearchWindowContext* result) {
    if (!result) return E_POINTER;
    try {
        if (packet.size() < headerBytes || packet.size() > maximumPacket) return invalidPacket;
        Reader input{packet}; std::array<DWORD, 4> header{};
        auto hr = input.take(header.data(), headerBytes);
        if (FAILED(hr)) return hr;
        if (header[0] != packetMagic || header[1] != packetVersion || header[2] != packet.size() || header[3]) return invalidPacket;
        SearchWindowContext candidate;
        if (FAILED(hr = input.text(candidate.query))) return hr;
        if (candidate.query.empty()) return invalidPacket;
        if (FAILED(hr = input.item(candidate.primaryScope))) return hr;
        bool present = false;
        if (FAILED(hr = input.flag(present))) return hr;
        if (present && FAILED(hr = input.item(candidate.closeOrigin))) return hr;
        if (FAILED(hr = input.flag(candidate.recursive))) return hr;
        DWORD count = 0; if (FAILED(hr = input.number(count))) return hr;
        if (!count || count > maximumLocations) return invalidPacket;
        std::vector<OwnedPidl> owned; std::vector<PCIDLIST_ABSOLUTE> raw;
        for (DWORD i = 0; i < count; ++i) {
            ComPtr<IShellItem> item; if (FAILED(hr = input.item(item))) return hr;
            PIDLIST_ABSOLUTE pidl = nullptr; hr = SHGetIDListFromObject(item.Get(), &pidl); OwnedPidl scope(pidl);
            if (FAILED(hr)) return hr;
            if (!pidl) return E_UNEXPECTED;
            raw.push_back(pidl); owned.push_back(std::move(scope));
        }
        if (FAILED(hr = SHCreateShellItemArrayFromIDLists(count, raw.data(), &candidate.scopes))) return hr;
        if (FAILED(hr = input.number(count))) return hr;
        if (count > maximumLocations) return invalidPacket;
        bool included = count == 0;
        for (DWORD i = 0; i < count; ++i) {
            SearchScopeRule rule;
            if (FAILED(hr = input.item(rule.folder)) || FAILED(hr = input.flag(rule.recursive)) || FAILED(hr = input.flag(rule.excluded))) return hr;
            included = included || !rule.excluded; candidate.rules.push_back(std::move(rule));
        }
        if (!included) return invalidPacket;
        if (FAILED(hr = input.flag(present))) return hr;
        if (present) { SearchViewPresentation value; if (FAILED(hr = readPresentation(input, value))) return hr; candidate.presentation = std::move(value); }
        if (FAILED(hr = input.flag(present))) return hr;
        if (present) {
            SearchFileProperties value;
            for (auto* field : {&value.author, &value.kind, &value.description, &value.tags}) if (FAILED(hr = input.optionalText(*field))) return hr;
            candidate.fileProperties = std::move(value);
        }
        if (input.offset != packet.size()) return invalidPacket;
        *result = std::move(candidate); return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}

SearchWindowMapping::SearchWindowMapping(SearchWindowMapping&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
SearchWindowMapping& SearchWindowMapping::operator=(SearchWindowMapping&& other) noexcept {
    if (this != &other) { if (handle_) CloseHandle(handle_); handle_ = std::exchange(other.handle_, nullptr); } return *this;
}
SearchWindowMapping::~SearchWindowMapping() { if (handle_) CloseHandle(handle_); }
HRESULT SearchWindowMapping::create(const SearchWindowContext& context, SearchWindowMapping* result) {
    if (!result) return E_POINTER;
    std::vector<BYTE> packet; auto hr = encodeSearchWindowContext(context, &packet); if (FAILED(hr)) return hr;
    Handle writable{CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(packet.size()), nullptr)};
    if (!writable.value) return HRESULT_FROM_WIN32(GetLastError());
    {
        View view{MapViewOfFile(writable.value, FILE_MAP_WRITE, 0, 0, packet.size())};
        if (!view.value) return HRESULT_FROM_WIN32(GetLastError());
        std::memcpy(view.value, packet.data(), packet.size());
    }
    SearchWindowMapping candidate;
    if (!DuplicateHandle(GetCurrentProcess(), writable.value, GetCurrentProcess(), &candidate.handle_, FILE_MAP_READ, TRUE, 0)) return HRESULT_FROM_WIN32(GetLastError());
    *result = std::move(candidate); return S_OK;
}
HRESULT SearchWindowMapping::read(SearchWindowContext* result) const {
    return handle_ ? readMapping(handle_, result) : E_HANDLE;
}
HRESULT consumeSearchWindowContext(ULONG_PTR handleValue, SearchWindowContext* result) {
    if (!result) return E_POINTER;
    if (!handleValue || handleValue == static_cast<ULONG_PTR>(-1) || handleValue == static_cast<ULONG_PTR>(-2)) return E_INVALIDARG;
    const auto handle = reinterpret_cast<HANDLE>(handleValue); DWORD flags = 0;
    if (!GetHandleInformation(handle, &flags)) return HRESULT_FROM_WIN32(GetLastError());
    if (!(flags & HANDLE_FLAG_INHERIT)) return E_INVALIDARG;
    // A reduced mapping must refuse write access. Do not consume a writable
    // mapping or an unrelated command-line handle, even if a packet looks valid.
    View writable{MapViewOfFile(handle, FILE_MAP_WRITE, 0, 0, 0)};
    if (writable.value) return E_ACCESSDENIED;
    SearchWindowContext candidate; const auto hr = readMapping(handle, &candidate);
    if (FAILED(hr)) return hr;
    if (!CloseHandle(handle)) return HRESULT_FROM_WIN32(GetLastError());
    *result = std::move(candidate); return S_OK;
}
HRESULT launchSearchWindow(const std::wstring& executable, const SearchWindowContext& context, HANDLE* processHandle) {
    if (processHandle) *processHandle = nullptr;
    if (executable.empty() || executable.find(L'\0') != std::wstring::npos) return E_INVALIDARG;
    try {
        SearchWindowMapping mapping; auto hr = SearchWindowMapping::create(context, &mapping); if (FAILED(hr)) return hr;
        SIZE_T bytes = 0; InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        if (!bytes) return HRESULT_FROM_WIN32(GetLastError());
        std::vector<BYTE> storage(bytes);
        auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes)) return HRESULT_FROM_WIN32(GetLastError());
        struct Attributes { LPPROC_THREAD_ATTRIBUTE_LIST value; ~Attributes() { DeleteProcThreadAttributeList(value); } } cleanup{attributes};
        HANDLE inherited = mapping.handle();
        if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &inherited, sizeof(inherited), nullptr, nullptr)) return HRESULT_FROM_WIN32(GetLastError());
        auto command = quote(executable) + L" --search-context-handle " + std::to_wstring(reinterpret_cast<ULONG_PTR>(inherited));
        if (command.size() >= 32767) return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
        STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.lpAttributeList = attributes;
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT,
                            nullptr, nullptr, &startup.StartupInfo, &process)) return HRESULT_FROM_WIN32(GetLastError());
        Handle childProcess{process.hProcess}, childThread{process.hThread};
        if (processHandle) *processHandle = std::exchange(childProcess.value, nullptr);
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}
} // namespace explorer
