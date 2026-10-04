#include "explorer/library.hpp"
#include "explorer/search.hpp"

#include <windows.h>
#include <shlobj.h>
#include <propkey.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <vfw.h>
#include <imapi2fs.h>
#include <shlwapi.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

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
        for (int index = 1; index < count; ++index) {
            const std::wstring_view option(arguments[index]);
            require(index+1 < count,"Each fixture argument requires a value");
            const fs::path value(arguments[++index]);
            if (option == L"--path" && root.empty()) root = fs::absolute(value).lexically_normal();
            else if (option == L"--executable" && executable.empty()) executable = fs::absolute(value).lexically_normal();
            else if (option == L"--report" && report.empty()) report = value;
            else throw std::runtime_error("Unknown or duplicate fixture argument");
        }
        require(!root.empty() && !executable.empty() && !report.empty(),"Require --path, --executable and --report");
        report = (report.is_absolute() ? report : root/report).lexically_normal();
        validateRoot(root); regularPath(executable,false);
        require(report.parent_path() == root,"Fixture report must remain inside the owned root");
        const std::array<fs::path,7> outputs{L"Owned search.search-ms",L"Owned library.library-ms",L"Application.exe",L"Application shortcut.lnk",L"Music/Owned audio.wav",L"Videos/Owned video.avi",L"Owned disc image.iso"};
        for (const auto& output : outputs) absent(root/output);
        absent(report);
        const auto originalInputDesktop = inputDesktopName();
        require(!visibleProcessInputWindows(),"Fixture process must have no visible input-desktop windows");
        PrivateDesktop desktop; ComApartment apartment;
        require(CopyFileW(executable.c_str(),(root/L"Application.exe").c_str(),TRUE),"Exclusively copy owned application fixture without executing it");
        ComPtr<IShellLinkW> link; check(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&link)),"Create owned native shortcut");
        check(link->SetPath((root/L"Application.exe").c_str()),"Set owned shortcut target");
        check(link->SetWorkingDirectory(root.c_str()),"Set owned shortcut working directory");
        ComPtr<IPersistFile> persist; check(link.As(&persist),"Query owned shortcut writer");
        check(persist->Save((root/L"Application shortcut.lnk").c_str(),TRUE),"Save owned shortcut");
        check(explorer::saveSearch(L"System.FileName:=\"Read me.txt\"",item(root).Get(),true,root/L"Owned search.search-ms"),"Save genuine native search fixture");
        explorer::ShellLibrary library; check(explorer::ShellLibrary::create(library),"Create owned native library");
        check(library.addFolder(root/L"Documents"),"Include owned Documents in library");
        check(library.addFolder(root/L"Pictures"),"Include owned Pictures in library");
        check(library.addFolder(root/L"Music"),"Include owned Music in library");
        check(library.setDefaultSaveFolder(root/L"Documents"),"Set owned library save location");
        check(library.optimize(explorer::LibraryKind::Documents),"Set native Documents library type");
        ComPtr<IShellItem> saved; check(library.save(root,L"Owned library",saved),"Save genuine owned library");
        library = explorer::ShellLibrary{};
        explorer::ShellLibrary persisted;
        check(explorer::ShellLibrary::load(saved.Get(),false,persisted),"Reload actual owned library file");
        std::vector<explorer::LibraryFolder> locations;
        check(persisted.folders(locations),"Read actual saved library locations");
        require(locations.size() == 3,"Saved library does not contain all three owned locations");
        fs::path defaultLocation;
        check(persisted.defaultSavePath(defaultLocation),"Read actual saved library default");
        require(defaultLocation == root/L"Documents","Saved library default differs from owned Documents");
        GUID type{}; check(persisted.folderType(type),"Read actual saved library template");
        require(explorer::libraryKindForType(type) == explorer::LibraryKind::Documents,"Saved library template differs from Documents");
        createBytes(root/L"Music/Owned audio.wav",wave());
        createBytes(root/L"Videos/Owned video.avi",avi());
        validateMedia(root/L"Music/Owned audio.wav",streamtypeAUDIO,44100,2);
        validateMedia(root/L"Videos/Owned video.avi",streamtypeVIDEO,1,16);
        createDiscImage(root);
        std::string json = "{\"headless\":true,\"privateDesktop\":true,\"inputDesktopUnchanged\":true,\"visibleInputDesktopWindows\":false,\"passed\":true,\"executedFixture\":false,\"created\":7,\"isoBuilder\":\"IMAPI2FS\",\"isoBuilderStatus\":0,\"isoVolumeVerified\":true,\"libraryLocations\":3,\"libraryDefault\":\"Documents\",\"libraryTemplate\":\"Documents\",\"fixtureFiles\":[";
        const std::array<const wchar_t*,7> types{L".search-ms",L".library-ms",L".exe",L".lnk",L".wav",L".avi",L".iso"};
        for (std::size_t index = 0; index < outputs.size(); ++index) {
            if (index) json += ',';
            json += metadata(root,outputs[index],types[index],index == 4 ? L"music" : index == 5 ? L"video" : nullptr);
        }
        json += "]}\n";
        require(inputDesktopName() == originalInputDesktop,"Input desktop changed during fixture creation");
        require(!visibleProcessInputWindows(),"Fixture process published a visible input-desktop window");
        createBytes(report,std::vector<std::uint8_t>(json.begin(),json.end()));
        std::cout << "Created 7 genuine owned native visual fixtures; no application execution, mount, burning or user namespace changes.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "Native fixture builder: " << error.what() << '\n'; return 1; }
}
