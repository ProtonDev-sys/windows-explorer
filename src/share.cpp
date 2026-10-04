#include "explorer/share.hpp"

#include <roapi.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.h>
#include <algorithm>
#include <atomic>
#include <cwctype>
#include <mutex>
#include <set>

namespace explorer {
namespace fs = std::filesystem;
namespace transfer = winrt::Windows::ApplicationModel::DataTransfer;
namespace storage = winrt::Windows::Storage;
namespace foundation = winrt::Windows::Foundation;
namespace {
HRESULT exceptionResult() noexcept {
    try { throw; }
    catch (const winrt::hresult_error& error) { return error.code(); }
    catch (const fs::filesystem_error& error) { return HRESULT_FROM_WIN32(error.code().value()); }
    catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    catch (...) { return E_FAIL; }
}
bool validText(const std::wstring& text, size_t maximum, bool controls) {
    if (text.empty() || text.size() > maximum) return false;
    for (size_t index = 0; index < text.size(); ++index) {
        const wchar_t character = text[index];
        if (!character || (controls && character < 32)) return false;
        if (character >= 0xd800 && character <= 0xdbff) {
            if (++index == text.size() || text[index] < 0xdc00 || text[index] > 0xdfff) return false;
        } else if (character >= 0xdc00 && character <= 0xdfff) return false;
    }
    return true;
}
struct CaseLess {
    bool operator()(const std::wstring& a, const std::wstring& b) const {
        return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
    }
};
HRESULT normalizeFile(const fs::path& input, fs::path& output) {
    const std::wstring raw = input.native();
    if (!validText(raw, 32766, true) || raw.starts_with(L"\\\\?\\") || raw.starts_with(L"\\\\.\\")) return E_INVALIDARG;
    for (size_t index = 0; index < raw.size(); ++index)
        if (raw[index] == L':' && !(index == 1 && std::iswalpha(raw[0]))) return E_INVALIDARG;
    std::error_code error;
    const fs::path path = fs::absolute(input, error).lexically_normal();
    if (error) return HRESULT_FROM_WIN32(error.value());
    if (path.native().size() > 32766) return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return HRESULT_FROM_WIN32(GetLastError());
    if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    output = path;
    return S_OK;
}

struct DeliveryState {
    std::atomic<HRESULT> result{S_OK};
    std::atomic<bool> active{true};
    HWND owner = nullptr;
    UINT message = 0;
    std::mutex mutex;
    std::shared_ptr<const SharePayload> payload;
    void report(HRESULT hr) noexcept {
        result.store(hr);
        if (active.load() && owner && message) PostMessageW(owner, message, static_cast<WPARAM>(static_cast<ULONG>(hr)), 0);
    }
};

winrt::fire_and_forget cancelAtDeadline(foundation::IAsyncInfo operation,
                                      foundation::DateTime deadline) {
    try {
        const auto remaining = deadline - winrt::clock::now();
        if (remaining.count() > 0) co_await winrt::resume_after(remaining);
        if (operation.Status() == foundation::AsyncStatus::Started) operation.Cancel();
    } catch (...) {} // Operation cancellation/teardown is best effort.
}

winrt::fire_and_forget resolveStorageItems(std::shared_ptr<const SharePayload> payload,
                                         std::shared_ptr<DeliveryState> state,
                                         transfer::DataProviderRequest request) {
    transfer::DataProviderDeferral deferral{nullptr};
    foundation::DateTime deadline{};
    bool deadlineKnown = false;
    HRESULT result = S_OK;
    try {
        deferral = request.GetDeferral();
        state->report(E_PENDING);
        co_await winrt::resume_background();
        deadline = request.Deadline();
        deadlineKnown = true;
        auto items = winrt::single_threaded_vector<storage::IStorageItem>();
        for (const auto& path : payload->files) {
            if (!state->active.load()) winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_CANCELLED));
            if (winrt::clock::now() >= deadline) winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
            const auto operation = storage::StorageFile::GetFileFromPathAsync(path.native());
            cancelAtDeadline(operation.as<foundation::IAsyncInfo>(), deadline);
            auto file = co_await operation;
            if (!state->active.load()) winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_CANCELLED));
            if (winrt::clock::now() >= deadline) winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
            items.Append(file);
        }
        request.SetData(items);
    } catch (...) {
        result = exceptionResult();
        if (deadlineKnown && winrt::clock::now() >= deadline) result = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
    // Publish the original resolution error before releasing the recipient's
    // wait, so completed readers never race an E_PENDING result.
    state->report(result);
    if (deferral) {
        try { deferral.Complete(); }
        catch (...) { if (SUCCEEDED(result)) state->report(exceptionResult()); }
    }
}

transfer::DataPackage packageFor(std::shared_ptr<const SharePayload> payload,
                                const std::shared_ptr<DeliveryState>& state) {
    transfer::DataPackage package;
    package.Properties().Title(payload->title);
    package.RequestedOperation(transfer::DataPackageOperation::Copy);
    for (const auto& extension : payload->fileTypes) package.Properties().FileTypes().Append(extension);
    package.SetDataProvider(transfer::StandardDataFormats::StorageItems(),
        [payload = std::move(payload), state](const transfer::DataProviderRequest& request) {
            resolveStorageItems(payload, state, request);
        });
    return package;
}
HRESULT validatePayload(const SharePayload& input, SharePayload& normalized) {
    // Recompute extension metadata from validated files. Public structs cannot
    // smuggle unsupported formats or stale selection metadata into a package.
    return makeSharePayload(input.files, input.title, normalized);
}
}

HRESULT makeSharePayload(const std::vector<fs::path>& files, const std::wstring& title,
                         SharePayload& output) {
    try {
        if (files.empty() || files.size() > maximumShareFiles || !validText(title, maximumShareTitle, true)) return E_INVALIDARG;
        if (std::all_of(title.begin(), title.end(), [](wchar_t value) { return std::iswspace(value) != 0; })) return E_INVALIDARG;
        SharePayload planned;
        planned.title = title;
        std::set<std::wstring, CaseLess> names, extensions;
        for (const auto& file : files) {
            fs::path path;
            const HRESULT hr = normalizeFile(file, path);
            if (FAILED(hr)) return hr;
            if (!names.insert(path.native()).second) return HRESULT_FROM_WIN32(ERROR_DUP_NAME);
            std::wstring extension = path.extension().native();
            if (!extension.empty() && extensions.insert(extension).second) planned.fileTypes.push_back(std::move(extension));
            planned.files.push_back(std::move(path));
        }
        output = std::move(planned);
        return S_OK;
    } catch (...) { return exceptionResult(); }
}

HRESULT makeSharePayload(IShellItemArray* selection, const std::wstring& title,
                         SharePayload& output) {
    try {
        if (!selection) return E_INVALIDARG;
        DWORD count = 0;
        HRESULT hr = selection->GetCount(&count);
        if (FAILED(hr)) return hr;
        if (!count || count > maximumShareFiles) return E_INVALIDARG;
        std::vector<fs::path> files;
        files.reserve(count);
        for (DWORD index = 0; index < count; ++index) {
            winrt::com_ptr<IShellItem> item;
            hr = selection->GetItemAt(index, item.put());
            if (FAILED(hr)) return hr;
            SFGAOF attributes = 0;
            hr = item->GetAttributes(SFGAO_FILESYSTEM, &attributes);
            if (FAILED(hr)) return hr;
            // ZIP files expose SFGAO_FOLDER through the compressed-folder
            // namespace while remaining regular shareable filesystem files.
            // The path planner below rejects actual directories and links.
            if (!(attributes & SFGAO_FILESYSTEM)) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
            PWSTR path = nullptr;
            hr = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
            struct PathScope { PWSTR value; ~PathScope() { CoTaskMemFree(value); } } scope{path};
            if (FAILED(hr)) return hr;
            if (!path) return E_UNEXPECTED;
            files.emplace_back(path);
        }
        return makeSharePayload(files, title, output);
    } catch (...) { return exceptionResult(); }
}

struct NativeShare::Impl {
    HWND owner = nullptr;
    DWORD thread = 0;
    bool apartment = false;
    winrt::com_ptr<IDataTransferManagerInterop> interop;
    transfer::DataTransferManager manager{nullptr};
    winrt::event_token token{};
    bool subscribed = false;
    std::shared_ptr<DeliveryState> state = std::make_shared<DeliveryState>();
    ~Impl() {
        state->active.store(false);
        if (subscribed && manager) { try { manager.DataRequested(token); } catch (...) {} }
        manager = nullptr;
        interop = nullptr;
        if (apartment && thread == GetCurrentThreadId()) RoUninitialize();
    }
};

NativeShare::NativeShare() = default;
NativeShare::~NativeShare() { reset(); }
bool NativeShare::ready() const noexcept { return impl_ && impl_->manager && impl_->interop; }
HRESULT NativeShare::lastResult() const noexcept { return impl_ ? impl_->state->result.load() : CO_E_NOTINITIALIZED; }
void NativeShare::reset() noexcept { impl_.reset(); }

HRESULT NativeShare::initialize(HWND owner, UINT resultMessage) {
    try {
        if (impl_ && impl_->thread != GetCurrentThreadId()) return RPC_E_WRONG_THREAD;
        if (!owner || !IsWindow(owner)) return HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
        DWORD process = 0;
        const DWORD thread = GetWindowThreadProcessId(owner, &process);
        if (thread != GetCurrentThreadId() || process != GetCurrentProcessId()) return RPC_E_WRONG_THREAD;
        if (resultMessage && (resultMessage < WM_APP || resultMessage > 0xbfff)) return E_INVALIDARG;
        auto candidate = std::make_unique<Impl>();
        candidate->thread = thread;
        candidate->owner = owner;
        candidate->state->owner = owner;
        candidate->state->message = resultMessage;
        const HRESULT initialized = RoInitialize(RO_INIT_SINGLETHREADED);
        if (FAILED(initialized)) return initialized;
        candidate->apartment = true;
        candidate->interop = winrt::get_activation_factory<transfer::DataTransferManager, IDataTransferManagerInterop>();
        winrt::check_hresult(candidate->interop->GetForWindow(owner, winrt::guid_of<transfer::DataTransferManager>(), winrt::put_abi(candidate->manager)));
        // The callback captures shared state, never the NativeShare object.
        const auto state = candidate->state;
        // Payload publication belongs to the same STA as this event callback.
        candidate->token = candidate->manager.DataRequested([state](const transfer::DataTransferManager&, const transfer::DataRequestedEventArgs& args) {
            if (!state->active.load()) return;
            transfer::DataRequestDeferral deferral{nullptr};
            try {
                const auto request = args.Request();
                deferral = request.GetDeferral();
                std::shared_ptr<const SharePayload> payload;
                { std::lock_guard lock(state->mutex); payload = state->payload; }
                if (!payload) {
                    request.FailWithDisplayText(L"Select one or more files to share.");
                    state->report(E_INVALIDARG);
                } else {
                    request.Data(packageFor(std::move(payload), state));
                }
            } catch (...) { state->report(exceptionResult()); }
            if (deferral) { try { deferral.Complete(); } catch (...) { state->report(exceptionResult()); } }
        });
        candidate->subscribed = true;
        impl_ = std::move(candidate);
        return S_OK;
    } catch (...) { return exceptionResult(); }
}

HRESULT NativeShare::show(const SharePayload& payload) {
    try {
        if (!ready()) return CO_E_NOTINITIALIZED;
        if (impl_->thread != GetCurrentThreadId()) return RPC_E_WRONG_THREAD;
        if (!IsWindow(impl_->owner)) return HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
        SharePayload normalized;
        const HRESULT hr = validatePayload(payload, normalized);
        if (FAILED(hr)) return hr;
        auto snapshot = std::make_shared<const SharePayload>(std::move(normalized));
        { std::lock_guard lock(impl_->state->mutex); impl_->state->payload = std::move(snapshot); }
        impl_->state->result.store(S_OK);
        return impl_->interop->ShowShareUIForWindow(impl_->owner);
    } catch (...) { return exceptionResult(); }
}

HRESULT NativeShare::makePackage(const SharePayload& payload, IInspectable** package) {
    if (!package) return E_POINTER;
    *package = nullptr;
    try {
        if (!ready()) return CO_E_NOTINITIALIZED;
        if (impl_->thread != GetCurrentThreadId()) return RPC_E_WRONG_THREAD;
        SharePayload normalized;
        const HRESULT hr = validatePayload(payload, normalized);
        if (FAILED(hr)) return hr;
        auto result = packageFor(std::make_shared<const SharePayload>(std::move(normalized)), impl_->state);
        *package = static_cast<IInspectable*>(winrt::detach_abi(result));
        return S_OK;
    } catch (...) { return exceptionResult(); }
}
}
