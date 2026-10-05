#pragma once

#include "explorer/core.hpp"
#include <aclapi.h>
#include <sddl.h>
#include <winioctl.h>
#include <array>
#include <cstring>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace explorer::test {
namespace state_security {
inline void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
inline void succeeded(HRESULT status, const char* message) {
    if (FAILED(status)) {
        std::cerr << message << " HRESULT=0x" << std::hex << static_cast<unsigned long>(status) << std::dec << '\n';
        throw std::runtime_error(message);
    }
}
struct File {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~File() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct Dacl {
    std::vector<BYTE> descriptor;
    std::wstring sddl;
    SECURITY_DESCRIPTOR_CONTROL control = 0;
};
inline Dacl dacl(const std::filesystem::path& path) {
    DWORD length = 0;
    GetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION, nullptr, 0, &length);
    require(length != 0 && length <= 131072, "read bounded owned state-file DACL size");
    Dacl result; result.descriptor.resize(length);
    require(GetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION, result.descriptor.data(), length, &length) != FALSE,
            "read exact owned state-file DACL");
    DWORD revision = 0;
    require(GetSecurityDescriptorControl(result.descriptor.data(), &result.control, &revision) != FALSE,
            "read owned state-file DACL control");
    PWSTR text = nullptr;
    require(ConvertSecurityDescriptorToStringSecurityDescriptorW(result.descriptor.data(), SDDL_REVISION_1,
        DACL_SECURITY_INFORMATION, &text, nullptr) != FALSE && text, "encode owned DACL internally");
    result.sddl = text; LocalFree(text); return result;
}
inline void requireDacl(const Dacl& before, const std::filesystem::path& path) {
    const auto after = dacl(path);
    constexpr auto mask = SE_DACL_PRESENT | SE_DACL_DEFAULTED | SE_DACL_PROTECTED |
        SE_DACL_AUTO_INHERIT_REQ | SE_DACL_AUTO_INHERITED;
    if (before.sddl != after.sddl || (before.control & mask) != (after.control & mask)) {
        // Never report trustees, SDDL, identities or personal paths.
        std::cerr << "owned state DACL control before=0x" << std::hex << before.control << " after=0x"
                  << after.control << std::dec << '\n';
        throw std::runtime_error("state replacement changed exact DACL ACE order/rights/flags or inheritance control");
    }
}
inline std::wstring trustee() {
    File token;
    require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value) != FALSE, "read owned fixture trustee token");
    DWORD length = 0; GetTokenInformation(token.value, TokenUser, nullptr, 0, &length);
    require(length != 0 && length <= 131072, "read bounded owned trustee size");
    std::vector<BYTE> bytes(length);
    require(GetTokenInformation(token.value, TokenUser, bytes.data(), length, &length) != FALSE, "read owned trustee");
    PWSTR text = nullptr;
    require(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid, &text) != FALSE && text,
            "encode fixture trustee internally");
    const std::wstring result(text); LocalFree(text); return result;
}
inline void legacyDacl(const std::filesystem::path& path, const std::wstring& sddl) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    require(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr) != FALSE,
            "create exclusively owned legacy descriptor");
    // Only fixture generation uses this documented low-level legacy setter.
    const auto set = SetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION, descriptor);
    LocalFree(descriptor); require(set != FALSE, "set only exclusively owned legacy DACL");
}
inline void modernDacl(const std::filesystem::path& path, bool protect) {
    auto state = dacl(path); PACL acl = nullptr; BOOL present = FALSE, defaulted = FALSE;
    require(GetSecurityDescriptorDacl(state.descriptor.data(), &present, &acl, &defaulted) != FALSE,
            "decode owned modern inheritance fixture");
    File file{CreateFileW(path.c_str(), READ_CONTROL | WRITE_DAC, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    require(file.value != INVALID_HANDLE_VALUE, "open only owned inheritance fixture");
    require(SetSecurityInfo(file.value, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION |
        (protect ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION),
        nullptr, nullptr, acl, nullptr) == ERROR_SUCCESS, "modernize only owned DACL");
}
struct Metadata {
    FILE_BASIC_INFO basic{};
    ULONGLONG volume = 0;
    std::array<BYTE, 16> identity{};
};
inline Metadata metadata(const std::filesystem::path& path) {
    File file{CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    Metadata result; FILE_ID_INFO identity{};
    require(file.value != INVALID_HANDLE_VALUE &&
        GetFileInformationByHandleEx(file.value, FileBasicInfo, &result.basic, sizeof(result.basic)) &&
        GetFileInformationByHandleEx(file.value, FileIdInfo, &identity, sizeof(identity)), "read exact owned state-file metadata");
    result.volume = identity.VolumeSerialNumber;
    std::memcpy(result.identity.data(), identity.FileId.Identifier, result.identity.size()); return result;
}
inline std::vector<BYTE> read(const std::filesystem::path& path) {
    File file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    LARGE_INTEGER length{};
    require(file.value != INVALID_HANDLE_VALUE && GetFileSizeEx(file.value, &length) && length.QuadPart >= 0 &&
        length.QuadPart <= 4000000, "read bounded owned state bytes");
    std::vector<BYTE> bytes(static_cast<size_t>(length.QuadPart)); DWORD count = 0;
    require(ReadFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) && count == bytes.size(),
            "read complete owned state bytes"); return bytes;
}
inline std::set<std::wstring> members(const std::filesystem::path& root) {
    std::set<std::wstring> result;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
        result.insert(entry.path().lexically_relative(root).native());
    return result;
}
} // namespace state_security

// Callers supply their REAL encoder and decoder. `replacement=false` selects
// the baseline state, `true` a distinct complete state. All security mutation
// is confined to newly created children of the caller's exclusively owned root.
template<class Write, class Verify>
void stateFileSecurityProfiles(const std::filesystem::path& ownedRoot, std::wstring_view codecName,
                              Write&& write, Verify&& verify) {
    namespace fs = std::filesystem;
    using namespace state_security;
    require(!codecName.empty() && codecName.find_first_of(L"/\\:") == std::wstring_view::npos,
            "owned security fixture name must be a leaf");
    const auto parent = ownedRoot / (std::wstring(codecName) + L"-security");
    require(fs::create_directory(parent), "create fresh exclusively owned security parent");
    const auto user = trustee();
    legacyDacl(parent, L"D:P(A;OICI;FA;;;" + user + L")(A;OICI;FR;;;WD)(A;OICI;0x1200a9;;;BU)");
    for (const auto* profile : {L"legacy-inherited", L"legacy-explicit", L"protected", L"modern-inherited",
                               L"modern-protected", L"deny-data-write"}) {
        const std::wstring name(profile); const auto path = parent / (name + L".dat");
        succeeded(write(path, false), "create actual codec baseline for owned DACL profile"); verify(path, false);
        if (name == L"legacy-explicit") legacyDacl(path, L"D:(A;;FA;;;" + user + L")(A;;FR;;;WD)");
        if (name == L"protected" || name == L"modern-protected")
            legacyDacl(path, L"D:P(A;;FA;;;" + user + L")(A;;FR;;;WD)");
        if (name == L"deny-data-write") legacyDacl(path, L"D:P(D;;0x2;;;WD)(A;;FA;;;" + user + L")(A;;FR;;;WD)");
        const bool modern = name.starts_with(L"modern");
        if (modern) modernDacl(path, name == L"modern-protected");
        const auto permissions = dacl(path);
        require(((permissions.control & SE_DACL_AUTO_INHERITED) != 0) == modern,
                "legacy/modern fixture did not establish its actual native inheritance state");
        const auto before = metadata(path); const auto beforeBytes = read(path); const auto expectedMembers = members(parent);
        succeeded(write(path, true), "replace actual codec on owned DACL profile"); verify(path, true);
        requireDacl(permissions, path);
        const auto after = metadata(path);
        require(read(path) != beforeBytes && after.basic.CreationTime.QuadPart == before.basic.CreationTime.QuadPart &&
            after.basic.FileAttributes == before.basic.FileAttributes && members(parent) == expectedMembers,
            "replacement lost actual codec bytes/metadata or left staging entries");
        require(after.volume == before.volume && after.identity != before.identity,
                "complete state replacement did not use a new same-volume file identity");
        const auto finalBytes = read(path);
        const auto unchanged = [&](const Metadata& expected) {
            const auto now = metadata(path);
            require(now.volume == expected.volume && now.identity == expected.identity && read(path) == finalBytes &&
                now.basic.LastWriteTime.QuadPart == expected.basic.LastWriteTime.QuadPart &&
                now.basic.ChangeTime.QuadPart == expected.basic.ChangeTime.QuadPart &&
                now.basic.FileAttributes == expected.basic.FileAttributes && members(parent) == expectedMembers,
                "failed state write changed existing bytes/identity/timestamps or leaked a stage");
            requireDacl(permissions, path); verify(path, true);
        };
        {
            File lock{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
            require(lock.value != INVALID_HANDLE_VALUE, "lock only owned codec against replacement");
            require(FAILED(write(path, false)), "state codec ignored a reader denying replacement"); unchanged(after);
        }
        require(SetFileAttributesW(path.c_str(), after.basic.FileAttributes | FILE_ATTRIBUTE_READONLY) != FALSE,
                "set only owned read-only codec fixture");
        const auto readOnly = metadata(path);
        require(FAILED(write(path, false)), "state codec ignored owned read-only file"); unchanged(readOnly);
        require(SetFileAttributesW(path.c_str(), after.basic.FileAttributes) != FALSE, "restore owned fixture attributes");
        if (name == L"modern-protected") {
            {
                File file{CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, nullptr)};
                USHORT format = COMPRESSION_FORMAT_DEFAULT; DWORD returned = 0;
                require(file.value != INVALID_HANDLE_VALUE && DeviceIoControl(file.value, FSCTL_SET_COMPRESSION,
                    &format, sizeof(format), nullptr, 0, &returned, nullptr), "compress only owned state fixture");
            }
            const auto compressed = metadata(path);
            require((compressed.basic.FileAttributes & FILE_ATTRIBUTE_COMPRESSED) != 0,
                    "native compression was not established on owned state fixture");
            succeeded(write(path, false), "replace actual compressed codec preserving native compression"); verify(path, false);
            requireDacl(permissions, path);
            const auto now = metadata(path);
            require(now.basic.FileAttributes == compressed.basic.FileAttributes &&
                now.basic.CreationTime.QuadPart == compressed.basic.CreationTime.QuadPart &&
                members(parent) == expectedMembers, "state replacement lost actual compression/metadata or leaked a stage");
        }
    }
}
} // namespace explorer::test
