// Included by the candidate native_quick_access_tests.cpp inside its existing
// anonymous namespace after the original independent NativeState/Row helpers.
// All UIA interfaces stay on the windowless MTA; no provider command is called.
struct GestureUnavailable : std::runtime_error {
    explicit GestureUnavailable(const char* why) : std::runtime_error(why) {}
};
struct GestureBstr {
    BSTR value = nullptr;
    ~GestureBstr() { SysFreeString(value); }
};
struct GestureRuntimeId {
    SAFEARRAY* value = nullptr;
    ~GestureRuntimeId() { if (value) SafeArrayDestroy(value); }
};
// Existing read metadata only; never a retained interface or synthetic ID.
struct GestureUnavailableRuntimeEvidence {
    UINT phase=0;
    size_t node=0,depth=0;
    CONTROLTYPEID type=0;
    RECT bounds{};
    bool enabled=false,offscreen=true,runtimeUnavailable=false;
    UINT nameLength=0,nameCopied=0;
    std::array<wchar_t,257> namePrefix{};
};
// Actual native bridge reads; plain receipts only, never retained COM owners.
struct GestureLegacyBridgeEvidence {
    HRESULT windowRead=E_PENDING,receiverRead=E_PENDING,roleRead=E_PENDING,nameRead=E_PENDING;
    HRESULT locationRead=E_PENDING,stateRead=E_PENDING,identityQuery=E_PENDING,identityRead=E_PENDING,receiverAfter=E_PENDING;
    HWND window=nullptr;
    int child=0;LONG role=0,state=0;VARTYPE roleType=VT_EMPTY,stateType=VT_EMPTY;
    RECT bounds{};
    UINT nameLength=0;
    std::array<wchar_t,257> namePrefix{};
    DWORD identityLength=0;
    bool identityAbsent=false;
};
// Fixed-size per-MTA diagnostic only. No interfaces/strings escape the worker,
// no extra UIA/native property call is made, and original limits remain exact.
struct GestureReadTrace {
    UINT phase=0,stage=0,gate=0,toolbarCount=0,targetCount=0,menuRows=0;
    UINT emptyToolbarReads=0,emptyToolbarProofs=0;
    UINT unavailableDiscoveryReads=0,unavailableDiscoverySamples=0;
    bool unavailableDiscoveryTruncated=false;
    std::array<GestureUnavailableRuntimeEvidence,32> unavailableDiscovery{};
    UINT pathProofs=0,pathCollisions=0,legacyReads=0,legacyNativePairs=0,legacyAbsentPairs=0;
    HRESULT legacyPattern=E_PENDING,legacyAccessible=E_PENDING,legacyChild=E_PENDING,legacyUnknown=E_PENDING;
    int legacyChildId=0;
    bool legacyNull=false;
    UINT legacyDistinctPairs=0,legacyBridgePairs=0;
    std::array<GestureLegacyBridgeEvidence,2> legacyBridge{};
    UINT unavailableIdentityRole=0; // 1 command-caption candidate, 2 current menu, 3 item, 4 prior menu.
    GestureUnavailableRuntimeEvidence unavailableIdentity;
    HRESULT nativeResult=E_PENDING,returned=E_PENDING;
    size_t node=0,queued=0,depth=0,siblings=0;
    UINT nameLength=0,nameCopied=0,runtimeDimensions=0;
    LONG runtimeFirst=0,runtimeLast=-1;
    long long runtimeCount=0;
    int process=0;CONTROLTYPEID controlType=0;RECT bounds{};HWND root=nullptr;
    std::array<wchar_t,257> namePrefix{};
};
thread_local GestureReadTrace gestureReadTrace;
HRESULT gestureTraceResult(HRESULT result,UINT gate=0) noexcept {
    gestureReadTrace.returned=result;gestureReadTrace.gate=gate;return result;
}

struct GestureElement {
    ComPtr<IUIAutomationElement> element;
    std::wstring name;
    CONTROLTYPEID type = 0;
    RECT bounds{};
    bool enabled = false, offscreen = true;
    std::vector<int> runtime;
    HWND nativeWindow=nullptr,walkRoot=nullptr;
    size_t parentIndex=512; // Actual bounded walk relation, never an element ID.
    bool runtimeUnavailable=false; // A well-formed empty array is unavailable identity.
    bool emptyToolbarRuntime=false; // Retained toolbar proof remains independent.
};
struct GestureConfig {
    std::wstring toolbar = L"Quick Access Toolbar";
    std::wstring add = L"Add to Quick Access Toolbar";
    std::wstring remove = L"Remove from Quick Access Toolbar";
};
std::wstring gestureEnvironment(const wchar_t* name, const wchar_t* fallback) {
    std::array<wchar_t, 257> bytes{};
    SetLastError(ERROR_SUCCESS);
    const auto length = GetEnvironmentVariableW(name, bytes.data(), static_cast<DWORD>(bytes.size()));
    if (!length) {
        require(GetLastError() == ERROR_ENVVAR_NOT_FOUND, "Native gesture label environment is invalid/empty");
        return fallback;
    }
    require(length < bytes.size() && length <= 256, "Native gesture label is unbounded");
    return std::wstring(bytes.data(), length);
}
bool gestureUnavailableStatus(HRESULT result) noexcept {
    return result == E_NOINTERFACE || result == E_NOTIMPL || result == UIA_E_NOTSUPPORTED ||
        result == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) || result == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
}
HRESULT gestureElementRead(IUIAutomationElement* element, GestureElement& result,
                           bool allowUnavailableRuntime=false) {
    if (!element) return gestureTraceResult(E_NOINTERFACE);
    GestureElement next; next.element = element;
    gestureReadTrace.nameLength=gestureReadTrace.nameCopied=0;gestureReadTrace.namePrefix.fill(0);
    gestureReadTrace.controlType=0;gestureReadTrace.bounds={};gestureReadTrace.process=0;
    gestureReadTrace.runtimeDimensions=0;gestureReadTrace.runtimeCount=0;
    gestureReadTrace.runtimeFirst=0;gestureReadTrace.runtimeLast=-1;
    int process = 0;
    gestureReadTrace.stage=30;
    auto hr = element->get_CurrentProcessId(&process);gestureReadTrace.nativeResult=hr;gestureReadTrace.process=process;
    if (hr != S_OK) return gestureTraceResult(hr);
    if (process != static_cast<int>(GetCurrentProcessId())) return gestureTraceResult(E_ACCESSDENIED);
    gestureReadTrace.stage=31;
    GestureBstr name; hr = element->get_CurrentName(&name.value);gestureReadTrace.nativeResult=hr;
    if (hr != S_OK) return gestureTraceResult(hr);
    const auto size = name.value ? SysStringLen(name.value) : 0;
    gestureReadTrace.nameLength=size;gestureReadTrace.nameCopied=std::min(size,256u);
    if(name.value)std::copy_n(name.value,gestureReadTrace.nameCopied,gestureReadTrace.namePrefix.begin());
    if (size > 256) return gestureTraceResult(HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER),16);
    if (name.value) next.name.assign(name.value, size);
    gestureReadTrace.stage=32;
    hr = element->get_CurrentControlType(&next.type);gestureReadTrace.nativeResult=hr;gestureReadTrace.controlType=next.type;
    if (hr != S_OK) return gestureTraceResult(hr);
    gestureReadTrace.stage=33;
    hr = element->get_CurrentBoundingRectangle(&next.bounds);gestureReadTrace.nativeResult=hr;gestureReadTrace.bounds=next.bounds;
    if (hr != S_OK) return gestureTraceResult(hr);
    BOOL enabled = FALSE, offscreen = TRUE;
    gestureReadTrace.stage=34;
    hr = element->get_CurrentIsEnabled(&enabled);gestureReadTrace.nativeResult=hr;if (hr != S_OK) return gestureTraceResult(hr);
    gestureReadTrace.stage=35;
    hr = element->get_CurrentIsOffscreen(&offscreen);gestureReadTrace.nativeResult=hr;if (hr != S_OK) return gestureTraceResult(hr);
    next.enabled = enabled != FALSE; next.offscreen = offscreen != FALSE;
    gestureReadTrace.stage=42;
    UIA_HWND native=nullptr;hr=element->get_CurrentNativeWindowHandle(&native);gestureReadTrace.nativeResult=hr;
    if(hr!=S_OK)return gestureTraceResult(hr);
    next.nativeWindow=reinterpret_cast<HWND>(native);
    gestureReadTrace.stage=36;
    GestureRuntimeId id; hr = element->GetRuntimeId(&id.value);gestureReadTrace.nativeResult=hr;
    if (hr != S_OK || !id.value) return gestureTraceResult(hr == S_OK ? E_UNEXPECTED : hr);
    gestureReadTrace.stage=37;gestureReadTrace.runtimeDimensions=SafeArrayGetDim(id.value);
    if(gestureReadTrace.runtimeDimensions!=1)return gestureTraceResult(E_UNEXPECTED,64);
    gestureReadTrace.stage=38;
    VARTYPE type = VT_EMPTY; hr = SafeArrayGetVartype(id.value, &type);gestureReadTrace.nativeResult=hr;
    if (hr != S_OK || type != VT_I4) return gestureTraceResult(hr == S_OK ? E_UNEXPECTED : hr);
    LONG first = 0, last = -1;
    gestureReadTrace.stage=39;
    hr = SafeArrayGetLBound(id.value, 1, &first);gestureReadTrace.nativeResult=hr;gestureReadTrace.runtimeFirst=first;
    if (hr != S_OK) return gestureTraceResult(hr);
    gestureReadTrace.stage=40;
    hr = SafeArrayGetUBound(id.value, 1, &last);gestureReadTrace.nativeResult=hr;gestureReadTrace.runtimeLast=last;
    if (hr != S_OK) return gestureTraceResult(hr);
    const auto count = static_cast<long long>(last) - first + 1;gestureReadTrace.runtimeCount=count;
    // Actual v142/v144 native discovery includes ToolBar and unrelated Button
    // controls with S_OK, a typed one-dimensional array, and exactly zero items.
    // Retain unavailable metadata for discovery; no empty vector is an identity.
    // Ordinary/fresh action readers remain strict by default.
    if (count==0 && allowUnavailableRuntime) {
        next.runtimeUnavailable=true;
        next.emptyToolbarRuntime=next.type==UIA_ToolBarControlTypeId;
        if(next.emptyToolbarRuntime)++gestureReadTrace.emptyToolbarReads;
        result=std::move(next);return gestureTraceResult(S_OK);
    }
    if (count <= 0 || count > 64) return gestureTraceResult(HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER),32);
    for (LONG index = first;; ++index) {
        gestureReadTrace.stage=41;
        int value = 0; hr = SafeArrayGetElement(id.value, &index, &value);gestureReadTrace.nativeResult=hr;
        if (hr != S_OK) return gestureTraceResult(hr);
        next.runtime.push_back(value); if (index == last) break;
    }
    result = std::move(next); return gestureTraceResult(S_OK);
}
GestureUnavailableRuntimeEvidence gestureUnavailableEvidence(const GestureElement& row) noexcept {
    GestureUnavailableRuntimeEvidence value;
    value.type=row.type;value.bounds=row.bounds;value.enabled=row.enabled;value.offscreen=row.offscreen;
    value.runtimeUnavailable=row.runtimeUnavailable;
    value.nameLength=static_cast<UINT>(row.name.size());
    value.nameCopied=std::min(value.nameLength,256u);
    std::copy_n(row.name.begin(),value.nameCopied,value.namePrefix.begin());
    return value;
}
void gestureUnavailableIdentity(const GestureElement& row, UINT role) noexcept {
    gestureReadTrace.unavailableIdentityRole=role;
    gestureReadTrace.unavailableIdentity=gestureUnavailableEvidence(row);
    gestureReadTrace.unavailableIdentity.phase=gestureReadTrace.phase;
}
// Raw-walker breadth-first traversal of one independently owned HWND subtree.
// No desktop root, content namespace, profile or global accessibility events.
HRESULT gestureWalk(IUIAutomation* automation, IUIAutomationElement* root,
                    ULONGLONG deadline, std::vector<GestureElement>& output, HWND rootWindow) {
    if (!automation || !root || !rootWindow) return gestureTraceResult(E_NOINTERFACE);
    ComPtr<IUIAutomationTreeWalker> walker;
    gestureReadTrace.stage=21;
    auto hr = automation->get_RawViewWalker(&walker);gestureReadTrace.nativeResult=hr;
    if (hr != S_OK || !walker) return gestureTraceResult(hr == S_OK ? E_NOINTERFACE : hr);
    struct Node { ComPtr<IUIAutomationElement> element; unsigned depth; size_t parent; };
    std::vector<Node> nodes; nodes.push_back({root, 0, 512});
    std::vector<GestureElement> result;
    for (size_t index = 0; index < nodes.size(); ++index) {
        gestureReadTrace.node=index;gestureReadTrace.queued=nodes.size();gestureReadTrace.depth=nodes[index].depth;
        if (GetTickCount64() >= deadline) return gestureTraceResult(HRESULT_FROM_WIN32(ERROR_TIMEOUT),4);
        if (nodes.size() > 512 || nodes[index].depth > 32)
            return gestureTraceResult(HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER),(nodes.size()>512?2u:0u)|(nodes[index].depth>32?8u:0u));
        GestureElement read; hr = gestureElementRead(nodes[index].element.Get(), read,true);
        if (hr != S_OK) return hr;
        read.parentIndex=nodes[index].parent;read.walkRoot=rootWindow;
        if(read.runtimeUnavailable) {
            ++gestureReadTrace.unavailableDiscoveryReads;
            if(gestureReadTrace.unavailableDiscoverySamples<gestureReadTrace.unavailableDiscovery.size()) {
                auto evidence=gestureUnavailableEvidence(read);
                evidence.phase=gestureReadTrace.phase;evidence.node=index;evidence.depth=nodes[index].depth;
                gestureReadTrace.unavailableDiscovery[gestureReadTrace.unavailableDiscoverySamples++]=evidence;
            } else gestureReadTrace.unavailableDiscoveryTruncated=true;
        }
        result.push_back(std::move(read));
        ComPtr<IUIAutomationElement> child;
        gestureReadTrace.stage=50;
        hr = walker->GetFirstChildElement(nodes[index].element.Get(), &child);gestureReadTrace.nativeResult=hr;
        if (hr != S_OK && hr != S_FALSE) return gestureTraceResult(hr);
        unsigned children = 0;
        while (child) {
            ++children;gestureReadTrace.siblings=children;gestureReadTrace.queued=nodes.size();
            // Preserve original short-circuit clock sampling and result; the
            // receipt identifies which original bound rejected the same node.
            const bool tooManyChildren=children>128,full=nodes.size()>=512;
            const bool expired=!tooManyChildren&&!full&&GetTickCount64()>=deadline;
            if (tooManyChildren || full || expired)
                return gestureTraceResult(HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER),(tooManyChildren?1u:0u)|(full?2u:0u)|(expired?4u:0u));
            nodes.push_back({child, nodes[index].depth + 1, index});
            ComPtr<IUIAutomationElement> sibling;
            gestureReadTrace.stage=51;
            hr = walker->GetNextSiblingElement(child.Get(), &sibling);gestureReadTrace.nativeResult=hr;
            if (hr != S_OK && hr != S_FALSE) return gestureTraceResult(hr);
            child = std::move(sibling);
        }
    }
    output = std::move(result); return gestureTraceResult(S_OK);
}

HRESULT gestureEmptyToolbarProof(IUIAutomation* automation, IUIAutomationElement* current,
    const GestureElement& expected, HWND owner, DWORD creator, ULONGLONG deadline);
HRESULT gestureDescendant(IUIAutomation* automation, IUIAutomationElement* element,
                         IUIAutomationElement* ancestor, bool& result,
                         const GestureElement* emptyToolbar=nullptr, HWND owner=nullptr,
                         DWORD creator=0, ULONGLONG deadline=0) {
    result = false;
    ComPtr<IUIAutomationTreeWalker> walker;
    gestureReadTrace.stage=60;
    auto hr = automation->get_RawViewWalker(&walker);gestureReadTrace.nativeResult=hr;
    if (hr != S_OK || !walker) return hr == S_OK ? E_NOINTERFACE : hr;
    ComPtr<IUIAutomationElement> current = element;
    for (unsigned depth = 0; current && depth <= 32; ++depth) {
        gestureReadTrace.stage=61;gestureReadTrace.depth=depth;
        BOOL same = FALSE; hr = automation->CompareElements(current.Get(), ancestor, &same);gestureReadTrace.nativeResult=hr;
        if (hr != S_OK) return hr;
        if (same) {
            // CompareElements is necessary but empty arrays are never sufficient
            // identity. Prove the actual ancestor type/caption/bounds and HWND too.
            if(emptyToolbar) {
                hr=gestureEmptyToolbarProof(automation,current.Get(),*emptyToolbar,owner,creator,deadline);
                if(hr!=S_OK)return hr;
            }
            result = true; return S_OK;
        }
        ComPtr<IUIAutomationElement> parent;
        gestureReadTrace.stage=62;
        hr = walker->GetParentElement(current.Get(), &parent);gestureReadTrace.nativeResult=hr;
        if (hr != S_OK && hr != S_FALSE) return hr;
        current = std::move(parent);
    }
    return gestureTraceResult(current ? HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) : S_OK,current?8u:0u);
}
HRESULT gestureReceiver(IUIAutomation* automation, IUIAutomationElement* element,
                        HWND owner, DWORD creator, qat_gesture_protocol::WindowIdentity& result) {
    ComPtr<IUIAutomationTreeWalker> walker;
    gestureReadTrace.stage=63;
    auto hr = automation->get_RawViewWalker(&walker);gestureReadTrace.nativeResult=hr;
    if (hr != S_OK || !walker) return hr == S_OK ? E_NOINTERFACE : hr;
    ComPtr<IUIAutomationElement> current = element;
    for (unsigned depth = 0; current && depth <= 32; ++depth) {
        gestureReadTrace.stage=64;gestureReadTrace.depth=depth;
        UIA_HWND native = nullptr; hr = current->get_CurrentNativeWindowHandle(&native);gestureReadTrace.nativeResult=hr;
        if (hr != S_OK) return hr;
        if (native) {gestureReadTrace.stage=65;const auto ownedRead=qat_gesture_protocol::readOwnedWindow(reinterpret_cast<HWND>(native), owner, creator, result);
            gestureReadTrace.nativeResult=ownedRead;return gestureTraceResult(ownedRead);}
        gestureReadTrace.stage=66;
        ComPtr<IUIAutomationElement> parent; hr = walker->GetParentElement(current.Get(), &parent);gestureReadTrace.nativeResult=hr;
        if (hr != S_OK && hr != S_FALSE) return hr;
        current = std::move(parent);
    }
    return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
}
// Called only after native CompareElements returned exact S_OK/TRUE on an
// actual raw-parent chain and the independently unique ToolBar had no ID.
// Name alone is not identity: both fresh elements must retain the same full
// type/caption/bounds/state and nearest exact owned native receiver tuple.
HRESULT gestureEmptyToolbarProof(IUIAutomation* automation, IUIAutomationElement* current,
    const GestureElement& expected, HWND owner, DWORD creator, ULONGLONG deadline) {
    if(!automation||!current||!expected.element||!owner||!creator||!deadline||
       !expected.emptyToolbarRuntime||!expected.runtime.empty()||
       expected.type!=UIA_ToolBarControlTypeId||expected.name.empty()||expected.offscreen||
       expected.bounds.left>=expected.bounds.right||expected.bounds.top>=expected.bounds.bottom)
        return gestureTraceResult(E_UNEXPECTED);
    if(GetTickCount64()>=deadline)return gestureTraceResult(HRESULT_FROM_WIN32(ERROR_TIMEOUT),4);
    GestureElement actual,ancestor;
    auto hr=gestureElementRead(current,actual,true);
    if(hr!=S_OK)return hr;
    if(GetTickCount64()>=deadline)return gestureTraceResult(HRESULT_FROM_WIN32(ERROR_TIMEOUT),4);
    hr=gestureElementRead(expected.element.Get(),ancestor,true);
    if(hr!=S_OK)return hr;
    const auto sameMetadata=[&](const GestureElement& value) {
        return value.emptyToolbarRuntime&&value.runtime.empty()&&value.type==expected.type&&
            value.name==expected.name&&value.enabled==expected.enabled&&value.offscreen==expected.offscreen&&
            qat_gesture_protocol::equalRect(value.bounds,expected.bounds);
    };
    if(!sameMetadata(actual)||!sameMetadata(ancestor))return gestureTraceResult(E_ABORT);
    if(GetTickCount64()>=deadline)return gestureTraceResult(HRESULT_FROM_WIN32(ERROR_TIMEOUT),4);
    qat_gesture_protocol::WindowIdentity actualReceiver,ancestorReceiver;
    hr=gestureReceiver(automation,current,owner,creator,actualReceiver);
    if(hr!=S_OK)return hr;
    if(GetTickCount64()>=deadline)return gestureTraceResult(HRESULT_FROM_WIN32(ERROR_TIMEOUT),4);
    hr=gestureReceiver(automation,expected.element.Get(),owner,creator,ancestorReceiver);
    if(hr!=S_OK)return hr;
    if(actualReceiver.window!=ancestorReceiver.window||actualReceiver.root!=ancestorReceiver.root||
       actualReceiver.parent!=ancestorReceiver.parent||actualReceiver.process!=ancestorReceiver.process||
       actualReceiver.thread!=ancestorReceiver.thread||
       !qat_gesture_protocol::equalRect(actualReceiver.bounds,ancestorReceiver.bounds)||
       std::wcscmp(actualReceiver.windowClass.data(),ancestorReceiver.windowClass.data())!=0)
        return gestureTraceResult(E_ABORT);
    hr=qat_gesture_protocol::sameOwnedWindow(ancestorReceiver,owner,creator);
    if(hr!=S_OK)return gestureTraceResult(hr);
    if(GetTickCount64()>=deadline)return gestureTraceResult(HRESULT_FROM_WIN32(ERROR_TIMEOUT),4);
    ++gestureReadTrace.emptyToolbarProofs;
    return gestureTraceResult(S_OK);
}

struct GestureWindows {
    DWORD creator = 0;
    std::array<HWND, 64> handles{};
    std::array<bool, 64> visible{};
    UINT count = 0;
    bool overflow = false,expired=false;
    ULONGLONG deadline=0;
};
BOOL CALLBACK gestureEnumWindow(HWND window, LPARAM parameter) noexcept {
    auto& windows = *reinterpret_cast<GestureWindows*>(parameter);
    if(GetTickCount64()>=windows.deadline){windows.expired=true;return FALSE;}
    DWORD process = 0;
    if (GetWindowThreadProcessId(window, &process) != windows.creator || process != GetCurrentProcessId()) return TRUE;
    if(std::find(windows.handles.begin(),windows.handles.begin()+windows.count,window)!=windows.handles.begin()+windows.count)return TRUE;
    if (windows.count == windows.handles.size()) { windows.overflow = true; return FALSE; }
    windows.handles[windows.count] = window; windows.visible[windows.count] = IsWindowVisible(window) != FALSE;
    ++windows.count; return TRUE;
}
HRESULT gestureWindows(DWORD creator, GestureWindows& output, ULONGLONG deadline) noexcept {
    GestureWindows next; next.creator = creator;next.deadline=deadline;
    SetLastError(ERROR_SUCCESS);
    const auto enumerated = EnumThreadWindows(creator, gestureEnumWindow, reinterpret_cast<LPARAM>(&next));
    const auto error = GetLastError();
    if (next.expired) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    if (next.overflow) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    if (!enumerated) return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
    const auto topCount=next.count;
    for(UINT index=0;index<topCount;++index) {
        if(GetTickCount64()>=deadline)return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        // EnumChildWindows recursively enumerates real children; its return
        // value is explicitly unused by the API contract. Callback flags carry
        // our existing exact 64-HWND bound and absolute-deadline admission.
        EnumChildWindows(next.handles[index],gestureEnumWindow,reinterpret_cast<LPARAM>(&next));
        if(next.expired)return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        if(next.overflow)return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }
    if(GetTickCount64()>=deadline)return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    output = next; return S_OK;
}
bool gestureOwnedPopup(HWND window, HWND owner, DWORD creator) noexcept {
    DWORD process = 0;
    if (!window || !IsWindow(window) || !IsWindowVisible(window) ||
        GetWindowThreadProcessId(window, &process) != creator || process != GetCurrentProcessId()) return false;
    if (window == owner || IsChild(owner, window)) return true;
    HWND current = window;
    for (unsigned depth = 0; current && depth < 16; ++depth) {
        current = GetWindow(current, GW_OWNER);
        if (current == owner || (current && IsChild(owner, current))) return true;
    }
    return false;
}
HRESULT gestureMenuReceiver(IUIAutomation* automation, IUIAutomationElement* row,
                            HWND owner, DWORD creator, const RECT& rowBounds, HWND& result,
                            qat_gesture_protocol::WindowIdentity* identity = nullptr) {
    ComPtr<IUIAutomationTreeWalker> walker;
    auto hr = automation->get_RawViewWalker(&walker);
    if (hr != S_OK || !walker) return hr == S_OK ? E_NOINTERFACE : hr;
    ComPtr<IUIAutomationElement> current = row;
    for (unsigned depth = 0; current && depth <= 32; ++depth) {
        UIA_HWND native = nullptr; hr = current->get_CurrentNativeWindowHandle(&native);
        if (hr != S_OK) return hr;
        if (native) {
            const auto window = reinterpret_cast<HWND>(native);
            if (!gestureOwnedPopup(window, owner, creator)) return E_ACCESSDENIED;
            RECT bounds{};
            SetLastError(ERROR_SUCCESS);
            if (!GetWindowRect(window, &bounds)) return qat_gesture_protocol::nativeError();
            if (rowBounds.left < bounds.left || rowBounds.top < bounds.top || rowBounds.right > bounds.right ||
                rowBounds.bottom > bounds.bottom || rowBounds.left >= rowBounds.right || rowBounds.top >= rowBounds.bottom)
                return E_ACCESSDENIED;
            qat_gesture_protocol::WindowIdentity actual;
            actual.window = window; actual.root = GetAncestor(window, GA_ROOT); actual.parent = GetAncestor(window, GA_PARENT);
            actual.thread = GetWindowThreadProcessId(window, &actual.process); actual.bounds = bounds;
            SetLastError(ERROR_SUCCESS);
            const auto length = GetClassNameW(window, actual.windowClass.data(), static_cast<int>(actual.windowClass.size()));
            if (!length) return qat_gesture_protocol::nativeError();
            if (length == static_cast<int>(actual.windowClass.size()) - 1) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
            if (actual.thread != creator || actual.process != GetCurrentProcessId() || !gestureOwnedPopup(window, owner, creator))
                return E_ACCESSDENIED;
            if (identity) *identity = actual;
            result = window; return S_OK;
        }
        ComPtr<IUIAutomationElement> parent; hr = walker->GetParentElement(current.Get(), &parent);
        if (hr != S_OK && hr != S_FALSE) return hr;
        current = std::move(parent);
    }
    return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
}
#include "native_qat_identity_fixture.hpp"

struct GestureShared {
    qat_gesture_protocol::ContextTicket ticket;
    qat_gesture_protocol::ContextReceipt context;
    std::atomic<bool> prepared{false}, proceed{false}, cancel{false};
    HRESULT preparedResult = E_PENDING, actionResult = E_PENDING;
    GestureReadTrace failureReadback; // Plain MTA-owned copy; creator reads only after kernel join.
    HRESULT invokeQuery = E_PENDING;
    bool invokePresent = false, rowEnabled = false, rowOffscreen = true;
    HWND popup = nullptr;
    RECT rowBounds{};
    std::vector<int> rowRuntime;
    unsigned matchingRows = 0;
    bool invoked = false;
};
void gestureUiWorker(const std::shared_ptr<GestureShared>& shared, HDESK desktop,
                     GestureConfig config, std::wstring controlName, bool qatControl,
                     bool disabledObservation, std::shared_ptr<std::promise<HRESULT>> promise) noexcept {
    HRESULT result = E_FAIL;
    gestureReadTrace={};gestureReadTrace.phase=1;gestureReadTrace.stage=1;gestureReadTrace.root=shared->ticket.owner;
    const bool attached = SetThreadDesktop(desktop) != FALSE;
    if (!attached) { result = qat_gesture_protocol::nativeError(); gestureReadTrace.nativeResult=result;gestureReadTrace.returned=result;shared->failureReadback=gestureReadTrace;
        shared->preparedResult = result; shared->prepared.store(true); promise->set_value(result); return; }
    gestureReadTrace.stage=2;
    const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);gestureReadTrace.nativeResult=initialized;
    if (FAILED(initialized)) { gestureReadTrace.returned=initialized;shared->failureReadback=gestureReadTrace;
        shared->preparedResult = initialized; shared->prepared.store(true); promise->set_value(initialized); return; }
    gestureReadTrace.stage=3;
    const auto cancellation = CoEnableCallCancellation(nullptr);gestureReadTrace.nativeResult=cancellation;
    try {
        result = cancellation;
        if (result == S_OK) {
            ComPtr<IUIAutomation2> automation;
            gestureReadTrace.stage=4;
            result = CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation));gestureReadTrace.nativeResult=result;
            if (result == S_OK && !automation) result = E_NOINTERFACE;
            if (result == S_OK) {gestureReadTrace.stage=5;result = automation->put_AutoSetFocus(FALSE);gestureReadTrace.nativeResult=result;}
            if (result == S_OK) {gestureReadTrace.stage=6;result = automation->put_ConnectionTimeout(1000);gestureReadTrace.nativeResult=result;}
            if (result == S_OK) {gestureReadTrace.stage=7;result = automation->put_TransactionTimeout(2000);gestureReadTrace.nativeResult=result;}
            ComPtr<IUIAutomationElement> root;
            if (result == S_OK) {gestureReadTrace.stage=8;result = automation->ElementFromHandle(shared->ticket.owner, &root);gestureReadTrace.nativeResult=result;}
            if (result == S_OK && !root) result = E_NOINTERFACE;
            std::vector<GestureElement> before;
            if (result == S_OK) {gestureReadTrace.phase=10;
                result = gestureWalk(automation.Get(), root.Get(), shared->ticket.deadline, before,shared->ticket.owner);}
            ComPtr<IUIAutomationElement> toolbar;GestureElement toolbarEvidence;size_t toolbarIndex=512;
            unsigned toolbarCount = 0;
            if (result == S_OK) for (const auto& row : before) {
                if (row.type == UIA_ToolBarControlTypeId) std::wcout << L"QAT UIA toolbar name=\"" << row.name << L"\" offscreen=" << row.offscreen << std::endl;
                if (row.type == UIA_ToolBarControlTypeId && row.name == config.toolbar && !row.offscreen) {
                    toolbar = row.element;toolbarEvidence=row;toolbarIndex=static_cast<size_t>(&row-before.data());++toolbarCount;
                }
            }
            gestureReadTrace.toolbarCount=toolbarCount;
            if (result == S_OK && toolbarCount != 1) {gestureReadTrace.phase=11;result = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);}
            if(result==S_OK&&toolbarEvidence.emptyToolbarRuntime) {
                // Prove the independently unique retained container itself even
                // for Add, whose target is outside this toolbar ancestor chain.
                gestureReadTrace.stage=70;BOOL same=FALSE;
                result=automation->CompareElements(toolbar.Get(),toolbar.Get(),&same);gestureReadTrace.nativeResult=result;
                if(result==S_OK&&!same)result=gestureTraceResult(E_ABORT);
                if(result==S_OK)result=gestureEmptyToolbarProof(automation.Get(),toolbar.Get(),toolbarEvidence,
                    shared->ticket.owner,shared->ticket.creator,shared->ticket.deadline);
            }
            GestureElement target;unsigned targets=0;size_t targetIndex=512;
            if(result==S_OK)for(size_t index=0;index<before.size();++index) {
                const auto& row=before[index];
                if(row.name!=controlName||row.offscreen||!row.enabled||
                   (row.type!=UIA_ButtonControlTypeId&&row.type!=UIA_SplitButtonControlTypeId))continue;
                gestureReadTrace.phase=20;
                bool inToolbar=false;result=gesturePathDescendant(before,index,toolbarIndex,inToolbar);
                if(result!=S_OK)break;
                if(!row.runtimeUnavailable&&!toolbarEvidence.runtimeUnavailable) {
                    bool nativeInToolbar=false;
                    result=gestureDescendant(automation.Get(),row.element.Get(),toolbar.Get(),nativeInToolbar);
                    if(result!=S_OK)break;
                    if(nativeInToolbar!=inToolbar){result=E_ABORT;break;}
                }
                if(inToolbar==qatControl){target=row;targetIndex=index;++targets;}
            }
            gestureReadTrace.targetCount=targets;
            if(result==S_OK&&targets!=1){gestureReadTrace.phase=21;result=HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);}
            std::vector<GestureElement> targetProofRows;size_t targetProofIndex=512;
            if(result==S_OK) {
                gestureReadTrace.phase=22;
                result=gestureReacquire(automation.Get(),before,targetIndex,shared->ticket.owner,
                    shared->ticket.creator,shared->ticket.deadline,targetProofRows,targetProofIndex);
                if(result!=S_OK&&target.runtimeUnavailable)gestureUnavailableIdentity(target,1);
            }
            if(result==S_OK) {
                target=targetProofRows[targetProofIndex];gestureReadTrace.phase=30;
                result=gestureReceiver(automation.Get(),target.element.Get(),shared->ticket.owner,
                    shared->ticket.creator,shared->ticket.receiverIdentity);
            }
            GestureWindows initialWindows;
            if (result == S_OK) {
                shared->ticket.elementBounds = target.bounds;
                shared->ticket.screenPoint = {target.bounds.left + (target.bounds.right - target.bounds.left) / 2,
                    target.bounds.top + (target.bounds.bottom - target.bounds.top) / 2};
            }
            shared->preparedResult = result; shared->prepared.store(true, std::memory_order_release);
            while (result == S_OK && !shared->proceed.load(std::memory_order_acquire) && !shared->cancel.load() && GetTickCount64() < shared->ticket.deadline) Sleep(1);
            if (result == S_OK && (!shared->proceed.load() || shared->cancel.load())) result = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            // Creator's exact native inventory/epoch check has completed. The
            // second independent current walk and native HWND inventory finish
            // immediately before posting this one original context opening.
            if(result==S_OK) {
                std::vector<GestureElement> finalTargetRows;size_t finalTargetIndex=512;
                result=gestureReacquire(automation.Get(),targetProofRows,targetProofIndex,shared->ticket.owner,
                    shared->ticket.creator,shared->ticket.deadline,finalTargetRows,finalTargetIndex);
                if(result==S_OK) {
                    qat_gesture_protocol::WindowIdentity currentReceiver;
                    result=gestureReceiver(automation.Get(),finalTargetRows[finalTargetIndex].element.Get(),shared->ticket.owner,
                        shared->ticket.creator,currentReceiver);
                    if(result==S_OK&&!gestureEqualReceiver(currentReceiver,shared->ticket.receiverIdentity))result=E_ABORT;
                }
                if(result==S_OK) {
                    gestureReadTrace.phase=31;gestureReadTrace.stage=90;
                    result=gestureWindows(shared->ticket.creator,initialWindows,shared->ticket.deadline);
                    gestureReadTrace.nativeResult=result;
                }
            }
            if (result == S_OK) {
                gestureReadTrace.phase=32;gestureReadTrace.stage=91;
                if (!PostMessageW(shared->ticket.owner, qat_gesture_protocol::dispatchMessage, shared->ticket.sequence, 0))
                    result = qat_gesture_protocol::nativeError();
                gestureReadTrace.nativeResult=result;
            }
            while (result == S_OK && !shared->context.entered.load(std::memory_order_acquire) && !shared->context.returned.load() &&
                !shared->cancel.load() && GetTickCount64() < shared->ticket.deadline) Sleep(1);
            if (result == S_OK && !shared->context.entered.load()) result = shared->context.before.load() != E_PENDING ?
                shared->context.before.load() : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            ComPtr<IUIAutomationInvokePattern> invoke;
            bool found = false;
            while (result == S_OK && !found && !shared->cancel.load() && GetTickCount64() < shared->ticket.deadline) {
                // A concrete creator-side denial is not an unavailable menu.
                // Only returned publishes after; receiverAfter may legitimately
                // fail after successful native Remove and is not this gate.
                if(shared->context.returned.load(std::memory_order_acquire)) {
                    const auto contextResult=shared->context.after.load();
                    if(contextResult!=S_OK) {
                        gestureReadTrace.phase=34;gestureReadTrace.stage=94;
                        result=contextResult;break;
                    }
                }
                gestureReadTrace.phase=33;gestureReadTrace.stage=92;
                GestureWindows windows; result = gestureWindows(shared->ticket.creator, windows,shared->ticket.deadline);
                gestureReadTrace.nativeResult=result;
                if (result != S_OK) break;
                std::vector<HWND> roots;
                for(UINT index=0;index<windows.count;++index) {
                    const auto window=windows.handles[index];
                    if(!gestureNewPopup(initialWindows,window,shared->ticket.owner,shared->ticket.creator))continue;
                    bool nested=false;
                    for(UINT other=0;other<windows.count&&!nested;++other) {
                        const auto parent=windows.handles[other];
                        nested=parent!=window&&IsChild(parent,window)&&
                            gestureNewPopup(initialWindows,parent,shared->ticket.owner,shared->ticket.creator);
                    }
                    if(!nested)roots.push_back(window);
                }
                unsigned matches = 0; HWND selectedRoot = nullptr;
                std::vector<GestureActionPath> matchedActions;
                std::vector<GestureElement> selectedRows;size_t selectedIndex=512;
                for (const auto window : roots) {
                    if (!gestureOwnedPopup(window, shared->ticket.owner, shared->ticket.creator)) continue;
                    ComPtr<IUIAutomationElement> popupRoot; result = automation->ElementFromHandle(window, &popupRoot);
                    if (result != S_OK || !popupRoot) { if (result == S_OK) result = E_NOINTERFACE; break; }
                    gestureReadTrace.phase=40;gestureReadTrace.root=window;
                    std::vector<GestureElement> rows; result = gestureWalk(automation.Get(), popupRoot.Get(), shared->ticket.deadline, rows,window);
                    if (result != S_OK) break;
                    unsigned menuRows = 0;
                    for(size_t menuIndex=0;menuIndex<rows.size();++menuIndex) {
                        const auto& menu=rows[menuIndex];
                        if(menu.type!=UIA_MenuControlTypeId||menu.offscreen)continue;
                        // Newness is the native owned HWND visibility epoch,
                        // never absence/equality of prior empty RuntimeIds.
                        for(size_t rowIndex=0;rowIndex<rows.size();++rowIndex) {
                            const auto& row=rows[rowIndex];
                            if(row.type!=UIA_MenuItemControlTypeId||row.offscreen)continue;
                            bool descendant=false;result=gesturePathDescendant(rows,rowIndex,menuIndex,descendant);
                            if(result!=S_OK)break;
                            if(!row.runtimeUnavailable&&!menu.runtimeUnavailable) {
                                bool nativeDescendant=false;
                                result=gestureDescendant(automation.Get(),row.element.Get(),menu.element.Get(),nativeDescendant);
                                if(result!=S_OK)break;
                                if(nativeDescendant!=descendant){result=E_ABORT;break;}
                            }
                            if(!descendant)continue;
                            if(++menuRows>128){gestureReadTrace.menuRows=menuRows;gestureReadTrace.gate=128;
                                result=HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);break;}
                            std::wcout<<L"QAT native context row name=\""<<row.name<<L"\" enabled="<<row.enabled<<L" root="<<window<<std::endl;
                            const auto& expected=qatControl?config.remove:config.add;
                            if(row.name!=expected)continue;
                            GestureActionPath actionPath;result=gestureCapturePath(rows,rowIndex,shared->ticket.owner,
                                shared->ticket.creator,shared->ticket.deadline,actionPath);
                            if(result!=S_OK)break;
                            const auto duplicate=std::any_of(matchedActions.begin(),matchedActions.end(),[&](const auto& prior){
                                return gestureEqualPath(prior,actionPath);
                            });
                            if(!duplicate){matchedActions.push_back(std::move(actionPath));++matches;
                                selectedRoot=window;selectedRows=rows;selectedIndex=rowIndex;}
                        }
                        if(result!=S_OK)break;
                    }
                    if (result != S_OK) break;
                }
                shared->matchingRows = matches;
                if (result != S_OK) break;
                if (matches > 1) { result = E_UNEXPECTED; break; }
                if (matches == 1) {
                    std::vector<GestureElement> freshRows;size_t freshIndex=512;
                    result=gestureReacquire(automation.Get(),selectedRows,selectedIndex,shared->ticket.owner,
                        shared->ticket.creator,shared->ticket.deadline,freshRows,freshIndex);
                    if(result!=S_OK)break;
                    const auto& fresh=freshRows[freshIndex];
                    if(fresh.offscreen||!gestureNewPopup(initialWindows,selectedRoot,shared->ticket.owner,shared->ticket.creator)) {
                        result=E_ABORT;break;
                    }
                    HWND actualMenuWindow=nullptr;
                    qat_gesture_protocol::WindowIdentity menuIdentity;
                    result=gestureMenuReceiver(automation.Get(),fresh.element.Get(),shared->ticket.owner,
                        shared->ticket.creator,fresh.bounds,actualMenuWindow,&menuIdentity);
                    if(result!=S_OK)break;
                    if(!gestureNewPopup(initialWindows,actualMenuWindow,shared->ticket.owner,shared->ticket.creator)) {
                        result=E_ABORT;break;
                    }
                    shared->popup = actualMenuWindow; shared->rowRuntime = fresh.runtime; shared->rowBounds = fresh.bounds;
                    shared->rowEnabled = fresh.enabled; shared->rowOffscreen = fresh.offscreen;
                    std::wcout << L"QAT native menu HWND=" << menuIdentity.window << L" parent=" << menuIdentity.parent
                        << L" root=" << menuIdentity.root << L" class=\"" << menuIdentity.windowClass.data()
                        << L"\" process=" << menuIdentity.process << L" thread=" << menuIdentity.thread << std::endl;
                    shared->invokeQuery = fresh.element->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&invoke));
                    shared->invokePresent = invoke != nullptr;
                    if (disabledObservation) {
                        result = fresh.enabled ? E_FAIL : S_OK; found = true;
                    } else if (!fresh.enabled) { result = E_FAIL; break; }
                    else if (shared->invokeQuery != S_OK || !invoke) {
                        result = shared->invokeQuery == S_OK ? E_NOINTERFACE : shared->invokeQuery; break;
                    } else {
                        std::vector<GestureElement> finalRows;size_t finalIndex=512;
                        result=gestureReacquire(automation.Get(),freshRows,freshIndex,shared->ticket.owner,
                            shared->ticket.creator,shared->ticket.deadline,finalRows,finalIndex);
                        if(result!=S_OK)break;
                        const auto& finalRow=finalRows[finalIndex];
                        HWND finalWindow=nullptr;qat_gesture_protocol::WindowIdentity finalIdentity;
                        result=gestureMenuReceiver(automation.Get(),finalRow.element.Get(),shared->ticket.owner,
                            shared->ticket.creator,finalRow.bounds,finalWindow,&finalIdentity);
                        if(result!=S_OK)break;
                        if(finalWindow!=actualMenuWindow||!finalRow.enabled||finalRow.offscreen||
                           !gestureEqualReceiver(finalIdentity,menuIdentity)||
                           !gestureNewPopup(initialWindows,finalWindow,shared->ticket.owner,shared->ticket.creator)) {
                            result=E_ABORT;break;
                        }
                        if (GetTickCount64() >= shared->ticket.deadline) { result = HRESULT_FROM_WIN32(ERROR_TIMEOUT); break; }
                        shared->invoked = true; shared->actionResult = invoke->Invoke(); result = shared->actionResult; found = true;
                    }
                } else Sleep(1);
            }
            if(result==S_OK&&!found&&shared->context.returned.load(std::memory_order_acquire)) {
                const auto contextResult=shared->context.after.load();
                if(contextResult!=S_OK) {
                    gestureReadTrace.phase=34;gestureReadTrace.stage=94;
                    result=contextResult;
                }
            }
            if (result == S_OK && !found) {
                // Derived no-popup result; do not label the last target receiver
                // read as the native API that returned ERROR_NOT_SUPPORTED.
                gestureReadTrace.phase=39;gestureReadTrace.stage=93;
                result = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
            }
            // Native disabled menu inspection has no Invoke. Failure also only
            // cancels this exact owned opening; it cannot rescue its result.
            if (shared->context.entered.load() && (disabledObservation || result != S_OK))
                PostMessageW(shared->ticket.owner, qat_gesture_protocol::cancelMessage, shared->ticket.sequence, 0);
            invoke.Reset(); target.element.Reset(); toolbar.Reset(); root.Reset();
        }
    } catch (const std::bad_alloc&) { result = E_OUTOFMEMORY; }
    catch (...) { result = E_FAIL; }
    if (!shared->prepared.load()) { shared->preparedResult = result; shared->prepared.store(true, std::memory_order_release); }
    if (shared->context.entered.load() && result != S_OK)
        PostMessageW(shared->ticket.owner, qat_gesture_protocol::cancelMessage, shared->ticket.sequence, 0);
    if (cancellation == S_OK) { const auto disabled = CoDisableCallCancellation(nullptr); if (result == S_OK && FAILED(disabled)) result = disabled; }
    shared->failureReadback=gestureReadTrace;shared->failureReadback.returned=result;
    CoUninitialize(); promise->set_value(result);
}

struct GestureSource {
    explorer::NativeRibbon* ribbon = nullptr;
    IUIFramework* framework = nullptr; // Strong creator alias lives in gesture().
    std::uint64_t epoch = 0;
    unsigned* executions = nullptr;
    UINT applicationCommand=0,physicalCommand=0;
};
bool gestureSourceCurrent(void* context, const qat_gesture_protocol::ContextTicket& ticket) noexcept {
    const auto& source = *static_cast<GestureSource*>(context);
    return GetCurrentThreadId() == ticket.creator && source.ribbon && source.executions &&
        *source.executions == 0 && source.ribbon->valid() && source.ribbon->nativeFramework() == source.framework &&
        source.ribbon->callbackEntryEpoch() == source.epoch&&
        source.applicationCommand==ticket.applicationCommand&&source.physicalCommand==ticket.physicalCommand&&
        source.physicalCommand!=0;
}
std::wstring gestureCommandLabel(explorer::NativeRibbon& ribbon, UINT command) {
    // Button Label is invalidation-only. Its raw unsupported framework getter
    // is a diagnostic receipt, never the control-selection admission proof.
    // commandLabel returns owned markup/native-currentValue metadata; the
    // worker must still find exactly one actual visible public UIA Name and
    // each genuine gesture must produce the strict original native row ID.
    Variant rawLabel;
    const auto rawRead = ribbon.nativeFramework()->GetUICommandProperty(
        ribbon.nativeCommandId(command), UI_PKEY_Label, &rawLabel.value);
    std::wstring label;
    const auto metadataRead = ribbon.commandLabel(command, label);
    std::cout << "QAT target label metadata applicationId=" << command
        << " nativeId=" << ribbon.nativeCommandId(command)
        << " rawUnsupportedFrameworkGetHRESULT=" << static_cast<ULONG>(rawRead)
        << " rawFrameworkGetVT=" << rawLabel.value.vt
        << " ownedCommandMetadataHRESULT=" << static_cast<ULONG>(metadataRead)
        << " nativeFrameworkGetLabelProof=0 actualUIANameUniquenessRequired=1" << std::endl;
    if (metadataRead == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) || metadataRead == E_NOTIMPL || metadataRead == E_NOINTERFACE)
        throw GestureUnavailable("Owned command metadata is unavailable for exact actual UIA discovery");
    exact(metadataRead, "Read owned command metadata for actual public UIA discovery");
    require(!label.empty() && label.size() <= 256 && label.find(L'\0') == std::wstring::npos,
        "Owned command metadata is empty, truncated or contains embedded NUL");
    return label;
}
NativeState gesture(explorer::NativeRibbon& ribbon, HWND owner, unsigned& executions,
                    const GestureConfig& config, UINT command, bool qatControl,
                    bool disabledObservation, ULONGLONG fixtureDeadline, const char* phase) {
    isolation(owner, fixtureDeadline);
    const auto original = readNative(ribbon, owner, fixtureDeadline, phase);
    ComPtr<IUIFramework> framework = ribbon.nativeFramework();
    const auto physicalCommand=ribbon.nativeCommandId(command);
    require(physicalCommand!=0,"Gesture command has no current native physical identity");
    const auto commandRows=std::count_if(original.rows.begin(),original.rows.end(),[&](const auto& row){
        return row.commandRead==S_OK&&row.commandConvert==S_OK&&row.command==physicalCommand;
    });
    require(commandRows==(qatControl?1:0),"Gesture target disagrees with actual complete native QAT command inventory");
    GestureSource source{&ribbon,framework.Get(),ribbon.callbackEntryEpoch(),&executions,command,physicalCommand};
    auto shared = std::make_shared<GestureShared>();
    static UINT_PTR nextSequence = 1;
    shared->ticket.sequence = nextSequence++;
    shared->ticket.applicationCommand=command;shared->ticket.physicalCommand=physicalCommand;
    shared->ticket.owner = owner; shared->ticket.creator = GetCurrentThreadId();
    const auto now = GetTickCount64();
    require(fixtureDeadline > now + 500, "Gesture lacks original admission/drain budget");
    shared->ticket.deadline = std::min<ULONGLONG>(fixtureDeadline - 500, now + 4500);
    shared->ticket.drainDeadline = std::min<ULONGLONG>(fixtureDeadline, shared->ticket.deadline + 500);
    exact(qat_gesture_protocol::readOwnedWindow(owner, owner, GetCurrentThreadId(), shared->ticket.ownerIdentity),
        "Capture exact creator-owned private QAT owner");
    const auto controlName = gestureCommandLabel(ribbon, command);
    isolation(owner, fixtureDeadline);
    require(gestureSourceCurrent(&source, shared->ticket), "Label metadata/native diagnostic reentry changed original gesture source");
    std::wcout << L"QAT gesture target phase=" << phase << L" applicationId=" << command
        << L" nativeId=" << ribbon.nativeCommandId(command) << L" label=\"" << controlName
        << L"\" qatInstance=" << qatControl << L" disabledObservation=" << disabledObservation << std::endl;
    const auto desktop = GetThreadDesktop(GetCurrentThreadId());
    require(desktop != nullptr, "Retain actual creator private desktop");
    auto promise = std::make_shared<std::promise<HRESULT>>(); auto future = promise->get_future();
    std::thread worker(gestureUiWorker, shared, desktop, config, controlName, qatControl, disabledObservation, promise);
    HANDLE kernel = worker.native_handle(); const DWORD threadId = GetThreadId(kernel);
    const auto fatal = [](const char* reason) {
        std::cerr << "FATAL owned QAT gesture " << reason << "; no owner/COM/desktop unwind while worker or native callback lives" << std::endl;
        TerminateProcess(GetCurrentProcess(), 13); std::_Exit(13);
    };
    if (!threadId) fatal("worker identity unavailable");
    // Retained outside worker, installed only once its plain target is published.
    std::unique_ptr<qat_gesture_protocol::CreatorContextDispatch> dispatch;
    bool cancellationSent = false;
    HRESULT creatorResult = S_OK;
    for (;;) {
        const auto waited = WaitForSingleObject(kernel, 0);
        if (waited != WAIT_TIMEOUT && waited != WAIT_OBJECT_0) fatal("kernel wait failed");
        if (shared->prepared.load(std::memory_order_acquire) && !dispatch &&
            shared->preparedResult == S_OK && !shared->cancel.load()) {
            // Publication is the only reader admission for the MTA-filled ticket.
            try {
                if (!gestureSourceCurrent(&source, shared->ticket)) creatorResult = E_ABORT;
                else {
                    exactState(original, readNative(ribbon, owner, shared->ticket.deadline, "gesture-before-native-menu"));
                    dispatch = std::make_unique<qat_gesture_protocol::CreatorContextDispatch>(shared->ticket,
                        shared->context, gestureSourceCurrent, &source);
                    creatorResult = dispatch->install();
                }
            } catch (const std::bad_alloc&) { creatorResult = E_OUTOFMEMORY; }
            catch (...) { creatorResult = E_FAIL; }
            if (creatorResult == S_OK) shared->proceed.store(true, std::memory_order_release);
            else shared->cancel.store(true);
        }
        if (waited == WAIT_OBJECT_0) break;
        const auto current = GetTickCount64();
        if (current >= shared->ticket.deadline && !cancellationSent) {
            cancellationSent = true; shared->cancel.store(true);
            const auto cancelled = CoCancelCall(threadId, 0);
            if (dispatch) PostMessageW(owner, qat_gesture_protocol::cancelMessage, shared->ticket.sequence, 0);
            std::cout << "QAT gesture phase=" << phase << " cancellation=" << static_cast<ULONG>(cancelled) << std::endl;
        }
        if (current >= shared->ticket.drainDeadline) fatal("actual worker exit deadline");
        MSG message{};
        for (unsigned count = 0; count < 64 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++count) {
            if (message.message == WM_QUIT) fatal("unexpected host quit");
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        // This creator pump may enter the native modal menu; MTA Invoke/cancel
        // is what ends it. The original outer process watchdog remains needed.
        DWORD index = 0;
        const auto after = GetTickCount64();
        const auto remaining = after < shared->ticket.drainDeadline ? shared->ticket.drainDeadline - after : 0;
        const auto waitedCom = CoWaitForMultipleHandles(COWAIT_DISPATCH_CALLS | COWAIT_DISPATCH_WINDOW_MESSAGES,
            static_cast<DWORD>(std::min<ULONGLONG>(remaining, 10)), 1, &kernel, &index);
        if (FAILED(waitedCom) && waitedCom != RPC_S_CALLPENDING) fatal("COM-dispatch wait failed");
        if (SUCCEEDED(waitedCom) && index != 0) fatal("invalid COM kernel index");
    }
    worker.join();
    if (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) fatal("worker omitted completion receipt");
    const auto action = future.get();
    // Flush only the already posted numeric cancellation while bridge remains
    // installed. No new context opening, Invoke, selection or provider retry.
    MSG cancellationMessage{};
    while (PeekMessageW(&cancellationMessage, owner, qat_gesture_protocol::cancelMessage,
        qat_gesture_protocol::cancelMessage, PM_REMOVE)) DispatchMessageW(&cancellationMessage);
    std::cout << "QAT gesture phase=" << phase << " prepared=" << static_cast<ULONG>(shared->preparedResult)
        << " creator=" << static_cast<ULONG>(creatorResult) << " action=" << static_cast<ULONG>(action)
        << " entered=" << shared->context.entered.load() << " returned=" << shared->context.returned.load()
        << " contextBefore=" << static_cast<ULONG>(shared->context.before.load())
        << " contextAfter=" << static_cast<ULONG>(shared->context.after.load())
        << " receiverAfter=" << static_cast<ULONG>(shared->context.receiverAfter.load())
        << " clientBefore=" << static_cast<ULONG>(shared->context.clientBefore.load())
        << " beforeUp=" << static_cast<ULONG>(shared->context.beforeUp.load())
        << " downEntered=" << shared->context.downEntered.load() << " downReturned=" << shared->context.downReturned.load()
        << " upEntered=" << shared->context.upEntered.load() << " upReturned=" << shared->context.upReturned.load()
        << " clientPoint=" << shared->context.clientX.load() << ',' << shared->context.clientY.load()
        << " captureBefore=" << shared->context.captureBefore.load() << " captureAfterDown=" << shared->context.captureAfterDown.load()
        << " menuMatches=" << shared->matchingRows << " menuEnabled=" << shared->rowEnabled
        << " menuOffscreen=" << shared->rowOffscreen << " invokeQuery=" << static_cast<ULONG>(shared->invokeQuery)
        << " invokePresent=" << shared->invokePresent << " invoked=" << shared->invoked
        << " nativeInvoke=" << static_cast<ULONG>(shared->actionResult) << " popup=" << shared->popup
        << " kernelJoined=1 callbacks=" << executions
        << " emptyToolbarReads=" << shared->failureReadback.emptyToolbarReads
        << " emptyToolbarProofs=" << shared->failureReadback.emptyToolbarProofs
        << " actualMenuRuntimeNonempty=" << !shared->rowRuntime.empty() << std::endl;
    if(action!=S_OK) {
        const auto& read=shared->failureReadback;
        std::cout<<"QAT bounded discovery failure phase="<<read.phase<<" stage="<<read.stage<<" gateMask="<<read.gate
            <<" rawNativeHRESULT="<<static_cast<ULONG>(read.nativeResult)<<" returnedHRESULT="<<static_cast<ULONG>(read.returned)
            <<" node="<<read.node<<" queued="<<read.queued<<" depth="<<read.depth<<" siblings="<<read.siblings
            <<" nameLength="<<read.nameLength<<" nameCopied="<<read.nameCopied
            <<" runtimeDimensions="<<read.runtimeDimensions<<" runtimeFirst="<<read.runtimeFirst<<" runtimeLast="<<read.runtimeLast
            <<" runtimeCount="<<read.runtimeCount<<" process="<<read.process<<" controlType="<<read.controlType
            <<" bounds="<<read.bounds.left<<","<<read.bounds.top<<","<<read.bounds.right<<","<<read.bounds.bottom
            <<" root="<<read.root<<" toolbarMatches="<<read.toolbarCount<<" targetMatches="<<read.targetCount
            <<" menuRows="<<read.menuRows<<" emptyToolbarReads="<<read.emptyToolbarReads
            <<" emptyToolbarProofs="<<read.emptyToolbarProofs<<" kernelJoined=1 limitsChanged=0"<<std::endl;
        std::wcout<<L"QAT bounded discovery actual owned name prefix=\""<<read.namePrefix.data()<<L"\""<<std::endl;
    }

    const auto& identityReceipt=shared->failureReadback;
    std::cout<<"QAT rooted native identity proofs="<<identityReceipt.pathProofs<<" collisions="<<identityReceipt.pathCollisions
        <<" legacyReads="<<identityReceipt.legacyReads<<" legacyNativePairs="<<identityReceipt.legacyNativePairs
        <<" legacyAbsentPairs="<<identityReceipt.legacyAbsentPairs
        <<" lastLegacyPatternHRESULT="<<static_cast<ULONG>(identityReceipt.legacyPattern)
        <<" lastLegacyAccessibleHRESULT="<<static_cast<ULONG>(identityReceipt.legacyAccessible)
        <<" lastLegacyChildHRESULT="<<static_cast<ULONG>(identityReceipt.legacyChild)
        <<" lastLegacyUnknownHRESULT="<<static_cast<ULONG>(identityReceipt.legacyUnknown)
        <<" lastLegacyChildId="<<identityReceipt.legacyChildId<<" lastLegacyNull="<<identityReceipt.legacyNull
        <<" applicationId="<<shared->ticket.applicationCommand<<" physicalId="<<shared->ticket.physicalCommand
        <<" emptyRuntimeIdentity=0 nativePopupVisibilityEpochRequired=1 kernelJoined=1"<<std::endl;
    std::cout<<"QAT actual Legacy wrapper instances distinct="<<identityReceipt.legacyDistinctPairs
        <<" nativeBridgePairs="<<identityReceipt.legacyBridgePairs<<" canonicalEqualityPromoted=0 kernelJoined=1"<<std::endl;
    for(UINT slot=0;slot<identityReceipt.legacyBridge.size();++slot) {
        const auto& value=identityReceipt.legacyBridge[slot];
        std::cout<<"QAT actual Legacy bridge slot="<<slot<<" windowHRESULT="<<static_cast<ULONG>(value.windowRead)
            <<" receiverHRESULT="<<static_cast<ULONG>(value.receiverRead)<<" roleHRESULT="<<static_cast<ULONG>(value.roleRead)
            <<" roleVT="<<value.roleType<<" role="<<value.role<<" childId="<<value.child
            <<" nameHRESULT="<<static_cast<ULONG>(value.nameRead)<<" nameLength="<<value.nameLength
            <<" locationHRESULT="<<static_cast<ULONG>(value.locationRead)<<" stateHRESULT="<<static_cast<ULONG>(value.stateRead)
            <<" stateVT="<<value.stateType<<" state="<<value.state
            <<" identityQueryHRESULT="<<static_cast<ULONG>(value.identityQuery)
            <<" identityReadHRESULT="<<static_cast<ULONG>(value.identityRead)<<" identityBytes="<<value.identityLength
            <<" identityAbsent="<<value.identityAbsent<<" receiverAfterHRESULT="<<static_cast<ULONG>(value.receiverAfter)
            <<" actualHWND="<<value.window<<" bounds="<<value.bounds.left<<","<<value.bounds.top<<","<<value.bounds.right<<","<<value.bounds.bottom
            <<" kernelJoined=1"<<std::endl;
        std::wcout<<L"QAT actual Legacy bridge name slot="<<slot<<L" name=\""<<value.namePrefix.data()<<L"\""<<std::endl;
    }
    // Plain already-read evidence only, after actual kernel exit and join.
    const auto& runtimeInventory=shared->failureReadback;
    std::cout<<"QAT unavailable-runtime inventory reads="<<runtimeInventory.unavailableDiscoveryReads
        <<" samples="<<runtimeInventory.unavailableDiscoverySamples<<" sampleCapacity="<<runtimeInventory.unavailableDiscovery.size()
        <<" truncated="<<runtimeInventory.unavailableDiscoveryTruncated
        <<" blockedIdentityRole="<<runtimeInventory.unavailableIdentityRole<<" kernelJoined=1"<<std::endl;
    const auto printUnavailable=[](const GestureUnavailableRuntimeEvidence& value,UINT sample,UINT role) {
        std::wcout<<L"QAT actual unavailable-runtime metadata sample="<<sample<<L" role="<<role<<L" phase="<<value.phase;
        if(!role)std::wcout<<L" node="<<value.node<<L" depth="<<value.depth;
        std::wcout<<L" controlType="<<value.type<<L" enabled="<<value.enabled
            <<L" offscreen="<<value.offscreen<<L" runtimeUnavailable="<<value.runtimeUnavailable
            <<L" bounds="<<value.bounds.left<<L","<<value.bounds.top<<L","<<value.bounds.right<<L","<<value.bounds.bottom
            <<L" nameLength="<<value.nameLength<<L" nameCopied="<<value.nameCopied<<L" name=\""<<value.namePrefix.data()
            <<L"\" kernelJoined=1"<<std::endl;
    };
    for(UINT index=0;index<runtimeInventory.unavailableDiscoverySamples;++index)
        printUnavailable(runtimeInventory.unavailableDiscovery[index],index,0);
    if(runtimeInventory.unavailableIdentityRole)
        printUnavailable(runtimeInventory.unavailableIdentity,0,runtimeInventory.unavailableIdentityRole);

    if (shared->context.entered.load() && !shared->context.returned.load()) fatal("native callback still active after worker exit");
    dispatch.reset(); // Actual worker and any native callback have both exited.
    isolation(owner, fixtureDeadline);
    require(gestureSourceCurrent(&source, shared->ticket), "Native gesture changed original source/framework generation");
    if (creatorResult != S_OK) exact(creatorResult, "Creator gesture admission failed");
    if (action != S_OK) {
        exactState(original, readNative(ribbon, owner, fixtureDeadline, "unavailable-native-gesture-preserved"));
        if (gestureUnavailableStatus(action)) throw GestureUnavailable("Actual owned native gesture route/pattern unavailable (raw receipts retained)");
        exact(action, "Actual native gesture action failed");
    }
    require(!cancellationSent && shared->context.entered.load() && shared->context.returned.load() &&
        shared->context.before.load() == S_OK && shared->context.after.load() == S_OK && shared->matchingRows == 1,
        "Native gesture lacked one current source-owned context opening");
    require(shared->context.clientBefore.load() == S_OK && shared->context.beforeUp.load() == S_OK &&
        shared->context.downEntered.load() && shared->context.downReturned.load() &&
        shared->context.upEntered.load() && shared->context.upReturned.load(),
        "Native right-click lacked one admitted complete down/up pair");
    require(disabledObservation ? !shared->rowEnabled && !shared->invoked :
        shared->rowEnabled && shared->invokeQuery == S_OK && shared->invokePresent && shared->invoked && shared->actionResult == S_OK,
        "Native gesture/pattern evidence is incomplete");
    materialize(ribbon, owner, fixtureDeadline);
    return readNative(ribbon, owner, fixtureDeadline, "after-native-context-gesture");
}
void gestureAdded(const NativeState& before, const NativeState& after, explorer::NativeRibbon& ribbon, UINT command) {
    require(after.collectionIdentity.Get() == before.collectionIdentity.Get() && after.dock == before.dock &&
        after.rows.size() == before.rows.size() + 1, "Native context Add lost collection/dock/count authority");
    for (size_t index = 0; index < before.rows.size(); ++index)
        require(sameRow(before.rows[index], after.rows[index]), "Native context Add changed an untouched whole row");
    const auto& added = after.rows.back();
    require(added.commandRead == S_OK && added.commandConvert == S_OK && added.command == ribbon.nativeCommandId(command),
        "Native context Add did not append the original command ID");
    require(std::count_if(after.rows.begin(), after.rows.end(), [&](const auto& row) {
        return row.commandRead == S_OK && row.commandConvert == S_OK && row.command == added.command;
    }) == 1, "Native context Add created duplicate IDs");
}
void runGestureLayout(explorer::RibbonLayout layout, ULONGLONG deadline) {
    using namespace explorer;
    GestureConfig config;
    config.toolbar = gestureEnvironment(L"WINDOWSEXPLORER_QAT_TOOLBAR_LABEL", L"Quick Access Toolbar");
    config.add = gestureEnvironment(L"WINDOWSEXPLORER_QAT_ADD_LABEL", L"Add to Quick Access Toolbar");
    config.remove = gestureEnvironment(L"WINDOWSEXPLORER_QAT_REMOVE_LABEL", L"Remove from Quick Access Toolbar");
    Window window{CreateWindowExW(0, L"WindowsExplorerNativeQATRegression", L"Owned real QAT context gestures",
        WS_OVERLAPPEDWINDOW, 40, 40, 1240, 800, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr)};
    require(window.handle != nullptr, "Create exclusively owned private QAT gesture root");
    unsigned executions = 0; NativeRibbon ribbon;
    RibbonCallbacks callbacks;
    callbacks.execute = [&](UINT) { ++executions; return E_ACCESSDENIED; };
    callbacks.executeItem = [&](UINT, UINT) { ++executions; return E_ACCESSDENIED; };
    callbacks.query = [](UINT command) { RibbonCommandState result; result.enabled = command != Copy; return result; };
    exact(ribbon.initialize(window.handle, GetModuleHandleW(nullptr), std::move(callbacks), layout), "Initialize actual gesture framework");
    if (layout == RibbonLayout::InstalledWindows10 && (ribbon.layout() != layout || ribbon.installedLayoutStatus() != S_OK))
        throw GestureUnavailable("Installed native Ribbon unavailable; authored fallback is not gesture coverage");
    ShowWindow(window.handle, SW_SHOWNOACTIVATE); SetActiveWindow(window.handle);
    require(IsWindowVisible(window.handle) && GetActiveWindow() == window.handle, "Present only this private gesture root");
    materialize(ribbon, window.handle, deadline);
    auto initial = readNative(ribbon, window.handle, deadline, "gesture-initial");
    const std::array<UINT, 2> defaults{Properties, NewFolder}; ids(initial, ribbon, defaults);
    auto cut = gesture(ribbon, window.handle, executions, config, Cut, false, false, deadline, "real-add-cut");
    gestureAdded(initial, cut, ribbon, Cut);
    auto removed = gesture(ribbon, window.handle, executions, config, Cut, true, false, deadline, "real-remove-cut");
    exactState(initial, removed);
    cut = gesture(ribbon, window.handle, executions, config, Cut, false, false, deadline, "real-readd-cut");
    gestureAdded(removed, cut, ribbon, Cut);
    auto paste = gesture(ribbon, window.handle, executions, config, Paste, false, false, deadline, "real-add-paste");
    gestureAdded(cut, paste, ribbon, Paste);
    auto withoutCut = gesture(ribbon, window.handle, executions, config, Cut, true, false, deadline, "real-order-remove-cut");
    auto expected = paste; expected.rows.erase(expected.rows.begin() + 2); exactState(expected, withoutCut);
    auto append = gesture(ribbon, window.handle, executions, config, Cut, false, false, deadline, "real-order-append-cut");
    gestureAdded(withoutCut, append, ribbon, Cut);
    const std::array<UINT, 4> appendOrder{Properties, NewFolder, Paste, Cut}; ids(append, ribbon, appendOrder);
    std::cout << "QAT actual-native-context evidence addRemoveVerified=1 untouchedWholeRowsVerified=1 removeReaddOrderVerified=1"
        << " laterCapacityCoverageStillPending=1" << std::endl;
    const std::array<UINT, 20> capacity{Copy, Cut, Paste, CopyPath, PasteShortcut, Rename, NewFolder, Properties, Open, Edit,
        Print, Undo, SelectAll, SelectNone, Invert, PreviewPane, DetailsPane, HiddenItems, Extensions, Checkboxes};
    exact(ribbon.setQuickAccessCommands(std::span<const UINT>(capacity.data(), 19)), "Setup actual19 (not mislabeled gesture additions)");
    materialize(ribbon, window.handle, deadline);
    auto nineteen = readNative(ribbon, window.handle, deadline, "gesture-capacity19");
    ids(nineteen, ribbon, std::span<const UINT>(capacity.data(), 19));
    auto twentyByGesture = gesture(ribbon, window.handle, executions, config, FileHistory, false, false, deadline, "real19-to20-add");
    gestureAdded(nineteen, twentyByGesture, ribbon, FileHistory);
    require(twentyByGesture.rows.size() == 20, "Native19-to20 gesture count incomplete");
    auto historyRemoved = gesture(ribbon, window.handle, executions, config, FileHistory, true, false, deadline, "real20-to19-remove");
    exactState(nineteen, historyRemoved);
    exact(ribbon.setQuickAccessCommands(capacity), "Setup actual20 distinct commands excluding calibrated History");
    materialize(ribbon, window.handle, deadline);
    auto twenty = readNative(ribbon, window.handle, deadline, "gesture-capacity20"); ids(twenty, ribbon, capacity);
    auto rejected = gesture(ribbon, window.handle, executions, config, FileHistory, false, true, deadline, "real-disabled21st-add");
    exactState(twenty, rejected);
    require(executions == 0, "Native QAT gesture invoked an application/Shell command");
    isolation(window.handle, deadline);
    std::cout << "PASS genuine native QAT gestures layout=" << (layout == RibbonLayout::Authored ? "authored" : "installed")
        << " actualAddRemove=1 untouchedWholeRows=1 removeReaddOrder=1 native19to20=1 disabled21stAdd=1"
        << " nativeMoveGesture=NOT_COVERED opaqueSeparator=NOT_COVERED(no actual native QAT separator)" << std::endl;
}
