#pragma once
#include "explorer/headless_visual.hpp"
#include "explorer/ribbon.hpp"

#include <commctrl.h>
#include <oleacc.h>
#include <uiautomation.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <memory>
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
thread_local ULONGLONG ownedPinDeadline = 0;
thread_local HWND ownedPinHost = nullptr;
void checkOwnedPinDeadline() {
    if(ownedPinDeadline&&GetTickCount64()>=ownedPinDeadline)
        throw std::runtime_error("Owned native pin fixture exceeded its absolute deadline");
}
void require(bool condition, const char* message) {
    checkOwnedPinDeadline();
    if (!condition) throw std::runtime_error(message);
}
void succeeded(HRESULT result, const char* message) {
    checkOwnedPinDeadline();
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
    const auto result=element->get_CurrentName(&text);
    struct Text {BSTR value;~Text(){SysFreeString(value);}} owned{text};
    succeeded(result, "Read owned accessibility name");return std::wstring(text?text:L"");
}
std::vector<ComPtr<IUIAutomationElement>> ownedElements(IUIAutomation* automation, DWORD uiThread) {
    std::vector<HWND> windows;
    EnumThreadWindows(uiThread, [](HWND window, LPARAM context) -> BOOL {
        DWORD process = 0;
        GetWindowThreadProcessId(window, &process);
        if(process==GetCurrentProcessId()&&IsWindowVisible(window)&&
           (window==ownedPinHost||GetAncestor(window,GA_ROOTOWNER)==ownedPinHost))
            reinterpret_cast<std::vector<HWND>*>(context)->push_back(window);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&windows));
    checkOwnedPinDeadline();
    ComPtr<IUIAutomationCondition> all;
    succeeded(automation->CreateTrueCondition(&all), "Create owned accessibility condition");
    require(all!=nullptr,"Owned accessibility condition is absent");
    std::vector<ComPtr<IUIAutomationElement>> result;
    for (const auto window : windows) {
        ComPtr<IUIAutomationElement> root;
        ComPtr<IUIAutomationElementArray> children;
        succeeded(automation->ElementFromHandle(window, &root), "Read private window accessibility root");
        require(root!=nullptr,"Owned accessibility root is absent");
        succeeded(root->FindAll(TreeScope_Descendants, all.Get(), &children), "Read owned accessibility descendants");
        require(children!=nullptr,"Owned accessibility array is absent");
        int count = 0;
        succeeded(children->get_Length(&count), "Read owned accessibility count");
        require(count >= 0 && count <= 1024, "Owned accessibility tree exceeded bound");
        for (int index = 0; index < count; ++index) {
            ComPtr<IUIAutomationElement> item;
            succeeded(children->GetElement(index, &item), "Read owned accessibility descendant");
            require(item!=nullptr,"Owned accessibility descendant is absent");
            int process = 0;
            succeeded(item->get_CurrentProcessId(&process), "Read accessibility process ownership");
            require(process == static_cast<int>(GetCurrentProcessId()), "Accessibility descendant escaped owned process");
            result.push_back(std::move(item));
        }
    }
    return result;
}
ComPtr<IUIAutomationElement> findOwned(IUIAutomation* automation, DWORD uiThread, std::wstring_view name) {
    ComPtr<IUIAutomationElement> found;
    for (const auto& element : ownedElements(automation, uiThread))if(accessibleName(element.Get())==name) {
        require(!found,"Owned accessibility caption is ambiguous");found=element;
    }
    return found;
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
struct ActionHandshake {std::atomic<bool> ready=false,admitted=false,cancelled=false;};
struct ActionResult {
    HRESULT result = E_FAIL;
    bool opened = false;
    unsigned presses = 0;
    bool finalPressed = false;
    bool secondFinalPressed = false;
};

// Same public native accessibility route, exact label from genuine App source.
// Hosted admission, native source identity and profile cleanup are caller-owned.
ActionResult openAndPress(HDESK desktop, DWORD uiThread, bool initialPin, unsigned count,
    std::wstring rowName,HWND host,ULONGLONG hardDeadline,
    std::shared_ptr<ActionHandshake> handshake) {
    ownedPinDeadline=hardDeadline;ownedPinHost=host;
    ActionResult result;
    try {
        DWORD process=0;
        require(host&&IsWindow(host)&&GetWindowThreadProcessId(host,&process)==uiThread&&process==GetCurrentProcessId(),
            "Accessibility host is not the original owned app window/thread");
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
            require(pressed(pin.Get()) == initialPin, "Native initial pin differs from original displayed source");
            handshake->ready=true;
            while(!handshake->admitted&&!handshake->cancelled) {
                checkOwnedPinDeadline();std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            require(!handshake->cancelled&&handshake->admitted,"Original native row admission was cancelled before pressing");
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
        result.finalPressed=pressRow(rowName);
        result.result = S_OK;
    } catch (const std::exception& error) {
        std::cerr << "Owned accessibility action failed: " << error.what() << '\n';
    }
    checkOwnedPinDeadline();
    return result;
}

}
