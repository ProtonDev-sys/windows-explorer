#include "explorer/headless_visual.hpp"
#include "explorer/commands.hpp"
#include "explorer/ribbon_commands.hpp"
#include "explorer/library.hpp"
#include "explorer/startup_desktop.hpp"

#include <wincodec.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <wrl/client.h>
#include <uiribbon.h>
#include <UIRibbonPropertyHelpers.h>
#include <propvarutil.h>
#include <structuredquery.h>
#include <shlobj.h>
#include <aclapi.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <unordered_set>

namespace explorer {
namespace {
using Microsoft::WRL::ComPtr;
constexpr uint32_t UnpaintedPixel = 0x00ff00ff;
constexpr size_t MaximumPixels = 64ULL * 1024 * 1024;
constexpr size_t MaximumWidgets = 4096;
thread_local const PrivateDesktop* currentDesktop = nullptr;

HRESULT win32Failure() noexcept {
    const auto error = GetLastError();
    return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
}

struct DesktopHandle {
    HDESK value = nullptr;
    ~DesktopHandle() { if (value) CloseDesktop(value); }
};

HRESULT objectName(HANDLE handle, std::wstring& name) {
    DWORD length = 0;
    GetUserObjectInformationW(handle, UOI_NAME, nullptr, 0, &length);
    if (!length || length > 32768 * sizeof(wchar_t)) return win32Failure();
    std::wstring value(length / sizeof(wchar_t), L'\0');
    if (!GetUserObjectInformationW(handle, UOI_NAME, value.data(), length, &length)) return win32Failure();
    value.resize(wcsnlen_s(value.c_str(), value.size()));
    name = std::move(value);
    return S_OK;
}

HRESULT inputName(std::wstring& name) {
    DesktopHandle input{OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS | DESKTOP_ENUMERATE)};
    if (!input.value) return win32Failure();
    return objectName(input.value, name);
}

std::wstring windowText(HWND window) {
    const int length = GetWindowTextLengthW(window);
    if (length <= 0) return {};
    const auto bounded = std::min(length, 32768);
    std::wstring value(static_cast<size_t>(bounded) + 1, L'\0');
    const auto read = GetWindowTextW(window, value.data(), bounded + 1);
    value.resize(static_cast<size_t>(std::max(0, read)));
    return value;
}

std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const auto length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!length) return {};
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
    return result;
}

std::string json(const std::wstring& value) {
    const auto encoded = utf8(value);
    std::string result = "\"";
    constexpr char digits[] = "0123456789abcdef";
    for (const auto raw : encoded) {
        const auto ch = static_cast<unsigned char>(raw);
        if (ch == '"' || ch == '\\') { result += '\\'; result += static_cast<char>(ch); }
        else if (ch < 0x20) {
            result += "\\u00"; result += digits[ch >> 4]; result += digits[ch & 15];
        } else result += static_cast<char>(ch);
    }
    result += '"';
    return result;
}

void rectJson(std::ostream& stream, const RECT& rect) {
    stream << '[' << rect.left << ',' << rect.top << ',' << rect.right << ',' << rect.bottom << ']';
}

HRESULT createNewFile(const std::filesystem::path& output, const void* data, size_t length) {
    if (output.empty() || !output.is_absolute() || length > MAXDWORD) return E_INVALIDARG;
    const auto file = CreateFileW(output.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return win32Failure();
    DWORD written = 0;
    HRESULT hr = WriteFile(file, data, static_cast<DWORD>(length), &written, nullptr)
        ? S_OK : win32Failure();
    if (SUCCEEDED(hr) && written != length) hr = HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
    if (SUCCEEDED(hr) && !FlushFileBuffers(file)) hr = win32Failure();
    CloseHandle(file);
    if (FAILED(hr)) DeleteFileW(output.c_str()); // only the file we just created
    return hr;
}

struct Dib {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;
    uint32_t* pixels = nullptr;
    ~Dib() {
        if (dc && previous) SelectObject(dc, previous);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
    }
    HRESULT initialize(unsigned width, unsigned height) {
        dc = CreateCompatibleDC(nullptr);
        if (!dc) return win32Failure();
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = static_cast<LONG>(width);
        info.bmiHeader.biHeight = -static_cast<LONG>(height);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bitmap || !bits) return win32Failure();
        pixels = static_cast<uint32_t*>(bits);
        previous = SelectObject(dc, bitmap);
        if (!previous || previous == HGDI_ERROR) return win32Failure();
        std::fill_n(pixels, static_cast<size_t>(width) * height, UnpaintedPixel);
        return S_OK;
    }
};

HRESULT encodePng(const std::filesystem::path& output, const Dib& dib,
                  unsigned width, unsigned height, unsigned dpi) {
    ComPtr<IWICImagingFactory> factory;
    auto hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;
    ComPtr<IStream> stream;
    hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
    if (FAILED(hr)) return hr;
    ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (FAILED(hr)) return hr;
    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) return hr;
    ComPtr<IWICBitmapFrameEncode> frame;
    hr = encoder->CreateNewFrame(&frame, nullptr);
    if (FAILED(hr)) return hr;
    hr = frame->Initialize(nullptr);
    if (FAILED(hr)) return hr;
    hr = frame->SetSize(width, height);
    if (FAILED(hr)) return hr;
    hr = frame->SetResolution(dpi, dpi);
    if (FAILED(hr)) return hr;
    auto format = GUID_WICPixelFormat32bppBGRA;
    hr = frame->SetPixelFormat(&format);
    if (FAILED(hr)) return hr;
    if (format != GUID_WICPixelFormat32bppBGRA) return WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
    const auto stride = width * sizeof(uint32_t);
    const auto size = static_cast<size_t>(stride) * height;
    if (size > MAXDWORD) return E_OUTOFMEMORY;
    // PrintWindow/GDI produces BGR with an undefined alpha byte. PNG's native
    // BGRA format needs opaque alpha; RGB drawing output is preserved exactly.
    for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i) dib.pixels[i] |= 0xff000000;
    hr = frame->WritePixels(height, static_cast<UINT>(stride), static_cast<UINT>(size),
                            reinterpret_cast<BYTE*>(dib.pixels));
    if (FAILED(hr)) return hr;
    hr = frame->Commit();
    if (FAILED(hr)) return hr;
    hr = encoder->Commit();
    if (FAILED(hr)) return hr;
    STATSTG stat{};
    hr = stream->Stat(&stat, STATFLAG_NONAME);
    if (FAILED(hr)) return hr;
    if (stat.cbSize.QuadPart > MAXDWORD) return E_OUTOFMEMORY;
    HGLOBAL memory = nullptr;
    hr = GetHGlobalFromStream(stream.Get(), &memory);
    if (FAILED(hr)) return hr;
    const auto data = GlobalLock(memory);
    if (!data) return win32Failure();
    hr = createNewFile(output, data, static_cast<size_t>(stat.cbSize.QuadPart));
    GlobalUnlock(memory);
    return hr;
}

void collectWidgets(HWND root, const RECT& captureRect, VisualCaptureReport& report) {
    struct Enumeration {
        HWND root; const RECT* rect; VisualCaptureReport* report;
    } enumeration{root, &captureRect, &report};
    EnumChildWindows(root, [](HWND child, LPARAM param) -> BOOL {
        auto& state = *reinterpret_cast<Enumeration*>(param);
        if (state.report->widgets.size() == MaximumWidgets) return FALSE;
        VisualWidget widget;
        widget.id = GetDlgCtrlID(child);
        wchar_t name[256]{};
        GetClassNameW(child, name, static_cast<int>(std::size(name)));
        widget.className = name;
        widget.text = windowText(child);
        widget.enabled = IsWindowEnabled(child) != FALSE;
        widget.visible = IsWindowVisible(child) != FALSE;
        GetWindowRect(child, &widget.bounds);
        OffsetRect(&widget.bounds, -state.rect->left, -state.rect->top);
        for (auto parent = GetParent(child); parent && parent != state.root && widget.depth < 64;
             parent = GetParent(parent)) ++widget.depth;
        if (widget.visible && widget.bounds.right > widget.bounds.left &&
            widget.bounds.bottom > widget.bounds.top) ++state.report->visibleChildren;
        DWORD process = 0;
        if (widget.visible && (widget.id == 901 || widget.id == 902) && widget.className == TOOLBARCLASSNAMEW &&
            GetWindowThreadProcessId(child, &process) == GetCurrentThreadId() && process == GetCurrentProcessId()) {
            auto& buttons = widget.id == 901 ? state.report->navigationButtons : state.report->breadcrumbButtons;
            const auto count = SendMessageW(child, TB_BUTTONCOUNT, 0, 0);
            if (count >= 0 && count <= 128) for (int index = 0; index < count; ++index) {
                TBBUTTON native{};
                VisualCaptureReport::ToolbarButtonReadback button;
                button.index = index;
                if (!SendMessageW(child, TB_GETBUTTON, index, reinterpret_cast<LPARAM>(&native))) {
                    button.read = E_FAIL;
                    buttons.push_back(button);
                    continue;
                }
                if (native.fsState & TBSTATE_HIDDEN) continue;
                button.command = native.idCommand;
                TBBUTTONINFOW info{};
                info.cbSize = sizeof(info);
                info.dwMask = TBIF_SIZE | TBIF_IMAGE | TBIF_STATE | TBIF_STYLE;
                const auto read = SendMessageW(child, TB_GETBUTTONINFOW, native.idCommand, reinterpret_cast<LPARAM>(&info));
                const auto rectRead = SendMessageW(child, TB_GETITEMRECT, index, reinterpret_cast<LPARAM>(&button.bounds));
                button.read = read >= 0 && rectRead ? S_OK : E_FAIL;
                button.width = info.cx;
                button.image = info.iImage;
                button.state = info.fsState;
                button.style = info.fsStyle;
                buttons.push_back(button);
            }
        }
        state.report->widgets.push_back(std::move(widget));
        return TRUE;
    }, reinterpret_cast<LPARAM>(&enumeration));
}

void pixelStatistics(const Dib& dib, unsigned width, unsigned height,
                     VisualCaptureReport& report) {
    const size_t count = static_cast<size_t>(width) * height;
    size_t unpainted = 0;
    size_t ink = 0;
    std::unordered_set<uint32_t> colors;
    colors.reserve(4096);
    for (size_t i = 0; i < count; ++i) {
        const auto rgb = dib.pixels[i] & 0x00ffffff;
        if (rgb == UnpaintedPixel) ++unpainted;
        // A color-independent ink check: luminance departure from pure white.
        // Dark screenshots still need unique colors and visible native widgets.
        const auto blue = rgb & 0xff;
        const auto green = (rgb >> 8) & 0xff;
        const auto red = (rgb >> 16) & 0xff;
        if (red < 220 || green < 220 || blue < 220) ++ink;
        if (colors.size() < 65536) colors.insert(rgb);
    }
    report.uniqueColors = static_cast<unsigned>(colors.size());
    report.inkFraction = static_cast<double>(ink) / static_cast<double>(count);
    report.unpaintedFraction = static_cast<double>(unpainted) / static_cast<double>(count);
}

} // namespace

std::string nativePopupExpansionJson(const NativePopupCapture& popup) {
    std::ostringstream out;
    out << "{\"stage\":" << popup.stage << ",\"candidateCount\":" << popup.candidateCount
        << ",\"ribbonWindow\":" << popup.ribbonWindow << ",\"ribbonBounds\":";
    rectJson(out,popup.ribbonBounds);
    out << ",\"nativeCommand\":" << popup.nativeCommand << ",\"commandType\":" << popup.commandType
        << ",\"executionAttempts\":" << popup.executionAttempts << ",\"parents\":[";
    bool first=true;
    for(const auto& parent:popup.parents) {
        out << (first?"":",") << "{\"type\":" << parent.type
            << ",\"enabled\":" << (parent.enabled?"true":"false")
            << ",\"offscreen\":" << (parent.offscreen?"true":"false") << ",\"bounds\":";
        rectJson(out,parent.bounds);
        out << ",\"elementReadHresult\":" << static_cast<long>(parent.elementRead)
            << ",\"typeReadHresult\":" << static_cast<long>(parent.typeRead)
            << ",\"enabledReadHresult\":" << static_cast<long>(parent.enabledRead)
            << ",\"offscreenReadHresult\":" << static_cast<long>(parent.offscreenRead)
            << ",\"boundsReadHresult\":" << static_cast<long>(parent.boundsRead)
            << ",\"patternReadHresult\":" << static_cast<long>(parent.patternRead)
            << ",\"stateReadHresult\":" << static_cast<long>(parent.stateRead)
            << ",\"expandState\":" << parent.expandState
            << ",\"parentReadHresult\":" << static_cast<long>(parent.parentRead)
            << ",\"parentType\":" << parent.parentType
            << ",\"parentSameName\":" << (parent.parentSameName?"true":"false") << ",\"parentBounds\":";
        rectJson(out,parent.parentBounds);
        out << ",\"ribbonAncestorReadHresult\":" << static_cast<long>(parent.ribbonAncestorRead)
            << ",\"ribbonAncestor\":" << (parent.ribbonAncestor?"true":"false")
            << ",\"ribbonBoundsContain\":" << (parent.ribbonBoundsContain?"true":"false")
            << ",\"toolbarBoundsContain\":" << (parent.toolbarBoundsContain?"true":"false")
            << ",\"accepted\":" << (parent.accepted?"true":"false")
            << ",\"automationIdReadHresult\":" << static_cast<long>(parent.automationIdRead)
            << ",\"automationId\":" << json(parent.automationId) << '}';first=false;
    }
    out << "]}";return out.str();
}

HRESULT validateRelativeTodayQuery(std::wstring_view query) noexcept {
    if(query.empty()||query.size()>32768||query.find(L'\0')!=std::wstring_view::npos)return E_INVALIDARG;
    try {
        ComPtr<IQueryParserManager> manager;
        auto hr=CoCreateInstance(__uuidof(QueryParserManager),nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&manager));
        if(FAILED(hr))return hr;
        ComPtr<IQueryParser> parser;
        hr=manager->CreateLoadedParser(L"SystemIndex",GetUserDefaultUILanguage(),IID_PPV_ARGS(&parser));if(FAILED(hr))return hr;
        hr=manager->InitializeOptions(FALSE,TRUE,parser.Get());if(FAILED(hr))return hr;
        struct Leaf {
            PWSTR property=nullptr,semantic=nullptr;
            CONDITION_OPERATION operation{};
            PROPVARIANT value{};
            ~Leaf(){CoTaskMemFree(property);CoTaskMemFree(semantic);PropVariantClear(&value);}
        } expected,actual;
        const auto read=[&](const wchar_t* text,Leaf& leaf)->HRESULT {
            ComPtr<IQuerySolution> solution;
            auto result=parser->Parse(text,nullptr,&solution);if(FAILED(result))return result;
            ComPtr<ICondition> condition;result=solution->GetQuery(&condition,nullptr);if(FAILED(result))return result;
            CONDITION_TYPE type{};result=condition->GetConditionType(&type);if(FAILED(result))return result;
            if(type!=CT_LEAF_CONDITION)return E_INVALIDARG;
            result=condition->GetComparisonInfo(&leaf.property,&leaf.operation,&leaf.value);if(FAILED(result))return result;
            return condition->GetValueType(&leaf.semantic);
        };
        hr=read(L"System.DateModified:System.StructuredQueryType.DateTime#Today",expected);if(FAILED(hr))return hr;
        const std::wstring text(query);
        hr=read(text.c_str(),actual);if(FAILED(hr))return hr;
        // Compare the public unresolved condition fields against Windows' own
        // Today parser result. Never decode its internal relative-date token
        // or resolve it to the current calendar day for this persistence proof.
        const auto valid=expected.property&&expected.semantic&&expected.value.vt==VT_LPWSTR&&expected.value.pwszVal&&
            wcscmp(expected.property,L"System.DateModified")==0&&
            wcscmp(expected.semantic,L"System.StructuredQueryType.DateTime")==0&&
            actual.property&&actual.semantic&&actual.value.vt==VT_LPWSTR&&actual.value.pwszVal&&
            actual.operation==expected.operation&&wcscmp(actual.property,expected.property)==0&&
            wcscmp(actual.semantic,expected.semantic)==0&&wcscmp(actual.value.pwszVal,expected.value.pwszVal)==0;
        return valid?S_OK:E_INVALIDARG;
    }catch(...){return E_OUTOFMEMORY;}
}

PrivateDesktop::~PrivateDesktop() {
    if (borrowedInitialContext_) {
        // Windows owns the initial connection handle. Even partial/failing
        // diagnostic teardown must never pass it to CloseDesktop or restore
        // another desktop. The child process retains that connection to exit.
        if (currentDesktop == this) currentDesktop = nullptr;
        return;
    }
    if (diagnosticDefault_) {
        // The admission fixture normally observes this result explicitly.
        // Never close a still-attached desktop after a failed restoration.
        finishAtomicLowForDiagnostic();
        return;
    }
    if (currentDesktop == this) currentDesktop = nullptr;
    if (!desktop_) return;
    // Never restore a thread with still-live windows: SetThreadDesktop refuses
    // that transition. A failed restore leaves the owned handle to process exit.
    if (thread_ == GetCurrentThreadId() && SetThreadDesktop(previous_)) CloseDesktop(desktop_);
}

HRESULT PrivateDesktop::initialize() {
    if (desktop_ || currentDesktop) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
    std::wstring originalInput;
    auto hr = inputName(originalInput);
    if (FAILED(hr)) return hr;
    GUID id{};
    hr = CoCreateGuid(&id);
    if (FAILED(hr)) return hr;
    wchar_t guid[40]{};
    if (!StringFromGUID2(id, guid, static_cast<int>(std::size(guid)))) return E_FAIL;
    const auto privateName = L"WindowsExplorer.Visual." + std::to_wstring(GetCurrentProcessId()) + L"." + guid;
    // DESKTOP_SWITCHDESKTOP is deliberately absent. The handle cannot activate
    // this desktop, and no desktop-switch API is used by this module.
    const auto previous = GetThreadDesktop(GetCurrentThreadId());
    if (!previous) return win32Failure();
    DesktopHandle created{CreateDesktopW(privateName.c_str(), nullptr, nullptr, 0,
        DESKTOP_CREATEWINDOW | DESKTOP_CREATEMENU | DESKTOP_ENUMERATE | DESKTOP_HOOKCONTROL |
        DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS, nullptr)};
    if (!created.value) return win32Failure();
    if (!SetThreadDesktop(created.value)) return win32Failure();
    desktop_ = created.value;
    created.value = nullptr;
    previous_ = previous;
    thread_ = GetCurrentThreadId();
    name_ = privateName;
    inputName_ = std::move(originalInput);
    hr = verifyIsolation();
    if (FAILED(hr)) {
        if (SetThreadDesktop(previous_)) CloseDesktop(desktop_);
        desktop_ = nullptr;
        previous_ = nullptr;
        name_.clear();
        inputName_.clear();
    } else currentDesktop = this;
    return hr;
}

HRESULT PrivateDesktop::adoptInitialForDiagnostic(const StartupDesktopChild& context) {
    if (desktop_ || diagnosticDefault_ || borrowedInitialContext_ || currentDesktop)
        return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
    if (!context.ready() || context.arm() != StartupDesktopArm::InitialPrivate) return E_ACCESSDENIED;
    APTTYPE apartment{}; APTTYPEQUALIFIER qualifier{};
    const auto apartmentRead = CoGetApartmentType(&apartment, &qualifier);
    if (apartmentRead != CO_E_NOTINITIALIZED && !(apartmentRead == S_OK &&
        apartment == APTTYPE_MTA && qualifier == APTTYPEQUALIFIER_IMPLICIT_MTA)) return E_ACCESSDENIED;
    auto hr = context.verifyInitial();
    if (FAILED(hr)) return hr;
    try {
        const auto initial = GetThreadDesktop(GetCurrentThreadId());
        if (!initial || initial != context.initialDesktop()) return E_ACCESSDENIED;
        std::wstring actual, input;
        hr = objectName(initial, actual);
        if (SUCCEEDED(hr)) hr = inputName(input);
        if (FAILED(hr)) return hr;
        if (_wcsicmp(actual.c_str(), context.desktopName().c_str()) != 0 ||
            _wcsicmp(input.c_str(), context.inputName().c_str()) != 0 ||
            _wcsicmp(actual.c_str(), input.c_str()) == 0) return E_ACCESSDENIED;
        // Stage all allocation before committing the borrowed handle or TLS.
        // No DesktopHandle owns this GetThreadDesktop result on any branch.
        desktop_ = initial; thread_ = GetCurrentThreadId();
        name_.swap(actual); inputName_.swap(input);
        borrowedInitialContext_ = &context;
        hr = verifyIsolation();
        if (FAILED(hr)) {
            desktop_ = nullptr; thread_ = 0;
            borrowedInitialContext_ = nullptr;
            name_.clear(); inputName_.clear();
            return hr;
        }
        currentDesktop = this;
        return S_OK;
    } catch (...) {
        if (borrowedInitialContext_ == &context) {
            desktop_ = nullptr; thread_ = 0; borrowedInitialContext_ = nullptr;
            name_.clear(); inputName_.clear();
        }
        return E_OUTOFMEMORY;
    }
}

HRESULT PrivateDesktop::finishInitialForDiagnostic() {
    if (!borrowedInitialContext_ || !desktop_ || diagnosticDefault_ || previous_ ||
        thread_ != GetCurrentThreadId() || currentDesktop != this ||
        GetThreadDesktop(GetCurrentThreadId()) != desktop_) return E_ACCESSDENIED;
    auto hr = borrowedInitialContext_->verifyInitial();
    if (SUCCEEDED(hr)) hr = verifyIsolation();
    if (FAILED(hr)) return hr; // retain the borrowed connection until safe exit
    currentDesktop = nullptr;
    desktop_ = nullptr; thread_ = 0; borrowedInitialContext_ = nullptr;
    name_.clear(); inputName_.clear();
    return S_OK;
}

HRESULT PrivateDesktop::initializeAtomicLowForDiagnostic(DiagnosticAtomicLabelReadback& readback, ULONGLONG deadline) {
    readback = {};
    const auto budget = [&] { return GetTickCount64() < deadline; };
    if (desktop_ || diagnosticDefault_ || currentDesktop)
        return readback.guard = HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
    APTTYPE apartment{}; APTTYPEQUALIFIER qualifier{};
    readback.apartmentRead = CoGetApartmentType(&apartment, &qualifier);
    if (SUCCEEDED(readback.apartmentRead)) {
        readback.apartmentType = static_cast<int>(apartment);
        readback.apartmentQualifier = static_cast<int>(qualifier);
    }
    const bool implicitMta = readback.apartmentRead == S_OK && apartment == APTTYPE_MTA &&
        qualifier == APTTYPEQUALIFIER_IMPLICIT_MTA;
    if (readback.apartmentRead != CO_E_NOTINITIALIZED && !implicitMta)
        return readback.guard = E_ACCESSDENIED;
    HANDLE unexpected = nullptr;
    if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &unexpected)) {
        CloseHandle(unexpected); return readback.guard = E_ACCESSDENIED;
    }
    if (GetLastError() != ERROR_NO_TOKEN) return readback.guard = win32Failure();
    if (!budget()) return readback.guard = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    std::wstring originalInput;
    auto hr = inputName(originalInput);
    if (FAILED(hr)) return readback.guard = hr;
    const auto previous = GetThreadDesktop(GetCurrentThreadId());
    if (!previous) return readback.guard = win32Failure();
    const auto station = GetProcessWindowStation();
    if (!station) return readback.guard = win32Failure();
    GUID id{};
    hr = CoCreateGuid(&id);
    if (FAILED(hr)) return readback.guard = hr;
    wchar_t guid[40]{};
    if (!StringFromGUID2(id, guid, static_cast<int>(std::size(guid)))) return readback.guard = E_FAIL;
    const auto prefix = L"WindowsExplorer.Admission." + std::to_wstring(GetCurrentProcessId()) + L"." + guid;
    auto defaultName = prefix + L".Default";
    auto lowName = prefix + L".Low";
    alignas(SID) std::array<BYTE, SECURITY_MAX_SID_SIZE> sid{};
    DWORD sidBytes = static_cast<DWORD>(sid.size());
    alignas(ACL) std::array<BYTE, sizeof(ACL) + sizeof(SYSTEM_MANDATORY_LABEL_ACE) + SECURITY_MAX_SID_SIZE> acl{};
    const auto sacl = reinterpret_cast<PACL>(acl.data());
    SECURITY_DESCRIPTOR descriptor{};
    if (!CreateWellKnownSid(WinLowLabelSid, nullptr, sid.data(), &sidBytes) ||
        !InitializeAcl(sacl, static_cast<DWORD>(acl.size()), ACL_REVISION) ||
        !AddMandatoryAce(sacl, ACL_REVISION, 0, SYSTEM_MANDATORY_LABEL_NO_WRITE_UP, sid.data()) ||
        !InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorSacl(&descriptor, TRUE, sacl, FALSE)) return readback.guard = win32Failure();
    // An absent DACL permits the native parent/default inheritance mechanism;
    // a present NULL DACL would instead grant unrestricted access. Supply no
    // owner/group or DACL flags, and verify the resulting native descriptors.
    SECURITY_DESCRIPTOR_CONTROL suppliedControl{}; DWORD suppliedRevision = 0;
    if (!IsValidSecurityDescriptor(&descriptor) ||
        !GetSecurityDescriptorControl(&descriptor, &suppliedControl, &suppliedRevision) ||
        suppliedControl != SE_SACL_PRESENT || descriptor.Owner || descriptor.Group || descriptor.Dacl)
        return readback.guard = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), &descriptor, FALSE};
    constexpr ACCESS_MASK access = READ_CONTROL | DESKTOP_CREATEWINDOW | DESKTOP_CREATEMENU | DESKTOP_ENUMERATE |
        DESKTOP_HOOKCONTROL | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS;
    if (!budget()) return readback.guard = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    if (GetProcessWindowStation() != station) return readback.guard = E_ACCESSDENIED;
    DesktopHandle baseline{CreateDesktopW(defaultName.c_str(), nullptr, nullptr, 0, access, nullptr)};
    readback.defaultCreated = baseline.value ? S_OK : win32Failure();
    if (FAILED(readback.defaultCreated)) return readback.defaultCreated;
    if (!budget()) return readback.guard = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    if (GetProcessWindowStation() != station) return readback.guard = E_ACCESSDENIED;
    DesktopHandle low{CreateDesktopW(lowName.c_str(), nullptr, nullptr, 0, access, &attributes)};
    readback.lowCreated = low.value ? S_OK : win32Failure();
    if (FAILED(readback.lowCreated)) return readback.lowCreated;
    struct Security {
        PSECURITY_DESCRIPTOR value = nullptr;
        PSID owner = nullptr, group = nullptr;
        PACL dacl = nullptr;
        SECURITY_DESCRIPTOR_CONTROL control{};
        DWORD labels = 0, rid = 0, mask = 0, flags = 0;
        ~Security() { if (value) LocalFree(value); }
        HRESULT read(HDESK handle, ULONGLONG limit) {
            if (GetTickCount64() >= limit) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            const auto error = GetSecurityInfo(handle, SE_WINDOW_OBJECT,
                OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION,
                &owner, &group, &dacl, nullptr, &value);
            if (error) return HRESULT_FROM_WIN32(error);
            DWORD revision = 0;
            if (!value || !IsValidSecurityDescriptor(value) || !owner || !IsValidSid(owner) ||
                !group || !IsValidSid(group) || !dacl || !IsValidAcl(dacl) ||
                !GetSecurityDescriptorControl(value, &control, &revision)) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            PACL labelsAcl = nullptr; BOOL present = FALSE, defaulted = FALSE;
            if (!GetSecurityDescriptorSacl(value, &present, &labelsAcl, &defaulted)) return win32Failure();
            if (!present || !labelsAcl) return S_OK;
            if (!IsValidAcl(labelsAcl)) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            for (DWORD index = 0; index < labelsAcl->AceCount; ++index) {
                if (GetTickCount64() >= limit) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                void* raw = nullptr;
                if (!GetAce(labelsAcl, index, &raw)) return win32Failure();
                const auto ace = static_cast<const SYSTEM_MANDATORY_LABEL_ACE*>(raw);
                if (ace->Header.AceType != SYSTEM_MANDATORY_LABEL_ACE_TYPE) continue;
                constexpr auto offset = offsetof(SYSTEM_MANDATORY_LABEL_ACE, SidStart);
                const auto bytes = ace->Header.AceSize >= offset ? ace->Header.AceSize - offset : 0;
                const auto actualSid = reinterpret_cast<const SID*>(&ace->SidStart);
                const SID_IDENTIFIER_AUTHORITY authority = SECURITY_MANDATORY_LABEL_AUTHORITY;
                if (bytes < offsetof(SID, SubAuthority) + sizeof(DWORD) || actualSid->SubAuthorityCount != 1 ||
                    !IsValidSid(const_cast<SID*>(actualSid)) ||
                    std::memcmp(&actualSid->IdentifierAuthority, &authority, sizeof(authority)) != 0)
                    return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
                ++labels; rid = actualSid->SubAuthority[0]; mask = ace->Mask; flags = ace->Header.AceFlags;
            }
            return S_OK;
        }
    } defaultSecurity, lowSecurity;
    readback.defaultSecurity = defaultSecurity.read(baseline.value, deadline);
    readback.defaultLabels = defaultSecurity.labels; readback.defaultRid = defaultSecurity.rid;
    readback.defaultMask = defaultSecurity.mask; readback.defaultFlags = defaultSecurity.flags;
    readback.defaultControl = defaultSecurity.control;
    if (FAILED(readback.defaultSecurity)) return readback.defaultSecurity;
    readback.lowSecurity = lowSecurity.read(low.value, deadline);
    readback.lowLabels = lowSecurity.labels; readback.lowRid = lowSecurity.rid;
    readback.lowMask = lowSecurity.mask; readback.lowFlags = lowSecurity.flags;
    readback.lowControl = lowSecurity.control;
    if (FAILED(readback.lowSecurity)) return readback.lowSecurity;
    constexpr auto saclControl = SE_SACL_PRESENT | SE_SACL_DEFAULTED | SE_SACL_AUTO_INHERIT_REQ |
        SE_SACL_AUTO_INHERITED | SE_SACL_PROTECTED;
    const auto nonlabelControl = static_cast<SECURITY_DESCRIPTOR_CONTROL>(~saclControl);
    readback.nonlabelControlEqual = (defaultSecurity.control & nonlabelControl) == (lowSecurity.control & nonlabelControl);
    readback.daclEqual = defaultSecurity.dacl->AclSize == lowSecurity.dacl->AclSize &&
        std::memcmp(defaultSecurity.dacl, lowSecurity.dacl, defaultSecurity.dacl->AclSize) == 0;
    readback.ownerEqual = EqualSid(defaultSecurity.owner, lowSecurity.owner) != FALSE;
    readback.groupEqual = EqualSid(defaultSecurity.group, lowSecurity.group) != FALSE;
    USEROBJECTFLAGS defaultFlags{}, lowFlags{};
    DWORD length = 0;
    if (!GetUserObjectInformationW(baseline.value, UOI_FLAGS, &defaultFlags, sizeof(defaultFlags), &length) ||
        !GetUserObjectInformationW(low.value, UOI_FLAGS, &lowFlags, sizeof(lowFlags), &length))
        return readback.guard = win32Failure();
    readback.noninheritable = !defaultFlags.fInherit && !lowFlags.fInherit;
    std::wstring openedDefault, openedLow, finalInput;
    hr = objectName(baseline.value, openedDefault);
    if (SUCCEEDED(hr)) hr = objectName(low.value, openedLow);
    if (SUCCEEDED(hr)) hr = inputName(finalInput);
    if (FAILED(hr)) return readback.guard = hr;
    if (!readback.nonlabelControlEqual || !readback.daclEqual || !readback.ownerEqual || !readback.groupEqual ||
        lowSecurity.labels != 1 || lowSecurity.rid != SECURITY_MANDATORY_LOW_RID ||
        lowSecurity.mask != SYSTEM_MANDATORY_LABEL_NO_WRITE_UP || lowSecurity.flags != 0 || !readback.noninheritable ||
        GetProcessWindowStation() != station ||
        openedDefault != defaultName || openedLow != lowName || _wcsicmp(finalInput.c_str(), originalInput.c_str()) != 0 ||
        _wcsicmp(openedDefault.c_str(), finalInput.c_str()) == 0 || _wcsicmp(openedLow.c_str(), finalInput.c_str()) == 0)
        return readback.equivalentSecurity = E_ACCESSDENIED;
    readback.equivalentSecurity = S_OK;
    if (!budget()) return readback.guard = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    if (!SetThreadDesktop(low.value)) return readback.guard = win32Failure();
    desktop_ = low.value; low.value = nullptr;
    diagnosticDefault_ = baseline.value; baseline.value = nullptr;
    previous_ = previous; thread_ = GetCurrentThreadId();
    name_ = std::move(lowName); diagnosticDefaultName_ = std::move(defaultName); inputName_ = std::move(originalInput);
    currentDesktop = this;
    readback.guard = verifyIsolation();
    readback.exactOwnedCurrent = SUCCEEDED(readback.guard) && GetThreadDesktop(GetCurrentThreadId()) == desktop_;
    return readback.guard;
}

HRESULT PrivateDesktop::finishAtomicLowForDiagnostic() {
    if (!desktop_ || !diagnosticDefault_ || thread_ != GetCurrentThreadId() || currentDesktop != this ||
        GetThreadDesktop(GetCurrentThreadId()) != desktop_) return E_ACCESSDENIED;
    const auto isolation = verifyIsolation();
    if (!SetThreadDesktop(previous_)) return win32Failure();
    currentDesktop = nullptr;
    HRESULT result = isolation;
    if (CloseDesktop(desktop_)) desktop_ = nullptr;
    else result = win32Failure();
    if (CloseDesktop(diagnosticDefault_)) diagnosticDefault_ = nullptr;
    else if (SUCCEEDED(result)) result = win32Failure();
    if (SUCCEEDED(result)) {
        previous_ = nullptr; thread_ = 0;
        name_.clear(); diagnosticDefaultName_.clear(); inputName_.clear();
    }
    return result;
}

const PrivateDesktop* PrivateDesktop::current() noexcept { return currentDesktop; }

HRESULT PrivateDesktop::verifyIsolation(bool* inputDesktopUnchanged) const {
    if (inputDesktopUnchanged) *inputDesktopUnchanged = false;
    if (!desktop_ || thread_ != GetCurrentThreadId()) return E_ACCESSDENIED;
    std::wstring current;
    auto hr = objectName(GetThreadDesktop(GetCurrentThreadId()), current);
    if (FAILED(hr)) return hr;
    std::wstring input;
    hr = inputName(input);
    if (FAILED(hr)) return hr;
    const bool unchanged = _wcsicmp(input.c_str(), inputName_.c_str()) == 0;
    if (inputDesktopUnchanged) *inputDesktopUnchanged = unchanged;
    return _wcsicmp(current.c_str(), name_.c_str()) == 0 &&
        _wcsicmp(current.c_str(), input.c_str()) != 0 && unchanged ? S_OK : E_ACCESSDENIED;
}

HRESULT PrivateDesktop::visibleWindowsOnInputDesktop(bool& visible) const {
    visible = false;
    const auto isolation = verifyIsolation();
    if (FAILED(isolation)) return isolation;
    DesktopHandle input{OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS | DESKTOP_ENUMERATE)};
    if (!input.value) return win32Failure();
    struct Observation { DWORD process; bool visible; } state{GetCurrentProcessId(), false};
    SetLastError(ERROR_SUCCESS);
    const auto enumerated = EnumDesktopWindows(input.value, [](HWND window, LPARAM param) -> BOOL {
        auto& observation = *reinterpret_cast<Observation*>(param);
        DWORD process = 0;
        GetWindowThreadProcessId(window, &process);
        if (process == observation.process && IsWindowVisible(window)) observation.visible = true;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&state));
    if (!enumerated) return win32Failure();
    visible = state.visible;
    return S_OK;
}

VisualSourceReadLease::~VisualSourceReadLease() {
    if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
}

HRESULT VisualSourceReadLease::acquire(const PrivateDesktop& desktop, const std::filesystem::path& path) {
    if (FAILED(desktop.verifyIsolation()) || PrivateDesktop::current() != &desktop) return E_ACCESSDENIED;
    if (held()) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
    if (path.empty() || !path.is_absolute()) return E_INVALIDARG;
    struct File { HANDLE value = INVALID_HANDLE_VALUE;
        ~File() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); } } file;
    file.value = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL, nullptr);
    if (file.value == INVALID_HANDLE_VALUE) return win32Failure();
    FILE_ATTRIBUTE_TAG_INFO tag{};
    DWORD flags = 0;
    if (!GetFileInformationByHandleEx(file.value, FileAttributeTagInfo, &tag, sizeof(tag)) ||
        !GetHandleInformation(file.value, &flags)) return win32Failure();
    if (flags & HANDLE_FLAG_INHERIT) return E_ACCESSDENIED;
    if (tag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE |
        FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS)) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    file_ = file.value;
    file.value = INVALID_HANDLE_VALUE;
    return S_OK;
}

struct DocumentsLibraryVisualSource::Impl {
    VisualSourceReadLease lease; // Destroyed after retained native interfaces.
    struct FileSnapshot {
        FILE_ID_INFO identity{};
        FILE_BASIC_INFO basic{};
        LARGE_INTEGER size{};
        std::vector<BYTE> bytes;
    } file;
    struct Metadata {
        DocumentsLibrarySourceReadback readback;
        GUID type{};
        ComPtr<IShellItem> privateSave, publicSave;
    } metadata;
    ComPtr<IShellItem> item;
    std::wstring path;
    std::wstring parsingName;
    DWORD thread = 0;

    static bool sameIdentity(const FILE_ID_INFO& a, const FILE_ID_INFO& b) noexcept {
        return a.VolumeSerialNumber == b.VolumeSerialNumber &&
            std::memcmp(a.FileId.Identifier, b.FileId.Identifier, sizeof(a.FileId.Identifier)) == 0;
    }
    static bool sameFileMetadata(const FileSnapshot& a, const FileSnapshot& b) noexcept {
        return sameIdentity(a.identity, b.identity) && a.size.QuadPart == b.size.QuadPart &&
            a.basic.CreationTime.QuadPart == b.basic.CreationTime.QuadPart &&
            a.basic.LastWriteTime.QuadPart == b.basic.LastWriteTime.QuadPart &&
            a.basic.ChangeTime.QuadPart == b.basic.ChangeTime.QuadPart &&
            a.basic.FileAttributes == b.basic.FileAttributes;
    }
    static HRESULT readFile(const std::wstring& path, FileSnapshot& result) {
        struct File { HANDLE value = INVALID_HANDLE_VALUE;
            ~File() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); } } file;
        file.value = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL, nullptr);
        if (file.value == INVALID_HANDLE_VALUE) return win32Failure();
        FILE_ATTRIBUTE_TAG_INFO tag{};
        if (!GetFileInformationByHandleEx(file.value, FileAttributeTagInfo, &tag, sizeof(tag))) return win32Failure();
        constexpr DWORD unsupportedAttributes = FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE |
            FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS;
        if (tag.FileAttributes & unsupportedAttributes) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        const auto readMetadata = [&](FileSnapshot& value) -> HRESULT {
            if (!GetFileInformationByHandleEx(file.value, FileIdInfo, &value.identity, sizeof(value.identity)) ||
                !GetFileInformationByHandleEx(file.value, FileBasicInfo, &value.basic, sizeof(value.basic)) ||
                !GetFileSizeEx(file.value, &value.size)) return win32Failure();
            if (value.basic.FileAttributes & unsupportedAttributes) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
            return (value.basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? E_INVALIDARG : S_OK;
        };
        FileSnapshot before, after;
        auto hr = readMetadata(before);
        if (FAILED(hr)) return hr;
        if (before.size.QuadPart <= 0 || before.size.QuadPart > 1024 * 1024)
            return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
        const auto byteCount = static_cast<DWORD>(before.size.QuadPart); // bounded above to 1 MiB
        before.bytes.resize(byteCount);
        DWORD copied = 0;
        if (!ReadFile(file.value, before.bytes.data(), byteCount, &copied, nullptr))
            return win32Failure();
        if (copied != byteCount) return HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
        hr = readMetadata(after);
        if (FAILED(hr)) return hr;
        if (!sameFileMetadata(before, after)) return HRESULT_FROM_WIN32(ERROR_RETRY);
        result = std::move(before);
        return S_OK;
    }
    static HRESULT readMetadata(Metadata& result) {
        ComPtr<IShellLibrary> library;
        auto hr = SHLoadLibraryFromKnownFolder(FOLDERID_DocumentsLibrary,
            STGM_READ | STGM_SHARE_DENY_NONE, IID_PPV_ARGS(&library));
        result.readback.loadRead = hr;
        if (FAILED(hr)) return hr;
        if (!library) return E_UNEXPECTED;
        LIBRARYOPTIONFLAGS options{};
        result.readback.optionsRead = library->GetOptions(&options);
        result.readback.options = static_cast<DWORD>(options);
        result.readback.typeRead = library->GetFolderType(&result.type);
        result.readback.documentsType = SUCCEEDED(result.readback.typeRead) &&
            libraryKindForType(result.type) == LibraryKind::Documents;
        ComPtr<IShellItemArray> folders;
        result.readback.foldersRead = library->GetFolders(LFF_ALLITEMS, IID_PPV_ARGS(&folders));
        if (SUCCEEDED(result.readback.foldersRead)) result.readback.foldersRead = folders ?
            folders->GetCount(&result.readback.folderCount) : E_UNEXPECTED;
        result.readback.privateSaveRead = library->GetDefaultSaveFolder(DSFT_PRIVATE, IID_PPV_ARGS(&result.privateSave));
        result.readback.publicSaveRead = library->GetDefaultSaveFolder(DSFT_PUBLIC, IID_PPV_ARGS(&result.publicSave));
        if (FAILED(result.readback.optionsRead)) return result.readback.optionsRead;
        if (FAILED(result.readback.typeRead)) return result.readback.typeRead;
        return result.readback.foldersRead;
    }
    static bool sourceUnavailable(HRESULT hr) noexcept {
        return hr == E_INVALIDARG || hr == E_ACCESSDENIED || hr == E_FAIL || hr == E_NOTIMPL ||
            hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND) ||
            hr == HRESULT_FROM_WIN32(ERROR_INVALID_DATA) || hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) ||
            hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION) || hr == HRESULT_FROM_WIN32(ERROR_LOCK_VIOLATION) ||
            hr == STG_E_SHAREVIOLATION || hr == STG_E_LOCKVIOLATION || hr == STG_E_ACCESSDENIED;
    }
    static bool sameItem(IShellItem* a, IShellItem* b) {
        if (!a || !b) return a == b;
        int order = 1;
        return SUCCEEDED(a->Compare(b, SICHINT_CANONICAL, &order)) && order == 0;
    }
};

DocumentsLibraryVisualSource::DocumentsLibraryVisualSource() noexcept = default;
DocumentsLibraryVisualSource::~DocumentsLibraryVisualSource() = default;

HRESULT DocumentsLibraryVisualSource::resolve(const PrivateDesktop& desktop, std::wstring& location,
                                               DocumentsLibrarySourceReadback& readback) {
    readback = {};
    readback.requested = true;
    if (FAILED(desktop.verifyIsolation()) || PrivateDesktop::current() != &desktop) return E_ACCESSDENIED;
    if (impl_) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
    APTTYPE apartment{}; APTTYPEQUALIFIER qualifier{};
    const auto initialized = CoGetApartmentType(&apartment, &qualifier);
    if (FAILED(initialized)) return initialized;
    if (apartment != APTTYPE_STA && apartment != APTTYPE_MAINSTA) return E_ACCESSDENIED;
    try {
        auto candidate = std::make_unique<Impl>();
        // Protect the descriptor before any namespace/library binding can
        // inspect it. No known-folder creation or data-provider recall flags.
        PWSTR path = nullptr;
        readback.pathRead = SHGetKnownFolderPath(FOLDERID_DocumentsLibrary, 0, nullptr, &path);
        struct Text { PWSTR value; ~Text() { CoTaskMemFree(value); } } text{path};
        readback.resolveRead = readback.pathRead;
        if (FAILED(readback.pathRead)) {
            readback.unavailable = Impl::sourceUnavailable(readback.pathRead);
            return readback.pathRead;
        }
        if (!path || !*path || !std::filesystem::path(path).is_absolute()) return E_UNEXPECTED;
        candidate->path = path;
        readback.leaseRead = candidate->lease.acquire(desktop, candidate->path);
        readback.writeProtected = candidate->lease.held();
        if (FAILED(readback.leaseRead)) {
            readback.unsupported = readback.leaseRead == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
            readback.unavailable = Impl::sourceUnavailable(readback.leaseRead);
            return readback.leaseRead;
        }
        readback.fileRead = Impl::readFile(candidate->path, candidate->file);
        if (FAILED(readback.fileRead)) {
            readback.unavailable = Impl::sourceUnavailable(readback.fileRead);
            return readback.fileRead;
        }
        PIDLIST_ABSOLUTE raw = nullptr;
        readback.knownFolderRead = SHGetKnownFolderIDList(FOLDERID_DocumentsLibrary, 0, nullptr, &raw);
        readback.resolveRead = readback.knownFolderRead;
        struct Pidl { PIDLIST_ABSOLUTE value; ~Pidl() { CoTaskMemFree(value); } } pidl{raw};
        if (SUCCEEDED(readback.resolveRead)) {
            readback.itemRead = raw ? SHCreateItemFromIDList(raw, IID_PPV_ARGS(&candidate->item)) : E_UNEXPECTED;
            readback.resolveRead = readback.itemRead;
        }
        PWSTR parsingName = nullptr;
        if (SUCCEEDED(readback.resolveRead)) {
            readback.parsingNameRead = candidate->item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &parsingName);
            readback.resolveRead = readback.parsingNameRead;
        }
        Text parsingText{parsingName};
        if (FAILED(readback.resolveRead)) {
            readback.unavailable = Impl::sourceUnavailable(readback.resolveRead);
            return readback.resolveRead;
        }
        if (!path || !*path || !std::filesystem::path(path).is_absolute() || !parsingName || !*parsingName) return E_UNEXPECTED;
        candidate->parsingName = parsingName;
        ComPtr<IShellItem> parsed;
        readback.parsingItemRead = SHCreateItemFromParsingName(candidate->parsingName.c_str(), nullptr, IID_PPV_ARGS(&parsed));
        if (FAILED(readback.parsingItemRead)) return readback.resolveRead = readback.parsingItemRead;
        int same = 1;
        readback.parsingIdentityRead = candidate->item->Compare(parsed.Get(), SICHINT_CANONICAL, &same);
        if (FAILED(readback.parsingIdentityRead)) return readback.resolveRead = readback.parsingIdentityRead;
        if (same != 0) return readback.resolveRead = E_INVALIDARG;
        auto hr = Impl::readMetadata(candidate->metadata);
        const auto resolution = readback;
        readback = candidate->metadata.readback;
        readback.requested = true;
        readback.leaseRead = resolution.leaseRead;
        readback.writeProtected = resolution.writeProtected;
        readback.resolveRead = resolution.resolveRead;
        readback.knownFolderRead = resolution.knownFolderRead;
        readback.itemRead = resolution.itemRead;
        readback.pathRead = resolution.pathRead;
        readback.parsingNameRead = resolution.parsingNameRead;
        readback.parsingItemRead = resolution.parsingItemRead;
        readback.parsingIdentityRead = resolution.parsingIdentityRead;
        readback.fileRead = resolution.fileRead;
        const auto metadataRead = hr;
        Impl::FileSnapshot after;
        readback.fileRead = Impl::readFile(candidate->path, after);
        if (FAILED(readback.fileRead)) return readback.fileRead;
        readback.backingFileUnchanged = Impl::sameFileMetadata(candidate->file, after) && candidate->file.bytes == after.bytes;
        if (!readback.backingFileUnchanged)
            return HRESULT_FROM_WIN32(ERROR_RETRY);
        if (FAILED(metadataRead)) {
            readback.unavailable = FAILED(readback.loadRead) && Impl::sourceUnavailable(readback.loadRead);
            return metadataRead;
        }
        const auto isolation = desktop.verifyIsolation();
        if (FAILED(isolation)) return isolation;
        candidate->thread = GetCurrentThreadId();
        location = candidate->parsingName;
        impl_ = std::move(candidate);
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}

HRESULT DocumentsLibraryVisualSource::verify(const PrivateDesktop& desktop, IShellItem* current,
                                              DocumentsLibrarySourceReadback& readback) const {
    readback.verification = {};
    readback.verification.stage = 1;
    readback.currentMatches = readback.backingFileUnchanged = readback.metadataUnchanged = false;
    if (!impl_ || !impl_->lease.held() || !current || impl_->thread != GetCurrentThreadId() ||
        FAILED(desktop.verifyIsolation()) || PrivateDesktop::current() != &desktop) return readback.verifyRead = E_ACCESSDENIED;
    try {
        auto& proof = readback.verification;
        proof.stage = 2;
        int same = 1;
        proof.currentRead = impl_->item->Compare(current, SICHINT_CANONICAL, &same);
        readback.currentMatches = SUCCEEDED(proof.currentRead) && same == 0;
        if (!readback.currentMatches) return readback.verifyRead = E_INVALIDARG;
        proof.stage = 3;
        PIDLIST_ABSOLUTE raw = nullptr;
        proof.knownFolderRead = SHGetKnownFolderIDList(FOLDERID_DocumentsLibrary, 0, nullptr, &raw);
        auto hr = proof.knownFolderRead;
        struct Pidl { PIDLIST_ABSOLUTE value; ~Pidl() { CoTaskMemFree(value); } } pidl{raw};
        ComPtr<IShellItem> known;
        if (SUCCEEDED(hr)) hr = proof.knownItemRead = raw ? SHCreateItemFromIDList(raw, IID_PPV_ARGS(&known)) : E_UNEXPECTED;
        if (FAILED(hr)) return readback.verifyRead = hr;
        proof.stage = 4;
        same = 1;
        proof.knownIdentityRead = impl_->item->Compare(known.Get(), SICHINT_CANONICAL, &same);
        proof.knownMatches = SUCCEEDED(proof.knownIdentityRead) && same == 0;
        if (!proof.knownMatches) return readback.verifyRead = HRESULT_FROM_WIN32(ERROR_RETRY);
        proof.stage = 5;
        PWSTR path = nullptr;
        hr = proof.pathRead = SHGetKnownFolderPath(FOLDERID_DocumentsLibrary, 0, nullptr, &path);
        struct Text { PWSTR value; ~Text() { CoTaskMemFree(value); } } text{path};
        if (FAILED(hr)) return readback.verifyRead = hr;
        proof.pathMatches = path && _wcsicmp(path, impl_->path.c_str()) == 0;
        if (!proof.pathMatches)
            return readback.verifyRead = HRESULT_FROM_WIN32(ERROR_RETRY);
        proof.stage = 6;
        Impl::Metadata metadata;
        hr = proof.metadataRead = Impl::readMetadata(metadata);
        if (FAILED(hr)) return readback.verifyRead = hr;
        const auto& before = impl_->metadata.readback;
        const auto& after = metadata.readback;
        proof.stage = 7;
        proof.optionsUnchanged = before.options == after.options;
        proof.typeUnchanged = impl_->metadata.type == metadata.type;
        proof.folderCountUnchanged = before.folderCount == after.folderCount;
        proof.privateSaveStatusUnchanged = before.privateSaveRead == after.privateSaveRead;
        proof.publicSaveStatusUnchanged = before.publicSaveRead == after.publicSaveRead;
        const auto compareSave = [](IShellItem* a, IShellItem* b, HRESULT& status) {
            if (!a || !b) return a == b; // Getter HRESULTs separately prove unchanged absence.
            int order = 1;
            status = a->Compare(b, SICHINT_CANONICAL, &order);
            return SUCCEEDED(status) && order == 0;
        };
        proof.privateSaveMatches = compareSave(impl_->metadata.privateSave.Get(), metadata.privateSave.Get(), proof.privateSaveRead);
        proof.publicSaveMatches = compareSave(impl_->metadata.publicSave.Get(), metadata.publicSave.Get(), proof.publicSaveRead);
        readback.metadataUnchanged = proof.optionsUnchanged && proof.typeUnchanged && proof.folderCountUnchanged &&
            proof.privateSaveStatusUnchanged && proof.publicSaveStatusUnchanged && proof.privateSaveMatches && proof.publicSaveMatches;
        proof.stage = 8;
        Impl::FileSnapshot file;
        readback.fileRead = Impl::readFile(impl_->path, file);
        if (FAILED(readback.fileRead)) return readback.verifyRead = readback.fileRead;
        proof.stage = 9;
        proof.fileIdentityUnchanged = Impl::sameIdentity(impl_->file.identity, file.identity);
        proof.fileSizeUnchanged = impl_->file.size.QuadPart == file.size.QuadPart;
        proof.fileCreationUnchanged = impl_->file.basic.CreationTime.QuadPart == file.basic.CreationTime.QuadPart;
        proof.fileWriteUnchanged = impl_->file.basic.LastWriteTime.QuadPart == file.basic.LastWriteTime.QuadPart;
        proof.fileChangeUnchanged = impl_->file.basic.ChangeTime.QuadPart == file.basic.ChangeTime.QuadPart;
        proof.fileAttributesUnchanged = impl_->file.basic.FileAttributes == file.basic.FileAttributes;
        proof.fileBytesUnchanged = impl_->file.bytes == file.bytes;
        readback.backingFileUnchanged = proof.fileIdentityUnchanged && proof.fileSizeUnchanged && proof.fileCreationUnchanged &&
            proof.fileWriteUnchanged && proof.fileChangeUnchanged && proof.fileAttributesUnchanged && proof.fileBytesUnchanged;
        proof.stage = 10;
        hr = desktop.verifyIsolation();
        if (FAILED(hr)) return readback.verifyRead = hr;
        proof.stage = 11;
        return readback.verifyRead = readback.metadataUnchanged && readback.backingFileUnchanged ? S_OK :
            HRESULT_FROM_WIN32(ERROR_RETRY);
    } catch (const std::bad_alloc&) { return readback.verifyRead = E_OUTOFMEMORY; }
}

HRESULT DocumentsLibraryVisualSource::verifyMetadata(const PrivateDesktop& desktop,
                                                     DocumentsLibrarySourceReadback& readback) const {
    return verify(desktop, impl_ ? impl_->item.Get() : nullptr, readback);
}

HRESULT captureWindowPng(const PrivateDesktop& desktop, HWND window,
                         const std::filesystem::path& output,
                         const VisualCaptureOptions& options,
                         VisualCaptureReport& report) {
    if (!window || !IsWindow(window) || output.empty() || !output.is_absolute() ||
        (options.nativeClientPrint && options.includeFrame) ||
        options.layoutDpi < 48 || options.layoutDpi > 768 || options.minimumInkFraction < 0 ||
        options.minimumInkFraction > 1 || options.maximumUnpaintedFraction < 0 ||
        options.maximumUnpaintedFraction > 1) return E_INVALIDARG;
    const auto owner = GetWindowThreadProcessId(window, nullptr);
    if (owner != GetCurrentThreadId()) return E_ACCESSDENIED;
    VisualCaptureReport result;
    result.searchDateExpansion=options.searchDatePopup;
    auto hr = desktop.verifyIsolation(&result.inputDesktopUnchanged);
    if (FAILED(hr)) return hr;
    hr = desktop.visibleWindowsOnInputDesktop(result.visibleInputDesktopWindows);
    if (FAILED(hr)) return hr;
    if (result.visibleInputDesktopWindows) return E_ACCESSDENIED;
    result.desktopName = desktop.name();
    result.inputDesktopName = desktop.originalInputName();
    result.windowDpi = GetDpiForWindow(window);
    result.layoutDpi = options.layoutDpi;
    if (options.ribbonFramework) {
        result.ribbonLayout = options.ribbonLayout;
        result.installedRibbonStatus = options.installedRibbonStatus;
        result.ribbonFeaturesRead = options.ribbonFeaturesRead;
        result.ribbonFeatures = options.ribbonFeatures;
        result.ribbonContexts = options.ribbonContexts;
        result.ribbonProviders = options.ribbonProviders;
        result.searchDateMenu = options.searchDateMenu;
        result.commandReadiness = options.commandReadiness;
        DWORD size = sizeof(result.hideFileExt);
        const auto registry = RegGetValueW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", L"HideFileExt",
            RRF_RT_REG_DWORD, nullptr, &result.hideFileExt, &size);
        PROPVARIANT value{};
        const auto native = options.ribbonFramework->GetUICommandProperty(Extensions, UI_PKEY_BooleanValue, &value);
        BOOL checked = FALSE;
        const auto converted = SUCCEEDED(native) ? PropVariantToBoolean(value, &checked) : native;
        PropVariantClear(&value);
        result.fileExtensionsRead = registry == ERROR_SUCCESS && result.hideFileExt <= 1 && SUCCEEDED(converted);
        result.osFileExtensions = result.hideFileExt == 0;
        result.ribbonFileExtensions = checked != FALSE;
        for (const auto command : std::array<UINT, 9>{RibbonExtractToGallery, Extract,
                 RibbonLibraryOptimizeMenu, LibraryOptimize, LibraryDefault,
                 RibbonEmail, RibbonSpecificPeople, RibbonStopSharing, RibbonShareGallery}) {
            VisualCaptureReport::RibbonCommandReadback readback;
            readback.id = command;
            value = {};
            readback.enabledRead = options.ribbonFramework->GetUICommandProperty(command, UI_PKEY_Enabled, &value);
            checked = FALSE;
            if (SUCCEEDED(readback.enabledRead)) readback.enabledRead = PropVariantToBoolean(value, &checked);
            readback.enabled = SUCCEEDED(readback.enabledRead) && checked != FALSE;
            PropVariantClear(&value);
            if (command == Extract) {
                value = {};
                readback.labelRead = options.ribbonFramework->GetUICommandProperty(command, UI_PKEY_Label, &value);
                PWSTR label = nullptr;
                if (SUCCEEDED(readback.labelRead)) readback.labelRead = PropVariantToStringAlloc(value, &label);
                if (SUCCEEDED(readback.labelRead) && label) readback.label = label;
                CoTaskMemFree(label);
                PropVariantClear(&value);
            } else {
                value = {};
                readback.itemsRead = options.ribbonFramework->GetUICommandProperty(command, UI_PKEY_ItemsSource, &value);
                ComPtr<IUICollection> items;
                if (SUCCEEDED(readback.itemsRead)) {
                    readback.itemsRead = value.vt == VT_UNKNOWN && value.punkVal ?
                        value.punkVal->QueryInterface(IID_PPV_ARGS(&items)) : E_NOINTERFACE;
                }
                if (SUCCEEDED(readback.itemsRead)) readback.itemsRead = items ? items->GetCount(&readback.itemCount) : E_UNEXPECTED;
                if (SUCCEEDED(readback.itemsRead) && readback.itemCount) {
                    ComPtr<IUnknown> first;
                    ComPtr<IUISimplePropertySet> properties;
                    readback.imageRead = items->GetItem(0, &first);
                    if (SUCCEEDED(readback.imageRead)) readback.imageRead = first ? first.As(&properties) : E_UNEXPECTED;
                    PROPVARIANT itemImage{};
                    if (SUCCEEDED(readback.imageRead)) readback.imageRead = properties ?
                        properties->GetValue(UI_PKEY_ItemImage, &itemImage) : E_UNEXPECTED;
                    ComPtr<IUIImage> nativeImage;
                    if (SUCCEEDED(readback.imageRead)) readback.imageRead = itemImage.vt == VT_UNKNOWN && itemImage.punkVal ?
                        itemImage.punkVal->QueryInterface(IID_PPV_ARGS(&nativeImage)) : E_NOINTERFACE;
                    readback.firstItemImage = SUCCEEDED(readback.imageRead) && nativeImage;
                    PropVariantClear(&itemImage);
                }
                PropVariantClear(&value);
            }
            result.ribbonCommands.push_back(std::move(readback));
        }
    }
    RECT windowRect{};
    if (!GetWindowRect(window, &windowRect) || !GetClientRect(window, &result.clientBounds)) return win32Failure();
    MapWindowPoints(window, nullptr, reinterpret_cast<POINT*>(&result.clientBounds), 2);
    RECT printRect = options.includeFrame ? windowRect : result.clientBounds;
    RECT captureRect = printRect;
    HWND printSource = window;
    const auto targetClientScreen = result.clientBounds;
    if (options.nativeClientCropPrintLayout &&
        (!options.nativeClientCropSource ||
         (*options.nativeClientCropPrintLayout & ~static_cast<DWORD>(LAYOUT_RTL | LAYOUT_BITMAPORIENTATIONPRESERVED)))) return E_INVALIDARG;
    if (options.nativeClientCropPrintFlags && (!options.nativeClientCropSource ||
        (*options.nativeClientCropPrintFlags != 0 && *options.nativeClientCropPrintFlags != PW_RENDERFULLCONTENT))) return E_INVALIDARG;
    if (options.nativeClientCropSource) {
        printSource = options.nativeClientCropSource;
        DWORD sourceProcess=0;
        if(options.includeFrame||options.nativeClientPrint||options.nativeClientCropSourceImage.empty()||
            !options.nativeClientCropSourceImage.is_absolute()||options.nativeClientCropSourceImage==output||
            !IsWindow(printSource)||GetWindowThreadProcessId(printSource,&sourceProcess)!=GetCurrentThreadId()||
            sourceProcess!=GetCurrentProcessId()||GetAncestor(window,GA_ROOT)!=printSource||!IsChild(printSource,window))return E_ACCESSDENIED;
        if(!GetWindowRect(printSource,&printRect))return win32Failure();
        if(captureRect.left<printRect.left||captureRect.top<printRect.top||captureRect.right>printRect.right||
            captureRect.bottom>printRect.bottom)return E_INVALIDARG;
        result.nativeClientCropped=true;
        result.nativeClientCropSourceImage=options.nativeClientCropSourceImage;
        result.nativeClientCropPrintLayout=options.nativeClientCropPrintLayout;
        const auto sourceDc=GetWindowDC(printSource);
        if(!sourceDc)return win32Failure();
        result.printSourceDcLayout=GetLayout(sourceDc);
        result.printSourceDcMapMode=GetMapMode(sourceDc);
        ReleaseDC(printSource,sourceDc);
        if(result.printSourceDcLayout==GDI_ERROR||!result.printSourceDcMapMode)return E_FAIL;
    }
    result.printSourceWindow=reinterpret_cast<UINT_PTR>(printSource);
    result.printTargetWindow=reinterpret_cast<UINT_PTR>(window);
    result.printSourceBounds=printRect;
    result.printTargetClientBounds=targetClientScreen;
    if (options.includeFrame && options.trimInvisibleFrame) {
        RECT extended{};
        if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &extended, sizeof(extended))) &&
            extended.left >= windowRect.left && extended.top >= windowRect.top &&
            extended.right <= windowRect.right && extended.bottom <= windowRect.bottom &&
            extended.right > extended.left && extended.bottom > extended.top) {
            captureRect = extended;
            result.invisibleFrameTrimmed = !EqualRect(&captureRect, &windowRect);
        }
    }
    OffsetRect(&result.clientBounds, -captureRect.left, -captureRect.top);
    const auto width = captureRect.right - captureRect.left;
    const auto height = captureRect.bottom - captureRect.top;
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192 ||
        static_cast<size_t>(width) * static_cast<size_t>(height) > MaximumPixels) return E_INVALIDARG;
    result.width = static_cast<unsigned>(width);
    result.height = static_cast<unsigned>(height);
    collectWidgets(window, captureRect, result);
    if (options.requireVisibleChildren && !result.visibleChildren) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    Dib nativeDib;
    const auto printWidth = static_cast<unsigned>(printRect.right - printRect.left);
    const auto printHeight = static_cast<unsigned>(printRect.bottom - printRect.top);
    if (printWidth > 8192 || printHeight > 8192 ||
        static_cast<size_t>(printWidth) * printHeight > MaximumPixels) return E_INVALIDARG;
    hr = nativeDib.initialize(printWidth, printHeight);
    if (FAILED(hr)) return hr;
    if(result.nativeClientCropped) {
        result.printMemoryDcInitialLayout=GetLayout(nativeDib.dc);
        result.printMemoryDcInitialMapMode=GetMapMode(nativeDib.dc);
        if(options.nativeClientCropPrintLayout&&SetLayout(nativeDib.dc,*options.nativeClientCropPrintLayout)==GDI_ERROR)return win32Failure();
        result.printMemoryDcBeforeLayout=GetLayout(nativeDib.dc);
        result.printMemoryDcBeforeMapMode=GetMapMode(nativeDib.dc);
    }
    // PW_RENDERFULLCONTENT asks modern native controls to draw their content.
    // This captures the HWND's actual native surface/print implementation;
    // Windows can use a painted surface without dispatching WM_PRINT.
    result.printWindowFlags = options.nativeClientCropPrintFlags ? *options.nativeClientCropPrintFlags :
        options.nativeClientPrint ? PW_CLIENTONLY :
        PW_RENDERFULLCONTENT | (options.includeFrame || result.nativeClientCropped ? 0 : PW_CLIENTONLY);
    result.printWindowSucceeded = PrintWindow(printSource, nativeDib.dc, result.printWindowFlags) != FALSE;
    GdiFlush();
    if(result.nativeClientCropped) {
        result.printMemoryDcAfterLayout=GetLayout(nativeDib.dc);
        result.printMemoryDcAfterMapMode=GetMapMode(nativeDib.dc);
    }
    if (!result.printWindowSucceeded) return win32Failure();
    if(result.nativeClientCropped) {
        RECT sourceNow{},targetNow{},clientNow{};
        if(!IsWindow(window)||!IsWindow(printSource)||GetAncestor(window,GA_ROOT)!=printSource||!IsChild(printSource,window)||
            !GetWindowRect(printSource,&sourceNow)||!EqualRect(&sourceNow,&printRect)||
            !GetWindowRect(window,&targetNow)||!EqualRect(&targetNow,&windowRect)||!GetClientRect(window,&clientNow))return E_FAIL;
        SetLastError(ERROR_SUCCESS);
        const auto mapped=MapWindowPoints(window,nullptr,reinterpret_cast<POINT*>(&clientNow),2);
        if((!mapped&&GetLastError()!=ERROR_SUCCESS)||!EqualRect(&clientNow,&targetClientScreen))return E_FAIL;
        hr=desktop.verifyIsolation();if(FAILED(hr))return hr;
        hr=encodePng(options.nativeClientCropSourceImage,nativeDib,printWidth,printHeight,options.layoutDpi);
        if(FAILED(hr))return hr;
    }
    if(options.ribbonFramework) {
        PROPVARIANT selected{};
        result.layoutGallerySelectedRead=options.ribbonFramework->GetUICommandProperty(
            RibbonLayoutGallery,UI_PKEY_SelectedItem,&selected);
        ULONG index=0xffffffffu;
        if(SUCCEEDED(result.layoutGallerySelectedRead))
            result.layoutGallerySelectedRead=PropVariantToUInt32(selected,&index);
        if(SUCCEEDED(result.layoutGallerySelectedRead))result.layoutGallerySelected=index;
        PropVariantClear(&selected);
    }
    if(options.nativeRibbonFramework&&options.layoutGalleryNativeCommand) {
        result.layoutGalleryNativeCommand=options.layoutGalleryNativeCommand;
        PROPVARIANT selected{};ULONG index=0xffffffffu;
        result.layoutGalleryNativeSelectedRead=options.nativeRibbonFramework->GetUICommandProperty(
            options.layoutGalleryNativeCommand,UI_PKEY_SelectedItem,&selected);
        if(SUCCEEDED(result.layoutGalleryNativeSelectedRead))
            result.layoutGalleryNativeSelectedRead=PropVariantToUInt32(selected,&index);
        if(SUCCEEDED(result.layoutGalleryNativeSelectedRead))result.layoutGalleryNativeSelected=index;
        PropVariantClear(&selected);
    }
    const auto navigation = GetDlgItem(window, 901);
    for (auto& button : result.navigationButtons)
        button.drawRead = navigation ? chromeToolbarDrawReadback(navigation,
            static_cast<UINT>(button.command), &button.draw) : HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    const auto breadcrumb = GetDlgItem(window, 902);
    for (auto& button : result.breadcrumbButtons)
        button.drawRead = breadcrumb ? chromeToolbarDrawReadback(breadcrumb,
            static_cast<UINT>(button.command), &button.draw) : HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    if(options.searchDateMenu.requested) {
        auto& popupRead=result.searchDatePopup;
        const auto& popup=options.searchDatePopup;
        popupRead.window=reinterpret_cast<UINT_PTR>(popup.window);
        popupRead.physicalRows=static_cast<UINT>(popup.rowBounds.size());
        const auto printPopup=[&]()->HRESULT {
            if(!options.searchDateMenu.expanded||FAILED(options.searchDateMenu.read)||
                options.searchDateMenu.expectedRows!=8||options.searchDateMenu.matchedRows!=8||
                popup.rowBounds.size()!=8||!popup.window||!IsWindow(popup.window)||!IsWindowVisible(popup.window)||
                popup.window==window||GetAncestor(popup.window,GA_ROOTOWNER)!=window)return E_INVALIDARG;
            DWORD process=0;
            const auto thread=GetWindowThreadProcessId(popup.window,&process);
            if(thread!=owner||process!=GetCurrentProcessId())return E_ACCESSDENIED;
            std::wstring popupDesktop;
            auto checked=objectName(GetThreadDesktop(thread),popupDesktop);
            if(FAILED(checked))return checked;
            if(popupDesktop!=desktop.name())return E_ACCESSDENIED;
            checked=desktop.verifyIsolation();if(FAILED(checked))return checked;
            RECT popupRect{};
            if(!GetWindowRect(popup.window,&popupRect))return win32Failure();
            if(!EqualRect(&popupRect,&popup.bounds))return HRESULT_FROM_WIN32(ERROR_RETRY);
            popupRead.bounds=popupRect;OffsetRect(&popupRead.bounds,-captureRect.left,-captureRect.top);
            const auto popupWidth=popupRect.right-popupRect.left,popupHeight=popupRect.bottom-popupRect.top;
            if(popupWidth<=0||popupHeight<=0||popupWidth>8192||popupHeight>8192||
                static_cast<size_t>(popupWidth)*popupHeight>MaximumPixels)return E_INVALIDARG;
            for(const auto& row:popup.rowBounds) {
                if(row.right<=row.left||row.bottom<=row.top||row.left<popupRect.left||row.top<popupRect.top||
                    row.right>popupRect.right||row.bottom>popupRect.bottom||row.left<captureRect.left||
                    row.top<captureRect.top||row.right>captureRect.right||row.bottom>captureRect.bottom)return E_INVALIDARG;
            }
            Dib popupDib;
            checked=popupDib.initialize(static_cast<unsigned>(popupWidth),static_cast<unsigned>(popupHeight));
            if(FAILED(checked))return checked;
            // The Date menu is a separate native owned HWND. Render its actual
            // surface, then place the unscaled pixels at its measured location.
            popupRead.printed=PrintWindow(popup.window,popupDib.dc,PW_RENDERFULLCONTENT)!=FALSE;
            if(!GdiFlush()||!popupRead.printed)return win32Failure();
            VisualCaptureReport popupStats;
            pixelStatistics(popupDib,static_cast<unsigned>(popupWidth),static_cast<unsigned>(popupHeight),popupStats);
            popupRead.uniqueColors=popupStats.uniqueColors;popupRead.inkFraction=popupStats.inkFraction;
            popupRead.unpaintedFraction=popupStats.unpaintedFraction;
            if(popupStats.uniqueColors<options.minimumUniqueColors||popupStats.inkFraction<options.minimumInkFraction||
                popupStats.unpaintedFraction>options.maximumUnpaintedFraction)return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            popupRead.minimumRowUniqueColors=std::numeric_limits<unsigned>::max();
            popupRead.minimumRowInkFraction=1;
            for(const auto& row:popup.rowBounds) {
                std::unordered_set<uint32_t> colors;size_t ink=0;
                for(LONG y=row.top;y<row.bottom;++y)for(LONG x=row.left;x<row.right;++x) {
                    const auto rgb=popupDib.pixels[static_cast<size_t>(y-popupRect.top)*popupWidth+
                        static_cast<size_t>(x-popupRect.left)]&0x00ffffff;
                    colors.insert(rgb);
                    if((rgb&0xff)<220||((rgb>>8)&0xff)<220||((rgb>>16)&0xff)<220)++ink;
                }
                const auto fraction=static_cast<double>(ink)/
                    (static_cast<double>(row.right-row.left)*(row.bottom-row.top));
                popupRead.minimumRowUniqueColors=std::min(popupRead.minimumRowUniqueColors,static_cast<unsigned>(colors.size()));
                popupRead.minimumRowInkFraction=std::min(popupRead.minimumRowInkFraction,fraction);
                if(colors.size()<2||fraction<options.minimumInkFraction)return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            }
            RECT currentRect{};
            if(!IsWindowVisible(popup.window)||GetAncestor(popup.window,GA_ROOTOWNER)!=window||
                !GetWindowRect(popup.window,&currentRect)||!EqualRect(&currentRect,&popupRect))
                return HRESULT_FROM_WIN32(ERROR_RETRY);
            RECT overlap{};
            if(!IntersectRect(&overlap,&popupRect,&printRect))return E_INVALIDARG;
            for(LONG y=overlap.top;y<overlap.bottom;++y)
                std::copy_n(popupDib.pixels+static_cast<size_t>(y-popupRect.top)*popupWidth+
                        static_cast<size_t>(overlap.left-popupRect.left),static_cast<size_t>(overlap.right-overlap.left),
                    nativeDib.pixels+static_cast<size_t>(y-printRect.top)*printWidth+
                        static_cast<size_t>(overlap.left-printRect.left));
            popupRead.ownedPrivate=true;
            return desktop.verifyIsolation();
        };
        popupRead.read=printPopup();
        if(FAILED(popupRead.read)) {report=std::move(result);return report.searchDatePopup.read;}
    } else if(options.searchDatePopup.window)return E_INVALIDARG;
    std::unique_ptr<Dib> cropped;
    if (result.invisibleFrameTrimmed || result.nativeClientCropped) {
        cropped = std::make_unique<Dib>();
        hr = cropped->initialize(result.width, result.height);
        if (FAILED(hr)) return hr;
        const auto offsetX = static_cast<unsigned>(captureRect.left - printRect.left);
        const auto offsetY = static_cast<unsigned>(captureRect.top - printRect.top);
        for (unsigned y = 0; y < result.height; ++y)
            std::copy_n(nativeDib.pixels + static_cast<size_t>(y + offsetY) * printWidth + offsetX,
                        result.width, cropped->pixels + static_cast<size_t>(y) * result.width);
    }
    const auto& dib = cropped ? *cropped : nativeDib;
    pixelStatistics(dib, result.width, result.height, result);
    const auto& inspection = options.pixelInspectionBounds;
    if(!IsRectEmpty(&inspection)) {
        if(inspection.left < 0 || inspection.top < 0 || inspection.right > width ||
            inspection.bottom > height) return E_INVALIDARG;
        result.pixelInspectionBounds=inspection;
        result.pixelInspectionBackground=options.pixelInspectionBackground;
        std::unordered_set<uint32_t> colors;
        size_t ink=0;
        uint64_t hash=14695981039346656037ull;
        const auto background=options.pixelInspectionBackground;
        const auto backgroundRgb=background ? (static_cast<uint32_t>(GetRValue(*background))<<16) |
            (static_cast<uint32_t>(GetGValue(*background))<<8) | GetBValue(*background) : 0u;
        for(LONG y=inspection.top;y<inspection.bottom;++y)for(LONG x=inspection.left;x<inspection.right;++x) {
            const auto rgb=dib.pixels[static_cast<size_t>(y)*result.width+static_cast<size_t>(x)]&0x00ffffff;
            colors.insert(rgb);
            if(background ? rgb!=backgroundRgb : (rgb&0xff)<220 || ((rgb>>8)&0xff)<220 || ((rgb>>16)&0xff)<220)++ink;
            for(unsigned shift=0;shift<24;shift+=8) {hash^=(rgb>>shift)&0xff;hash*=1099511628211ull;}
        }
        result.inspectionPixelHash=hash;
        result.inspectionUniqueColors=static_cast<unsigned>(colors.size());
        result.inspectionInkFraction=static_cast<double>(ink)/
            (static_cast<double>(inspection.right-inspection.left)*(inspection.bottom-inspection.top));
    }
    if (result.uniqueColors < options.minimumUniqueColors ||
        result.inkFraction < options.minimumInkFraction ||
        result.unpaintedFraction > options.maximumUnpaintedFraction) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    hr = desktop.verifyIsolation(&result.inputDesktopUnchanged);
    if (FAILED(hr)) return hr;
    hr = desktop.visibleWindowsOnInputDesktop(result.visibleInputDesktopWindows);
    if (FAILED(hr)) return hr;
    if (result.visibleInputDesktopWindows) return E_ACCESSDENIED;
    hr = encodePng(output, dib, result.width, result.height, options.layoutDpi);
    if (FAILED(hr)) return hr;
    report = std::move(result);
    return S_OK;
}

std::string documentsLibrarySourceJson(const DocumentsLibrarySourceReadback& source) {
    std::ostringstream stream;
    stream << std::boolalpha << "{\"requested\":" << source.requested
        << ",\"publishable\":" << (source.requested ? "false" : "null");
    const auto field = [&](const char* name, auto value) { stream << ",\"" << name << "\":" << value; };
    field("unavailable", source.unavailable); field("unsupported", source.unsupported);
    field("displayUnsupported", source.displayUnsupported); field("writeProtected", source.writeProtected);
    field("leaseReadHresult", static_cast<long>(source.leaseRead));
    field("resolveReadHresult", static_cast<long>(source.resolveRead));
    field("knownFolderReadHresult", static_cast<long>(source.knownFolderRead));
    field("itemReadHresult", static_cast<long>(source.itemRead));
    field("pathReadHresult", static_cast<long>(source.pathRead));
    field("parsingNameReadHresult", static_cast<long>(source.parsingNameRead));
    field("parsingItemReadHresult", static_cast<long>(source.parsingItemRead));
    field("parsingIdentityReadHresult", static_cast<long>(source.parsingIdentityRead));
    field("loadReadHresult", static_cast<long>(source.loadRead));
    field("fileReadHresult", static_cast<long>(source.fileRead));
    field("verifyReadHresult", static_cast<long>(source.verifyRead));
    field("currentMatches", source.currentMatches); field("backingFileUnchanged", source.backingFileUnchanged);
    field("metadataUnchanged", source.metadataUnchanged);
    field("optionsReadHresult", static_cast<long>(source.optionsRead)); field("options", source.options);
    field("typeReadHresult", static_cast<long>(source.typeRead)); field("documentsType", source.documentsType);
    field("foldersReadHresult", static_cast<long>(source.foldersRead)); field("folderCount", source.folderCount);
    field("privateSaveReadHresult", static_cast<long>(source.privateSaveRead));
    field("publicSaveReadHresult", static_cast<long>(source.publicSaveRead));
    const auto& proof = source.verification;
    stream << ",\"verification\":{\"stage\":" << proof.stage;
    field("currentReadHresult", static_cast<long>(proof.currentRead));
    field("knownFolderReadHresult", static_cast<long>(proof.knownFolderRead));
    field("knownItemReadHresult", static_cast<long>(proof.knownItemRead));
    field("knownIdentityReadHresult", static_cast<long>(proof.knownIdentityRead));
    field("pathReadHresult", static_cast<long>(proof.pathRead));
    field("metadataReadHresult", static_cast<long>(proof.metadataRead));
    field("privateSaveReadHresult", static_cast<long>(proof.privateSaveRead));
    field("publicSaveReadHresult", static_cast<long>(proof.publicSaveRead));
    field("knownMatches", proof.knownMatches); field("pathMatches", proof.pathMatches);
    field("optionsUnchanged", proof.optionsUnchanged); field("typeUnchanged", proof.typeUnchanged);
    field("folderCountUnchanged", proof.folderCountUnchanged);
    field("privateSaveStatusUnchanged", proof.privateSaveStatusUnchanged);
    field("publicSaveStatusUnchanged", proof.publicSaveStatusUnchanged);
    field("privateSaveMatches", proof.privateSaveMatches); field("publicSaveMatches", proof.publicSaveMatches);
    field("fileIdentityUnchanged", proof.fileIdentityUnchanged); field("fileSizeUnchanged", proof.fileSizeUnchanged);
    field("fileCreationUnchanged", proof.fileCreationUnchanged); field("fileWriteUnchanged", proof.fileWriteUnchanged);
    field("fileChangeUnchanged", proof.fileChangeUnchanged); field("fileAttributesUnchanged", proof.fileAttributesUnchanged);
    field("fileBytesUnchanged", proof.fileBytesUnchanged);
    stream << "}}";
    return stream.str();
}

HRESULT writeVisualCaptureReport(const std::filesystem::path& output,
                                 const VisualCaptureReport& report) {
    std::ostringstream stream;
    stream << "{\n  \"headless\":true,\n  \"privateDesktop\":true,\n  \"renderer\":\"native-PrintWindow-WIC\",\n"
        << "  \"width\":" << report.width << ",\n  \"height\":" << report.height
        << ",\n  \"windowDpi\":" << report.windowDpi << ",\n  \"layoutDpi\":" << report.layoutDpi
        << ",\n  \"ribbonLayout\":" << json(report.ribbonLayout)
        << ",\n  \"installedRibbonStatus\":" << static_cast<long>(report.installedRibbonStatus)
        << ",\n  \"documentsLibrarySource\":" << documentsLibrarySourceJson(report.documentsLibrarySource)
        << ",\n  \"desktop\":" << json(report.desktopName) << ",\n  \"inputDesktop\":" << json(report.inputDesktopName)
        << ",\n  \"inputDesktopUnchanged\":" << (report.inputDesktopUnchanged ? "true" : "false")
        << ",\n  \"visibleInputDesktopWindows\":" << (report.visibleInputDesktopWindows ? "true" : "false")
        << ",\n  \"printWindowSucceeded\":" << (report.printWindowSucceeded ? "true" : "false")
        << ",\n  \"printWindowFlags\":" << report.printWindowFlags
        << ",\n  \"nativeClientCrop\":{\"cropped\":" << (report.nativeClientCropped?"true":"false")
        << ",\"sourceWindow\":" << report.printSourceWindow << ",\"targetWindow\":" << report.printTargetWindow
        << ",\"sourceImage\":" << json(report.nativeClientCropSourceImage.wstring())
        << ",\"requestedDcLayout\":" << (report.nativeClientCropPrintLayout?std::to_string(*report.nativeClientCropPrintLayout):"null")
        << ",\"sourceDcLayout\":" << report.printSourceDcLayout << ",\"sourceDcMapMode\":" << report.printSourceDcMapMode
        << ",\"memoryDcInitialLayout\":" << report.printMemoryDcInitialLayout << ",\"memoryDcInitialMapMode\":" << report.printMemoryDcInitialMapMode
        << ",\"memoryDcBeforeLayout\":" << report.printMemoryDcBeforeLayout << ",\"memoryDcBeforeMapMode\":" << report.printMemoryDcBeforeMapMode
        << ",\"memoryDcAfterLayout\":" << report.printMemoryDcAfterLayout << ",\"memoryDcAfterMapMode\":" << report.printMemoryDcAfterMapMode
        << ",\"sourceBounds\":";
    rectJson(stream,report.printSourceBounds);stream << ",\"targetClientBounds\":";
    rectJson(stream,report.printTargetClientBounds);stream << '}'
        << ",\n  \"invisibleFrameTrimmed\":" << (report.invisibleFrameTrimmed ? "true" : "false")
        << ",\n  \"uniqueColors\":" << report.uniqueColors
        << ",\n  \"inkFraction\":" << std::setprecision(8) << report.inkFraction
        << ",\n  \"unpaintedFraction\":" << report.unpaintedFraction
        << ",\n  \"pixelInspection\":{\"bounds\":";
    rectJson(stream,report.pixelInspectionBounds);
    stream << ",\"uniqueColors\":" << report.inspectionUniqueColors
        << ",\"inkFraction\":" << report.inspectionInkFraction
        << ",\"pixelHash\":\"" << report.inspectionPixelHash << "\",\"backgroundColorref\":"
        << (report.pixelInspectionBackground ? std::to_string(*report.pixelInspectionBackground) : "null") << '}'
        << ",\n  \"visibleChildren\":" << report.visibleChildren << ",\n  \"clientBounds\":";
    rectJson(stream, report.clientBounds);
    stream << ",\n  \"nativeLayoutGallery\":{\"logicalCommand\":" << RibbonLayoutGallery
        << ",\"selectedReadHresult\":" << static_cast<long>(report.layoutGallerySelectedRead)
        << ",\"selectedIndex\":" << report.layoutGallerySelected
        << ",\"nativeCommand\":" << report.layoutGalleryNativeCommand
        << ",\"nativeSelectedReadHresult\":" << static_cast<long>(report.layoutGalleryNativeSelectedRead)
        << ",\"nativeSelectedIndex\":" << report.layoutGalleryNativeSelected << '}';
    stream << ",\n  \"installedFeatures\":{\"source\":\"read-only native edition, Media Foundation, disc-burning policy, and disk-cleanup capability\","
        << "\"readRequested\":" << (report.ribbonFeaturesRead ? "true" : "false")
        << ",\"bitLocker\":" << (report.ribbonFeatures.bitLocker ? "true" : "false")
        << ",\"editionHresult\":" << static_cast<long>(report.ribbonFeatures.editionStatus)
        << ",\"mediaFoundation\":" << (report.ribbonFeatures.mediaFoundation ? "true" : "false")
        << ",\"mediaFoundationHresult\":" << static_cast<long>(report.ribbonFeatures.mediaFoundationStatus)
        << ",\"discBurning\":" << (report.ribbonFeatures.discBurning ? "true" : "false")
        << ",\"discBurningHresult\":" << static_cast<long>(report.ribbonFeatures.discBurningStatus)
        << ",\"diskCleanup\":" << (report.ribbonFeatures.diskCleanup ? "true" : "false")
        << ",\"diskCleanupHresult\":" << static_cast<long>(report.ribbonFeatures.diskCleanupStatus) << '}';
    stream << ",\n  \"nativeRibbonContexts\":[";
    for (size_t i = 0; i < report.ribbonContexts.size(); ++i) {
        const auto& context = report.ribbonContexts[i];
        stream << (i ? "," : "") << "{\"logicalContext\":" << context.logicalContext
            << ",\"nativeIdentifier\":" << context.nativeIdentifier
            << ",\"availability\":" << context.availability
            << ",\"readHresult\":" << static_cast<long>(context.read) << '}';
    }
    stream << ']';
    stream << ",\n  \"nativeRibbonProviders\":[";
    for (size_t i = 0; i < report.ribbonProviders.size(); ++i) {
        const auto& provider = report.ribbonProviders[i];
        stream << (i ? "," : "") << "{\"command\":" << provider.command
            << ",\"selectedCount\":" << provider.selectedCount
            << ",\"cachedReadHresult\":" << static_cast<long>(provider.cachedRead)
            << ",\"cachedEnabled\":" << (provider.cachedEnabled ? "true" : "false")
            << ",\"cachedChecked\":" << (provider.cachedChecked ? "true" : "false")
            << ",\"cachedNativeState\":" << provider.cachedNativeState
            << ",\"pending\":" << (provider.pending ? "true" : "false")
            << ",\"slowStateCompleted\":" << (provider.slowStateCompleted ? "true" : "false")
            << ",\"nativeReadHresult\":" << static_cast<long>(provider.nativeRead)
            << ",\"nativeState\":" << provider.nativeState
            << ",\"registeredAttempted\":" << (provider.registeredAttempted ? "true" : "false")
            << ",\"registeredReadHresult\":" << static_cast<long>(provider.registeredRead)
            << ",\"registeredGenericReadHresult\":" << static_cast<long>(provider.registeredGenericRead)
            << ",\"registeredPlanStatus\":" << static_cast<long>(provider.registeredPlanStatus)
            << ",\"registeredRoute\":" << provider.registeredRoute
            << ",\"registeredRawStateReadHresult\":" << static_cast<long>(provider.registeredRawStateRead)
            << ",\"registeredState\":" << provider.registeredState
            << ",\"registeredCommandId\":" << provider.registeredCommandId
            << ",\"registeredSubmenu\":" << (provider.registeredSubmenu ? "true" : "false")
            << ",\"registeredEnabled\":" << (provider.registeredEnabled ? "true" : "false")
            << ",\"registeredReadMs\":" << provider.registeredReadMs
            << ",\"registeredSynchronous\":" << (provider.registeredSynchronous ? "true" : "false")
            << ",\"registeredPreservationReadHresult\":" << static_cast<long>(provider.registeredPreservationRead)
            << ",\"registeredTargetPreserved\":" << (provider.registeredTargetPreserved ? "true" : "false")
            << ",\"registeredSourcesPreserved\":" << (provider.registeredSourcesPreserved ? "true" : "false")
            << ",\"registeredSettingsPreserved\":" << (provider.registeredSettingsPreserved ? "true" : "false")
            << ",\"selectionMenuAttempted\":" << (provider.selectionMenuAttempted ? "true" : "false")
            << ",\"selectionMenuReadHresult\":" << static_cast<long>(provider.selectionMenuRead)
            << ",\"selectionMenuPlanReadHresult\":" << static_cast<long>(provider.selectionMenuPlanRead)
            << ",\"selectionMenuPlanStatus\":" << static_cast<long>(provider.selectionMenuPlanStatus)
            << ",\"selectionMenuPlanRoute\":" << provider.selectionMenuPlanRoute
            << ",\"selectionMenuPlanEnabled\":" << (provider.selectionMenuPlanEnabled ? "true" : "false")
            << ",\"selectionMenuRawReadHresult\":" << static_cast<long>(provider.selectionMenuRawRead)
            << ",\"selectionMenuMatches\":" << provider.selectionMenuMatches
            << ",\"selectionMenuState\":" << provider.selectionMenuState
            << ",\"selectionMenuCommandId\":" << provider.selectionMenuCommandId
            << ",\"selectionMenuSubmenu\":" << (provider.selectionMenuSubmenu ? "true" : "false")
            << ",\"selectionMenuAncestorDisabled\":" << (provider.selectionMenuAncestorDisabled ? "true" : "false")
            << ",\"selectionMenuReadMs\":" << provider.selectionMenuReadMs << '}';
    }
    stream << ']';
    stream << ",\n  \"commandReadiness\":{\"requested\":" << (report.commandReadiness.requested ? "true" : "false")
        << ",\"ready\":" << (report.commandReadiness.ready ? "true" : "false")
        << ",\"readHresult\":" << static_cast<long>(report.commandReadiness.read)
        << ",\"waitMs\":" << report.commandReadiness.waitMs
        << ",\"namespaceGeneration\":" << report.commandReadiness.namespaceGeneration
        << ",\"pendingCapabilities\":" << report.commandReadiness.pendingCapabilities
        << ",\"workerTasks\":" << report.commandReadiness.workerTasks
        << ",\"statePolls\":" << report.commandReadiness.statePolls
        << ",\"selectionBatchPending\":" << (report.commandReadiness.selectionBatchPending ? "true" : "false") << '}';
    stream << ",\n  \"searchDateMenu\":{\"requested\":" << (report.searchDateMenu.requested ? "true" : "false")
        << ",\"readHresult\":" << static_cast<long>(report.searchDateMenu.read)
        << ",\"expanded\":" << (report.searchDateMenu.expanded ? "true" : "false")
        << ",\"expectedRows\":" << report.searchDateMenu.expectedRows
        << ",\"matchedRows\":" << report.searchDateMenu.matchedRows
        << ",\"relativeToday\":" << (report.searchDateMenu.relativeToday ? "true" : "false")
        << ",\"scopeReadHresult\":" << static_cast<long>(report.searchDateMenu.scopeRead)
        << ",\"resultReadHresult\":" << static_cast<long>(report.searchDateMenu.resultRead)
        << ",\"resultCount\":" << report.searchDateMenu.resultCount
        << ",\"expectedResults\":" << report.searchDateMenu.expectedResults
        << ",\"matchedIdentities\":" << report.searchDateMenu.matchedIdentities
        << ",\"unexpectedPaths\":" << report.searchDateMenu.unexpectedPaths
        << ",\"duplicateIdentities\":" << report.searchDateMenu.duplicateIdentities
        << ",\"submitReadHresult\":" << static_cast<long>(report.searchDateMenu.submitRead)
        << ",\"submitCount\":" << report.searchDateMenu.submitCount
        << ",\"recentCount\":" << report.searchDateMenu.recentCount
        << ",\"recentMatchesQuery\":" << (report.searchDateMenu.recentMatchesQuery?"true":"false")
        << ",\"scopeNavigationReadHresult\":" << static_cast<long>(report.searchDateMenu.scopeNavigationRead)
        << ",\"physicalScopeReady\":" << (report.searchDateMenu.physicalScopeReady?"true":"false")
        << ",\"savedInputUnchanged\":" << (report.searchDateMenu.savedInputUnchanged?"true":"false")
        << ",\"nativeViewChanged\":" << (report.searchDateMenu.nativeViewChanged?"true":"false")
        << ",\"scopePreserved\":" << (report.searchDateMenu.scopePreserved?"true":"false")
        << ",\"historyCommitted\":" << (report.searchDateMenu.historyCommitted?"true":"false")
        << ",\"factoryRetained\":" << (report.searchDateMenu.factoryRetained?"true":"false")
        << ",\"retainedFactoriesBefore\":" << report.searchDateMenu.retainedFactoriesBefore
        << ",\"retainedFactoriesAfter\":" << report.searchDateMenu.retainedFactoriesAfter
        << ",\"navigationDelta\":" << report.searchDateMenu.navigationDelta
        << ",\"recentEnabledReadHresult\":" << static_cast<long>(report.searchDateMenu.recentEnabledRead)
        << ",\"recentEnabled\":" << (report.searchDateMenu.recentEnabled?"true":"false") << '}';
    stream << ",\n  \"searchDatePopup\":{\"window\":" << report.searchDatePopup.window
        << ",\"readHresult\":" << static_cast<long>(report.searchDatePopup.read)
        << ",\"ownedPrivate\":" << (report.searchDatePopup.ownedPrivate?"true":"false")
        << ",\"printed\":" << (report.searchDatePopup.printed?"true":"false")
        << ",\"physicalRows\":" << report.searchDatePopup.physicalRows << ",\"bounds\":";
    rectJson(stream,report.searchDatePopup.bounds);
    stream << ",\"uniqueColors\":" << report.searchDatePopup.uniqueColors
        << ",\"inkFraction\":" << report.searchDatePopup.inkFraction
        << ",\"unpaintedFraction\":" << report.searchDatePopup.unpaintedFraction
        << ",\"minimumRowUniqueColors\":" << report.searchDatePopup.minimumRowUniqueColors
        << ",\"minimumRowInkFraction\":" << report.searchDatePopup.minimumRowInkFraction << '}';
    stream << ",\n  \"searchDateExpansion\":" << nativePopupExpansionJson(report.searchDateExpansion);
    stream << ",\n  \"globalSettings\":{\"fileNameExtensions\":{\"source\":\"HKCU Explorer Advanced HideFileExt and native UI_PKEY_BooleanValue\","
        << "\"readSucceeded\":" << (report.fileExtensionsRead ? "true" : "false")
        << ",\"registryValue\":" << report.hideFileExt
        << ",\"osValue\":" << (report.osFileExtensions ? "true" : "false")
        << ",\"ribbonValue\":" << (report.ribbonFileExtensions ? "true" : "false")
        << ",\"matchesOs\":" << (report.fileExtensionsRead && report.osFileExtensions == report.ribbonFileExtensions ? "true" : "false")
        << "}}";
    stream << ",\n  \"nativeRibbonCommands\":[";
    for (size_t i = 0; i < report.ribbonCommands.size(); ++i) {
        const auto& command = report.ribbonCommands[i];
        stream << (i ? "," : "") << "{\"id\":" << command.id
            << ",\"enabledReadHresult\":" << static_cast<long>(command.enabledRead)
            << ",\"enabled\":" << (command.enabled ? "true" : "false")
            << ",\"labelReadHresult\":" << static_cast<long>(command.labelRead)
            << ",\"label\":" << json(command.label)
            << ",\"itemsReadHresult\":" << static_cast<long>(command.itemsRead)
            << ",\"itemCount\":" << command.itemCount
            << ",\"imageReadHresult\":" << static_cast<long>(command.imageRead)
            << ",\"firstItemHasNativeImage\":" << (command.firstItemImage ? "true" : "false") << '}';
    }
    stream << ']';
    auto toolbarJson = [&](const char* name, int id, const std::vector<VisualCaptureReport::ToolbarButtonReadback>& buttons) {
        stream << ",\n  \"" << name << "\":{\"id\":" << id << ",\"coordinateSpace\":\"toolbar-client\",\"buttons\":[";
        for (size_t i = 0; i < buttons.size(); ++i) {
            const auto& button = buttons[i];
            stream << (i ? "," : "") << "{\"index\":" << button.index << ",\"command\":" << button.command
                << ",\"cx\":" << button.width << ",\"iImage\":" << button.image
                << ",\"state\":" << static_cast<unsigned>(button.state) << ",\"style\":" << static_cast<unsigned>(button.style)
                << ",\"readHresult\":" << static_cast<long>(button.read) << ",\"bounds\":";
            rectJson(stream, button.bounds);
            stream << ",\"customDraw\":{\"readHresult\":" << static_cast<long>(button.drawRead)
                << ",\"observed\":" << (button.drawRead == S_OK ? "true" : "false");
            if (button.drawRead == S_OK) {
                const auto& draw = button.draw;
                stream << ",\"command\":" << draw.command << ",\"dpi\":" << draw.dpi << ",\"bounds\":";
                rectJson(stream, draw.bounds);
                stream << ",\"viewportOrigin\":[" << draw.viewportOrigin.x << ',' << draw.viewportOrigin.y
                    << "],\"windowOrigin\":[" << draw.windowOrigin.x << ',' << draw.windowOrigin.y
                    << "],\"mapMode\":" << draw.mapMode << ",\"graphicsMode\":" << draw.graphicsMode
                    << ",\"deviceDpi\":[" << draw.deviceDpiX << ',' << draw.deviceDpiY
                    << "],\"textAlignment\":" << draw.textAlignment
                    << ",\"transformRead\":" << (draw.transformRead ? "true" : "false")
                    << ",\"worldTransform\":[" << draw.transform.eM11 << ',' << draw.transform.eM12 << ','
                    << draw.transform.eM21 << ',' << draw.transform.eM22 << ',' << draw.transform.eDx << ',' << draw.transform.eDy
                    << "],\"enabled\":" << (draw.state.enabled ? "true" : "false")
                    << ",\"hot\":" << (draw.state.hot ? "true" : "false")
                    << ",\"pressed\":" << (draw.state.pressed ? "true" : "false")
                    << ",\"checked\":" << (draw.state.checked ? "true" : "false");
            }
            stream << '}';
            stream << '}';
        }
        stream << "]}";
    };
    toolbarJson("breadcrumbToolbar", 902, report.breadcrumbButtons);
    toolbarJson("navigationToolbar", 901, report.navigationButtons);
    stream << ",\n  \"widgets\":[";
    for (size_t i = 0; i < report.widgets.size(); ++i) {
        const auto& widget = report.widgets[i];
        stream << (i ? ",\n" : "\n") << "    {\"depth\":" << widget.depth << ",\"id\":" << widget.id
            << ",\"class\":" << json(widget.className) << ",\"text\":" << json(widget.text)
            << ",\"visible\":" << (widget.visible ? "true" : "false")
            << ",\"enabled\":" << (widget.enabled ? "true" : "false") << ",\"bounds\":";
        rectJson(stream, widget.bounds);
        stream << '}';
    }
    stream << "\n  ]\n}\n";
    const auto data = stream.str();
    return createNewFile(output, data.data(), data.size());
}

} // namespace explorer
