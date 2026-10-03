#include "explorer/extra_operations.hpp"

#include <shobjidl.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <system_error>

namespace explorer {
namespace {
namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

class Handle final {
public:
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    ~Handle() { if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return value_; }
    bool valid() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
private:
    HANDLE value_;
};

HRESULT lastError() { return HRESULT_FROM_WIN32(GetLastError()); }
HRESULT malformed() { return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT); }
HRESULT unsupported() { return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED); }
HRESULT failure(const std::error_code& error) {
    return HRESULT_FROM_WIN32(error.value() ? static_cast<DWORD>(error.value()) : ERROR_GEN_FAILURE);
}

HRESULT absolutePath(const fs::path& input, fs::path& output) {
    if (input.empty()) return E_INVALIDARG;
    std::error_code error;
    output = fs::absolute(input, error).lexically_normal();
    if (error) return failure(error);
    return S_OK;
}

HRESULT absent(const fs::path& path) {
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
        return HRESULT_FROM_WIN32(ERROR_FILE_EXISTS);
    const DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
        ? S_OK : HRESULT_FROM_WIN32(error);
}

HRESULT plainAncestors(fs::path path) {
    // Reject junctions and symlinks rather than letting an archive escape via
    // a pre-existing filesystem alias. All supplied ancestors must exist.
    for (;;) {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) return lastError();
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
        const fs::path parent = path.parent_path();
        if (parent.empty() || parent == path) return S_OK;
        path = parent;
    }
}

HRESULT plainTree(const fs::path& root) {
    HRESULT hr = plainAncestors(root);
    if (FAILED(hr)) return hr;
    const DWORD attributes = GetFileAttributesW(root.c_str());
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) return S_OK;
    std::error_code error;
    fs::recursive_directory_iterator it(root, fs::directory_options::none, error), end;
    if (error) return failure(error);
    while (it != end) {
        const DWORD childAttributes = GetFileAttributesW(it->path().c_str());
        if (childAttributes == INVALID_FILE_ATTRIBUTES) return lastError();
        if (childAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
        it.increment(error);
        if (error) return failure(error);
    }
    return S_OK;
}

struct CaseLess {
    bool operator()(const std::wstring& left, const std::wstring& right) const {
        return CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                                    right.data(), static_cast<int>(right.size()), TRUE) == CSTR_LESS_THAN;
    }
};

bool inside(const fs::path& parent, const fs::path& candidate) {
    auto first = parent.begin();
    auto second = candidate.begin();
    for (; first != parent.end(); ++first, ++second) {
        if (second == candidate.end()) return false;
        if (CompareStringOrdinal(first->c_str(), -1, second->c_str(), -1, TRUE) != CSTR_EQUAL) return false;
    }
    return true;
}

void removeOwnedTree(const fs::path& path) {
    // Never recurse into a reparse point, even during cleanup after a failed
    // archive. Clear read-only attributes only on owned non-link entries.
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return;
    const bool directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        if (directory) RemoveDirectoryW(path.c_str()); else DeleteFileW(path.c_str());
        return;
    }
    if (directory) {
        WIN32_FIND_DATAW data{};
        const HANDLE search = FindFirstFileW((path / L"*").c_str(), &data);
        if (search != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(data.cFileName, L".") && wcscmp(data.cFileName, L"..")) {
                    try { removeOwnedTree(path / data.cFileName); } catch (...) {}
                }
            } while (FindNextFileW(search, &data));
            FindClose(search);
        }
    }
    if (attributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(path.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
    if (directory) RemoveDirectoryW(path.c_str()); else DeleteFileW(path.c_str());
}

class StagingDirectory final {
public:
    ~StagingDirectory() {
        if (!path.empty()) {
            // The path was exclusively created by this instance, never supplied
            // as a cleanup target by the caller or by archive metadata.
            try { removeOwnedTree(path); } catch (...) {}
        }
    }
    HRESULT create(const fs::path& parent) {
        HRESULT hr = plainAncestors(parent);
        if (FAILED(hr)) return hr;
        if (!(GetFileAttributesW(parent.c_str()) & FILE_ATTRIBUTE_DIRECTORY)) return HRESULT_FROM_WIN32(ERROR_DIRECTORY);
        for (int attempt = 0; attempt != 4; ++attempt) {
            GUID guid{};
            hr = CoCreateGuid(&guid);
            if (FAILED(hr)) return hr;
            wchar_t name[64]{};
            if (!StringFromGUID2(guid, name, static_cast<int>(std::size(name)))) return E_FAIL;
            const fs::path candidate = parent / (std::wstring(L".windows-explorer-") + name);
            if (CreateDirectoryW(candidate.c_str(), nullptr)) { path = candidate; return S_OK; }
            if (GetLastError() != ERROR_ALREADY_EXISTS) return lastError();
        }
        return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
    }
    fs::path path;
};

std::wstring quote(const std::wstring& argument) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') { ++slashes; continue; }
        if (character == L'\"') {
            result.append(slashes * 2 + 1, L'\\');
            result += L'\"';
        } else {
            result.append(slashes, L'\\');
            result += character;
        }
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    result += L'\"';
    return result;
}

HRESULT runTar(const std::vector<std::wstring>& arguments, const fs::path& workingDirectory) {
    std::array<wchar_t, 32768> systemDirectory{};
    const UINT length = GetSystemDirectoryW(systemDirectory.data(), static_cast<UINT>(systemDirectory.size()));
    if (!length || length >= systemDirectory.size()) return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
    const fs::path executable = fs::path(systemDirectory.data()) / L"tar.exe";
    if (GetFileAttributesW(executable.c_str()) == INVALID_FILE_ATTRIBUTES) return lastError();
    std::wstring command = quote(executable.native());
    for (const auto& argument : arguments) command += L" " + quote(argument);
    if (command.size() >= 32767) return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, workingDirectory.c_str(), &startup, &process)) return lastError();
    Handle thread(process.hThread), child(process.hProcess);
    const DWORD wait = WaitForSingleObject(child.get(), 10 * 60 * 1000);
    if (wait != WAIT_OBJECT_0) {
        const HRESULT hr = wait == WAIT_TIMEOUT ? HRESULT_FROM_WIN32(ERROR_TIMEOUT) : lastError();
        TerminateProcess(child.get(), ERROR_CANCELLED);
        WaitForSingleObject(child.get(), INFINITE);
        return hr;
    }
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(child.get(), &exitCode)) return lastError();
    return exitCode == 0 ? S_OK : HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);
}

uint16_t u16(const unsigned char* value) {
    return static_cast<uint16_t>(value[0] | static_cast<unsigned>(value[1]) << 8);
}
uint32_t u32(const unsigned char* value) {
    return static_cast<uint32_t>(u16(value) | static_cast<uint32_t>(u16(value + 2)) << 16);
}

class ZipReader final {
public:
    explicit ZipReader(const fs::path& path)
        : file_(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr)) {}
    HRESULT initialize() {
        if (!file_.valid()) return lastError();
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file_.get(), &size)) return lastError();
        size_ = static_cast<uint64_t>(size.QuadPart);
        return S_OK;
    }
    bool read(uint64_t offset, void* buffer, size_t length) {
        if (offset > size_ || length > size_ - offset || length > MAXDWORD) return false;
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(file_.get(), position, nullptr, FILE_BEGIN)) return false;
        DWORD count = 0;
        return ReadFile(file_.get(), buffer, static_cast<DWORD>(length), &count, nullptr) && count == length;
    }
    uint64_t size() const { return size_; }
private:
    Handle file_;
    uint64_t size_ = 0;
};

bool safeExtras(const std::vector<unsigned char>& extras) {
    size_t offset = 0;
    while (offset < extras.size()) {
        if (extras.size() - offset < 4) return false;
        const uint16_t id = u16(extras.data() + offset);
        const uint16_t length = u16(extras.data() + offset + 2);
        offset += 4;
        if (length > extras.size() - offset) return false;
        // Only time and ownership metadata are understood here. Reject ZIP64,
        // alternate Unicode names, link records, encryption and unknown extras.
        if (id != 0x5455 && id != 0x7875 && id != 0x000a && id != 0x5855) return false;
        offset += length;
    }
    return true;
}

bool safeName(const std::vector<unsigned char>& bytes, bool utf8, std::wstring& name, bool& directory) {
    if (bytes.empty() || bytes.size() > 32767) return false;
    const char* raw = reinterpret_cast<const char*>(bytes.data());
    const int count = MultiByteToWideChar(utf8 ? CP_UTF8 : 437, utf8 ? MB_ERR_INVALID_CHARS : 0,
                                        raw, static_cast<int>(bytes.size()), nullptr, 0);
    if (!count) return false;
    name.resize(static_cast<size_t>(count));
    if (!MultiByteToWideChar(utf8 ? CP_UTF8 : 437, utf8 ? MB_ERR_INVALID_CHARS : 0,
                            raw, static_cast<int>(bytes.size()), name.data(), count)) return false;
    std::replace(name.begin(), name.end(), L'\\', L'/');
    while (name.starts_with(L"./")) name.erase(0, 2);
    if (name.empty() || name.front() == L'/') return false;
    directory = name.back() == L'/';
    if (directory) name.pop_back();
    if (name.empty()) return false;
    size_t offset = 0;
    while (offset < name.size()) {
        const size_t slash = name.find(L'/', offset);
        const std::wstring part = name.substr(offset, slash == std::wstring::npos ? slash : slash - offset);
        if (part.empty() || part == L"." || part == L".." || part.back() == L'.' || part.back() == L' ') return false;
        for (const wchar_t character : part)
            if (character < 32 || character == L':' || character == L'<' || character == L'>' ||
                character == L'"' || character == L'|' || character == L'?' || character == L'*') return false;
        std::wstring stem = part.substr(0, part.find(L'.'));
        while (!stem.empty() && (stem.back() == L' ' || stem.back() == L'.')) stem.pop_back();
        auto equals = [&](const wchar_t* reserved) {
            return CompareStringOrdinal(stem.c_str(), -1, reserved, -1, TRUE) == CSTR_EQUAL;
        };
        if (equals(L"CON") || equals(L"PRN") || equals(L"AUX") || equals(L"NUL") || equals(L"CONIN$") || equals(L"CONOUT$")) return false;
        if (stem.size() == 4 && (CompareStringOrdinal(stem.data(), 3, L"COM", 3, TRUE) == CSTR_EQUAL ||
                                CompareStringOrdinal(stem.data(), 3, L"LPT", 3, TRUE) == CSTR_EQUAL) &&
            ((stem[3] >= L'1' && stem[3] <= L'9') || stem[3] == L'\u00b9' || stem[3] == L'\u00b2' || stem[3] == L'\u00b3')) return false;
        if (slash == std::wstring::npos) break;
        offset = slash + 1;
        if (offset == name.size()) return false;
    }
    return true;
}

struct ZipEntry {
    uint32_t offset = 0, compressed = 0, expanded = 0, crc = 0;
    uint16_t flags = 0, method = 0;
    bool directory = false;
    std::wstring name;
    std::vector<unsigned char> rawName;
};

HRESULT validateZip(ZipReader& reader, std::vector<ZipEntry>& entries) {
    if (reader.size() < 22) return malformed();
    if (reader.size() >= MAXDWORD) return unsupported();
    const size_t tailLength = static_cast<size_t>((std::min)(reader.size(), uint64_t{65557}));
    std::vector<unsigned char> tail(tailLength);
    if (!reader.read(reader.size() - tailLength, tail.data(), tail.size())) return malformed();
    size_t eocd = tailLength;
    for (size_t offset = tailLength - 22;; --offset) {
        if (u32(tail.data() + offset) == 0x06054b50 && offset + 22 + u16(tail.data() + offset + 20) == tailLength) {
            eocd = offset; break;
        }
        if (!offset) break;
    }
    if (eocd == tailLength) return malformed();
    const unsigned char* end = tail.data() + eocd;
    const uint16_t count = u16(end + 10);
    const uint32_t directorySize = u32(end + 12), directoryOffset = u32(end + 16);
    if (u16(end + 4) || u16(end + 6) || u16(end + 8) != count || count == 65535 ||
        directorySize == MAXDWORD || directoryOffset == MAXDWORD) return unsupported();
    if (static_cast<uint64_t>(directoryOffset) + directorySize != reader.size() - tailLength + eocd) return malformed();
    uint64_t cursor = directoryOffset, expandedTotal = 0;
    std::map<std::wstring, bool, CaseLess> names;
    for (uint32_t index = 0; index < count; ++index) {
        std::array<unsigned char, 46> header{};
        if (!reader.read(cursor, header.data(), header.size()) || u32(header.data()) != 0x02014b50) return malformed();
        ZipEntry entry;
        entry.flags = u16(header.data() + 8);
        entry.method = u16(header.data() + 10);
        entry.crc = u32(header.data() + 16);
        entry.compressed = u32(header.data() + 20);
        entry.expanded = u32(header.data() + 24);
        const uint16_t nameLength = u16(header.data() + 28), extraLength = u16(header.data() + 30), commentLength = u16(header.data() + 32);
        const uint32_t attributes = u32(header.data() + 38);
        const uint32_t mode = (attributes >> 16) & 0170000;
        entry.offset = u32(header.data() + 42);
        if (u16(header.data() + 34) || entry.compressed == MAXDWORD || entry.expanded == MAXDWORD || entry.offset == MAXDWORD)
            return unsupported();
        if ((entry.flags & ~uint16_t{0x080e}) || (entry.method != 0 && entry.method != 8)) return unsupported();
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) || (mode && mode != 0100000 && mode != 0040000)) return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
        entry.rawName.resize(nameLength);
        std::vector<unsigned char> extras(extraLength);
        if (!reader.read(cursor + 46, entry.rawName.data(), nameLength) ||
            !reader.read(cursor + 46 + nameLength, extras.data(), extraLength)) return malformed();
        if (!safeExtras(extras)) return unsupported();
        if (!safeName(entry.rawName, (entry.flags & 0x0800) != 0, entry.name, entry.directory)) return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
        const bool attributeDirectory = mode == 0040000 || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (attributeDirectory != entry.directory && (attributeDirectory || (entry.directory && mode == 0100000))) return malformed();
        if (entry.directory && entry.expanded != 0) return malformed();
        if (entry.method == 0 && entry.compressed != entry.expanded) return malformed();
        if (!names.emplace(entry.name, entry.directory).second) return HRESULT_FROM_WIN32(ERROR_DUP_NAME);
        expandedTotal += entry.expanded;
        // Classic ZIP's per-entry limits are retained, with a bounded aggregate
        // expansion to avoid accidental or malicious disk exhaustion.
        if (expandedTotal > uint64_t{64} * 1024 * 1024 * 1024) return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
        entries.push_back(std::move(entry));
        cursor += 46ull + nameLength + extraLength + commentLength;
        if (cursor > static_cast<uint64_t>(directoryOffset) + directorySize) return malformed();
    }
    if (cursor != static_cast<uint64_t>(directoryOffset) + directorySize) return malformed();
    for (const auto& entry : entries) {
        std::wstring parent = entry.name;
        for (;;) {
            const size_t slash = parent.rfind(L'/');
            if (slash == std::wstring::npos) break;
            parent.resize(slash);
            const auto found = names.find(parent);
            if (found != names.end() && !found->second) return malformed();
        }
    }
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.offset < b.offset; });
    cursor = 0;
    for (const auto& entry : entries) {
        if (entry.offset != cursor) return malformed(); // No SFX prefix, hidden records or overlapping entries.
        std::array<unsigned char, 30> header{};
        if (!reader.read(cursor, header.data(), header.size()) || u32(header.data()) != 0x04034b50 ||
            u16(header.data() + 6) != entry.flags || u16(header.data() + 8) != entry.method) return malformed();
        const uint16_t nameLength = u16(header.data() + 26), extraLength = u16(header.data() + 28);
        std::vector<unsigned char> name(nameLength), extras(extraLength);
        if (!reader.read(cursor + 30, name.data(), nameLength) || name != entry.rawName ||
            !reader.read(cursor + 30 + nameLength, extras.data(), extraLength)) return malformed();
        if (!safeExtras(extras)) return unsupported();
        if (!(entry.flags & 8)) {
            if (u32(header.data() + 14) != entry.crc || u32(header.data() + 18) != entry.compressed ||
                u32(header.data() + 22) != entry.expanded) return malformed();
        } else {
            if ((u32(header.data() + 14) && u32(header.data() + 14) != entry.crc) ||
                (u32(header.data() + 18) && u32(header.data() + 18) != entry.compressed) ||
                (u32(header.data() + 22) && u32(header.data() + 22) != entry.expanded)) return malformed();
        }
        cursor += 30ull + nameLength + extraLength + entry.compressed;
        if (entry.flags & 8) {
            std::array<unsigned char, 16> descriptor{};
            if (!reader.read(cursor, descriptor.data(), 12)) return malformed();
            const bool signature = u32(descriptor.data()) == 0x08074b50;
            if (signature && !reader.read(cursor, descriptor.data(), 16)) return malformed();
            const unsigned char* values = descriptor.data() + (signature ? 4 : 0);
            if (u32(values) != entry.crc || u32(values + 4) != entry.compressed || u32(values + 8) != entry.expanded) return malformed();
            cursor += signature ? 16 : 12;
        }
        if (cursor > directoryOffset) return malformed();
    }
    return cursor == directoryOffset ? S_OK : malformed();
}

HRESULT verifyExtracted(const fs::path& root, const std::vector<ZipEntry>& entries) {
    HRESULT hr = plainTree(root);
    if (FAILED(hr)) return hr;
    std::map<std::wstring, const ZipEntry*, CaseLess> expected;
    for (const auto& entry : entries) expected.emplace(entry.name, &entry);
    std::error_code error;
    fs::recursive_directory_iterator it(root, error), end;
    if (error) return failure(error);
    while (it != end) {
        const fs::path relative = it->path().lexically_relative(root);
        const auto found = expected.find(relative.generic_wstring());
        const DWORD attributes = GetFileAttributesW(it->path().c_str());
        const bool directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (found == expected.end()) {
            if (!directory) return malformed(); // Implicit parent directories are allowed.
        } else {
            if (found->second->directory != directory) return malformed();
            if (!directory && fs::file_size(it->path(), error) != found->second->expanded) return malformed();
            if (error) return failure(error);
            expected.erase(found);
        }
        it.increment(error);
        if (error) return failure(error);
    }
    return expected.empty() ? S_OK : malformed();
}

HRESULT copySources(const std::vector<fs::path>& sources, const fs::path& destination) {
    std::error_code error;
    if (!fs::create_directory(destination, error)) return error ? failure(error) : E_FAIL;
    for (const auto& source : sources) {
        fs::copy(source, destination / source.filename(), fs::copy_options::recursive, error);
        if (error) return failure(error);
    }
    // CopyFile preserves read-only attributes. Clear them on these private
    // copies so archive timestamps and cleanup never mutate original inputs.
    fs::recursive_directory_iterator it(destination, error), end;
    if (error) return failure(error);
    while (it != end) {
        DWORD attributes = GetFileAttributesW(it->path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) return lastError();
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
        if ((attributes & FILE_ATTRIBUTE_READONLY) &&
            !SetFileAttributesW(it->path().c_str(), attributes & ~FILE_ATTRIBUTE_READONLY)) return lastError();
        it.increment(error);
        if (error) return failure(error);
    }
    return S_OK;
}
}

HRESULT ExtraOperations::createZip(const std::vector<fs::path>& sources, const fs::path& output) {
    try {
        if (sources.empty()) return E_INVALIDARG;
        fs::path absoluteOutput;
        HRESULT hr = absolutePath(output, absoluteOutput);
        if (FAILED(hr) || FAILED(hr = absent(absoluteOutput))) return hr;
        StagingDirectory staging;
        std::vector<std::wstring> arguments{L"--format", L"zip", L"--options", L"zip:hdrcharset=UTF-8", L"-cf"};
        std::vector<fs::path> absoluteSources;
        std::map<std::wstring, bool, CaseLess> rootNames;
        for (const auto& source : sources) {
            fs::path path;
            hr = absolutePath(source, path);
            if (FAILED(hr) || FAILED(hr = plainTree(path))) return hr;
            if (path.filename().empty()) return E_INVALIDARG;
            if (!rootNames.emplace(path.filename().native(), true).second) return HRESULT_FROM_WIN32(ERROR_DUP_NAME);
            if ((GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_DIRECTORY) && inside(path, absoluteOutput))
                return HRESULT_FROM_WIN32(ERROR_INVALID_PARAMETER);
            absoluteSources.push_back(std::move(path));
        }
        hr = staging.create(absoluteOutput.parent_path());
        if (FAILED(hr)) return hr;
        const fs::path temporary = staging.path / L"archive.zip";
        const fs::path contents = staging.path / L"contents";
        // Windows' bundled bsdtar opens archive filenames and changes folders
        // through narrow CRT APIs. Native wide-path staging plus relative ASCII
        // archive names avoids corrupting Unicode parent/output paths.
        hr = copySources(absoluteSources, contents);
        if (FAILED(hr)) return hr;
        arguments.push_back(L"../archive.zip");
        for (const auto& path : absoluteSources) {
            arguments.push_back(L"./" + path.filename().native());
        }
        hr = runTar(arguments, contents);
        if (FAILED(hr)) return hr;
        {
            ZipReader reader(temporary);
            std::vector<ZipEntry> entries;
            hr = reader.initialize();
            if (FAILED(hr) || FAILED(hr = validateZip(reader, entries))) return hr;
        }
        if (!MoveFileExW(temporary.c_str(), absoluteOutput.c_str(), MOVEFILE_WRITE_THROUGH)) return lastError();
        return S_OK;
    } catch (const fs::filesystem_error& error) { return failure(error.code()); }
      catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
      catch (...) { return E_FAIL; }
}

HRESULT ExtraOperations::extractZip(const fs::path& archive, const fs::path& destination) {
    try {
        fs::path absoluteArchive, absoluteDestination;
        HRESULT hr = absolutePath(archive, absoluteArchive);
        if (FAILED(hr) || FAILED(hr = absolutePath(destination, absoluteDestination)) ||
            FAILED(hr = absent(absoluteDestination)) || FAILED(hr = plainAncestors(absoluteArchive))) return hr;
        ZipReader reader(absoluteArchive); // Keep a read-only sharing lock through extraction.
        std::vector<ZipEntry> entries;
        hr = reader.initialize();
        if (FAILED(hr) || FAILED(hr = validateZip(reader, entries))) return hr;
        StagingDirectory staging;
        hr = staging.create(absoluteDestination.parent_path());
        if (FAILED(hr)) return hr;
        ULARGE_INTEGER available{};
        if (!GetDiskFreeSpaceExW(staging.path.c_str(), &available, nullptr, nullptr)) return lastError();
        uint64_t expanded = 0;
        for (const auto& entry : entries) expanded += entry.expanded;
        if (expanded > available.QuadPart) return HRESULT_FROM_WIN32(ERROR_DISK_FULL);
        const fs::path copiedArchive = staging.path / L"input.zip", contents = staging.path / L"contents";
        if (!CopyFileW(absoluteArchive.c_str(), copiedArchive.c_str(), TRUE)) return lastError();
        if (!CreateDirectoryW(contents.c_str(), nullptr)) return lastError();
        hr = runTar({L"--options", L"zip:hdrcharset=CP437", L"-xf", L"input.zip", L"-C", L"contents"}, staging.path);
        if (FAILED(hr) || FAILED(hr = verifyExtracted(contents, entries))) return hr;
        if (!MoveFileExW(contents.c_str(), absoluteDestination.c_str(), MOVEFILE_WRITE_THROUGH)) return lastError();
        return S_OK;
    } catch (const fs::filesystem_error& error) { return failure(error.code()); }
      catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
      catch (...) { return E_FAIL; }
}

HRESULT ExtraOperations::createShortcut(const fs::path& target, const fs::path& output) {
    try {
        fs::path absoluteTarget, absoluteOutput;
        HRESULT hr = absolutePath(target, absoluteTarget);
        if (FAILED(hr) || FAILED(hr = absolutePath(output, absoluteOutput)) || FAILED(hr = absent(absoluteOutput))) return hr;
        const DWORD attributes = GetFileAttributesW(absoluteTarget.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) return lastError();
        if (CompareStringOrdinal(absoluteOutput.extension().c_str(), -1, L".lnk", -1, TRUE) != CSTR_EQUAL) return E_INVALIDARG;
        StagingDirectory staging;
        hr = staging.create(absoluteOutput.parent_path());
        if (FAILED(hr)) return hr;
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) return initialized;
        struct ComScope { bool owns; ~ComScope() { if (owns) CoUninitialize(); } } com{SUCCEEDED(initialized)};
        ComPtr<IShellLinkW> link;
        hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
        if (FAILED(hr) || FAILED(hr = link->SetPath(absoluteTarget.c_str()))) return hr;
        const fs::path workingDirectory = (attributes & FILE_ATTRIBUTE_DIRECTORY) ? absoluteTarget : absoluteTarget.parent_path();
        if (FAILED(hr = link->SetWorkingDirectory(workingDirectory.c_str()))) return hr;
        ComPtr<IPersistFile> persist;
        if (FAILED(hr = link.As(&persist))) return hr;
        const fs::path temporary = staging.path / L"shortcut.lnk";
        if (FAILED(hr = persist->Save(temporary.c_str(), TRUE))) return hr;
        if (!MoveFileExW(temporary.c_str(), absoluteOutput.c_str(), MOVEFILE_WRITE_THROUGH)) return lastError();
        return S_OK;
    } catch (const fs::filesystem_error& error) { return failure(error.code()); }
      catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
      catch (...) { return E_FAIL; }
}
}
