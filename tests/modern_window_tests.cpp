#include "explorer/modern_window.hpp"
#include "explorer/native_apartment.hpp"
#include "explorer/theme.hpp"
#include "explorer/headless_crash.hpp"
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
void succeeded(HRESULT result,const char* message) {
    if(FAILED(result)) {std::cerr<<message<<": 0x"<<std::hex<<static_cast<unsigned long>(result)<<std::dec<<'\n';throw std::runtime_error(message);}
}
bool pumpUntil(const std::function<bool()>& condition,DWORD timeout=20000) {
    const auto deadline=GetTickCount64()+timeout;
    do {
        MSG message{};
        while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
            require(message.message!=WM_QUIT,"An embedded browser posted process quit");
            TranslateMessage(&message);DispatchMessageW(&message);
        }
        if(condition())return true;
        MsgWaitForMultipleObjectsEx(0,nullptr,10,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
    }while(GetTickCount64()<deadline);
    return false;
}
bool at(explorer::ExplorerApp* app,const std::filesystem::path& path) {
    if(!app||app->navigationPending()||app->currentLocation().empty())return false;
    explorer::ComPtr<IShellItem> actual,expected;
    if(FAILED(SHCreateItemFromParsingName(app->currentLocation().c_str(),nullptr,IID_PPV_ARGS(&actual)))||
       FAILED(SHCreateItemFromParsingName(path.c_str(),nullptr,IID_PPV_ARGS(&expected))))return false;
    int order=1;return SUCCEEDED(actual->Compare(expected.Get(),SICHINT_CANONICAL,&order))&&order==0;
}
struct OwnedFixture {
    std::filesystem::path root;
    OwnedFixture() {
        wchar_t temporary[MAX_PATH]{};require(GetTempPathW(MAX_PATH,temporary)>0,"Get fixture temp path");
        GUID id{};succeeded(CoCreateGuid(&id),"Create fixture identity");wchar_t identity[40]{};
        require(StringFromGUID2(id,identity,40)>0,"Format fixture identity");
        root=std::filesystem::path(temporary)/(std::wstring(L"WindowsExplorer.ModernTabs-")+identity);
        require(std::filesystem::create_directory(root),"Exclusively create owned root");
        for(const auto name:{L"first",L"second",L"third"})std::filesystem::create_directory(root/name);
        std::ofstream(root/L"first"/L"one.txt")<<"one";
        std::ofstream(root/L"first"/L"two.txt")<<"two";
        std::ofstream(root/L"second"/L"three.txt")<<"three";
    }
    ~OwnedFixture() {if(!root.empty()){std::error_code error;std::filesystem::remove_all(root,error);}}
};
LRESULT CALLBACK closeOnShow(HWND window,UINT message,WPARAM wparam,LPARAM lparam,UINT_PTR id,DWORD_PTR data) {
    if(message==WM_SHOWWINDOW&&wparam) {
        RemoveWindowSubclass(window,closeOnShow,id);
        ++*reinterpret_cast<unsigned*>(data);
        SendMessageW(window,WM_CLOSE,0,0);
    }
    return DefSubclassProc(window,message,wparam,lparam);
}
struct NestedSelection {
    explorer::ModernExplorerWindow& frame;
    explorer::ModernExplorerWindow::TabId target;
    unsigned calls=0;
    HRESULT status=E_PENDING;
};
LRESULT CALLBACK selectOnHide(HWND window,UINT message,WPARAM wparam,LPARAM lparam,UINT_PTR id,DWORD_PTR data) {
    if(message==WM_SHOWWINDOW&&!wparam) {
        RemoveWindowSubclass(window,selectOnHide,id);
        auto& nested=*reinterpret_cast<NestedSelection*>(data);
        ++nested.calls;nested.status=nested.frame.selectTab(nested.target);
    }
    return DefSubclassProc(window,message,wparam,lparam);
}
}
int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOOPENFILEERRORBOX);
    explorer::PrivateDesktop desktop;
    if(FAILED(desktop.initialize()))return 2;
    // Preserve a native crash for headless diagnosis without a debugger UI.
    const auto crashFile=std::filesystem::current_path()/(L"modern-window-crash-"+std::to_wstring(GetCurrentProcessId())+L".dmp");
    succeeded(explorer::initializeHeadlessCrashDump(true,crashFile),"Configure owned crash diagnostic");
    explorer::NativeApartmentOwner apartment;
    if(FAILED(apartment.initializeOle()))return 2;
    int result=0;
    try {
        succeeded(explorer::initializeProcessTheme(),"Initialize process-local theme");
        INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_WIN95_CLASSES|ICC_STANDARD_CLASSES};
        require(InitCommonControlsEx(&controls)!=FALSE,"Initialize native controls");
        OwnedFixture fixture;
        explorer::ModernExplorerWindow frame(GetModuleHandleW(nullptr),true);
        succeeded(frame.create((fixture.root/L"first").native()),"Create modern native frame");
        // Visible only on the guarded non-input desktop, so native focus and
        // show/close callbacks are exercised without displaying the app.
        ShowWindow(frame.window(),SW_SHOWNOACTIVATE);SetActiveWindow(frame.window());
        require(pumpUntil([&]{return at(frame.activeBrowser(),fixture.root/L"first");}),"Initial real folder ready");
        require(frame.tabCount()==1&&frame.activeBrowser()->compactChrome(),"Initial compact browser");
        const auto firstId=frame.activeTab();auto first=frame.activeBrowser();
        explorer::ComPtr<explorer::ExplorerApp> retainedFirst=first;
        succeeded(first->execute(explorer::SelectAll),"Select first tab's actual files");
        require(pumpUntil([&]{DWORD count=0;return first->headlessSelectionCount(&count)==S_OK&&count==2;}),"First tab exact selected count");
        succeeded(first->execute(explorer::SelectNone),"Clear first tab selection through its native view");
        require(pumpUntil([&]{DWORD count=0;return first->headlessSelectionCount(&count)==S_OK&&count==0;}),"First tab selection cleared");
        succeeded(first->execute(explorer::Invert),"Invert empty first tab selection through its native view");
        require(pumpUntil([&]{DWORD count=0;return first->headlessSelectionCount(&count)==S_OK&&count==2;}),"First tab inversion selects its exact complement");
        succeeded(first->navigate((fixture.root/L"third").native()),"Navigate first tab");
        require(pumpUntil([&]{return at(first,fixture.root/L"third");}),"First tab second location");
        succeeded(first->execute(explorer::Back),"First tab back history");
        require(pumpUntil([&]{return at(first,fixture.root/L"first");}),"First tab history restored");
        succeeded(first->execute(explorer::SelectAll),"Restore first tab selection");
        explorer::ModernExplorerWindow::TabId secondId=0;
        succeeded(frame.addTab((fixture.root/L"second").native(),&secondId),"Add independent second browser");
        auto second=frame.activeBrowser();
        require(second!=first&&secondId!=firstId&&frame.tabCount()==2,"Distinct real browser owners");
        require(pumpUntil([&]{return at(second,fixture.root/L"second");}),"Second tab actual folder");
        succeeded(second->execute(explorer::SelectAll),"Select second tab file");
        require(pumpUntil([&]{DWORD count=0;return second->headlessSelectionCount(&count)==S_OK&&count==1;}),"Second tab actual selected count");
        succeeded(frame.selectTab(firstId),"Switch back to retained first browser");
        require(frame.activeBrowser()==first&&at(first,fixture.root/L"first"),"First tab location preserved");
        require(pumpUntil([&]{DWORD count=0;return first->headlessSelectionCount(&count)==S_OK&&count==2;}),"First tab selection preserved");
        succeeded(first->execute(explorer::Forward),"Independent first tab forward history");
        require(pumpUntil([&]{return at(first,fixture.root/L"third");}),"First tab forward reaches its own destination");
        succeeded(frame.selectTab(secondId),"Activate second browser");
        require(at(second,fixture.root/L"second"),"Second tab unaffected by first history");
        require(second->execute(explorer::Back)==S_FALSE,"Second tab did not inherit first history");
        explorer::ModernExplorerWindow::TabId thirdId=0;
        succeeded(frame.addTab((fixture.root/L"third").native(),&thirdId),"Create nested activation target");
        const explorer::ComPtr<explorer::ExplorerApp> third=frame.activeBrowser();
        require(pumpUntil([&]{return at(third.Get(),fixture.root/L"third");}),"Nested activation target ready");
        succeeded(frame.selectTab(firstId),"Activate source for native hide callback");
        NestedSelection nested{frame,thirdId};
        require(SetWindowSubclass(first->window(),selectOnHide,43,reinterpret_cast<DWORD_PTR>(&nested))!=FALSE,"Attach owned nested selection fixture");
        succeeded(frame.selectTab(secondId),"Native hide callback supersedes outer activation");
        require(nested.calls==1&&SUCCEEDED(nested.status)&&frame.activeTab()==thirdId&&frame.activeBrowser()==third.Get(),"Newest reentrant activation owns commands and selected tab");
        require(IsWindowVisible(third->window())&&!IsWindowVisible(first->window())&&!IsWindowVisible(second->window()),"Only the newest selected browser remains visible on the private desktop");
        succeeded(frame.closeTab(thirdId),"Retire nested activation target");
        require(pumpUntil([&]{return frame.tabCount()==2;}),"Nested activation target fully retired");
        succeeded(frame.selectTab(secondId),"Restore second browser after nested activation");
        const auto search=GetDlgItem(second->window(),explorer::Search);
        require(search!=nullptr,"Second tab search control exists");SetFocus(search);
        require(GetFocus()==search,"Focus second tab search");
        succeeded(frame.closeTab(firstId),"Close inactive first tab");
        require(pumpUntil([&]{return frame.tabCount()==1;}),"Inactive tab fully destroyed");
        require(!retainedFirst->window()&&frame.activeBrowser()==second,"Close only destroys requested browser");
        require(GetFocus()==search,"Closing inactive tab preserves live search focus");
        succeeded(frame.addTab((fixture.root/L"first").native(),&secondId),"Create tab after browser retirement");
        require(pumpUntil([&]{return at(frame.activeBrowser(),fixture.root/L"first");}),"Replacement tab ready");
        unsigned showCloseCalls=0;
        const auto remaining=frame.tabIds();
        require(remaining.size()==2,"Two tabs before reentrant activation close");
        require(SetWindowSubclass(second->window(),closeOnShow,42,reinterpret_cast<DWORD_PTR>(&showCloseCalls))!=FALSE,"Attach owned reentrant close fixture");
        succeeded(frame.selectTab(remaining.front()),"Activate tab that closes during native show");
        require(pumpUntil([&]{return frame.tabCount()==1;}),"Reentrant close retires only its browser");
        require(showCloseCalls==1&&frame.activeTab()==secondId,"Activation close preserves remaining tab");
        succeeded(frame.shutdown(),"Close all actual browser sessions");
        require(!frame.window()&&frame.tabCount()==0,"Frame and child windows retired");
        retainedFirst.Reset();
        succeeded(desktop.verifyIsolation(),"Input desktop unchanged");
        bool visible=false;succeeded(desktop.visibleWindowsOnInputDesktop(visible),"Read input-desktop ownership");
        require(!visible,"No owned windows appeared on input desktop");
        std::cout<<"PASS: compact frame, independent native tab locations/history/selection, reentrant activation, inactive close and full teardown\n";
    }catch(const std::exception& error) {std::cerr<<"FAIL: "<<error.what()<<'\n';result=1;}
    apartment.finishOrTerminate();
    if(FAILED(desktop.verifyIsolation()))result=1;
    return result;
}
