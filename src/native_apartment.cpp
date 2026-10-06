#include "explorer/native_apartment.hpp"
#include <objbase.h>
#include <ole2.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <iterator>

namespace explorer {
namespace {
// This pointer denotes an explicitly live stack owner. It owns no module at
// thread exit and has no TLS destructor or global process-lifetime pin.
thread_local NativeApartmentOwner* currentOwner=nullptr;
HRESULT lastFailure() noexcept {
    const auto error=GetLastError();return HRESULT_FROM_WIN32(error?error:ERROR_GEN_FAILURE);
}
[[noreturn]] void unsafeCleanup(HRESULT failure) noexcept {
    std::fprintf(stderr,"Native apartment cleanup cannot safely retire HRESULT=0x%08lX.\n",static_cast<ULONG>(failure));
    std::fflush(stderr);TerminateProcess(GetCurrentProcess(),8);std::_Exit(8);
}
}

NativeApartmentOwner::NativeApartmentOwner() noexcept:creator_(GetCurrentThreadId()){}
NativeApartmentOwner::~NativeApartmentOwner(){if(active_){const auto result=finish();if(FAILED(result))unsafeCleanup(result);}}
HRESULT NativeApartmentOwner::initializeOle() noexcept{return initialize(Kind::Ole);}
HRESULT NativeApartmentOwner::initializeSta() noexcept{return initialize(Kind::Sta);}
void NativeApartmentOwner::finishOrTerminate() noexcept {const auto result=finish();if(FAILED(result))unsafeCleanup(result);}
HRESULT NativeApartmentOwner::initialize(Kind kind) noexcept {
    if(GetCurrentThreadId()!=creator_)return RPC_E_WRONG_THREAD;
    if(active_||kind_!=Kind::None)return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
    if(currentOwner&&(currentOwner->finishing_||currentOwner->outer_->finishing_))return E_ABORT;
    APTTYPE beforeType{};APTTYPEQUALIFIER beforeQualifier{};
    const auto before=CoGetApartmentType(&beforeType,&beforeQualifier);
    const bool beforeSta=before==S_OK&&(beforeType==APTTYPE_STA||beforeType==APTTYPE_MAINSTA);
    const bool beforeImplicit=before==S_OK&&beforeType==APTTYPE_MTA&&beforeQualifier==APTTYPEQUALIFIER_IMPLICIT_MTA;
    if(currentOwner&&(!currentOwner->active_||!beforeSta))return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
    nativeResult_=kind==Kind::Ole?OleInitialize(nullptr):CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if(FAILED(nativeResult_))return nativeResult_; // No native count was acquired.
    previous_=currentOwner;kind_=kind;
    if(!previous_&&(before!=CO_E_NOTINITIALIZED&&!beforeImplicit)) {
        // Balance our real successful call, but never adopt another host's
        // existing explicit apartment. First OleInitialize can be S_OK even
        // when a pre-existing unowned CoInitializeEx already owns this STA.
        if(kind==Kind::Ole)OleUninitialize();else CoUninitialize();
        failure_=HRESULT_FROM_WIN32(ERROR_INVALID_STATE);return failure_;
    }
    outer_=previous_?previous_->outer_:this;
    active_=true;currentOwner=this;++outer_->depth_;
    return nativeResult_;
}
HRESULT NativeApartmentOwner::preparePath() noexcept {
    if(path_[0])return S_OK;
    std::array<wchar_t,MAX_PATH> directory{};
    const auto length=GetSystemDirectoryW(directory.data(),static_cast<UINT>(directory.size()));
    if(!length)return lastFailure();
    constexpr wchar_t suffix[]=L"\\ExplorerFrame.dll";
    if(length>=directory.size()||length+std::size(suffix)>path_.size())return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    std::copy_n(directory.begin(),length,path_.begin());
    std::copy(std::begin(suffix),std::end(suffix),path_.begin()+length);
    return S_OK;
}
void NativeApartmentOwner::rememberFailure(HRESULT result) noexcept {if(FAILED(result)&&SUCCEEDED(failure_))failure_=result;}
HRESULT NativeApartmentOwner::currentAdmissionStatus() noexcept {
    const auto context=currentCleanupStatus();if(FAILED(context))return context;
    return currentOwner->outer_->failure_;
}
HRESULT NativeApartmentOwner::currentCleanupStatus() noexcept {
    if(!currentOwner)return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
    const auto outer=currentOwner->outer_;
    if(GetCurrentThreadId()!=outer->creator_)return RPC_E_WRONG_THREAD;
    if(!currentOwner->active_||currentOwner->finishing_||outer->finishing_)return E_ABORT;
    return S_OK;
}
HRESULT NativeApartmentOwner::observeLoadedExplorerFrame() noexcept {
    const auto admitted=currentCleanupStatus();if(FAILED(admitted))return admitted;
    return currentOwner->outer_->observeCode();
}
HRESULT NativeApartmentOwner::observeCode() noexcept {
    if(GetCurrentThreadId()!=creator_)return RPC_E_WRONG_THREAD;
    if(code_)return verifiedCode_?S_OK:failure_;
    const auto prepared=preparePath();if(FAILED(prepared)){rememberFailure(prepared);return prepared;}
    HMODULE actual=nullptr;
    if(!GetModuleHandleExW(0,path_.data(),&actual)) {
        const auto error=GetLastError();
        if(error==ERROR_MOD_NOT_FOUND)return S_FALSE; // Observe absence, never LoadLibrary.
        const auto failure=HRESULT_FROM_WIN32(error?error:ERROR_GEN_FAILURE);
        rememberFailure(failure);
        // After activation we cannot prove safe native Releases without a
        // reference. Stop before releasing providers or uninitializing COM.
        unsafeCleanup(failure);
    }
    code_=actual;++codeAcquisitions_; // Own the result before validation.
    std::array<wchar_t,MAX_PATH> actualPath{};
    const auto length=GetModuleFileNameW(code_,actualPath.data(),static_cast<DWORD>(actualPath.size()));
    MEMORY_BASIC_INFORMATION memory{};
    const auto queried=VirtualQuery(code_,&memory,sizeof(memory));
    const bool verified=!(reinterpret_cast<std::uintptr_t>(code_)&3u)&&length&&length<actualPath.size()&&
        CompareStringOrdinal(path_.data(),-1,actualPath.data(),-1,TRUE)==CSTR_EQUAL&&
        queried==sizeof(memory)&&memory.State==MEM_COMMIT&&memory.Type==MEM_IMAGE&&memory.AllocationBase==code_;
    if(!verified){rememberFailure(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));return failure_;}
    verifiedCode_=true;return S_OK;
}
HRESULT NativeApartmentOwner::finish() noexcept {
    if(GetCurrentThreadId()!=creator_)return RPC_E_WRONG_THREAD;
    if(!active_)return kind_==Kind::None?S_FALSE:HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
    if(currentOwner!=this||finishing_)return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
    if(outer_==this&&clients_)return E_PENDING; // No apartment call/release on rejection.
    finishing_=true;
    if(kind_==Kind::Ole)OleUninitialize();else CoUninitialize();
    // Keep the current owner and strong code reference through the whole real
    // call above, including pumped cleanup. Do not release in a COM spy hook.
    currentOwner=previous_;active_=false;--outer_->depth_;
    if(outer_!=this){finishing_=false;return outer_->failure_;}
    APTTYPE apartment{};APTTYPEQUALIFIER qualifier{};
    const auto remaining=CoGetApartmentType(&apartment,&qualifier);
    postApartmentRead_=remaining;postApartmentType_=static_cast<int>(apartment);postApartmentQualifier_=static_cast<int>(qualifier);
    // An implicit process MTA does not represent an explicit initialization
    // count on this creator (APTTYPEQUALIFIER's documented distinction).
    const bool implicitMta=remaining==S_OK&&apartment==APTTYPE_MTA&&qualifier==APTTYPEQUALIFIER_IMPLICIT_MTA;
    if(remaining!=CO_E_NOTINITIALIZED&&!implicitMta) {
        // An unowned outstanding initialization invalidates the final-boundary
        // contract. Do not force-balance counts or drop the retained module.
        unsafeCleanup(FAILED(remaining)?remaining:HRESULT_FROM_WIN32(ERROR_BUSY));
    }
    if(code_) {
        const auto released=code_;code_=nullptr;
        if(!FreeLibrary(released))unsafeCleanup(lastFailure());
        ++codeReleases_;
    }
    finishing_=false;return failure_;
}
NativeApartmentReadback NativeApartmentOwner::readback() const noexcept {
    NativeApartmentReadback result;result.creator=creator_;
    if(GetCurrentThreadId()!=creator_){result.failure=RPC_E_WRONG_THREAD;return result;}
    result.active=active_;result.finishing=finishing_;
    result.nativeInitialization=nativeResult_;
    result.postApartmentRead=postApartmentRead_;result.postApartmentType=postApartmentType_;result.postApartmentQualifier=postApartmentQualifier_;
    const auto root=outer_?outer_:this;
    result.depth=root->depth_;result.clients=root->clients_;result.heldCode=root->code_;
    result.codeAcquisitions=root->codeAcquisitions_;result.codeReleases=root->codeReleases_;
    result.verifiedCode=root->verifiedCode_;result.failure=root->failure_;return result;
}
HRESULT NativeApartmentClient::acquire() noexcept {
    if(outer_)return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
    const auto admitted=NativeApartmentOwner::currentAdmissionStatus();if(FAILED(admitted))return admitted;
    const auto root=currentOwner->outer_;
    const auto prepared=root->preparePath();if(FAILED(prepared)){root->rememberFailure(prepared);return prepared;}
    outer_=root;++outer_->clients_;return S_OK;
}
HRESULT NativeApartmentClient::observeLoadedCode() noexcept {
    if(!outer_)return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
    if(GetCurrentThreadId()!=outer_->creator_)return RPC_E_WRONG_THREAD;
    return outer_->observeCode();
}
void NativeApartmentClient::beforeNativeRelease() noexcept {
    if(!outer_)return;
    if(GetCurrentThreadId()!=outer_->creator_||!outer_->active_)unsafeCleanup(RPC_E_WRONG_THREAD);
    const auto observed=outer_->observeCode();
    // An acquired normal reference survives a verification failure and the
    // sticky failure is surfaced by the original creator's finish(). Absence
    // observes no executable DLL; it never initiates a load.
    if(FAILED(observed)&&!outer_->code_)unsafeCleanup(observed);
}
NativeApartmentClient::~NativeApartmentClient(){
    if(!outer_)return;
    if(GetCurrentThreadId()!=outer_->creator_||!outer_->active_)unsafeCleanup(RPC_E_WRONG_THREAD);
    // The enclosing native COM fields have now completed their Releases.
    // Observe any method/release-triggered activation before client retirement.
    beforeNativeRelease();
    --outer_->clients_;
}

} // namespace explorer
