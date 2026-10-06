#include "explorer/native_apartment.hpp"
#include "explorer/headless_visual.hpp"
#include "explorer/ribbon.hpp"

#include <commctrl.h>
#include <oleacc.h>
#include <uiautomation.h>
#include <wrl/client.h>
#include <array>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {
using Microsoft::WRL::ComPtr;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void succeeded(HRESULT result, const char* message) {
    if (FAILED(result)) throw std::runtime_error(std::string(message) + " HRESULT=" + std::to_string(static_cast<ULONG>(result)));
}
struct Window {
    HWND handle = nullptr;
    ~Window() { if (handle) DestroyWindow(handle); }
};
struct Variant {
    VARIANT value{};
    ~Variant() { VariantClear(&value); }
};
struct Apartment {
    HRESULT result = E_ACCESSDENIED;
    explicit Apartment(HDESK desktop) {
        if (SetThreadDesktop(desktop)) result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    }
    ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};
std::wstring accessibleName(IUIAutomationElement* element) {
    BSTR text = nullptr;
    succeeded(element->get_CurrentName(&text), "Read owned accessibility name");
    std::wstring result(text ? text : L"");
    SysFreeString(text);
    return result;
}
std::vector<ComPtr<IUIAutomationElement>> ownedElements(IUIAutomation* automation, DWORD uiThread) {
    std::vector<HWND> windows;
    EnumThreadWindows(uiThread, [](HWND window, LPARAM context) -> BOOL {
        DWORD process = 0;
        GetWindowThreadProcessId(window, &process);
        if (process == GetCurrentProcessId() && IsWindowVisible(window))
            reinterpret_cast<std::vector<HWND>*>(context)->push_back(window);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&windows));
    ComPtr<IUIAutomationCondition> all;
    succeeded(automation->CreateTrueCondition(&all), "Create owned accessibility condition");
    std::vector<ComPtr<IUIAutomationElement>> result;
    for (const auto window : windows) {
        ComPtr<IUIAutomationElement> root;
        ComPtr<IUIAutomationElementArray> children;
        succeeded(automation->ElementFromHandle(window, &root), "Read private window accessibility root");
        succeeded(root->FindAll(TreeScope_Descendants, all.Get(), &children), "Read owned accessibility descendants");
        int count = 0;
        succeeded(children->get_Length(&count), "Read owned accessibility count");
        require(count >= 0 && count <= 1024, "Owned accessibility tree exceeded bound");
        for (int index = 0; index < count; ++index) {
            ComPtr<IUIAutomationElement> item;
            succeeded(children->GetElement(index, &item), "Read owned accessibility descendant");
            int process = 0;
            succeeded(item->get_CurrentProcessId(&process), "Read accessibility process ownership");
            require(process == static_cast<int>(GetCurrentProcessId()), "Accessibility descendant escaped owned process");
            result.push_back(std::move(item));
        }
    }
    return result;
}
ComPtr<IUIAutomationElement> findOwned(IUIAutomation* automation, DWORD uiThread, std::wstring_view name) {
    for (const auto& element : ownedElements(automation, uiThread))
        if (accessibleName(element.Get()) == name) return element;
    return {};
}
bool pressed(IAccessible* button) {
    Variant identity, state;
    identity.value.vt = VT_I4;
    identity.value.lVal = CHILDID_SELF;
    succeeded(button->get_accState(identity.value, &state.value), "Read native pin state");
    require(state.value.vt == VT_I4, "Native pin state has unexpected type");
    require(!(state.value.lVal & STATE_SYSTEM_UNAVAILABLE), "Owned native pin is unavailable");
    return (state.value.lVal & STATE_SYSTEM_PRESSED) != 0;
}
struct ActionResult {
    HRESULT result = E_FAIL;
    bool opened = false;
    unsigned presses = 0;
    bool finalPressed = false;
    bool secondFinalPressed = false;
};

// Public accessibility acts only on our framework's own mock list. There is no
// Shell identity, native command provider invocation, input injection, synthetic
// mouse/key message, clipboard access, or global pin/history/settings mutation.
ActionResult openAndPress(HDESK desktop, DWORD uiThread, bool initialPin, unsigned count, bool bothRows) {
    ActionResult result;
    try {
        Apartment apartment(desktop);
        succeeded(apartment.result, "Initialize private accessibility MTA");
        ComPtr<IUIAutomation> automation;
        succeeded(CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&automation)), "Create windowless accessibility provider");
        ComPtr<IUIAutomation2> settings;
        succeeded(automation.As(&settings), "Read accessibility timeout interface");
        succeeded(settings->put_AutoSetFocus(FALSE), "Disable accessibility focus changes");
        succeeded(settings->put_ConnectionTimeout(3000), "Bound accessibility connection");
        succeeded(settings->put_TransactionTimeout(3000), "Bound accessibility transaction");
        auto file = findOwned(automation.Get(), uiThread, L"File tab");
        require(file != nullptr, "Owned native File tab is missing");
        ComPtr<IUIAutomationInvokePattern> invoke;
        succeeded(file->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&invoke)), "Read File tab invocation pattern");
        require(invoke != nullptr, "File tab has no native invocation pattern");
        succeeded(invoke->Invoke(), "Open owned native File menu");
        result.opened = true;

        const auto pressRow=[&](std::wstring_view name) {
            auto row = findOwned(automation.Get(), uiThread, name);
            const auto rowDeadline = GetTickCount64()+2000;
            while(!row&&GetTickCount64()<rowDeadline) {
                // Opening the menu returns before its asynchronous RecentItems
                // source publishes accessible rows. Observe the exact owned row;
                // do not invoke the menu or its default action a second time.
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                row = findOwned(automation.Get(),uiThread,name);
            }
            require(row != nullptr, "Owned native RecentItems row is missing");
            ComPtr<IUIAutomationLegacyIAccessiblePattern> legacy;
            succeeded(row->GetCurrentPatternAs(UIA_LegacyIAccessiblePatternId, IID_PPV_ARGS(&legacy)),
                      "Read native RecentItems accessibility interface");
            require(legacy != nullptr, "Owned RecentItems row has no native legacy interface");
            ComPtr<IAccessible> accessible;
            succeeded(legacy->GetIAccessible(&accessible), "Read native RecentItems MSAA object");
            require(accessible != nullptr, "Owned native MSAA row is absent");
            LONG children = 0;
            succeeded(accessible->get_accChildCount(&children), "Read owned native pin child count");
            require(children == 1, "Owned native pin structure differs");
            Variant child, self, role;
            child.value.vt = VT_I4;
            child.value.lVal = 1;
            self.value.vt = VT_I4;
            self.value.lVal = CHILDID_SELF;
            ComPtr<IDispatch> raw;
            ComPtr<IAccessible> pin;
            succeeded(accessible->get_accChild(child.value, &raw), "Read owned native pin child");
            require(raw != nullptr, "Owned native pin child object is absent");
            succeeded(raw.As(&pin), "Read owned native pin accessibility object");
            succeeded(pin->get_accRole(self.value, &role.value), "Read owned native pin role");
            require(role.value.vt == VT_I4 && role.value.lVal == ROLE_SYSTEM_PUSHBUTTON,
                    "Native recent-item pin is not its expected push button");
            require(pressed(pin.Get()) == initialPin, "Native initial pin differs from supplied mock");
            bool expected = initialPin;
            for (unsigned index = 0; index < count; ++index) {
                succeeded(pin->accDoDefaultAction(self.value), "Press owned native pin through public MSAA");
                ++result.presses;
                expected = !expected;
                const auto end = GetTickCount64() + 3000;
                while (pressed(pin.Get()) != expected && GetTickCount64() < end)
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                require(pressed(pin.Get()) == expected, "Native pin action did not change its actual pressed state");
            }
            return pressed(pin.Get());
        };
        result.finalPressed=pressRow(L"Owned Recent One");
        if(bothRows)result.secondFinalPressed=pressRow(L"Owned Recent Two");
        result.result = S_OK;
    } catch (const std::exception& error) {
        std::cerr << "Owned accessibility action failed: " << error.what() << '\n';
    }
    return result;
}
void pumpUntil(std::future<ActionResult>& task) {
    const auto end = GetTickCount64() + 15000;
    while (task.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        require(GetTickCount64() < end, "Owned accessibility task exceeded bound");
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        MsgWaitForMultipleObjectsEx(0, nullptr, 5, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
}
void runCase(explorer::PrivateDesktop& desktop, HINSTANCE instance, bool installed,
             bool initiallyPinned, unsigned presses, const char* description) {
    const auto uiThread = GetCurrentThreadId();
    struct Model {
        std::array<bool, 2> pins;
        unsigned callbacks = 0, unrelated = 0, sourceReads = 0;
        bool invalidIndexOrThread = false;
        std::vector<UINT> changedIndices;
    } model{{initiallyPinned, true}};
    Window host{CreateWindowExW(0, L"WindowsExplorerOwnedRecentItemsRegression",
        L"Owned RecentItems regression", WS_OVERLAPPEDWINDOW, 0, 0, 900, 600,
        nullptr, nullptr, instance, nullptr)};
    require(host.handle != nullptr, "Create owned private Ribbon host");
    explorer::NativeRibbon ribbon;
    explorer::RibbonCallbacks callbacks;
    callbacks.query = [](UINT) { return explorer::RibbonCommandState{}; };
    callbacks.execute = [&](UINT) { ++model.unrelated; return S_OK; };
    callbacks.executeItem = [&](UINT, UINT) { ++model.unrelated; return S_OK; };
    callbacks.items = [&](UINT command) {
        if (command != explorer::RibbonFrequentPlaces) return std::vector<explorer::RibbonItem>{};
        ++model.sourceReads;
        return std::vector<explorer::RibbonItem>{
            {0, L"Owned Recent One", model.pins[0], {}, L"No Shell identity or operation"},
            {1, L"Owned Recent Two", model.pins[1], {}, L"No Shell identity or operation"}};
    };
    callbacks.pinItem = [&](UINT index, bool value) {
        ++model.callbacks;
        if (GetCurrentThreadId() != uiThread || index >= model.pins.size()) {
            model.invalidIndexOrThread = true;
            return E_INVALIDARG;
        }
        model.changedIndices.push_back(index);
        model.pins[index] = value;
        return S_OK;
    };
    succeeded(ribbon.initialize(host.handle, instance, std::move(callbacks), installed
        ? explorer::RibbonLayout::InstalledWindows10 : explorer::RibbonLayout::Authored),
        "Initialize actual NativeRibbon recent-item host");
    if (installed) require(ribbon.layout() == explorer::RibbonLayout::InstalledWindows10,
                           "Installed stock Ribbon was unavailable for this requested regression");
    ShowWindow(host.handle, SW_SHOWNOACTIVATE);
    UpdateWindow(host.handle);
    auto action = std::async(std::launch::async, openAndPress,
                            GetThreadDesktop(uiThread), uiThread, initiallyPinned, presses, false);
    pumpUntil(action);
    const auto native = action.get();
    succeeded(native.result, "Execute actual owned native pin action");
    require(native.opened && native.presses == presses, "Native pin action count differs");
    const bool expected = initiallyPinned != ((presses & 1u) != 0);
    require(native.finalPressed == expected, "Native final pressed state differs");
    require(model.sourceReads > 0, "Native framework never read the mock RecentItems source");
    // Windows sends the complete padded pin array when the menu closes. Normal
    // framework teardown delivers that real callback while the mock remains alive.
    ribbon.reset();
    require(!model.invalidIndexOrThread, "Native padding invoked an unrelated row or thread");
    require(model.unrelated == 0, "Native pin action invoked a folder or other command");
    require(model.pins[0] == expected && model.pins[1], "Native callback changed the wrong mock pins");
    const unsigned changed = presses & 1u;
    require(model.callbacks == changed, "Native callback replayed unchanged or padding rows");
    require(model.changedIndices == (changed ? std::vector<UINT>{0} : std::vector<UINT>{}),
            "Native callback index did not preserve displayed source identity");
    bool visible = true, isolated = false;
    succeeded(desktop.verifyIsolation(&isolated), "Read private desktop isolation after pin callback");
    succeeded(desktop.visibleWindowsOnInputDesktop(visible), "Read input desktop visibility after pin callback");
    require(isolated && !visible, "Native recent-item test exposed input-desktop UI");
    std::cout << "PASS: " << description << " (" << (installed ? "installed" : "authored")
              << "; callbacks=" << model.callbacks << ")\n";
}
void runShutdownReentry(explorer::PrivateDesktop& desktop,HINSTANCE instance,bool installed,bool initializeEntry) {
    const auto uiThread=GetCurrentThreadId();
    std::array<bool,2> pins{false,false};
    unsigned callbacks=0,unrelated=0,reads=0,reentered=0;
    bool invalid=false;
    Window host{CreateWindowExW(0,L"WindowsExplorerOwnedRecentItemsRegression",L"Owned shutdown RecentItems reentry",
        WS_OVERLAPPEDWINDOW,0,0,900,600,nullptr,nullptr,instance,nullptr)};
    require(host.handle!=nullptr,"Create owned private shutdown reentry host");
    explorer::NativeRibbon ribbon;
    const auto layout=installed?explorer::RibbonLayout::InstalledWindows10:explorer::RibbonLayout::Authored;
    explorer::RibbonCallbacks handlers;
    handlers.query=[](UINT){return explorer::RibbonCommandState{};};
    handlers.execute=[&](UINT){++unrelated;return S_OK;};
    handlers.executeItem=[&](UINT,UINT){++unrelated;return S_OK;};
    handlers.items=[&](UINT command) {
        if(command!=explorer::RibbonFrequentPlaces)return std::vector<explorer::RibbonItem>{};
        ++reads;
        return std::vector<explorer::RibbonItem>{{0,L"Owned Recent One",pins[0],{},L"Owned shutdown model"},
                                               {1,L"Owned Recent Two",pins[1],{},L"Owned shutdown model"}};
    };
    handlers.pinItem=[&](UINT index,bool value) {
        ++callbacks;
        if(GetCurrentThreadId()!=uiThread||index!=0||!value||callbacks!=1){invalid=true;return E_INVALIDARG;}
        pins[0]=value;++reentered;
        if(initializeEntry) {
            // A genuine initialize entry, deliberately rejected before COM,
            // must still revoke the old Destroy callback's remaining writes.
            explorer::RibbonCallbacks obsolete;
            if(ribbon.initialize(nullptr,instance,std::move(obsolete),layout)!=E_INVALIDARG)invalid=true;
        } else ribbon.reset(); // Real nested reset while the old Impl is retired.
        return S_OK;
    };
    succeeded(ribbon.initialize(host.handle,instance,std::move(handlers),layout),"Initialize actual shutdown reentry Ribbon");
    require(ribbon.layout()==layout,"Shutdown reentry Ribbon silently changed requested layout");
    if(installed)require(ribbon.installedLayoutStatus()==S_OK,"Installed shutdown reentry Ribbon failed without fallback");
    ShowWindow(host.handle,SW_SHOWNOACTIVATE);UpdateWindow(host.handle);
    auto action=std::async(std::launch::async,openAndPress,GetThreadDesktop(uiThread),uiThread,false,1,true);
    pumpUntil(action);const auto native=action.get();
    succeeded(native.result,"Execute actual two-row owned native pin actions");
    require(native.opened&&native.presses==2&&native.finalPressed&&native.secondFinalPressed,
            "Actual native rows did not both change before shutdown");
    require(reads>0&&callbacks==0&&!pins[0]&&!pins[1],"Native pin array arrived before the actual Destroy boundary");
    // No manufactured Execute/value: Destroy delivers the framework's actual
    // complete padded RecentItems array from those two real MSAA pin actions.
    ribbon.reset();
    require(!invalid&&unrelated==0&&callbacks==1&&reentered==1&&pins[0]&&!pins[1],
            "Old Destroy callback continued after a newer reset/initialize entry");
    require(!ribbon.valid(),"Shutdown reentry left a retired Ribbon published");
    ribbon.reset();require(callbacks==1,"Completed retired transaction replayed on later reset");
    bool isolated=false,visible=true;
    succeeded(desktop.verifyIsolation(&isolated),"Private desktop unchanged after shutdown reentry");
    succeeded(desktop.visibleWindowsOnInputDesktop(visible),"Read input visibility after shutdown reentry");
    require(isolated&&!visible,"Shutdown reentry exposed input-desktop UI");
    std::cout<<"PASS: actual native shutdown RecentItems "<<(initializeEntry?"initialize-entry":"nested-reset")
        <<" revokes remaining original pins ("<<(installed?"installed":"authored")<<")\n";
}

}

int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    explorer::PrivateDesktop desktop;
    if (FAILED(desktop.initialize())) return 2;
    explorer::NativeApartmentOwner nativeApartment;
    const auto initialized = nativeApartment.initializeOle();
    if (FAILED(initialized)) return 3;
    int result = 0;
    try {
        const bool installed = argc > 1 && std::string_view(argv[1]) == "--installed";
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES};
        require(InitCommonControlsEx(&controls) != FALSE, "Initialize private common controls");
        WNDCLASSW cls{};
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpfnWndProc = DefWindowProcW;
        cls.lpszClassName = L"WindowsExplorerOwnedRecentItemsRegression";
        require(RegisterClassW(&cls) != 0, "Register owned private Ribbon regression host");
        runCase(desktop, cls.hInstance, installed, false, 0, "unchanged short list ignores native padding");
        runCase(desktop, cls.hInstance, installed, false, 1, "native pin updates exactly its owned row");
        runCase(desktop, cls.hInstance, installed, true, 1, "native unpin preserves the other owned pin");
        runCase(desktop, cls.hInstance, installed, false, 2, "two native presses preserve original source state");
        runShutdownReentry(desktop,cls.hInstance,installed,false);
        runShutdownReentry(desktop,cls.hInstance,installed,true);
        require(UnregisterClassW(cls.lpszClassName, cls.hInstance) != FALSE, "Release owned regression class");
        std::cout << "4/4 original native RecentItems cases and 2/2 actual shutdown reentry cases passed\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        result = 1;
    }
    nativeApartment.finishOrTerminate();
    return result;
}
