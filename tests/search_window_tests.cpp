#include "explorer/search_window.hpp"
#include "explorer/headless_visual.hpp"
#include <shlobj.h>
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <compare>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>

namespace {
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void succeeded(HRESULT status, const char* message) {
    if (FAILED(status)) { std::cerr << "searchWindow HRESULT=" << static_cast<unsigned long>(status) << '\n'; throw std::runtime_error(message); }
}
struct Handle { HANDLE value = nullptr; ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); } };
struct TaskFree { template<class T> void operator()(T* value) const noexcept { CoTaskMemFree(value); } };
struct Fixture {
    fs::path root;
    Fixture() {
        GUID value{}; succeeded(CoCreateGuid(&value), "create owned search-window fixture ID");
        wchar_t text[40]{}; require(StringFromGUID2(value, text, 40) != 0, "format owned search-window fixture ID");
        root = fs::temp_directory_path() / (std::wstring(L"Explorer-search-window-資料-") + text);
        require(fs::create_directory(root), "create exclusively owned search-window fixture");
    }
    ~Fixture() { std::error_code ignored; fs::remove_all(root, ignored); }
};
ComPtr<IShellItem> item(const fs::path& path) {
    ComPtr<IShellItem> result; succeeded(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&result)), "parse actual owned scope"); return result;
}
struct Identity {
    ULONGLONG volume = 0; std::array<BYTE, 16> file{};
    auto operator<=>(const Identity&) const = default;
};
Identity identity(const fs::path& path) {
    Handle file{CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
    require(file.value != INVALID_HANDLE_VALUE, "open owned identity only");
    FILE_ID_INFO info{}; require(GetFileInformationByHandleEx(file.value, FileIdInfo, &info, sizeof(info)), "read exact owned volume/128-bit ID");
    Identity result{info.VolumeSerialNumber}; std::copy_n(info.FileId.Identifier, result.file.size(), result.file.begin()); return result;
}
FILE_BASIC_INFO basic(const fs::path& path) {
    Handle file{CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
    FILE_BASIC_INFO result{};
    require(file.value != INVALID_HANDLE_VALUE && GetFileInformationByHandleEx(file.value, FileBasicInfo, &result, sizeof(result)),
            "read owned source metadata");
    return result;
}
std::string contents(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    require(file.good(), "read owned source bytes");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void sameItem(IShellItem* first, IShellItem* second) {
    int comparison = 1; succeeded(first->Compare(second, SICHINT_CANONICAL, &comparison), "compare real transferred native scope identity");
    require(comparison == 0, "search-window scope identity changed");
}
ComPtr<IShellItemArray> array(IShellItem* first, IShellItem* second) {
    PIDLIST_ABSOLUTE a = nullptr, b = nullptr;
    succeeded(SHGetIDListFromObject(first, &a), "read first real scope PIDL");
    const auto secondStatus = SHGetIDListFromObject(second, &b);
    if (FAILED(secondStatus)) { CoTaskMemFree(a); succeeded(secondStatus, "read second real scope PIDL"); }
    PCIDLIST_ABSOLUTE scopes[]{a, b}; ComPtr<IShellItemArray> result;
    const auto status = SHCreateShellItemArrayFromIDLists(2, scopes, &result);
    CoTaskMemFree(a); CoTaskMemFree(b); succeeded(status, "retain complete native two-scope array"); return result;
}
std::set<Identity> results(const explorer::SearchWindowContext& context) {
    ComPtr<IShellItem> search;
    succeeded(context.rules.empty()?explorer::createSearchFolderForScopes(context.query,context.scopes.Get(),&search,context.recursive):
        explorer::createSearchFolderForScopeRules(context.query,context.rules,&search), "rebuild fresh native folder from transferred original context");
    ComPtr<IShellFolder> folder; succeeded(search->BindToHandler(nullptr,BHID_SFObject,IID_PPV_ARGS(&folder)), "bind actual transferred-context search");
    ComPtr<IEnumIDList> enumeration;
    succeeded(folder->EnumObjects(nullptr,SHCONTF_NONFOLDERS,&enumeration), "enumerate actual transferred-context file identities");
    std::set<Identity> output;
    while(enumeration) {
        PITEMID_CHILD child = nullptr; const auto next = enumeration->Next(1,&child,nullptr); if(next==S_FALSE)break;
        succeeded(next,"read actual transferred-context result");
        ComPtr<IShellItem> value; const auto made=SHCreateItemWithParent(nullptr,folder.Get(),child,IID_PPV_ARGS(&value));CoTaskMemFree(child);
        succeeded(made,"resolve actual transferred-context result");
        PWSTR path=nullptr;const auto named=value->GetDisplayName(SIGDN_FILESYSPATH,&path);
        if(FAILED(named)){CoTaskMemFree(path);succeeded(named,"read actual result filesystem identity");}
        std::unique_ptr<wchar_t,TaskFree> ownedPath(path);
        const fs::path owned(path);
        require(output.insert(identity(owned)).second&&output.size()<=16,"duplicate or unbounded search-window results");
    }
    return output;
}
void nativeContextAndReadOnlyMapping() {
    Fixture fixture;const auto firstPath=fixture.root/L"first",secondPath=fixture.root/L"second",childPath=firstPath/L"child";
    require(fs::create_directories(childPath)&&fs::create_directory(secondPath),"create owned two-scope handoff hierarchy");
    const auto firstFile=firstPath/L"target.txt",secondFile=secondPath/L"target.txt",nested=childPath/L"target.txt";
    for(const auto& path:{firstFile,secondFile,nested}){std::ofstream file(path,std::ios::binary);file<<"target";require(file.good(),"write owned handoff member");}
    auto first=item(firstPath),second=item(secondPath),child=item(childPath);
    explorer::SearchWindowContext original;original.query=L"System.FileName:=\"target.txt\" AND System.Size:1..10";
    original.primaryScope=first;original.closeOrigin=second;original.scopes=array(first.Get(),second.Get());original.recursive=false;
    explorer::SearchViewPresentation presentation;presentation.mode=explorer::SearchViewMode::Content;presentation.iconSize=32;
    presentation.visibleColumns=std::vector<std::wstring>{L"System.ItemNameDisplay",L"System.DateModified"};
    presentation.groupBy=explorer::SearchViewOrder{L"System.Kind",SORT_ASCENDING};
    presentation.sort=std::vector<explorer::SearchViewOrder>{{L"System.DateModified",SORT_DESCENDING},{L"System.ItemNameDisplay",SORT_ASCENDING}};
    original.presentation=presentation;
    explorer::SearchFileProperties properties;properties.author=L"資料 \U0001F680 & %1";properties.kind=L"";
    properties.description=L"exact\r\nline\ttext";properties.tags=L"owned";original.fileProperties=properties;
    const std::set<Identity> expected{identity(firstFile),identity(secondFile)};
    const std::array sourceIds{identity(firstFile),identity(secondFile),identity(nested)};
    const std::array sourceMetadata{basic(firstFile),basic(secondFile),basic(nested)};
    const auto unsupportedSave=fixture.root/L"unsupported-full-view.search-ms";
    require(explorer::saveSearchForScopes(original.query,original.scopes.Get(),original.recursive,unsupportedSave,
        explorer::SearchSaveMode::CreateNew,&presentation,&properties)==HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)&&!fs::exists(unsupportedSave),
        "handoff fixture unexpectedly relies on a supported saved-search layout");
    require(results(original)==expected,"original actual native handoff scope membership mismatch");
    for(bool rulesPresent:{false,true}) {
        original.rules=rulesPresent?std::vector<explorer::SearchScopeRule>{{first,false,false},{second,false,false},{child,true,true}}:
                                   std::vector<explorer::SearchScopeRule>{};
        explorer::SearchWindowMapping mapping;succeeded(explorer::SearchWindowMapping::create(original,&mapping),"create anonymous reduced read-only handoff");
        DWORD flags=0;require(GetHandleInformation(mapping.handle(),&flags)&&(flags&HANDLE_FLAG_INHERIT),"handoff handle is not inheritable");
        auto writable=MapViewOfFile(mapping.handle(),FILE_MAP_WRITE,0,0,0);
        if(writable)UnmapViewOfFile(writable);
        require(!writable,"reduced handoff unexpectedly grants write access");
        explorer::SearchWindowContext copied;succeeded(mapping.read(&copied),"load complete real read-only handoff");
        require(copied.query==original.query&&copied.recursive==original.recursive&&copied.rules.size()==original.rules.size()&&
                copied.fileProperties==original.fileProperties&&copied.presentation&&copied.presentation->mode==presentation.mode&&
                copied.presentation->iconSize==presentation.iconSize&&copied.presentation->visibleColumns==presentation.visibleColumns&&
                copied.presentation->groupBy&&copied.presentation->groupBy->property==presentation.groupBy->property&&
                copied.presentation->groupBy->direction==presentation.groupBy->direction&&copied.presentation->sort&&copied.presentation->sort->size()==2&&
                (*copied.presentation->sort)[0].property==(*presentation.sort)[0].property&&(*copied.presentation->sort)[0].direction==SORT_DESCENDING&&
                (*copied.presentation->sort)[1].property==(*presentation.sort)[1].property&&(*copied.presentation->sort)[1].direction==SORT_ASCENDING,
                "handoff changed full layout/query/metadata or individual rule count");
        sameItem(copied.primaryScope.Get(),first.Get());sameItem(copied.closeOrigin.Get(),second.Get());
        DWORD count=0;succeeded(copied.scopes->GetCount(&count),"read complete transferred scope-array count");require(count==2,"empty rules lost a scope-array location");
        for(DWORD index=0;index<count;++index){ComPtr<IShellItem> scope;succeeded(copied.scopes->GetItemAt(index,&scope),"read every transferred scope location");sameItem(scope.Get(),index?second.Get():first.Get());}
        for(size_t index=0;index<copied.rules.size();++index){sameItem(copied.rules[index].folder.Get(),original.rules[index].folder.Get());
            require(copied.rules[index].recursive==original.rules[index].recursive&&copied.rules[index].excluded==original.rules[index].excluded,"handoff changed rule flags");}
        require(results(copied)==expected,"transferred native factory changed exact result FileIDs");
        HANDLE inherited=nullptr;require(DuplicateHandle(GetCurrentProcess(),mapping.handle(),GetCurrentProcess(),&inherited,0,TRUE,DUPLICATE_SAME_ACCESS),"duplicate actual read-only inherited handoff");
        explorer::SearchWindowContext consumed;succeeded(explorer::consumeSearchWindowContext(reinterpret_cast<ULONG_PTR>(inherited),&consumed),"consume genuine validated inherited mapping");
        require(!GetHandleInformation(inherited,&flags)&&GetLastError()==ERROR_INVALID_HANDLE&&results(consumed)==expected,
                "validated handoff handle was retained or lost native semantics");
    }
    size_t index=0;
    for(const auto& path:{firstFile,secondFile,nested}) {
        const auto after=basic(path);
        require(contents(path)=="target"&&identity(path)==sourceIds[index]&&
                after.LastWriteTime.QuadPart==sourceMetadata[index].LastWriteTime.QuadPart&&
                after.ChangeTime.QuadPart==sourceMetadata[index].ChangeTime.QuadPart&&
                after.FileAttributes==sourceMetadata[index].FileAttributes,"handoff changed owned source bytes, identity or metadata");
        ++index;
    }
}
void malformedPacketsAndHandleOwnership() {
    Fixture fixture;auto scope=item(fixture.root);explorer::SearchWindowContext original;
    original.query=L"System.FileName:=\"unchanged\"";original.primaryScope=scope;original.closeOrigin=scope;
    succeeded(SHCreateShellItemArrayFromShellItem(scope.Get(),IID_PPV_ARGS(&original.scopes)),"create real packet scope");
    std::vector<BYTE> packet;succeeded(explorer::encodeSearchWindowContext(original,&packet),"serialize original real scope packet");
    explorer::SearchWindowContext minimal;
    succeeded(explorer::decodeSearchWindowContext(packet,&minimal),"decode valid context with absent optional fields");
    require(!minimal.presentation&&!minimal.fileProperties&&minimal.recursive==original.recursive&&minimal.rules.empty(),
            "absent optional handoff fields acquired invented values");
    require(explorer::encodeSearchWindowContext(original,nullptr)==E_POINTER&&
            explorer::decodeSearchWindowContext(packet,nullptr)==E_POINTER&&
            explorer::consumeSearchWindowContext(0,nullptr)==E_POINTER,
            "null public handoff outputs were accepted");
    const auto number=[](std::vector<BYTE>& bytes,size_t offset,DWORD value){require(offset+sizeof(value)<=bytes.size(),"malformed fixture offset outside packet");std::memcpy(bytes.data()+offset,&value,sizeof(value));};
    std::vector<std::vector<BYTE>> invalid;
    invalid.emplace_back(packet.begin(),packet.begin()+3);
    auto changed=packet;number(changed,0,0);invalid.push_back(changed);
    changed=packet;number(changed,4,99);invalid.push_back(changed);
    changed=packet;number(changed,8,MAXDWORD);invalid.push_back(changed);
    changed=packet;number(changed,12,1);invalid.push_back(changed);
    changed=packet;number(changed,16,MAXDWORD);invalid.push_back(changed);
    changed=packet;changed[20]=0;changed[21]=0;invalid.push_back(changed);
    changed=packet;changed[20]=0;changed[21]=0xD8;invalid.push_back(changed);
    const auto pidlLengthOffset=20+original.query.size()*sizeof(wchar_t);
    changed=packet;number(changed,pidlLengthOffset,MAXDWORD);invalid.push_back(changed);
    changed=packet;changed[pidlLengthOffset+4]=0xFF;changed[pidlLengthOffset+5]=0xFF;invalid.push_back(changed);
    DWORD pidlLength=0;std::memcpy(&pidlLength,packet.data()+pidlLengthOffset,sizeof(pidlLength));
    changed=packet;number(changed,pidlLengthOffset+sizeof(DWORD)+pidlLength,2);invalid.push_back(changed);
    changed=packet;changed.push_back(0);number(changed,8,static_cast<DWORD>(changed.size()));invalid.push_back(changed);
    for(const auto& bytes:invalid){auto output=original;output.query=L"preserved output";
        require(FAILED(explorer::decodeSearchWindowContext(bytes,&output))&&output.query==L"preserved output"&&output.primaryScope.Get()==scope.Get(),
                "malformed packet changed public output or reached native factory acceptance");}
    auto encoded=packet;original.query.push_back(L'\0');
    require(explorer::encodeSearchWindowContext(original,&encoded)==E_INVALIDARG&&encoded==packet,"failed encoding changed packet output");
    SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};Handle event{CreateEventW(&security,FALSE,FALSE,nullptr)};
    require(event.value!=nullptr,"create owned unrelated inherited event");
    explorer::SearchWindowContext output;output.query=L"preserved output";DWORD flags=0;
    require(FAILED(explorer::consumeSearchWindowContext(reinterpret_cast<ULONG_PTR>(event.value),&output))&&output.query==L"preserved output"&&
            GetHandleInformation(event.value,&flags),"invalid CLI handle was closed or changed output");
    Handle writable{CreateFileMappingW(INVALID_HANDLE_VALUE,&security,PAGE_READWRITE,0,static_cast<DWORD>(packet.size()),nullptr)};
    require(writable.value!=nullptr,"create owned writable mapping negative");
    auto view=MapViewOfFile(writable.value,FILE_MAP_WRITE,0,0,packet.size());require(view!=nullptr,"map owned writable negative");
    std::memcpy(view,packet.data(),packet.size());UnmapViewOfFile(view);
    require(explorer::consumeSearchWindowContext(reinterpret_cast<ULONG_PTR>(writable.value),&output)==E_ACCESSDENIED&&
            output.query==L"preserved output"&&GetHandleInformation(writable.value,&flags),"writable mapping was consumed as a read-only handoff");
    explorer::SearchWindowMapping mapping;
    original.query=L"System.FileName:=\"unchanged\"";
    succeeded(explorer::SearchWindowMapping::create(original,&mapping),"create valid mapping for atomic negative controls");
    const auto preservedHandle=mapping.handle();
    auto invalidContext=original;invalidContext.rules={{scope,true,true}};
    require(explorer::SearchWindowMapping::create(invalidContext,&mapping)==E_INVALIDARG&&mapping.handle()==preservedHandle,
            "failed mapping creation replaced the caller's original handle");
    HANDLE nonInherited=nullptr;
    require(DuplicateHandle(GetCurrentProcess(),mapping.handle(),GetCurrentProcess(),&nonInherited,0,FALSE,DUPLICATE_SAME_ACCESS),
            "create owned non-inherited read mapping control");
    Handle retained{nonInherited};
    require(explorer::consumeSearchWindowContext(reinterpret_cast<ULONG_PTR>(nonInherited),&output)==E_INVALIDARG&&
            output.query==L"preserved output"&&GetHandleInformation(nonInherited,&flags),
            "non-inherited handle was consumed or changed public output");
}

constexpr wchar_t excludedHandleVariable[]=L"EXPLORER_SEARCH_WINDOW_TEST_EXCLUDED_HANDLE";
constexpr wchar_t excludedEventVariable[]=L"EXPLORER_SEARCH_WINDOW_TEST_EXCLUDED_EVENT";
using CompareHandles=BOOL(WINAPI*)(HANDLE,HANDLE);
CompareHandles objectComparator() {
    const auto module=GetModuleHandleW(L"kernelbase.dll");
    return module?reinterpret_cast<CompareHandles>(GetProcAddress(module,"CompareObjectHandles")):nullptr;
}
template<class Character> bool decimal(std::basic_string_view<Character> text,ULONG_PTR& result) {
    if(text.empty()||!std::all_of(text.begin(),text.end(),[](Character code){return code>='0'&&code<='9';}))return false;
    ULONG_PTR candidate=0;
    for(const auto code:text){const auto digit=static_cast<ULONG_PTR>(code-'0');
        if(candidate>((std::numeric_limits<ULONG_PTR>::max)()-digit)/10)return false;
        candidate=candidate*10+digit;}
    if(!candidate)return false;
    result=candidate;return true;
}
struct EnvironmentValue {
    const wchar_t* name;
    std::optional<std::wstring> original;
    explicit EnvironmentValue(const wchar_t* variable):name(variable) {
        const auto count=GetEnvironmentVariableW(name,nullptr,0);
        if(count){std::wstring value(count,L'\0');const auto read=GetEnvironmentVariableW(name,value.data(),count);
            require(read<count,"read existing process-only diagnostic environment");value.resize(read);original=std::move(value);}
        else if(GetLastError()!=ERROR_ENVVAR_NOT_FOUND)original=L"";
    }
    ~EnvironmentValue(){SetEnvironmentVariableW(name,original?original->c_str():nullptr);}
};
void explicitHandleListNativeChild() {
    Fixture fixture;const auto firstPath=fixture.root/L"first",secondPath=fixture.root/L"second";
    require(fs::create_directory(firstPath)&&fs::create_directory(secondPath),"create private child launch scopes");
    const auto firstFile=firstPath/L"target.txt",secondFile=secondPath/L"target.txt";
    for(const auto& path:{firstFile,secondFile}){std::ofstream file(path,std::ios::binary);file<<"owned child";require(file.good(),"write owned child launch member");}
    const std::array originalIds{identity(firstFile),identity(secondFile)};
    auto first=item(firstPath),second=item(secondPath);
    explorer::SearchWindowContext context;context.query=L"System.FileName:=\"target.txt\"";
    context.primaryScope=first;context.closeOrigin=second;context.scopes=array(first.Get(),second.Get());context.recursive=false;
    explorer::SearchViewPresentation presentation;presentation.mode=explorer::SearchViewMode::List;presentation.iconSize=16;
    context.presentation=presentation;explorer::SearchFileProperties properties;properties.author=L"資料 🚀";
    properties.description=L"exact\r\nchild\ttext";context.fileProperties=properties;
    const auto eventName=L"Local\\Explorer.SearchWindow.Excluded."+fixture.root.filename().wstring();
    SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};Handle excluded{CreateEventW(&security,FALSE,FALSE,eventName.c_str())};
    require(excluded.value!=nullptr&&GetLastError()!=ERROR_ALREADY_EXISTS,"create exclusively owned inheritable handle outside explicit list");
    Handle reference{OpenEventW(SYNCHRONIZE,FALSE,eventName.c_str())};
    const auto compare=objectComparator();
    require(compare&&reference.value&&compare(reference.value,excluded.value),"verify documented API and exact owned exclusion event identity");
    EnvironmentValue environment(excludedHandleVariable),eventEnvironment(excludedEventVariable);
    require(SetEnvironmentVariableW(excludedHandleVariable,std::to_wstring(reinterpret_cast<ULONG_PTR>(excluded.value)).c_str()),
            "set process-only child exclusion proof");
    require(SetEnvironmentVariableW(excludedEventVariable,eventName.c_str()),"set process-only actual event identity proof");
    std::wstring executable(32768,L'\0');const auto length=GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size()));
    require(length&&length<executable.size(),"read dedicated private test executable");executable.resize(length);
    Handle child;
    succeeded(explorer::launchSearchWindow(executable,context,&child.value),"launch actual explicit HANDLE_LIST private native child");
    require(child.value!=nullptr,"successful private launch did not return owned process handle");
    const auto wait=WaitForSingleObject(child.value,15000);
    if(wait!=WAIT_OBJECT_0){
        const bool stopped=TerminateProcess(child.value,4)!=FALSE;
        const auto reaped=WaitForSingleObject(child.value,5000);
        std::cout<<"native search-window child deadline stop="<<stopped<<" reap="<<reaped<<'\n';
        require(reaped==WAIT_OBJECT_0,"owned private child could not be reaped after its deadline");
    }
    require(wait==WAIT_OBJECT_0,"private inherited-context child exceeded bounded deadline");
    DWORD exitCode=STILL_ACTIVE;
    const bool exitRead=GetExitCodeProcess(child.value,&exitCode)!=FALSE;
    std::cout<<"native search-window child exit="<<exitCode<<" read="<<exitRead<<'\n';
    require(exitRead&&exitCode==0,"private child rejected inherited context/native FileIDs/handle isolation");
    HANDLE failedOutput=INVALID_HANDLE_VALUE;
    require(FAILED(explorer::launchSearchWindow((fixture.root/L"missing-native-child.exe").native(),context,&failedOutput))&&failedOutput==nullptr,
            "failed process launch published a process handle");
    require(identity(firstFile)==originalIds[0]&&identity(secondFile)==originalIds[1]&&
            contents(firstFile)=="owned child"&&contents(secondFile)=="owned child","private child altered owned source files");
}

int inheritedChildStage=10;
int inheritedChild(ULONG_PTR handleValue) {
    Handle identityHandle;
    require(DuplicateHandle(GetCurrentProcess(),reinterpret_cast<HANDLE>(handleValue),GetCurrentProcess(),&identityHandle.value,0,FALSE,DUPLICATE_SAME_ACCESS),
            "retain only a local kernel identity reference for closure proof");
    explorer::SearchWindowContext context;
    succeeded(explorer::consumeSearchWindowContext(handleValue,&context),"child consume exact production inherited mapping");
    DWORD flags=0;
    require(!GetHandleInformation(reinterpret_cast<HANDLE>(handleValue),&flags)&&GetLastError()==ERROR_INVALID_HANDLE,
            "private child did not close mapping immediately after successful validation");
    inheritedChildStage=11;
    require(context.query==L"System.FileName:=\"target.txt\""&&!context.recursive&&context.rules.empty()&&
            context.presentation&&context.presentation->mode==explorer::SearchViewMode::List&&context.presentation->iconSize==16&&
            context.fileProperties&&context.fileProperties->author==L"資料 🚀"&&
            context.fileProperties->description==L"exact\r\nchild\ttext","child lost complete transported literal or optional metadata");
    inheritedChildStage=12;
    DWORD count=0;succeeded(context.scopes->GetCount(&count),"child read every transferred native scope");
    require(count==2,"private native child lost one original scope");
    std::set<Identity> expected;
    for(DWORD index=0;index<count;++index){ComPtr<IShellItem> scope;succeeded(context.scopes->GetItemAt(index,&scope),"child read original scope identity");
        sameItem(scope.Get(),index?context.closeOrigin.Get():context.primaryScope.Get());
        PWSTR raw=nullptr;succeeded(scope->GetDisplayName(SIGDN_FILESYSPATH,&raw),"child resolve owned native scope");
        std::unique_ptr<wchar_t,TaskFree> path(raw);require(raw!=nullptr,"child scope lacks native filesystem path");
        expected.insert(identity(fs::path(raw)/L"target.txt"));}
    inheritedChildStage=13;
    require(expected.size()==2&&results(context)==expected,"actual inherited native child factory changed full scope FileIDs");
    inheritedChildStage=14;
    const auto consumed=reinterpret_cast<HANDLE>(handleValue);
    const bool numberReused=GetHandleInformation(consumed,&flags)!=FALSE;
    const auto compare=objectComparator();
    require(compare&&(!numberReused||!compare(consumed,identityHandle.value)),"native factory retained the consumed mapping object or public API unavailable");
    std::cout<<"PASS: private inherited native child scopes="<<count<<" identities="<<expected.size()<<'\n';return 0;
}
} // namespace
int runSearchWindowTests() {
    int failures=0;
    for(const auto& test:std::array<std::pair<const char*,void(*)()>,3>{{
        {"native complete search-window context and reduced mapping",nativeContextAndReadOnlyMapping},
        {"malformed search-window packets and strict handle ownership",malformedPacketsAndHandleOwnership},
        {"production explicit HANDLE_LIST private native child",explicitHandleListNativeChild}}}) {
        try{test.second();std::cout<<"PASS: "<<test.first<<'\n';}
        catch(const std::exception& error){++failures;std::cerr<<"FAIL: "<<test.first<<": "<<error.what()<<'\n';}
    }
    std::cout<<3-failures<<"/3 search-window handoff groups passed\n";return failures;
}

int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOOPENFILEERRORBOX|SEM_NOGPFAULTERRORBOX);
    std::cout.setf(std::ios::unitbuf);
    int argc=0;auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if(!argv)return 2;
    struct Arguments { wchar_t** value;~Arguments(){LocalFree(value);} } arguments{argv};
    ULONG_PTR handleValue=0;
    const bool child=argc==3&&std::wstring_view(argv[1])==L"--search-context-handle"&&decimal(std::wstring_view(argv[2]),handleValue);
    if(argc!=1&&!child)return 2;
    // Loader allocations can reuse an omitted handle number before wWinMain.
    // Compare the actual exclusively owned object rather than handle numbers.
    // These variables exist only for this test's child; normal launch has none.
    if(child){char text[32]{};const auto length=GetEnvironmentVariableA("EXPLORER_SEARCH_WINDOW_TEST_EXCLUDED_HANDLE",text,sizeof(text));
        ULONG_PTR excluded=0;DWORD flags=0;
        if(!length||length>=sizeof(text)||!decimal(std::string_view(text,length),excluded))return 30;
        const auto candidate=reinterpret_cast<HANDLE>(excluded);const bool candidateValid=GetHandleInformation(candidate,&flags)!=FALSE;
        wchar_t name[512]{};const auto nameLength=GetEnvironmentVariableW(excludedEventVariable,name,static_cast<DWORD>(std::size(name)));
        if(!nameLength||nameLength>=std::size(name))return 31;
        Handle reference{OpenEventW(SYNCHRONIZE,FALSE,name)};if(!reference.value)return 32;
        const auto compare=objectComparator();if(!compare)return 34;
        if(candidateValid&&compare(candidate,reference.value))return 33;}
    explorer::PrivateDesktop desktop;
    if(FAILED(desktop.initialize())||FAILED(desktop.verifyIsolation()))return 4;
    const auto clipboard=GetClipboardSequenceNumber();
    const auto initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if(FAILED(initialized))return 5;
    int failures=0;
    try{failures=child?inheritedChild(handleValue):runSearchWindowTests();}
    catch(const std::exception& error){failures=child?inheritedChildStage:failures+1;std::cerr<<"FAIL: private search-window test: "<<error.what()<<'\n';}
    bool inputUnchanged=false,visible=true;
    if(FAILED(desktop.verifyIsolation(&inputUnchanged))||!inputUnchanged||
       FAILED(desktop.visibleWindowsOnInputDesktop(visible))||visible||GetClipboardSequenceNumber()!=clipboard){
        ++failures;std::cerr<<"FAIL: private search-window desktop/window/clipboard isolation\n";}
    CoUninitialize();return failures;
}
