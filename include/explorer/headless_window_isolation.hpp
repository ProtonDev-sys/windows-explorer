#pragma once
#include <windows.h>
#include <array>
#include <cstdint>

namespace explorer {
class PrivateDesktop;

struct WindowMessageReceipt {
    LRESULT call=0;
    DWORD error=ERROR_SUCCESS;
    DWORD_PTR result=static_cast<DWORD_PTR>(0xA5B6C7D8E9FA1023ULL);
    UINT timeout=0;
    bool delivered() const noexcept;
};
struct WindowControlPacket {
    DWORD magic=0x50564D36,version=1,stage=0,different=0;
    DWORD parentProcess=0,process=0,thread=0;
    ULONG_PTR window=0;
    HRESULT status=E_PENDING;
    DWORD observedBeforeLocal=0,nullCount=0;
    WindowMessageReceipt local;
    std::array<wchar_t,256> desktop{};
    std::array<wchar_t,128> type{};
};
struct WindowKernelIdentity {
    DWORD process=0,thread=0,threadProcess=0,processWait=WAIT_FAILED,threadWait=WAIT_FAILED;
    bool exact=false;
};
struct WindowControlReadback {
    WindowControlPacket initial{},final{};
    WindowMessageReceipt message{};
    DWORD readyProcess=0,readyThread=0,readyThreadError=0,readyClassError=0;
    DWORD finalProcess=0,finalThread=0,finalThreadError=0,finalClassError=0;
    int readyClassRead=0,finalClassRead=0;
    HRESULT termination=E_PENDING,drain=E_PENDING,exitRead=E_PENDING;
    DWORD exitCode=STILL_ACTIVE;
    DWORD createdProcess=0,createdThread=0,kernelCount=0;
    std::array<WindowKernelIdentity,5> kernel{};
    bool initialExact=false,finalExact=false,expectedDelivery=false,kernelExited=false;
};
struct WindowIsolationStep {
    // Stable FNV-1a code of a SOURCE-CONSTANT protocol stage, not provider text.
    DWORD code=0;
    HRESULT status=E_PENDING;
    std::uint64_t fact=0;
    ULONGLONG elapsedMilliseconds=0;
};
struct WindowIsolationControlReport {
    HRESULT result=E_PENDING;
    DWORD creatorProcess=0,creatorThread=0;
    HDESK borrowedDesktop=nullptr; // Never close this handle.
    std::array<wchar_t,256> desktop{};
    std::uint64_t calibration=0;
    bool calibrated=false,overflow=false;
    DWORD stepCount=0;
    std::array<WindowIsolationStep,96> steps{};
    std::array<WindowControlReadback,2> controls{};
};

// Headless ONLY, before accessing foreign native renderer content or pixels.
// Two retained directly-created processes: one actual SAME private desktop,
// one DIFFERENT freshly-created owned private desktop. Each self-reports exact
// live native identity and two local WM_NULL counter controls. The parent sends
// exactly one WM_NULL per child, requires positive delta1 / negative delta0,
// kept-live final identity, then actual clean kernel exit. No input switching,
// global input, arbitrary window enumeration, COM, registry or shared-server
// termination. Original per-child4500ms / ready2000ms / final1000ms / drain
// 1000ms+1000ms bounds are retained. Failed final owned-child drain terminates
// only the current test process before private-desktop stack unwinding.
HRESULT runPrivateDesktopMessageControls(const PrivateDesktop& desktop,
    WindowIsolationControlReport* report) noexcept;

// Call FIRST in the WIN32 entry point, before COM/theme/common-controls/HWND.
// Recognizes only argv[1]=="--headless-window-isolation-control"; a recognized
// malformed mode is consumed with exit2. Exact argc8: mode, same|different,
// decimal parentPID, pipe, stop, release, expected private desktop name.
bool runHeadlessWindowControl(int argc,wchar_t* const* argv,int* exitCode) noexcept;

enum class PrivateWindowAdmission { None, ExactDesktopQuery, MessageChannelInference };
struct PrivateWindowSnapshot {
    HWND window=nullptr,parent=nullptr,root=nullptr;
    DWORD process=0,thread=0,threadError=0;
    int classRead=0;
    DWORD classError=0;
    RECT rectangle{};
    HRESULT geometry=E_PENDING,desktopRead=E_PENDING;
    HDESK borrowedDesktop=nullptr;
    DWORD desktopError=0;
    std::array<wchar_t,128> type{};
    std::array<wchar_t,256> desktop{};
};
struct PrivateWindowAdmissionReport {
    HRESULT result=E_PENDING,guardBefore=E_PENDING,guardAfter=E_PENDING;
    PrivateWindowAdmission admission=PrivateWindowAdmission::None;
    PrivateWindowSnapshot parentBefore{},parentAfter{},rootBefore{},rootAfter{};
    PrivateWindowSnapshot targetBefore{},targetAfter{};
    WindowMessageReceipt message{};
    bool ancestryBefore=false,ancestryAfter=false,exactSnapshot=false;
};

// ONE exact foreign descendant only, under the caller's original deadline.
// Both controls must be freshly calibrated on this thread/current connection.
// Native HWND/PID/TID/class/parent/root/geometry and private/input guards bracket
// the one finite WM_NULL. A known queried desktop mismatch always denies.
// Successful channel delivery is explicitly an INFERENCE, not an HDESK/name
// readback. The caller retains independent view/source/selection/epoch fences
// and must verify them before and after this call, before UIA/pixel admission.
// SendMessageTimeout ignores timeout for a shared input queue; the existing
// external owned-test-process watchdog remains the final hard bound.
// https://learn.microsoft.com/windows/win32/winstation/desktops
// https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-sendmessagetimeoutw
HRESULT admitForeignPrivateWindow(const PrivateDesktop& desktop,HWND ownedParent,
    HWND exactForeignWindow,const WindowIsolationControlReport& controls,
    ULONGLONG deadline,PrivateWindowAdmissionReport* report) noexcept;
} // namespace explorer
