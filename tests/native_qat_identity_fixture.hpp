// Included inside the fixture's anonymous namespace after real UIA/native
// readers. These are test-only owned read protocols, never command execution.
struct GestureDescriptor {
    std::wstring name;
    CONTROLTYPEID type=0;
    RECT bounds{};
    bool enabled=false,offscreen=true,runtimeUnavailable=false;
    HWND nativeWindow=nullptr;
    std::vector<int> runtime;
};
GestureDescriptor gestureDescriptor(const GestureElement& value) {
    return {value.name,value.type,value.bounds,value.enabled,value.offscreen,
        value.runtimeUnavailable,value.nativeWindow,value.runtime};
}
template<class Left,class Right>
bool gestureEqualDescriptor(const Left& left,const Right& right) noexcept {
    if(left.name!=right.name||left.type!=right.type||left.enabled!=right.enabled||
       left.offscreen!=right.offscreen||left.nativeWindow!=right.nativeWindow||
       left.runtimeUnavailable!=right.runtimeUnavailable||
       !qat_gesture_protocol::equalRect(left.bounds,right.bounds))return false;
    // Empty arrays are only unavailable-state receipts. The rooted, unique
    // full metadata/native path supplies addressing; no empty-ID equality.
    if(left.runtimeUnavailable)return left.runtime.empty()&&right.runtime.empty();
    return !left.runtime.empty()&&!right.runtime.empty()&&left.runtime==right.runtime;
}
bool gestureEqualReceiver(const qat_gesture_protocol::WindowIdentity& left,
                          const qat_gesture_protocol::WindowIdentity& right) noexcept {
    return left.window==right.window&&left.root==right.root&&left.parent==right.parent&&
        left.process==right.process&&left.thread==right.thread&&
        qat_gesture_protocol::equalRect(left.bounds,right.bounds)&&
        std::wcscmp(left.windowClass.data(),right.windowClass.data())==0;
}
struct GestureActionPath {
    HWND root=nullptr;
    std::vector<GestureDescriptor> nodes; // Actual root -> leaf descriptors.
};
bool gestureEqualPath(const GestureActionPath& left,const GestureActionPath& right) noexcept {
    if(left.root!=right.root||left.nodes.size()!=right.nodes.size()||left.nodes.empty())return false;
    for(size_t index=0;index<left.nodes.size();++index)
        if(!gestureEqualDescriptor(left.nodes[index],right.nodes[index]))return false;
    return true;
}
HRESULT gesturePathDescendant(const std::vector<GestureElement>& rows,size_t leaf,
                             size_t ancestor,bool& result) noexcept {
    result=false;
    if(rows.empty()||rows.size()>512||leaf>=rows.size()||ancestor>=rows.size())return E_UNEXPECTED;
    for(unsigned depth=0;depth<=32;++depth) {
        if(leaf==ancestor){result=true;return S_OK;}
        const auto parent=rows[leaf].parentIndex;
        if(parent==512)return leaf==0?S_OK:E_UNEXPECTED;
        if(parent>=leaf||parent>=rows.size())return E_UNEXPECTED;
        leaf=parent;
    }
    return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
}
HRESULT gestureCapturePath(const std::vector<GestureElement>& rows,size_t leaf,
                          HWND owner,DWORD creator,ULONGLONG deadline,GestureActionPath& output) {
    if(rows.empty()||rows.size()>512||leaf>=rows.size()||!owner||!creator)return E_UNEXPECTED;
    GestureActionPath path;path.root=rows[leaf].walkRoot;
    if(!path.root||!gestureOwnedPopup(path.root,owner,creator)||rows[0].nativeWindow!=path.root)
        return gestureTraceResult(E_ACCESSDENIED);
    std::array<size_t,33> indices{};size_t count=0;
    for(;;) {
        if(GetTickCount64()>=deadline)return gestureTraceResult(HRESULT_FROM_WIN32(ERROR_TIMEOUT),4);
        if(count==indices.size())return gestureTraceResult(HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER),8);
        const auto& row=rows[leaf];
        if(row.walkRoot!=path.root)return gestureTraceResult(E_UNEXPECTED);
        if(row.nativeWindow&&!gestureOwnedPopup(row.nativeWindow,owner,creator))return gestureTraceResult(E_ACCESSDENIED);
        // Each actual parent relation must uniquely address this child among
        // all siblings, including hidden/disabled peers, in this complete walk.
        size_t matches=0;
        for(const auto& peer:rows) {
            if(GetTickCount64()>=deadline)return gestureTraceResult(HRESULT_FROM_WIN32(ERROR_TIMEOUT),4);
            if(peer.parentIndex==row.parentIndex&&gestureEqualDescriptor(peer,row))++matches;
        }
        if(matches!=1){++gestureReadTrace.pathCollisions;return gestureTraceResult(E_UNEXPECTED,256);}
        indices[count++]=leaf;
        if(row.parentIndex==512){if(leaf!=0)return gestureTraceResult(E_UNEXPECTED);break;}
        if(row.parentIndex>=leaf||row.parentIndex>=rows.size())return gestureTraceResult(E_UNEXPECTED);
        leaf=row.parentIndex;
    }
    path.nodes.reserve(count);
    while(count)path.nodes.push_back(gestureDescriptor(rows[indices[--count]]));
    output=std::move(path);return S_OK;
}
struct GestureLegacyIdentity {
    HRESULT patternRead=E_PENDING,accessibleRead=E_PENDING,childRead=E_PENDING,unknownRead=E_PENDING;
    int child=0;
    bool patternAbsent=false,nativeNull=false;
    ComPtr<IUnknown> canonical;
    ComPtr<IAccessible> accessible; // Owned only on the original MTA.
};
HRESULT gestureLegacyRead(IUIAutomationElement* element,ULONGLONG deadline,GestureLegacyIdentity& output) {
    if(!element)return E_NOINTERFACE;
    if(GetTickCount64()>=deadline)return gestureTraceResult(HRESULT_FROM_WIN32(ERROR_TIMEOUT),4);
    GestureLegacyIdentity value;ComPtr<IUIAutomationLegacyIAccessiblePattern> pattern;
    gestureReadTrace.stage=80;++gestureReadTrace.legacyReads;
    value.patternRead=element->GetCurrentPatternAs(UIA_LegacyIAccessiblePatternId,IID_PPV_ARGS(&pattern));
    gestureReadTrace.nativeResult=value.patternRead;gestureReadTrace.legacyPattern=value.patternRead;
    gestureReadTrace.legacyAccessible=gestureReadTrace.legacyChild=gestureReadTrace.legacyUnknown=E_PENDING;
    gestureReadTrace.legacyChildId=0;gestureReadTrace.legacyNull=false;
    if(gestureUnavailableStatus(value.patternRead)&&!pattern) {
        value.patternAbsent=true;output=std::move(value);return S_OK;
    }
    if(value.patternRead!=S_OK||!pattern)return gestureTraceResult(value.patternRead==S_OK?E_UNEXPECTED:value.patternRead);
    if(GetTickCount64()>=deadline)return gestureTraceResult(HRESULT_FROM_WIN32(ERROR_TIMEOUT),4);
    gestureReadTrace.stage=81;
    value.childRead=pattern->get_CurrentChildId(&value.child);
    gestureReadTrace.nativeResult=value.childRead;gestureReadTrace.legacyChild=value.childRead;gestureReadTrace.legacyChildId=value.child;
    if(value.childRead!=S_OK)return gestureTraceResult(value.childRead);
    if(value.child<0)return gestureTraceResult(E_UNEXPECTED);
    if(GetTickCount64()>=deadline)return gestureTraceResult(HRESULT_FROM_WIN32(ERROR_TIMEOUT),4);
    ComPtr<IAccessible> accessible;gestureReadTrace.stage=82;
    value.accessibleRead=pattern->GetIAccessible(&accessible);
    gestureReadTrace.nativeResult=value.accessibleRead;gestureReadTrace.legacyAccessible=value.accessibleRead;
    if(value.accessibleRead!=S_OK)return gestureTraceResult(value.accessibleRead);
    value.nativeNull=!accessible;gestureReadTrace.legacyNull=value.nativeNull;
    if(accessible) {
        if(GetTickCount64()>=deadline)return gestureTraceResult(HRESULT_FROM_WIN32(ERROR_TIMEOUT),4);
        gestureReadTrace.stage=83;value.unknownRead=accessible.As(&value.canonical);
        gestureReadTrace.nativeResult=value.unknownRead;gestureReadTrace.legacyUnknown=value.unknownRead;
        if(value.unknownRead!=S_OK||!value.canonical)return gestureTraceResult(value.unknownRead==S_OK?E_UNEXPECTED:value.unknownRead);
        value.accessible=std::move(accessible);
    }
    // Native UIA controls may have no Legacy pattern. Bridges/proxies may
    // return S_OK/null IAccessible. Preserve that distinction; never fabricate
    // a canonical COM identity or claim the optional witness was available.
    output=std::move(value);return S_OK;
}
struct GestureIdentityBytes {
    BYTE* value=nullptr;
    ~GestureIdentityBytes(){CoTaskMemFree(value);}
};
struct GestureBridgeValue {
    GestureLegacyBridgeEvidence evidence;
    std::wstring name;
    std::vector<BYTE> identity;
};
HRESULT gestureBridgeWindowCurrent(const qat_gesture_protocol::WindowIdentity& expected,
                                  HWND owner,DWORD creator,ULONGLONG deadline) {
    if(GetTickCount64()>=deadline)return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    if(!gestureOwnedPopup(expected.window,owner,creator))return E_ACCESSDENIED;
    qat_gesture_protocol::WindowIdentity current;
    current.window=expected.window;current.root=GetAncestor(expected.window,GA_ROOT);
    current.parent=GetAncestor(expected.window,GA_PARENT);
    current.thread=GetWindowThreadProcessId(expected.window,&current.process);
    SetLastError(ERROR_SUCCESS);
    if(!GetWindowRect(expected.window,&current.bounds))return qat_gesture_protocol::nativeError();
    SetLastError(ERROR_SUCCESS);
    const auto length=GetClassNameW(expected.window,current.windowClass.data(),static_cast<int>(current.windowClass.size()));
    if(!length)return qat_gesture_protocol::nativeError();
    if(length==static_cast<int>(current.windowClass.size())-1)return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    if(current.thread!=creator||current.process!=GetCurrentProcessId()||
       !gestureEqualReceiver(current,expected)||!gestureOwnedPopup(current.window,owner,creator))return E_ABORT;
    return GetTickCount64()<deadline?S_OK:HRESULT_FROM_WIN32(ERROR_TIMEOUT);
}
HRESULT gestureBridgeRead(const GestureLegacyIdentity& legacy,const GestureElement& expected,
                         const qat_gesture_protocol::WindowIdentity& receiver,
                         HWND owner,DWORD creator,ULONGLONG deadline,UINT slot,GestureBridgeValue& output) {
    if(slot>=gestureReadTrace.legacyBridge.size()||!legacy.accessible||!legacy.canonical||legacy.child<0)
        return gestureTraceResult(E_UNEXPECTED);
    GestureBridgeValue value;
    auto& receipt=gestureReadTrace.legacyBridge[slot];receipt={};receipt.child=legacy.child;
    const auto before=[&](UINT stage){gestureReadTrace.stage=stage;
        return GetTickCount64()<deadline?S_OK:gestureTraceResult(HRESULT_FROM_WIN32(ERROR_TIMEOUT),4);};
    auto hr=before(90);if(hr!=S_OK)return hr;
    receipt.windowRead=WindowFromAccessibleObject(legacy.accessible.Get(),&receipt.window);
    gestureReadTrace.nativeResult=receipt.windowRead;
    if(receipt.windowRead!=S_OK||!receipt.window)return gestureTraceResult(receipt.windowRead==S_OK?E_UNEXPECTED:receipt.windowRead);
    if(receipt.window!=receiver.window)return gestureTraceResult(E_ABORT);
    receipt.receiverRead=gestureBridgeWindowCurrent(receiver,owner,creator,deadline);
    gestureReadTrace.nativeResult=receipt.receiverRead;if(receipt.receiverRead!=S_OK)return gestureTraceResult(receipt.receiverRead);
    VARIANT child{};child.vt=VT_I4;child.lVal=legacy.child;
    struct NativeVariant {VARIANT value{};~NativeVariant(){VariantClear(&value);}} role,state;
    hr=before(91);if(hr!=S_OK)return hr;
    receipt.roleRead=legacy.accessible->get_accRole(child,&role.value);gestureReadTrace.nativeResult=receipt.roleRead;
    receipt.roleType=role.value.vt;
    if(receipt.roleRead!=S_OK)return gestureTraceResult(receipt.roleRead);
    if(role.value.vt!=VT_I4)return gestureTraceResult(E_UNEXPECTED);
    receipt.role=role.value.lVal;
    const LONG expectedRole=expected.type==UIA_ButtonControlTypeId?ROLE_SYSTEM_PUSHBUTTON:
        expected.type==UIA_SplitButtonControlTypeId?ROLE_SYSTEM_SPLITBUTTON:
        expected.type==UIA_MenuItemControlTypeId?ROLE_SYSTEM_MENUITEM:0;
    if(!expectedRole||receipt.role!=expectedRole)return gestureTraceResult(E_ABORT);
    hr=before(92);if(hr!=S_OK)return hr;
    GestureBstr name;receipt.nameRead=legacy.accessible->get_accName(child,&name.value);
    gestureReadTrace.nativeResult=receipt.nameRead;
    if(receipt.nameRead!=S_OK)return gestureTraceResult(receipt.nameRead);
    receipt.nameLength=name.value?SysStringLen(name.value):0;
    if(!name.value||!receipt.nameLength||receipt.nameLength>256)return gestureTraceResult(E_UNEXPECTED);
    std::copy_n(name.value,receipt.nameLength,receipt.namePrefix.begin());
    value.name.assign(name.value,receipt.nameLength);
    if(value.name!=expected.name)return gestureTraceResult(E_ABORT);
    hr=before(93);if(hr!=S_OK)return hr;
    LONG left=0,top=0,width=0,height=0;
    receipt.locationRead=legacy.accessible->accLocation(&left,&top,&width,&height,child);
    gestureReadTrace.nativeResult=receipt.locationRead;
    if(receipt.locationRead!=S_OK)return gestureTraceResult(receipt.locationRead);
    const auto right=static_cast<long long>(left)+width,bottom=static_cast<long long>(top)+height;
    if(width<=0||height<=0||right>LONG_MAX||right<LONG_MIN||bottom>LONG_MAX||bottom<LONG_MIN)
        return gestureTraceResult(E_UNEXPECTED);
    receipt.bounds={left,top,static_cast<LONG>(right),static_cast<LONG>(bottom)};
    if(!qat_gesture_protocol::equalRect(receipt.bounds,expected.bounds))return gestureTraceResult(E_ABORT);
    hr=before(94);if(hr!=S_OK)return hr;
    receipt.stateRead=legacy.accessible->get_accState(child,&state.value);gestureReadTrace.nativeResult=receipt.stateRead;
    receipt.stateType=state.value.vt;
    if(receipt.stateRead!=S_OK)return gestureTraceResult(receipt.stateRead);
    if(state.value.vt!=VT_I4)return gestureTraceResult(E_UNEXPECTED);
    receipt.state=state.value.lVal;
    if(((receipt.state&STATE_SYSTEM_UNAVAILABLE)==0)!=expected.enabled||
       ((receipt.state&(STATE_SYSTEM_INVISIBLE|STATE_SYSTEM_OFFSCREEN))!=0)!=expected.offscreen)
        return gestureTraceResult(E_ABORT);
    hr=before(95);if(hr!=S_OK)return hr;
    ComPtr<IAccIdentity> identity;
    receipt.identityQuery=legacy.accessible.As(&identity);gestureReadTrace.nativeResult=receipt.identityQuery;
    if(receipt.identityQuery==E_NOINTERFACE&&!identity)receipt.identityAbsent=true;
    else {
        if(receipt.identityQuery!=S_OK||!identity)return gestureTraceResult(receipt.identityQuery==S_OK?E_UNEXPECTED:receipt.identityQuery);
        hr=before(96);if(hr!=S_OK)return hr;
        GestureIdentityBytes bytes;
        receipt.identityRead=identity->GetIdentityString(static_cast<DWORD>(legacy.child),&bytes.value,&receipt.identityLength);
        gestureReadTrace.nativeResult=receipt.identityRead;
        if(receipt.identityRead!=S_OK)return gestureTraceResult(receipt.identityRead);
        if(!bytes.value||!receipt.identityLength||receipt.identityLength>512)return gestureTraceResult(E_UNEXPECTED);
        value.identity.assign(bytes.value,bytes.value+receipt.identityLength);
    }
    hr=before(97);if(hr!=S_OK)return hr;
    receipt.receiverAfter=gestureBridgeWindowCurrent(receiver,owner,creator,deadline);
    gestureReadTrace.nativeResult=receipt.receiverAfter;if(receipt.receiverAfter!=S_OK)return gestureTraceResult(receipt.receiverAfter);
    value.evidence=receipt;output=std::move(value);return S_OK;
}
HRESULT gestureLegacyPair(IUIAutomation* automation,const GestureElement& retained,const GestureElement& reacquired,
                          const qat_gesture_protocol::WindowIdentity& retainedReceiver,
                          const qat_gesture_protocol::WindowIdentity& currentReceiver,
                          HWND owner,DWORD creator,ULONGLONG deadline) {
    if(!automation)return gestureTraceResult(E_NOINTERFACE);
    GestureLegacyIdentity left,right;
    auto hr=gestureLegacyRead(retained.element.Get(),deadline,left);if(hr!=S_OK)return hr;
    hr=gestureLegacyRead(reacquired.element.Get(),deadline,right);if(hr!=S_OK)return hr;
    if(left.patternRead!=right.patternRead||left.patternAbsent!=right.patternAbsent||
       left.accessibleRead!=right.accessibleRead||left.childRead!=right.childRead||
       left.child!=right.child||left.nativeNull!=right.nativeNull||left.unknownRead!=right.unknownRead)
        return gestureTraceResult(E_ABORT);
    if(left.canonical||right.canonical) {
        if(!left.canonical||!right.canonical)return gestureTraceResult(E_ABORT);
        if(left.canonical.Get()==right.canonical.Get()){++gestureReadTrace.legacyNativePairs;return S_OK;}
        ++gestureReadTrace.legacyDistinctPairs;
        // Only the original completed, collision-free rooted reacquisition may
        // use this narrow unavailable-RuntimeId native bridge protocol. Different
        // COM wrapper instances are never promoted to canonical COM equality.
        if(!retained.runtimeUnavailable||!reacquired.runtimeUnavailable||
           !retained.runtime.empty()||!reacquired.runtime.empty()||
           !retained.walkRoot||retained.walkRoot!=reacquired.walkRoot||
           !gestureEqualDescriptor(retained,reacquired)||!gestureEqualReceiver(retainedReceiver,currentReceiver))
            return gestureTraceResult(E_ABORT);
        GestureBridgeValue a,b;
        hr=gestureBridgeRead(left,retained,retainedReceiver,owner,creator,deadline,0,a);if(hr!=S_OK)return hr;
        hr=gestureBridgeRead(right,reacquired,currentReceiver,owner,creator,deadline,1,b);if(hr!=S_OK)return hr;
        if(a.evidence.window!=b.evidence.window||a.evidence.child!=b.evidence.child||
           a.evidence.role!=b.evidence.role||a.name!=b.name||a.evidence.state!=b.evidence.state||
           !qat_gesture_protocol::equalRect(a.evidence.bounds,b.evidence.bounds)||
           a.evidence.identityQuery!=b.evidence.identityQuery||a.evidence.identityAbsent!=b.evidence.identityAbsent||
           a.evidence.identityRead!=b.evidence.identityRead||a.identity!=b.identity)return gestureTraceResult(E_ABORT);
        // Release all retained native bridge owners before the last admission
        // reads: external Release may itself pump replacement/destruction.
        left.accessible.Reset();left.canonical.Reset();
        right.accessible.Reset();right.canonical.Reset();
        // Every Legacy property call and Release can cross a pumping boundary.
        // Re-read both genuine leaves and receivers after all native releases.
        if(GetTickCount64()>=deadline)return gestureTraceResult(HRESULT_FROM_WIN32(ERROR_TIMEOUT),4);
        GestureElement retainedAfter,currentAfter;
        hr=gestureElementRead(retained.element.Get(),retainedAfter,true);if(hr!=S_OK)return hr;
        if(!gestureEqualDescriptor(retainedAfter,retained))return gestureTraceResult(E_ABORT);
        if(GetTickCount64()>=deadline)return gestureTraceResult(HRESULT_FROM_WIN32(ERROR_TIMEOUT),4);
        hr=gestureElementRead(reacquired.element.Get(),currentAfter,true);if(hr!=S_OK)return hr;
        if(!gestureEqualDescriptor(currentAfter,reacquired))return gestureTraceResult(E_ABORT);
        if(GetTickCount64()>=deadline)return gestureTraceResult(HRESULT_FROM_WIN32(ERROR_TIMEOUT),4);
        qat_gesture_protocol::WindowIdentity retainedAfterReceiver,currentAfterReceiver;
        HWND retainedAfterWindow=nullptr,currentAfterWindow=nullptr;
        hr=gestureMenuReceiver(automation,retained.element.Get(),owner,creator,retained.bounds,retainedAfterWindow,&retainedAfterReceiver);
        if(hr!=S_OK)return hr;
        if(GetTickCount64()>=deadline)return gestureTraceResult(HRESULT_FROM_WIN32(ERROR_TIMEOUT),4);
        hr=gestureMenuReceiver(automation,reacquired.element.Get(),owner,creator,reacquired.bounds,currentAfterWindow,&currentAfterReceiver);
        if(hr!=S_OK)return hr;
        if(!gestureEqualReceiver(retainedAfterReceiver,retainedReceiver)||
           !gestureEqualReceiver(currentAfterReceiver,currentReceiver))return gestureTraceResult(E_ABORT);
        ++gestureReadTrace.legacyBridgePairs;
    } else ++gestureReadTrace.legacyAbsentPairs;
    return S_OK;
}
HRESULT gestureReacquire(IUIAutomation* automation,const std::vector<GestureElement>& expectedRows,size_t expectedIndex,
                        HWND owner,DWORD creator,ULONGLONG deadline,
                        std::vector<GestureElement>& output,size_t& outputIndex) {
    if(!automation||expectedIndex>=expectedRows.size())return E_INVALIDARG;
    const auto& expected=expectedRows[expectedIndex];
    if(expected.offscreen||expected.bounds.left>=expected.bounds.right||expected.bounds.top>=expected.bounds.bottom)
        return gestureTraceResult(E_ACCESSDENIED);
    GestureActionPath expectedPath;
    auto hr=gestureCapturePath(expectedRows,expectedIndex,owner,creator,deadline,expectedPath);if(hr!=S_OK)return hr;
    ComPtr<IUIAutomationElement> root;
    if(GetTickCount64()>=deadline)return gestureTraceResult(HRESULT_FROM_WIN32(ERROR_TIMEOUT),4);
    gestureReadTrace.stage=84;
    hr=automation->ElementFromHandle(expectedPath.root,&root);gestureReadTrace.nativeResult=hr;
    if(hr!=S_OK||!root)return gestureTraceResult(hr==S_OK?E_NOINTERFACE:hr);
    std::vector<GestureElement> rows;
    hr=gestureWalk(automation,root.Get(),deadline,rows,expectedPath.root);if(hr!=S_OK)return hr;
    size_t matches=0,selected=512;
    const auto expectedDescriptor=gestureDescriptor(expected);
    for(size_t index=0;index<rows.size();++index) {
        if(GetTickCount64()>=deadline)return gestureTraceResult(HRESULT_FROM_WIN32(ERROR_TIMEOUT),4);
        if(!gestureEqualDescriptor(rows[index],expectedDescriptor))continue;
        GestureActionPath path;hr=gestureCapturePath(rows,index,owner,creator,deadline,path);if(hr!=S_OK)return hr;
        if(gestureEqualPath(expectedPath,path)){++matches;selected=index;}
    }
    if(matches!=1){++gestureReadTrace.pathCollisions;return gestureTraceResult(E_ABORT,256);}
    GestureElement retained;
    hr=gestureElementRead(expected.element.Get(),retained,true);if(hr!=S_OK)return hr;
    if(!gestureEqualDescriptor(retained,expectedDescriptor))return gestureTraceResult(E_ABORT);
    if(!expected.runtimeUnavailable) {
        if(GetTickCount64()>=deadline)return gestureTraceResult(HRESULT_FROM_WIN32(ERROR_TIMEOUT),4);
        BOOL same=FALSE;gestureReadTrace.stage=85;
        hr=automation->CompareElements(expected.element.Get(),rows[selected].element.Get(),&same);gestureReadTrace.nativeResult=hr;
        if(hr!=S_OK||!same)return gestureTraceResult(hr==S_OK?E_ABORT:hr);
    }
    qat_gesture_protocol::WindowIdentity retainedReceiver,currentReceiver;
    HWND retainedWindow=nullptr,currentWindow=nullptr;
    hr=gestureMenuReceiver(automation,expected.element.Get(),owner,creator,expected.bounds,retainedWindow,&retainedReceiver);
    if(hr!=S_OK)return hr;
    hr=gestureMenuReceiver(automation,rows[selected].element.Get(),owner,creator,rows[selected].bounds,currentWindow,&currentReceiver);
    if(hr!=S_OK)return hr;
    if(retainedWindow!=currentWindow||!gestureEqualReceiver(retainedReceiver,currentReceiver))return gestureTraceResult(E_ABORT);
    hr=gestureLegacyPair(automation,expected,rows[selected],retainedReceiver,currentReceiver,owner,creator,deadline);if(hr!=S_OK)return hr;
    // All successful branches release their native Legacy owners on return.
    // Preserve the original post-Legacy-release native receiver fence too.
    if(GetTickCount64()>=deadline)return gestureTraceResult(HRESULT_FROM_WIN32(ERROR_TIMEOUT),4);
    qat_gesture_protocol::WindowIdentity retainedFinalReceiver,currentFinalReceiver;
    HWND retainedFinalWindow=nullptr,currentFinalWindow=nullptr;
    hr=gestureMenuReceiver(automation,expected.element.Get(),owner,creator,expected.bounds,retainedFinalWindow,&retainedFinalReceiver);
    if(hr!=S_OK)return hr;
    if(GetTickCount64()>=deadline)return gestureTraceResult(HRESULT_FROM_WIN32(ERROR_TIMEOUT),4);
    hr=gestureMenuReceiver(automation,rows[selected].element.Get(),owner,creator,rows[selected].bounds,currentFinalWindow,&currentFinalReceiver);
    if(hr!=S_OK)return hr;
    if(retainedFinalWindow!=retainedWindow||currentFinalWindow!=currentWindow||
       !gestureEqualReceiver(retainedFinalReceiver,retainedReceiver)||!gestureEqualReceiver(currentFinalReceiver,currentReceiver))
        return gestureTraceResult(E_ABORT);
    if(GetTickCount64()>=deadline)return gestureTraceResult(HRESULT_FROM_WIN32(ERROR_TIMEOUT),4);
    ++gestureReadTrace.pathProofs;
    output=std::move(rows);outputIndex=selected;return gestureTraceResult(S_OK);
}
bool gestureNewPopup(const GestureWindows& before,HWND window,HWND owner,DWORD creator) noexcept {
    if(window==owner||!gestureOwnedPopup(window,owner,creator))return false;
    const auto old=std::find(before.handles.begin(),before.handles.begin()+before.count,window);
    return old==before.handles.begin()+before.count||!before.visible[static_cast<size_t>(old-before.handles.begin())];
}
