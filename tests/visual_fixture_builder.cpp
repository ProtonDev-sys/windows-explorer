#include "explorer/library.hpp"
#include "explorer/search.hpp"
#include "explorer/saved_search.hpp"
#include "explorer/headless_visual.hpp"

#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <propkey.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <vfw.h>
#include <imapi2fs.h>
#include <shlwapi.h>
#include <array>
#include <algorithm>
#include <compare>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

void phase(const char* operation) noexcept {
    std::fprintf(stderr, "visual-fixture phase=%s tick=%llu\n", operation,
        static_cast<unsigned long long>(GetTickCount64()));
    std::fflush(stderr);
}

void check(HRESULT status, const char* operation) {
    if (FAILED(status)) throw std::runtime_error(std::string(operation) + " HRESULT=" + std::to_string(static_cast<unsigned long>(status)));
}
void require(bool condition, const char* operation) { if (!condition) throw std::runtime_error(operation); }
std::wstring inputDesktopName();

class PrivateDesktop final {
public:
    PrivateDesktop() : previous_(GetThreadDesktop(GetCurrentThreadId())) {
        GUID guid{}; check(CoCreateGuid(&guid),"Create desktop identifier");
        wchar_t name[40]{}; require(StringFromGUID2(guid,name,40) != 0,"Format desktop identifier");
        constexpr ACCESS_MASK rights = DESKTOP_CREATEWINDOW | DESKTOP_CREATEMENU | DESKTOP_ENUMERATE |
            DESKTOP_HOOKCONTROL | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS;
        const auto desktopName = std::wstring(L"ExplorerVisualFixture-") + name;
        desktop_ = CreateDesktopW(desktopName.c_str(),nullptr,nullptr,0,rights,nullptr);
        require(desktop_ != nullptr,"Create private fixture desktop");
        if (desktopName == inputDesktopName()) { CloseDesktop(desktop_); desktop_ = nullptr; throw std::runtime_error("Fixture desktop matches the input desktop"); }
        if (!SetThreadDesktop(desktop_)) { CloseDesktop(desktop_); desktop_ = nullptr; throw std::runtime_error("Attach private fixture desktop"); }
    }
    ~PrivateDesktop() { if (desktop_ && SetThreadDesktop(previous_)) CloseDesktop(desktop_); }
private:
    HDESK previous_ = nullptr;
    HDESK desktop_ = nullptr;
};
class ComApartment final {
public:
    ComApartment() { check(OleInitialize(nullptr),"Initialize private fixture COM apartment"); }
    ~ComApartment() { OleUninitialize(); }
};

std::wstring inputDesktopName() {
    HDESK desktop = OpenInputDesktop(0,FALSE,DESKTOP_READOBJECTS);
    require(desktop != nullptr,"Read input desktop identity");
    DWORD bytes = 0;
    GetUserObjectInformationW(desktop,UOI_NAME,nullptr,0,&bytes);
    std::vector<wchar_t> name(bytes/sizeof(wchar_t)+1,0);
    const bool ok = bytes && GetUserObjectInformationW(desktop,UOI_NAME,name.data(),bytes,&bytes);
    CloseDesktop(desktop);
    require(ok,"Read input desktop name"); return name.data();
}
bool visibleProcessInputWindows() {
    HDESK desktop = OpenInputDesktop(0,FALSE,DESKTOP_READOBJECTS | DESKTOP_ENUMERATE);
    require(desktop != nullptr,"Inspect process-owned input desktop windows");
    bool visible = false;
    const bool ok = EnumDesktopWindows(desktop,[](HWND window,LPARAM parameter)->BOOL {
        DWORD process = 0; GetWindowThreadProcessId(window,&process);
        if (process == GetCurrentProcessId() && IsWindowVisible(window)) *reinterpret_cast<bool*>(parameter) = true;
        return TRUE;
    },reinterpret_cast<LPARAM>(&visible));
    CloseDesktop(desktop); require(ok,"Inspect process-owned input windows"); return visible;
}

void regularPath(const fs::path& path, bool directory) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    require(attributes != INVALID_FILE_ATTRIBUTES,"Required fixture path is absent");
    require((attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0,"Fixture paths must not redirect through a reparse point");
    require(((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == directory,"Fixture path has the wrong type");
}
void validateRoot(const fs::path& root) {
    require(root.is_absolute(),"Fixture root must be absolute");
    regularPath(root,true);
    for (auto parent = root.parent_path(); !parent.empty(); parent = parent.parent_path()) {
        regularPath(parent,true);
        if (parent == parent.parent_path()) break;
    }
    const auto marker = root / L".native-visual-fixture";
    regularPath(marker,false);
    std::ifstream input(marker,std::ios::binary);
    std::string value((std::istreambuf_iterator<char>(input)),{});
    require(value.size() <= 48,"Owned fixture marker is too large");
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) value.pop_back();
    require(value.size() == 36 || value.size() == 38,"Owned fixture marker must contain a GUID");
    if (value.size() == 36) value = "{" + value + "}";
    const std::wstring wide(value.begin(),value.end());
    GUID guid{}; check(CLSIDFromString(wide.c_str(),&guid),"Validate owned fixture marker");
    for (const auto* name : {L"Documents",L"Pictures",L"Music",L"Videos"}) regularPath(root/name,true);
    regularPath(root/L"Read me.txt",false);
}
void absent(const fs::path& path) {
    SetLastError(ERROR_SUCCESS);
    const auto attributes = GetFileAttributesW(path.c_str());
    require(attributes == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND,"Fixture output already exists or is inaccessible");
}
void createBytes(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    HANDLE handle = CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    require(handle != INVALID_HANDLE_VALUE,"Exclusively create owned media");
    DWORD written = 0;
    const bool ok = bytes.size() <= MAXDWORD && WriteFile(handle,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr) && written == bytes.size();
    const bool flushed = ok && FlushFileBuffers(handle);
    CloseHandle(handle);
    require(flushed,"Write complete owned media payload");
}
void u16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value)); bytes.push_back(static_cast<std::uint8_t>(value >> 8));
}
void u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}
void four(std::vector<std::uint8_t>& bytes, const char* text) { for (unsigned index = 0; index < 4; ++index) bytes.push_back(static_cast<std::uint8_t>(text[index])); }
void chunk(std::vector<std::uint8_t>& destination, const char* tag, const std::vector<std::uint8_t>& payload) {
    four(destination,tag); u32(destination,static_cast<std::uint32_t>(payload.size()));
    destination.insert(destination.end(),payload.begin(),payload.end());
    if (payload.size() & 1) destination.push_back(0);
}
std::vector<std::uint8_t> wave() {
    // One second of silent 44.1-kHz, mono, signed 16-bit PCM.
    std::vector<std::uint8_t> format; u16(format,1); u16(format,1); u32(format,44100); u32(format,88200); u16(format,2); u16(format,16);
    std::vector<std::uint8_t> body; four(body,"WAVE"); chunk(body,"fmt ",format); chunk(body,"data",std::vector<std::uint8_t>(88200,0));
    std::vector<std::uint8_t> file; chunk(file,"RIFF",body); return file;
}
std::vector<std::uint8_t> avi() {
    // One uncompressed 2x2 24-bit bottom-up DIB frame, with a complete AVI 1.0
    // header, video stream and index. No installed encoder or playback is used.
    std::vector<std::uint8_t> main;
    for (const auto value : std::array<std::uint32_t,14>{1000000,16,0,0x10,1,0,1,16,2,2,0,0,0,0}) u32(main,value);
    std::vector<std::uint8_t> stream; four(stream,"vids"); four(stream,"DIB "); u32(stream,0); u16(stream,0); u16(stream,0);
    for (const auto value : std::array<std::uint32_t,8>{0,1,1,0,1,16,0xffffffffu,0}) u32(stream,value);
    u16(stream,0); u16(stream,0); u16(stream,2); u16(stream,2);
    std::vector<std::uint8_t> bitmap; u32(bitmap,40); u32(bitmap,2); u32(bitmap,2); u16(bitmap,1); u16(bitmap,24);
    for (const auto value : std::array<std::uint32_t,6>{0,16,0,0,0,0}) u32(bitmap,value);
    std::vector<std::uint8_t> streamList; four(streamList,"strl"); chunk(streamList,"strh",stream); chunk(streamList,"strf",bitmap);
    std::vector<std::uint8_t> headers; four(headers,"hdrl"); chunk(headers,"avih",main); chunk(headers,"LIST",streamList);
    const std::vector<std::uint8_t> pixels{0x20,0x60,0xc0, 0x20,0x60,0xc0, 0,0, 0xc0,0x60,0x20, 0xc0,0x60,0x20, 0,0};
    std::vector<std::uint8_t> movie; four(movie,"movi"); chunk(movie,"00db",pixels);
    std::vector<std::uint8_t> index; four(index,"00db"); u32(index,0x10); u32(index,4); u32(index,16);
    std::vector<std::uint8_t> body; four(body,"AVI "); chunk(body,"LIST",headers); chunk(body,"LIST",movie); chunk(body,"idx1",index);
    std::vector<std::uint8_t> file; chunk(file,"RIFF",body); return file;
}
void validateMedia(const fs::path& source, DWORD type, LONG expectedSamples, LONG expectedSampleBytes) {
    // Read the owned RIFF through the native AVI/WAVE parser. These calls only
    // parse/read streams; they do not create a player, contact devices or run
    // a codec configuration dialog. The parser stays on the private desktop.
    auto path = source; path.make_preferred();
    PAVIFILE file = nullptr; PAVISTREAM stream = nullptr;
    AVIFileInit();
    HRESULT status = AVIFileOpenW(&file,path.c_str(),OF_READ | OF_SHARE_DENY_WRITE,nullptr);
    if (SUCCEEDED(status)) status = AVIFileGetStream(file,&stream,type,0);
    AVISTREAMINFOW info{};
    if (SUCCEEDED(status)) status = AVIStreamInfoW(stream,&info,sizeof(info));
    std::array<std::uint8_t,16> sample{}; LONG bytes = 0, samples = 0;
    if (SUCCEEDED(status)) status = AVIStreamRead(stream,0,1,sample.data(),static_cast<LONG>(sample.size()),&bytes,&samples);
    if (stream) AVIStreamRelease(stream);
    if (file) AVIFileRelease(file);
    AVIFileExit();
    check(status,"Read valid owned native media stream");
    require(info.dwLength == static_cast<DWORD>(expectedSamples) && samples == 1 && bytes == expectedSampleBytes,
            "Native media parser rejected owned stream length or sample size");
    if (type == streamtypeVIDEO) require(info.rcFrame.right == 2 && info.rcFrame.bottom == 2,"Native AVI dimensions differ");
}
struct OwnedString final {
    BSTR value=nullptr;
    explicit OwnedString(const wchar_t* text):value(SysAllocString(text)) {require(value!=nullptr,"Allocate owned ISO metadata");}
    ~OwnedString(){SysFreeString(value);}
};
void createDiscImage(const fs::path& root) {
    // Public IMAPI2FS image construction only: no recorder, existing disc,
    // mount, media-import, burning, Shell verb or application invocation.
    // https://learn.microsoft.com/en-us/windows/win32/api/imapi2fs/nn-imapi2fs-ifilesystemimage
    ComPtr<IFileSystemImage> image;
    check(CoCreateInstance(__uuidof(MsftFileSystemImage),nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&image)),"Create native recorder-free ISO builder");
    check(image->ChooseImageDefaultsForMediaType(IMAPI_MEDIA_TYPE_CDR),"Choose explicit ISO capacity without a device");
    check(image->put_FileSystemsToCreate(static_cast<FsiFileSystems>(FsiFileSystemISO9660|FsiFileSystemJoliet)),"Choose native ISO9660 and Joliet");
    check(image->put_SessionStartBlock(0),"Create standalone single-session image");
    check(image->put_StageFiles(VARIANT_FALSE),"Keep ISO inputs in the owned stream");
    const OwnedString volume(L"OWNED_VISUAL");
    check(image->put_VolumeName(volume.value),"Set owned ISO volume label");
    ComPtr<IStream> input;
    check(SHCreateStreamOnFileEx((root/L"Read me.txt").c_str(),STGM_READ|STGM_SHARE_DENY_WRITE,FILE_ATTRIBUTE_NORMAL,FALSE,nullptr,&input),"Read only the owned ISO content");
    STATSTG source{};check(input->Stat(&source,STATFLAG_NONAME),"Read owned ISO input size");
    require(source.cbSize.QuadPart<=1024*1024,"Owned ISO input exceeds the bounded visual fixture");
    ComPtr<IFsiDirectoryItem> directory;
    check(image->get_Root(&directory),"Read native ISO root");
    const OwnedString name(L"Read me.txt");
    check(directory->AddFile(name.value,input.Get()),"Add only the owned text stream to ISO");
    ComPtr<IFileSystemImageResult> result;
    check(image->CreateResultImage(&result),"Create recorder-free native ISO result");
    ComPtr<IStream> stream;check(result->get_ImageStream(&stream),"Read actual native ISO stream");
    STATSTG size{};check(stream->Stat(&size,STATFLAG_NONAME),"Read bounded native ISO size");
    require(size.cbSize.QuadPart>=18*2048 && size.cbSize.QuadPart<=8*1024*1024 && size.cbSize.QuadPart%2048==0,"Native ISO size is invalid or exceeds fixture budget");
    LARGE_INTEGER start{};check(stream->Seek(start,STREAM_SEEK_SET,nullptr),"Rewind native ISO stream");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size.cbSize.QuadPart));
    ULONG count=0;check(stream->Read(bytes.data(),static_cast<ULONG>(bytes.size()),&count),"Read complete native ISO stream");
    require(count==bytes.size(),"Native ISO stream ended before its reported length");
    constexpr std::size_t primary=16*2048;
    require(bytes[primary]==1 && std::equal(bytes.begin()+primary+1,bytes.begin()+primary+6,"CD001") && bytes[primary+6]==1,
            "Native ISO lacks the ISO9660 primary descriptor");
    require(bytes[primary+128]==0 && bytes[primary+129]==8 && bytes[primary+130]==8 && bytes[primary+131]==0,
            "Native ISO logical block size differs from 2048 bytes");
    bool joliet=false,terminator=false;
    for(std::size_t sector=17;sector<32 && sector*2048<bytes.size();++sector) {
        const auto offset=sector*2048;
        require(std::equal(bytes.begin()+offset+1,bytes.begin()+offset+6,"CD001") && bytes[offset+6]==1,"Native ISO has an invalid volume descriptor");
        if(bytes[offset]==2)joliet=bytes[offset+88]=='%'&&bytes[offset+89]=='/'&&bytes[offset+90]=='E';
        if(bytes[offset]==255){terminator=true;break;}
    }
    require(joliet&&terminator,"Native ISO omitted requested Joliet or its descriptor terminator");
    createBytes(root/L"Owned disc image.iso",bytes);
}
ComPtr<IShellItem> item(const fs::path& path) {
    auto nativePath = path; nativePath.make_preferred();
    ComPtr<IShellItem> value; check(SHCreateItemFromParsingName(nativePath.c_str(),nullptr,IID_PPV_ARGS(&value)),"Read owned native item"); return value;
}
fs::path nativeItemPath(IShellItem* value) {
    struct Text {PWSTR value=nullptr;~Text(){CoTaskMemFree(value);}} text;
    require(value!=nullptr,"Read path from actual native item");
    check(value->GetDisplayName(SIGDN_FILESYSPATH,&text.value),"Read actual native filesystem path");
    require(text.value&&*text.value,"Native item has no filesystem path");return fs::path(text.value);
}
struct FileIdentity {
    ULONGLONG volume = 0;
    std::array<BYTE,16> id{};
    auto operator<=>(const FileIdentity&) const = default;
};
FileIdentity fileIdentity(const fs::path& path) {
    const auto handle=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    require(handle!=INVALID_HANDLE_VALUE,"Open owned search file identity");
    FILE_ID_INFO value{};const auto read=GetFileInformationByHandleEx(handle,FileIdInfo,&value,sizeof(value));
    const auto status=read?S_OK:HRESULT_FROM_WIN32(GetLastError());CloseHandle(handle);check(status,"Read owned native file identity");
    FileIdentity result{value.VolumeSerialNumber};std::copy_n(value.FileId.Identifier,result.id.size(),result.id.begin());return result;
}
struct DescriptorSnapshot {
    FileIdentity identity{};
    FILE_BASIC_INFO basic{};
    ULONGLONG bytes=0;
    std::array<BYTE,32> sha256{};
};
bool sameDescriptor(const DescriptorSnapshot& a,const DescriptorSnapshot& b) {
    return a.identity==b.identity&&a.bytes==b.bytes&&a.sha256==b.sha256&&
        a.basic.FileAttributes==b.basic.FileAttributes&&a.basic.CreationTime.QuadPart==b.basic.CreationTime.QuadPart&&
        a.basic.LastWriteTime.QuadPart==b.basic.LastWriteTime.QuadPart&&a.basic.ChangeTime.QuadPart==b.basic.ChangeTime.QuadPart;
}
DescriptorSnapshot descriptorSnapshot(const fs::path& path,ULONGLONG maximumBytes=1024*1024) {
    require(maximumBytes>0&&maximumBytes<=64ULL*1024*1024,"Invalid owned snapshot byte bound");
    // This is a bounded sampled snapshot, not a write/delete-denying lease.
    // Shared native library handles may remain open; metadata and the current
    // path identity are checked again after hashing and at each stage boundary.
    struct File {HANDLE value=INVALID_HANDLE_VALUE;~File(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}} file{
        CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,nullptr)};
    require(file.value!=INVALID_HANDLE_VALUE,"Open owned native descriptor snapshot");
    DescriptorSnapshot result;FILE_ID_INFO id{};LARGE_INTEGER bytes{};
    require(GetFileInformationByHandleEx(file.value,FileIdInfo,&id,sizeof(id))&&
        GetFileInformationByHandleEx(file.value,FileBasicInfo,&result.basic,sizeof(result.basic))&&GetFileSizeEx(file.value,&bytes),
        "Read complete native descriptor identity/metadata");
    require(!(result.basic.FileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))&&
        bytes.QuadPart>0&&static_cast<ULONGLONG>(bytes.QuadPart)<=maximumBytes,"Owned descriptor must be a bounded regular file");
    result.identity.volume=id.VolumeSerialNumber;std::copy_n(id.FileId.Identifier,result.identity.id.size(),result.identity.id.begin());
    result.bytes=static_cast<ULONGLONG>(bytes.QuadPart);
    const auto cng=[](NTSTATUS status,const char* operation){if(status<0)check(HRESULT_FROM_NT(status),operation);};
    struct Algorithm {BCRYPT_ALG_HANDLE value=nullptr;~Algorithm(){if(value)BCryptCloseAlgorithmProvider(value,0);}} algorithm;
    cng(BCryptOpenAlgorithmProvider(&algorithm.value,BCRYPT_SHA256_ALGORITHM,MS_PRIMITIVE_PROVIDER,0),"Open native SHA256 provider");
    struct Hash {BCRYPT_HASH_HANDLE value=nullptr;~Hash(){if(value)BCryptDestroyHash(value);}} hash;
    // Public Windows7+ object allocation is freed by BCryptDestroyHash.
    cng(BCryptCreateHash(algorithm.value,&hash.value,nullptr,0,nullptr,0,0),"Create native descriptor hash");
    std::array<BYTE,4096> buffer{};ULONGLONG total=0;
    for(;;) {
        DWORD count=0;require(ReadFile(file.value,buffer.data(),static_cast<DWORD>(buffer.size()),&count,nullptr),"Read owned descriptor bytes");
        if(!count)break;
        require(total<=result.bytes&&count<=result.bytes-total,"Owned descriptor grew during native snapshot");
        cng(BCryptHashData(hash.value,buffer.data(),count,0),"Hash complete native descriptor bytes");total+=count;
    }
    require(total==result.bytes,"Owned descriptor changed length during native snapshot");
    cng(BCryptFinishHash(hash.value,result.sha256.data(),static_cast<ULONG>(result.sha256.size()),0),"Finish native descriptor hash");
    FILE_ID_INFO afterId{};FILE_BASIC_INFO afterBasic{};LARGE_INTEGER afterBytes{};
    require(GetFileInformationByHandleEx(file.value,FileIdInfo,&afterId,sizeof(afterId))&&
        GetFileInformationByHandleEx(file.value,FileBasicInfo,&afterBasic,sizeof(afterBasic))&&GetFileSizeEx(file.value,&afterBytes),
        "Read descriptor snapshot completion metadata");
    require(id.VolumeSerialNumber==afterId.VolumeSerialNumber&&std::memcmp(id.FileId.Identifier,afterId.FileId.Identifier,16)==0&&
        afterBytes.QuadPart==bytes.QuadPart&&afterBasic.FileAttributes==result.basic.FileAttributes&&
        afterBasic.CreationTime.QuadPart==result.basic.CreationTime.QuadPart&&
        afterBasic.LastWriteTime.QuadPart==result.basic.LastWriteTime.QuadPart&&afterBasic.ChangeTime.QuadPart==result.basic.ChangeTime.QuadPart&&
        fileIdentity(path)==result.identity,"Owned descriptor identity/bytes/metadata changed during read");
    return result;
}
constexpr std::array<const wchar_t*,3> libraryFolderNames{L"Documents",L"Pictures",L"Music"};
constexpr std::array<const wchar_t*,3> firstArtifactNames{L"Owned search.search-ms",L"Application.exe",L"Application shortcut.lnk"};
constexpr std::array<const wchar_t*,2> searchFolderNames{L"Search scope",L"Search scope/Child"};
constexpr std::array<const wchar_t*,5> searchFileNames{L"Search scope/Today.txt",L"Search scope/Earlier.txt",
    L"Search scope/Child/Today child.txt",L"Search scope/Child/Earlier child.txt",L"Outside today.txt"};
struct SearchSourceSnapshot {
    std::array<FileIdentity,2> folders{};
    std::array<DescriptorSnapshot,5> files{};
};
SearchSourceSnapshot searchSourceSnapshot(const fs::path& root) {
    SearchSourceSnapshot result;
    for(size_t index=0;index<result.folders.size();++index){regularPath(root/searchFolderNames[index],true);result.folders[index]=fileIdentity(root/searchFolderNames[index]);}
    require(result.folders[0]!=result.folders[1],"Owned search scope folders have duplicate identities");
    std::set<FileIdentity> distinct;
    for(size_t index=0;index<result.files.size();++index){result.files[index]=descriptorSnapshot(root/searchFileNames[index],6);
        require(result.files[index].bytes==6&&distinct.insert(result.files[index].identity).second,"Owned search source bytes/identities changed");}
    return result;
}
bool sameSearchSource(const SearchSourceSnapshot& a,const SearchSourceSnapshot& b) {
    if(a.folders!=b.folders)return false;
    for(size_t index=0;index<a.files.size();++index)if(!sameDescriptor(a.files[index],b.files[index]))return false;
    return true;
}
std::array<FileIdentity,3> sourceFolderIdentities(const fs::path& root) {
    std::array<FileIdentity,3> result{};
    for(size_t index=0;index<result.size();++index){regularPath(root/libraryFolderNames[index],true);result[index]=fileIdentity(root/libraryFolderNames[index]);}
    require(std::set<FileIdentity>(result.begin(),result.end()).size()==result.size(),"Owned library source folders have duplicate identities");
    return result;
}
GUID fixtureGuid(const fs::path& root) {
    std::ifstream input(root/L".native-visual-fixture",std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(input)),{});
    while(!text.empty()&&(text.back()=='\r'||text.back()=='\n'))text.pop_back();
    require(text.size()==36||text.size()==38,"Read exact owned fixture GUID");
    if(text.size()==36)text="{"+text+"}";
    const std::wstring wide(text.begin(),text.end());GUID value{};check(CLSIDFromString(wide.c_str(),&value),"Read owned fixture GUID");return value;
}
struct LibraryStageHandoff {
    std::uint64_t magic=0x3142494c465745ULL;
    std::uint32_t version=1,bytes=static_cast<std::uint32_t>(sizeof(LibraryStageHandoff));
    GUID fixture{};
    FileIdentity root{},marker{};
    std::array<FileIdentity,3> sourceFolders{};
    DescriptorSnapshot library{};
    std::array<DescriptorSnapshot,3> firstArtifacts{};
    SearchSourceSnapshot searchSources{};
    std::array<DWORD,4> reserved{};
};
static_assert(std::is_trivially_copyable_v<LibraryStageHandoff>);
LibraryStageHandoff readLibraryHandoff(const fs::path& path) {
    regularPath(path,false);
    struct File {HANDLE value=INVALID_HANDLE_VALUE;~File(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}} file{
        CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr)};
    require(file.value!=INVALID_HANDLE_VALUE,"Open exact first-stage library handoff");
    LibraryStageHandoff value{};LARGE_INTEGER size{};DWORD read=0;
    require(GetFileSizeEx(file.value,&size)&&size.QuadPart==static_cast<LONGLONG>(sizeof(value))&&
        ReadFile(file.value,&value,static_cast<DWORD>(sizeof(value)),&read,nullptr)&&read==sizeof(value),"Read complete fixed-size library handoff");
    require(value.magic==LibraryStageHandoff{}.magic&&value.version==1u&&value.bytes==sizeof(value)&&
        std::all_of(value.reserved.begin(),value.reserved.end(),[](DWORD item){return item==0u;}),"Invalid library handoff version/shape");
    return value;
}
void verifyLibraryMembers(explorer::ShellLibrary& library,const std::array<FileIdentity,3>& expected,size_t count) {
    std::vector<explorer::LibraryFolder> locations;check(library.folders(locations),"Read actual persisted native library locations");
    require(locations.size()==count,"Saved library has an unexpected native member count");
    std::set<FileIdentity> actual;
    for(const auto& location:locations){require(location.item&&!location.path.empty(),"Owned library member is not a filesystem item");
        regularPath(location.path,true);require(actual.insert(fileIdentity(location.path)).second,"Native library contains a duplicate folder identity");}
    require(actual==std::set<FileIdentity>(expected.begin(),expected.begin()+count),"Native library contains another folder identity");
}
void modified(const fs::path& path,const FILETIME& value) {
    const auto handle=CreateFileW(path.c_str(),FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    require(handle!=INVALID_HANDLE_VALUE,"Open owned search date fixture");
    const auto written=SetFileTime(handle,nullptr,nullptr,&value);
    const auto status=written?S_OK:HRESULT_FROM_WIN32(GetLastError());CloseHandle(handle);check(status,"Set owned search fixture date");
}
void verifyTodaySearch(const fs::path& root);
void createTodaySearch(const fs::path& root) {
    const auto scope=root/L"Search scope",child=scope/L"Child";
    require(fs::create_directory(scope)&&fs::create_directory(child),"Exclusively create owned search scope");
    FILETIME today{},earlier{};GetSystemTimeAsFileTime(&today);
    SYSTEMTIME old{};old.wYear=2020;old.wMonth=1;old.wDay=2;
    require(SystemTimeToFileTime(&old,&earlier),"Create old search fixture date");
    std::array<fs::path,5> files{};
    for(size_t index=0;index<files.size();++index)files[index]=root/searchFileNames[index];
    for(size_t index=0;index<files.size();++index) {
        createBytes(files[index],std::vector<std::uint8_t>{'O','w','n','e','d','\n'});
        modified(files[index],index==1||index==3?earlier:today);
    }
    modified(child,earlier); // The recursive folder itself is not a Today result.
    constexpr auto query=L"System.DateModified:System.StructuredQueryType.DateTime#Today";
    for(const auto* rejected:{L"System.DateModified:System.StructuredQueryType.DateTime#Yesterday",
            L"System.DateModified:=2026-10-05T00:00:00Z",
            L"System.DateModified:System.StructuredQueryType.DateTime#Today AND System.Size:>0"})
        require(explorer::validateRelativeTodayQuery(rejected)==E_INVALIDARG,
            "Today persistence proof accepted a different relative date, absolute date or extra condition");
    const auto saved=root/L"Owned search.search-ms";
    const auto scoped=item(scope);
    phase("save-native-relative-today");
    check(explorer::saveSearch(query,scoped.Get(),true,saved),"Save genuine unresolved Today search fixture");
    verifyTodaySearch(root);
}
void verifyTodaySearch(const fs::path& root) {
    const auto scoped=item(root/L"Search scope");
    const auto saved=root/L"Owned search.search-ms";
    std::array<fs::path,5> files{};
    for(size_t index=0;index<files.size();++index)files[index]=root/searchFileNames[index];
    phase("read-native-relative-today");
    explorer::SavedSearchMetadata metadata;check(explorer::readSavedSearch(saved,&metadata),"Reload actual saved Today search metadata");
    check(explorer::validateRelativeTodayQuery(metadata.query),"Saved Today condition was frozen, omitted or replaced");
    require(metadata.scope&&metadata.recursive&&metadata.scopeRules.size()==1&&metadata.scopeRules[0].recursive&&
        metadata.scopeRules[0].folder&&!metadata.scopeRules[0].excluded,"Saved Today search scope is not the exact recursive include");
    int comparison=1;check(scoped->Compare(metadata.scope.Get(),SICHINT_CANONICAL,&comparison),"Compare native saved Today scope");
    require(comparison==0,"Saved Today search scope escaped the owned scope");
    comparison=1;check(scoped->Compare(metadata.scopeRules[0].folder.Get(),SICHINT_CANONICAL,&comparison),"Compare native saved Today scope rule");
    require(comparison==0,"Saved Today search rule escaped the owned scope");
    const std::array<ComPtr<IShellItem>,2> expectedItems{item(files[0]),item(files[2])};
    const std::set<FileIdentity> expected{fileIdentity(files[0]),fileIdentity(files[2])};
    require(expected.size()==2,"Owned Today fixtures must have distinct native file identities");
    const auto deadline=GetTickCount64()+5000;
    for(;;) {
        phase("bind-native-relative-today-folder");
        ComPtr<IShellFolder> folder;check(item(saved)->BindToHandler(nullptr,BHID_SFObject,IID_PPV_ARGS(&folder)),"Bind actual saved Today native folder");
        phase("enumerate-native-relative-today-folder");
        ComPtr<IEnumIDList> enumerator;check(folder->EnumObjects(nullptr,static_cast<SHCONTF>(SHCONTF_FOLDERS|SHCONTF_NONFOLDERS),&enumerator),"Enumerate actual saved Today native results");
        std::set<FileIdentity> actual;size_t count=0;
        while(enumerator) {
            phase("read-native-relative-today-result");
            PITEMID_CHILD pidl=nullptr;const auto next=enumerator->Next(1,&pidl,nullptr);if(next==S_FALSE)break;
            check(next,"Read actual saved Today result");require(pidl!=nullptr,"Today result lacks native identity");
            ComPtr<IShellItem> result;const auto created=SHCreateItemWithParent(nullptr,folder.Get(),pidl,IID_PPV_ARGS(&result));
            CoTaskMemFree(pidl);check(created,"Resolve actual saved Today result");
            PWSTR rawPath=nullptr;const auto named=result->GetDisplayName(SIGDN_FILESYSPATH,&rawPath);
            const fs::path resultPath=rawPath?rawPath:L"";CoTaskMemFree(rawPath);check(named,"Read actual Today filesystem identity");
            require(!resultPath.empty(),"Actual Today result lacks a filesystem identity");
            const auto resultFile=item(resultPath); // Unwrap the native search PIDL before canonical comparison.
            size_t expectedIndex=expectedItems.size();
            for(size_t index=0;index<expectedItems.size();++index) {
                int equal=1;check(expectedItems[index]->Compare(resultFile.Get(),SICHINT_CANONICAL,&equal),"Compare owned Today result identity");
                if(equal==0)expectedIndex=index;
            }
            require(expectedIndex<expectedItems.size(),"Today native search included an earlier, outside-scope or unexpected item");
            require(actual.insert(fileIdentity(resultPath)).second,"Today native search returned a duplicate identity");++count;
        }
        phase("native-relative-today-membership-readback");
        if(count==2&&actual==expected)break;
        require(GetTickCount64()<deadline,"Actual saved Today query did not return both exact recursive owned identities");
        MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
        MsgWaitForMultipleObjectsEx(0,nullptr,50,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
    }
}
std::string ascii(const std::wstring& value) {
    std::string result;
    for (const auto character : value) { require(character >= 0x20 && character < 0x7f,"Native fixture metadata must be printable ASCII"); result.push_back(static_cast<char>(character)); }
    return result;
}
std::string jsonString(const std::string& value) {
    std::string result = "\"";
    for (const auto character : value) { if (character == '\\' || character == '"') result.push_back('\\'); result.push_back(character); }
    return result + "\"";
}
template<size_t Count> std::string hexBytes(const std::array<BYTE,Count>& bytes) {
    constexpr char digits[]="0123456789abcdef";std::string result;result.reserve(Count*2);
    for(const auto value:bytes){result.push_back(digits[value>>4]);result.push_back(digits[value&15]);}return result;
}
std::string identityJson(const FileIdentity& value) {
    // Decimal volume is a string so PowerShell JSON does not lose uint64 bits.
    return "{\"volume\":"+jsonString(std::to_string(value.volume))+",\"fileId\":"+jsonString(hexBytes(value.id))+"}";
}
std::string descriptorJson(const DescriptorSnapshot& value) {
    return "{\"identity\":"+identityJson(value.identity)+",\"bytes\":"+std::to_string(value.bytes)+
        ",\"sha256\":"+jsonString(hexBytes(value.sha256))+",\"attributes\":"+std::to_string(value.basic.FileAttributes)+
        ",\"creation\":"+std::to_string(value.basic.CreationTime.QuadPart)+",\"write\":"+std::to_string(value.basic.LastWriteTime.QuadPart)+
        ",\"change\":"+std::to_string(value.basic.ChangeTime.QuadPart)+",\"access\":"+std::to_string(value.basic.LastAccessTime.QuadPart)+"}";
}
std::string searchSourceJson(const SearchSourceSnapshot& value,const char* stage) {
    std::string json="\"searchVerificationStage\":"+jsonString(stage)+",\"searchSourceStatePreserved\":true,\"searchSourceFolders\":[";
    for(size_t index=0;index<value.folders.size();++index){if(index)json+=',';json+="{\"name\":"+jsonString(ascii(searchFolderNames[index]))+
        ",\"identity\":"+identityJson(value.folders[index])+"}";}
    json+="],\"searchSourceFiles\":[";
    for(size_t index=0;index<value.files.size();++index){if(index)json+=',';json+="{\"name\":"+jsonString(ascii(searchFileNames[index]))+
        ",\"snapshot\":"+descriptorJson(value.files[index])+"}";}
    return json+"]";
}
std::string libraryStageJson(const char* stage,const fs::path& root,const std::array<FileIdentity,3>& folders,
                             const DescriptorSnapshot* before,const DescriptorSnapshot& after,const DescriptorSnapshot* handoff) {
    const auto guid=fixtureGuid(root);wchar_t text[40]{};require(StringFromGUID2(guid,text,static_cast<int>(std::size(text)))!=0,"Format owned fixture GUID receipt");
    std::string json="\"stage\":"+jsonString(stage)+",\"fixtureGuid\":"+jsonString(ascii(text))+
        ",\"fixtureRoot\":"+identityJson(fileIdentity(root))+",\"sourceFolders\":[";
    for(size_t index=0;index<folders.size();++index){if(index)json+=',';json+="{\"name\":"+jsonString(ascii(libraryFolderNames[index]))+
        ",\"identity\":"+identityJson(folders[index])+"}";}
    json+="],\"libraryDescriptorBefore\":"+(before?descriptorJson(*before):std::string("null"))+
        ",\"libraryDescriptorAfter\":"+descriptorJson(after)+",\"libraryDescriptorIdentityReplaced\":"+
        (before&&before->identity!=after.identity?"true":"false")+",\"handoff\":"+(handoff?descriptorJson(*handoff):std::string("null"));
    return json;
}
std::string metadata(const fs::path& root, const fs::path& relative, const wchar_t* expectedType, const wchar_t* expectedKind = nullptr) {
    ComPtr<IShellItem2> native; check(item(root/relative).As(&native),"Read native fixture property interface");
    ComPtr<IPropertyStore> store; check(native->GetPropertyStore(GPS_FASTPROPERTIESONLY | GPS_BESTEFFORT,IID_PPV_ARGS(&store)),"Read only fast fixture properties");
    PROPVARIANT type{}; PropVariantInit(&type);
    check(store->GetValue(PKEY_ItemType,&type),"Read native fixture item type");
    const bool typeMatches = type.vt == VT_LPWSTR && type.pwszVal && _wcsicmp(type.pwszVal,expectedType) == 0;
    const std::string typeText = type.vt == VT_LPWSTR && type.pwszVal ? ascii(type.pwszVal) : std::string{};
    PropVariantClear(&type); require(typeMatches,"Native fixture item type did not match its real file");
    PROPVARIANT kind{}; PropVariantInit(&kind);
    check(store->GetValue(PKEY_Kind,&kind),"Read native fixture Kind");
    std::string kinds = "["; bool expected = expectedKind == nullptr;
    if (kind.vt == (VT_VECTOR | VT_LPWSTR)) {
        for (ULONG index = 0; index < kind.calpwstr.cElems; ++index) {
            if (index) kinds += ',';
            const auto value = kind.calpwstr.pElems[index];
            if (value && expectedKind && _wcsicmp(value,expectedKind) == 0) expected = true;
            kinds += jsonString(value ? ascii(value) : std::string{});
        }
    } else require(kind.vt == VT_EMPTY,"Native fixture Kind has an unexpected property type");
    kinds += ']'; PropVariantClear(&kind); require(expected,"Native fixture Kind did not match its media context");
    return "{\"name\":" + jsonString(ascii(relative.generic_wstring())) + ",\"itemType\":" + jsonString(typeText) + ",\"kind\":" + kinds + ",\"status\":0}";
}
}

int wmain(int count, wchar_t** arguments) {
    try {
        fs::path root, executable, report;
        enum class Stage { All, First, Complete };Stage stage=Stage::All;bool stageSpecified=false;
        for (int index = 1; index < count; ++index) {
            const std::wstring_view option(arguments[index]);
            require(index+1 < count,"Each fixture argument requires a value");
            const fs::path value(arguments[++index]);
            if (option == L"--path" && root.empty()) root = fs::absolute(value).lexically_normal();
            else if (option == L"--executable" && executable.empty()) executable = fs::absolute(value).lexically_normal();
            else if (option == L"--report" && report.empty()) report = value;
            else if (option == L"--stage" && !stageSpecified) {
                stageSpecified=true;
                if(value==L"first")stage=Stage::First;
                else if(value==L"complete")stage=Stage::Complete;
                else if(value==L"all")stage=Stage::All;
                else throw std::runtime_error("Unknown native fixture stage");
            }
            else throw std::runtime_error("Unknown or duplicate fixture argument");
        }
        require(!root.empty() && !executable.empty() && !report.empty(),"Require --path, --executable and --report");
        report = (report.is_absolute() ? report : root/report).lexically_normal();
        validateRoot(root); regularPath(executable,false);
        require(report.parent_path() == root,"Fixture report must remain inside the owned root");
        const std::array<fs::path,7> outputs{L"Owned search.search-ms",L"Owned library.library-ms",L"Application.exe",L"Application shortcut.lnk",L"Music/Owned audio.wav",L"Videos/Owned video.avi",L"Owned disc image.iso"};
        for(size_t index=0;index<outputs.size();++index) {
            if(stage==Stage::Complete&&index<4)regularPath(root/outputs[index],false);else absent(root/outputs[index]);
        }
        const auto handoffPath=root/L".native-library-stage.bin";
        if(stage==Stage::First)absent(handoffPath);
        absent(report);
        const auto originalInputDesktop = inputDesktopName();
        require(!visibleProcessInputWindows(),"Fixture process must have no visible input-desktop windows");
        PrivateDesktop desktop; ComApartment apartment;
        const auto sourceFolders=sourceFolderIdentities(root);
        const auto rootIdentity=fileIdentity(root);
        std::optional<LibraryStageHandoff> prior;
        DescriptorSnapshot libraryBefore{},libraryAfter{},handoffSnapshot{};
        SearchSourceSnapshot searchSources{};
        if(stage==Stage::Complete) {
            phase("verify-exact-first-stage-native-handoff");
            handoffSnapshot=descriptorSnapshot(handoffPath);prior=readLibraryHandoff(handoffPath);
            require(sameDescriptor(handoffSnapshot,descriptorSnapshot(handoffPath)),"Library handoff changed during read");
            require(IsEqualGUID(prior->fixture,fixtureGuid(root))&&prior->root==rootIdentity&&
                prior->marker==fileIdentity(root/L".native-visual-fixture")&&prior->sourceFolders==sourceFolders,
                "First-stage native fixture/root/source identity changed");
            libraryBefore=descriptorSnapshot(root/outputs[1]);
            require(sameDescriptor(prior->library,libraryBefore),"Native library descriptor differs from exact first-stage publication");
            for(size_t index=0;index<firstArtifactNames.size();++index)
                require(sameDescriptor(prior->firstArtifacts[index],descriptorSnapshot(root/firstArtifactNames[index],64ULL*1024*1024)),
                    "First-stage native search/application/shortcut changed");
            searchSources=searchSourceSnapshot(root);
            require(sameSearchSource(prior->searchSources,searchSources),"First-stage Today search source identities/bytes/metadata changed");
        } else {
            phase("owned-native-shortcut");
            require(CopyFileW(executable.c_str(),(root/L"Application.exe").c_str(),TRUE),"Exclusively copy owned application fixture without executing it");
            ComPtr<IShellLinkW> link;check(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&link)),"Create owned native shortcut");
            check(link->SetPath((root/L"Application.exe").c_str()),"Set owned shortcut target");
            check(link->SetWorkingDirectory(root.c_str()),"Set owned shortcut working directory");
            ComPtr<IPersistFile> persist;check(link.As(&persist),"Query owned shortcut writer");
            check(persist->Save((root/L"Application shortcut.lnk").c_str(),TRUE),"Save owned shortcut");
            phase("owned-native-relative-today-search");createTodaySearch(root);
            searchSources=searchSourceSnapshot(root);
        }
        explorer::ShellLibrary library;
        if(stage==Stage::Complete) {
            phase("load-owned-native-library-for-completion");
            check(explorer::ShellLibrary::load(item(root/outputs[1]).Get(),true,library),"Load exact first-stage native library writable");
            require(sameDescriptor(libraryBefore,descriptorSnapshot(root/outputs[1])),"Native load changed the published library descriptor");
            verifyLibraryMembers(library,sourceFolders,1);
        } else {
            phase("owned-native-library");check(explorer::ShellLibrary::create(library),"Create owned native library");
            phase("include-owned-library-documents");check(library.addFolder(root/L"Documents"),"Include owned Documents in library");
        }
        if(stage!=Stage::First) {
            phase("include-owned-library-pictures");check(library.addFolder(root/L"Pictures"),"Include owned Pictures in library");
            phase("include-owned-library-music");check(library.addFolder(root/L"Music"),"Include owned Music in library");
            phase("set-owned-library-default-save-folder");check(library.setDefaultSaveFolder(root/L"Documents"),"Set owned library save location");
            phase("set-owned-library-template");check(library.optimize(explorer::LibraryKind::Documents),"Set native Documents library type");
        }
        ComPtr<IShellItem> saved;
        if(stage==Stage::Complete) {
            require(sameDescriptor(libraryBefore,descriptorSnapshot(root/outputs[1])),"Uncommitted native library unexpectedly changed its backing file");
            phase("commit-owned-native-library");check(library.commit(),"Commit genuine owned library completion");
        } else {
            phase("save-owned-native-library");check(library.save(root,L"Owned library",saved),"Save genuine owned library");
            require(saved&&fileIdentity(nativeItemPath(saved.Get()))==fileIdentity(root/outputs[1]),"Saved library has another native file identity");
            saved.Reset();
        }
        phase("release-owned-native-library");
        library = explorer::ShellLibrary{};
        explorer::ShellLibrary persisted;
        phase("reload-owned-native-library");
        check(explorer::ShellLibrary::load(item(root/outputs[1]).Get(),false,persisted),"Reload actual owned library file");
        phase("read-owned-library-locations");verifyLibraryMembers(persisted,sourceFolders,stage==Stage::First?1u:3u);
        libraryAfter=descriptorSnapshot(root/outputs[1]);
        require(sourceFolderIdentities(root)==sourceFolders&&fileIdentity(root)==rootIdentity,"Native library setup replaced owned source/root identities");
        if(stage==Stage::First) {
            persisted=explorer::ShellLibrary{};
            std::string fixtureFiles;
            constexpr std::array<const wchar_t*,4> types{L".search-ms",L".library-ms",L".exe",L".lnk"};
            for(size_t index=0;index<types.size();++index){if(index)fixtureFiles+=',';fixtureFiles+=metadata(root,outputs[index],types[index]);}
            libraryAfter=descriptorSnapshot(root/outputs[1]);
            LibraryStageHandoff handoff;handoff.fixture=fixtureGuid(root);handoff.root=rootIdentity;
            handoff.marker=fileIdentity(root/L".native-visual-fixture");handoff.sourceFolders=sourceFolders;handoff.library=libraryAfter;
            handoff.searchSources=searchSources;
            for(size_t index=0;index<firstArtifactNames.size();++index)handoff.firstArtifacts[index]=descriptorSnapshot(root/firstArtifactNames[index],64ULL*1024*1024);
            const auto bytes=reinterpret_cast<const std::uint8_t*>(&handoff);
            createBytes(handoffPath,std::vector<std::uint8_t>(bytes,bytes+sizeof(handoff)));handoffSnapshot=descriptorSnapshot(handoffPath);
            std::string json="{\"headless\":true,\"privateDesktop\":true,\"inputDesktopUnchanged\":true,\"visibleInputDesktopWindows\":false,\"passed\":true,"
                "\"completeFixture\":false,\"executedFixture\":false,\"created\":4,\"createdThisStage\":4,\"libraryLocations\":1,\"searchScope\":\"Search scope\",\"searchRelativeToday\":true,\"searchRecursive\":true,"
                "\"searchResultCount\":2,\"searchExactIdentities\":true,\"searchEarlierExcluded\":true,\"searchOutsideScopeExcluded\":true,";
            json+=libraryStageJson("first",root,sourceFolders,nullptr,libraryAfter,&handoffSnapshot)+","+
                searchSourceJson(searchSources,"first")+",\"fixtureFiles\":["+fixtureFiles+"]}\n";
            require(sourceFolderIdentities(root)==sourceFolders&&fileIdentity(root)==rootIdentity&&
                sameDescriptor(libraryAfter,descriptorSnapshot(root/outputs[1]))&&sameSearchSource(searchSources,searchSourceSnapshot(root)),
                "First-stage publication changed native source/descriptor identities");
            require(inputDesktopName()==originalInputDesktop&&!visibleProcessInputWindows(),"First-stage private/input desktop invariant changed");
            phase("native-fixture-first-stage-report");createBytes(report,std::vector<std::uint8_t>(json.begin(),json.end()));
            std::cout<<"Persisted one genuine owned native library member; complete fixture is not yet available.\n";return 0;
        }
        fs::path defaultLocation;
        phase("read-owned-library-default-save-path");
        check(persisted.defaultSavePath(defaultLocation),"Read actual saved library default");
        regularPath(defaultLocation,true);require(fileIdentity(defaultLocation)==sourceFolders[0],"Saved library default has another native folder identity");
        phase("read-owned-library-template");
        GUID type{}; check(persisted.folderType(type),"Read actual saved library template");
        require(explorer::libraryKindForType(type) == explorer::LibraryKind::Documents,"Saved library template differs from Documents");
        phase("owned-native-media");
        createBytes(root/L"Music/Owned audio.wav",wave());
        createBytes(root/L"Videos/Owned video.avi",avi());
        validateMedia(root/L"Music/Owned audio.wav",streamtypeAUDIO,44100,2);
        validateMedia(root/L"Videos/Owned video.avi",streamtypeVIDEO,1,16);
        phase("owned-native-recorder-free-iso");
        createDiscImage(root);
        if(stage==Stage::Complete){phase("verify-completed-native-relative-today-search");verifyTodaySearch(root);}
        require(sameSearchSource(searchSources,searchSourceSnapshot(root)),"Native completion changed the exact Today search source state");
        phase("native-fixture-fast-property-readback");
        std::string json = "{\"headless\":true,\"privateDesktop\":true,\"inputDesktopUnchanged\":true,\"visibleInputDesktopWindows\":false,\"passed\":true,\"completeFixture\":true,\"executedFixture\":false,\"created\":7,\"isoBuilder\":\"IMAPI2FS\",\"isoBuilderStatus\":0,\"isoVolumeVerified\":true,\"libraryLocations\":3,\"libraryDefault\":\"Documents\",\"libraryTemplate\":\"Documents\",\"searchScope\":\"Search scope\",\"searchRelativeToday\":true,\"searchRecursive\":true,\"searchResultCount\":2,\"searchExactIdentities\":true,\"searchEarlierExcluded\":true,\"searchOutsideScopeExcluded\":true,";
        json+="\"createdThisStage\":"+std::to_string(stage==Stage::Complete?3:7)+",";
        json+=libraryStageJson(stage==Stage::Complete?"complete":"all",root,sourceFolders,prior?&libraryBefore:nullptr,libraryAfter,prior?&handoffSnapshot:nullptr)+","+
            searchSourceJson(searchSources,stage==Stage::Complete?"complete":"all")+",\"fixtureFiles\":[";
        const std::array<const wchar_t*,7> types{L".search-ms",L".library-ms",L".exe",L".lnk",L".wav",L".avi",L".iso"};
        for (std::size_t index = 0; index < outputs.size(); ++index) {
            if (index) json += ',';
            json += metadata(root,outputs[index],types[index],index == 4 ? L"music" : index == 5 ? L"video" : nullptr);
        }
        json += "]}\n";
        require(sameDescriptor(libraryAfter,descriptorSnapshot(root/outputs[1])),"Native fixture property read changed the committed library descriptor");
        require(fileIdentity(root)==rootIdentity&&sourceFolderIdentities(root)==sourceFolders&&sameSearchSource(searchSources,searchSourceSnapshot(root)),
            "Final fixture root/source-folder/search-source state changed");
        if(prior) {
            require(sameDescriptor(handoffSnapshot,descriptorSnapshot(handoffPath)),"First-stage handoff changed during completion");
            for(size_t index=0;index<firstArtifactNames.size();++index)
                require(sameDescriptor(prior->firstArtifacts[index],descriptorSnapshot(root/firstArtifactNames[index],64ULL*1024*1024)),"First-stage semantic fixture changed during completion");
        }
        require(inputDesktopName() == originalInputDesktop,"Input desktop changed during fixture creation");
        require(!visibleProcessInputWindows(),"Fixture process published a visible input-desktop window");
        phase("native-fixture-report");
        createBytes(report,std::vector<std::uint8_t>(json.begin(),json.end()));
        std::cout << "Verified 7 genuine owned native visual fixtures; no application execution, mount, burning or user namespace changes.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "Native fixture builder: " << error.what() << '\n'; return 1; }
}
