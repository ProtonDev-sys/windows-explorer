#pragma once

// Test-only original Shell view event admission. Navigation completion and an
// item count do not substitute for the actual native EnumDone notification.
#include <windows.h>
#include <shobjidl.h>
#include <shldisp.h>
#include <shdispid.h>
#include <ocidl.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <atomic>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <utility>

namespace native_view_fixture {
class ActualEnumeration final {
    struct Record {
        const DWORD creator;
        explicit Record(DWORD originalCreator):creator(originalCreator){}
        std::atomic<HRESULT> status{S_OK};
        std::atomic<UINT> enumDone{0},selectionChanged{0};
        std::atomic<ULONGLONG> lastEnumTick{0};
        std::atomic<DISPID> lastMember{DISPID_UNKNOWN};
        std::atomic<WORD> lastFlags{0};
        std::atomic<UINT> lastArguments{MAXDWORD},lastNamedArguments{MAXDWORD};
        std::atomic<bool> retired{false};
    };
    class Sink final : public Microsoft::WRL::RuntimeClass<
        Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,DShellFolderViewEvents> {
        std::shared_ptr<Record> record_;
    public:
        explicit Sink(std::shared_ptr<Record> record):record_(std::move(record)){}
        HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* count) override {
            if(!count)return E_POINTER;*count=0;return S_OK;
        }
        HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT,LCID,ITypeInfo** info) override {
            if(!info)return E_POINTER;*info=nullptr;return DISP_E_BADINDEX;
        }
        HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID,LPOLESTR*,UINT,LCID,DISPID*) override {
            return DISP_E_UNKNOWNNAME;
        }
        HRESULT STDMETHODCALLTYPE Invoke(DISPID member,REFIID iid,LCID,WORD flags,
            DISPPARAMS* parameters,VARIANT* result,EXCEPINFO*,UINT*) override {
            // No COM calls, selection, native menu operations, or logging from
            // this callback. The shared record owns plain atomic values only.
            if(result){VariantInit(result);result->vt=VT_BOOL;result->boolVal=VARIANT_TRUE;}
            if(GetCurrentThreadId()!=record_->creator) {
                record_->status.store(RPC_E_WRONG_THREAD);return RPC_E_WRONG_THREAD;
            }
            if(record_->retired.load()){record_->status.store(E_ABORT);return E_ABORT;}
            if(member!=DISPID_FILELISTENUMDONE&&member!=DISPID_SELECTIONCHANGED)return S_OK;
            record_->lastMember.store(member);record_->lastFlags.store(flags);
            record_->lastArguments.store(parameters?parameters->cArgs:MAXDWORD);
            record_->lastNamedArguments.store(parameters?parameters->cNamedArgs:MAXDWORD);
            if(iid!=IID_NULL||flags!=DISPATCH_METHOD||!parameters||parameters->cArgs!=0||parameters->cNamedArgs!=0) {
                record_->status.store(DISP_E_BADPARAMCOUNT);return DISP_E_BADPARAMCOUNT;
            }
            if(member==DISPID_FILELISTENUMDONE) {
                if(record_->enumDone.load()==(std::numeric_limits<UINT>::max)()){record_->status.store(E_UNEXPECTED);return E_UNEXPECTED;}
                record_->lastEnumTick.store(GetTickCount64());record_->enumDone.fetch_add(1);
            }else {
                if(record_->selectionChanged.load()==(std::numeric_limits<UINT>::max)()){record_->status.store(E_UNEXPECTED);return E_UNEXPECTED;}
                record_->selectionChanged.fetch_add(1);
            }
            return S_OK;
        }
    };
    bool attempted_=false;
    const DWORD creator_=GetCurrentThreadId();
    DWORD cookie_=0;
    std::atomic<HRESULT> rejection_{S_OK};
    HRESULT attachment_=E_PENDING;
    const char* attachmentStage_="unattempted";
    Microsoft::WRL::ComPtr<IShellView> view_;
    Microsoft::WRL::ComPtr<IConnectionPoint> point_;
    Microsoft::WRL::ComPtr<Sink> sink_;
    std::shared_ptr<Record> record_;
    static void terminate(const char* phase,HRESULT status) noexcept {
        std::cerr<<"FAIL: actual native enumeration observer teardown phase="<<phase
            <<" HRESULT="<<static_cast<unsigned long>(status)<<'\n'<<std::flush;
        TerminateProcess(GetCurrentProcess(),8);std::terminate();
    }
    HRESULT reject(HRESULT status) noexcept {
        HRESULT accepted=S_OK;
        rejection_.compare_exchange_strong(accepted,status);
        return status; // Sticky first rejection; no attachment can heal it.
    }
public:
    ActualEnumeration()=default;
    ActualEnumeration(const ActualEnumeration&)=delete;
    ActualEnumeration& operator=(const ActualEnumeration&)=delete;
    ~ActualEnumeration(){detachOrTerminate();}
    HRESULT attach(IShellView* originalView) noexcept {
        // Construction fixes creator ownership. Reject foreign callbacks
        // before retaining the supplied view, allocating its record or COM.
        if(GetCurrentThreadId()!=creator_)return reject(RPC_E_WRONG_THREAD);
        if(const auto rejected=rejection_.load();rejected!=S_OK)return rejected;
        if(attempted_) {
            if(record_)record_->status.store(E_UNEXPECTED);
            return reject(E_UNEXPECTED); // Duplicate original-view notification is permanent failure.
        }
        attempted_=true;
        if(!originalView)return attachment_=E_POINTER;
        try {
            record_=std::make_shared<Record>(creator_);view_=originalView;
            Microsoft::WRL::ComPtr<IDispatch> automation;
            attachmentStage_="GetItemObject.SVGIO_BACKGROUND.IDispatch";
            attachment_=originalView->GetItemObject(SVGIO_BACKGROUND,IID_PPV_ARGS(&automation));
            if(attachment_!=S_OK||!automation)return attachment_=FAILED(attachment_)?attachment_:E_UNEXPECTED;
            Microsoft::WRL::ComPtr<IConnectionPointContainer> container;
            attachmentStage_="QI.IConnectionPointContainer";
            attachment_=automation.As(&container);
            if(attachment_!=S_OK||!container)return attachment_=FAILED(attachment_)?attachment_:E_UNEXPECTED;
            attachmentStage_="FindConnectionPoint.DShellFolderViewEvents";
            attachment_=container->FindConnectionPoint(DIID_DShellFolderViewEvents,&point_);
            if(attachment_!=S_OK||!point_)return attachment_=FAILED(attachment_)?attachment_:E_UNEXPECTED;
            sink_=Microsoft::WRL::Make<Sink>(record_);
            if(!sink_)return attachment_=E_OUTOFMEMORY;
            attachmentStage_="Advise";DWORD actualCookie=0;
            attachment_=point_->Advise(sink_.Get(),&actualCookie);
            if(attachment_!=S_OK||actualCookie==0) {
                // Failed Advise cannot grant authority to Unadvise a cookie.
                // An inconsistent nonzero output is rejected before touching
                // any unproved subscription or releasing its native owner.
                if(actualCookie!=0)terminate("Advise-unproved-cookie",attachment_);
                return attachment_=FAILED(attachment_)?attachment_:E_UNEXPECTED;
            }
            cookie_=actualCookie;attachmentStage_="attached";
            return attachment_=S_OK;
        }catch(const std::bad_alloc&){return attachment_=E_OUTOFMEMORY;}
         catch(...){return attachment_=E_FAIL;}
    }
    UINT completedCount() const noexcept {return record_?record_->enumDone.load():0;}
    HRESULT attachmentStatus() const noexcept {
        const auto rejected=rejection_.load();return rejected==S_OK?attachment_:rejected;
    }
    bool completed(IShellView* originalView) const noexcept {
        return GetCurrentThreadId()==creator_&&rejection_.load()==S_OK&&
            attachment_==S_OK&&cookie_&&originalView==view_.Get()&&
            record_&&!record_->retired.load()&&record_->status.load()==S_OK&&completedCount()!=0;
    }
    void emit(const char* fixture,const char* phase) const {
        std::cout<<"ActualNativeEnumeration fixture="<<fixture<<" phase="<<phase
            <<" attachmentStage="<<attachmentStage_<<" attachmentHRESULT="<<static_cast<unsigned long>(attachment_)
            <<" stickyRejectionHRESULT="<<static_cast<unsigned long>(rejection_.load())<<" creatorTID="<<creator_
            <<" currentTID="<<GetCurrentThreadId()<<" originalView="<<reinterpret_cast<uintptr_t>(view_.Get())
            <<" cookie="<<cookie_<<" enumDone="<<completedCount()
            <<" selectionChanged="<<(record_?record_->selectionChanged.load():0)
            <<" callbackHRESULT="<<static_cast<unsigned long>(record_?record_->status.load():E_PENDING)
            <<" lastMember="<<(record_?record_->lastMember.load():DISPID_UNKNOWN)
            <<" lastFlags="<<(record_?record_->lastFlags.load():0)
            <<" lastArgs="<<(record_?record_->lastArguments.load():MAXDWORD)
            <<" lastNamedArgs="<<(record_?record_->lastNamedArguments.load():MAXDWORD)
            <<" lastEnumTick="<<(record_?record_->lastEnumTick.load():0)<<'\n'<<std::flush;
    }
    void detachOrTerminate() noexcept {
        if(!attempted_)return;
        if(GetCurrentThreadId()!=creator_)terminate("creator",RPC_E_WRONG_THREAD);
        if(cookie_) {
            const auto status=point_?point_->Unadvise(cookie_):E_UNEXPECTED;
            if(status!=S_OK)terminate("Unadvise",status);
            cookie_=0;
        }
        if(record_)record_->retired.store(true);
        point_.Reset();sink_.Reset();view_.Reset();record_.reset();
        // Keep attempted_ true for the entire lifetime: teardown cannot
        // grant a later attachment or erase a duplicate/source rejection.
    }
};
}
