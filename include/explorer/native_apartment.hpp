#pragma once
#include <windows.h>
#include <array>

namespace explorer {

struct NativeApartmentReadback {
    DWORD creator=0;
    unsigned depth=0,clients=0,codeAcquisitions=0,codeReleases=0;
    bool active=false,finishing=false,verifiedCode=false;
    HMODULE heldCode=nullptr;
    HRESULT nativeInitialization=E_PENDING,failure=S_OK;
    HRESULT postApartmentRead=E_PENDING;
    int postApartmentType=-1,postApartmentQualifier=-1;
};

// Owns one real creator-STA initialization. Nested owned scopes share the
// outer executable-code lease and balance their own exact Co/Ole call.
// Finish after native clients/interfaces and HWNDs are retired. Only the
// outer matching uninitialize's return permits the single code release.
class NativeApartmentOwner final {
public:
    NativeApartmentOwner() noexcept;
    ~NativeApartmentOwner();
    NativeApartmentOwner(const NativeApartmentOwner&)=delete;
    NativeApartmentOwner& operator=(const NativeApartmentOwner&)=delete;
    HRESULT initializeOle() noexcept;
    HRESULT initializeSta() noexcept;
    HRESULT finish() noexcept;
    void finishOrTerminate() noexcept;
    HRESULT nativeInitializationResult() const noexcept{return GetCurrentThreadId()==creator_?nativeResult_:RPC_E_WRONG_THREAD;}
    NativeApartmentReadback readback() const noexcept;
    static HRESULT currentAdmissionStatus() noexcept;
    static HRESULT currentCleanupStatus() noexcept;
    // Call only after actual native activation/methods while a real native
    // owner still exists. No DLL is loaded, scanned or permanently pinned.
    static HRESULT observeLoadedExplorerFrame() noexcept;
private:
    friend class NativeApartmentClient;
    enum class Kind {None,Ole,Sta};
    HRESULT initialize(Kind kind) noexcept;
    HRESULT preparePath() noexcept;
    HRESULT observeCode() noexcept;
    void rememberFailure(HRESULT result) noexcept;
    const DWORD creator_=0;
    Kind kind_=Kind::None;
    HRESULT nativeResult_=E_PENDING,failure_=S_OK;
    HRESULT postApartmentRead_=E_PENDING;
    int postApartmentType_=-1,postApartmentQualifier_=-1;
    NativeApartmentOwner* previous_=nullptr;
    NativeApartmentOwner* outer_=nullptr;
    bool active_=false,finishing_=false,verifiedCode_=false;
    unsigned depth_=0,clients_=0,codeAcquisitions_=0,codeReleases_=0;
    HMODULE code_=nullptr;
    std::array<wchar_t,MAX_PATH> path_{};
};

// Declare before COM fields so its client reservation retires after their
// Releases. A client must never outlive its original creator owner.
class NativeApartmentClient final {
public:
    NativeApartmentClient() noexcept=default;
    ~NativeApartmentClient();
    NativeApartmentClient(const NativeApartmentClient&)=delete;
    NativeApartmentClient& operator=(const NativeApartmentClient&)=delete;
    HRESULT acquire() noexcept;
    HRESULT observeLoadedCode() noexcept;
    void beforeNativeRelease() noexcept;
    bool active() const noexcept{return outer_!=nullptr;}
private:
    NativeApartmentOwner* outer_=nullptr;
};

} // namespace explorer
