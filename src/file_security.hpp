#pragma once

#include <windows.h>
#include <aclapi.h>
#include <cstring>
#include <utility>
#include <vector>

namespace explorer::file_security {

// GetSecurityInfo is allowed to interpret legacy inheritance. The documented
// low-level READ API returns the descriptor of this exact held file without
// that interpretation. No SetKernelObjectSecurity call is used for files.
struct Descriptor {
    std::vector<BYTE> bytes;
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    bool present = false;
    bool defaulted = false;
    PACL dacl() const {
        PACL acl = nullptr; BOOL has = FALSE, wasDefaulted = FALSE;
        if (!bytes.empty()) GetSecurityDescriptorDacl(const_cast<BYTE*>(bytes.data()), &has, &acl, &wasDefaulted);
        return acl;
    }
};
inline HRESULT read(HANDLE file, Descriptor& output) {
    DWORD length = 0;
    if (GetKernelObjectSecurity(file, DACL_SECURITY_INFORMATION, nullptr, 0, &length)) return E_UNEXPECTED;
    auto error = GetLastError();
    if (error != ERROR_INSUFFICIENT_BUFFER) return HRESULT_FROM_WIN32(error);
    // The ACL length is a WORD; leave room for the self-relative descriptor.
    if (!length || length > 131072) return HRESULT_FROM_WIN32(ERROR_INVALID_SECURITY_DESCR);
    Descriptor next; next.bytes.resize(length);
    if (!GetKernelObjectSecurity(file, DACL_SECURITY_INFORMATION, next.bytes.data(), length, &length))
        return HRESULT_FROM_WIN32(GetLastError());
    if (!IsValidSecurityDescriptor(next.bytes.data())) return HRESULT_FROM_WIN32(ERROR_INVALID_SECURITY_DESCR);
    DWORD revision = 0; BOOL present = FALSE, defaulted = FALSE; PACL acl = nullptr;
    if (!GetSecurityDescriptorControl(next.bytes.data(), &next.control, &revision) ||
        !GetSecurityDescriptorDacl(next.bytes.data(), &present, &acl, &defaulted))
        return HRESULT_FROM_WIN32(GetLastError());
    if (acl && !IsValidAcl(acl)) return HRESULT_FROM_WIN32(ERROR_INVALID_ACL);
    next.present = present != FALSE; next.defaulted = defaulted != FALSE;
    output = std::move(next); return S_OK;
}
inline bool equal(const Descriptor& first, const Descriptor& second) {
    constexpr auto flags = SE_DACL_PRESENT | SE_DACL_DEFAULTED | SE_DACL_PROTECTED |
        SE_DACL_AUTO_INHERIT_REQ | SE_DACL_AUTO_INHERITED;
    if (first.bytes.empty() || second.bytes.empty() || (first.control & flags) != (second.control & flags) || first.present != second.present ||
        first.defaulted != second.defaulted) return false;
    const auto a = first.dacl(), b = second.dacl();
    if (!a || !b) return a == b;
    if (a->AclRevision != b->AclRevision || a->AceCount != b->AceCount) return false;
    for (DWORD index = 0; index < a->AceCount; ++index) {
        void *left = nullptr, *right = nullptr;
        if (!GetAce(a, index, &left) || !GetAce(b, index, &right)) return false;
        const auto size = static_cast<ACE_HEADER*>(left)->AceSize;
        if (size != static_cast<ACE_HEADER*>(right)->AceSize || std::memcmp(left, right, size)) return false;
    }
    return true;
}
inline SECURITY_ATTRIBUTES attributes(Descriptor& source) {
    return {sizeof(SECURITY_ATTRIBUTES), source.bytes.data(), FALSE};
}
inline HRESULT verifyCreated(HANDLE file, const Descriptor& source) {
    // CreateFile retains legacy ACE/control semantics, while it does not
    // retain the modern AUTO_INHERITED bit. Only an already modern source is
    // eligible for inheritance-aware conversion of the EMPTY temporary.
    if (source.control & SE_DACL_AUTO_INHERITED) {
        const auto protection = source.control & SE_DACL_PROTECTED ?
            PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION;
        const auto error = SetSecurityInfo(file, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | protection,
            nullptr, nullptr, source.dacl(), nullptr);
        if (error) return HRESULT_FROM_WIN32(error);
    }
    Descriptor created;
    const auto hr = read(file, created); if (FAILED(hr)) return hr;
    // If a parent/descriptor changes, refuse before any confidential query
    // bytes exist. Never silently use broader permissions or different ACEs.
    return equal(source, created) ? S_OK : HRESULT_FROM_WIN32(ERROR_INVALID_SECURITY_DESCR);
}

} // namespace explorer::file_security
