#include "explorer/headless_visual.hpp"
#include "explorer/commands.hpp"
#include "explorer/ribbon_commands.hpp"

#include <wincodec.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <wrl/client.h>
#include <uiribbon.h>
#include <UIRibbonPropertyHelpers.h>
#include <propvarutil.h>
#include <algorithm>
#include <array>
#include <cstdint>
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

PrivateDesktop::~PrivateDesktop() {
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

HRESULT captureWindowPng(const PrivateDesktop& desktop, HWND window,
                         const std::filesystem::path& output,
                         const VisualCaptureOptions& options,
                         VisualCaptureReport& report) {
    if (!window || !IsWindow(window) || output.empty() || !output.is_absolute() ||
        options.layoutDpi < 48 || options.layoutDpi > 768 || options.minimumInkFraction < 0 ||
        options.minimumInkFraction > 1 || options.maximumUnpaintedFraction < 0 ||
        options.maximumUnpaintedFraction > 1) return E_INVALIDARG;
    const auto owner = GetWindowThreadProcessId(window, nullptr);
    if (owner != GetCurrentThreadId()) return E_ACCESSDENIED;
    VisualCaptureReport result;
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
    const RECT printRect = options.includeFrame ? windowRect : result.clientBounds;
    RECT captureRect = printRect;
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
    // PW_RENDERFULLCONTENT asks modern native controls to draw their content.
    // This captures the HWND's actual native surface/print implementation;
    // Windows can use a painted surface without dispatching WM_PRINT.
    result.printWindowSucceeded = PrintWindow(window, nativeDib.dc,
        PW_RENDERFULLCONTENT | (options.includeFrame ? 0 : PW_CLIENTONLY)) != FALSE;
    GdiFlush();
    if (!result.printWindowSucceeded) return win32Failure();
    const auto navigation = GetDlgItem(window, 901);
    for (auto& button : result.navigationButtons)
        button.drawRead = navigation ? chromeToolbarDrawReadback(navigation,
            static_cast<UINT>(button.command), &button.draw) : HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    const auto breadcrumb = GetDlgItem(window, 902);
    for (auto& button : result.breadcrumbButtons)
        button.drawRead = breadcrumb ? chromeToolbarDrawReadback(breadcrumb,
            static_cast<UINT>(button.command), &button.draw) : HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    std::unique_ptr<Dib> cropped;
    if (result.invisibleFrameTrimmed) {
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
        std::unordered_set<uint32_t> colors;
        size_t ink=0;
        for(LONG y=inspection.top;y<inspection.bottom;++y)for(LONG x=inspection.left;x<inspection.right;++x) {
            const auto rgb=dib.pixels[static_cast<size_t>(y)*result.width+static_cast<size_t>(x)]&0x00ffffff;
            colors.insert(rgb);
            if((rgb&0xff)<220 || ((rgb>>8)&0xff)<220 || ((rgb>>16)&0xff)<220)++ink;
        }
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

HRESULT writeVisualCaptureReport(const std::filesystem::path& output,
                                 const VisualCaptureReport& report) {
    std::ostringstream stream;
    stream << "{\n  \"headless\":true,\n  \"privateDesktop\":true,\n  \"renderer\":\"native-PrintWindow-WIC\",\n"
        << "  \"width\":" << report.width << ",\n  \"height\":" << report.height
        << ",\n  \"windowDpi\":" << report.windowDpi << ",\n  \"layoutDpi\":" << report.layoutDpi
        << ",\n  \"ribbonLayout\":" << json(report.ribbonLayout)
        << ",\n  \"installedRibbonStatus\":" << static_cast<long>(report.installedRibbonStatus)
        << ",\n  \"desktop\":" << json(report.desktopName) << ",\n  \"inputDesktop\":" << json(report.inputDesktopName)
        << ",\n  \"inputDesktopUnchanged\":" << (report.inputDesktopUnchanged ? "true" : "false")
        << ",\n  \"visibleInputDesktopWindows\":" << (report.visibleInputDesktopWindows ? "true" : "false")
        << ",\n  \"printWindowSucceeded\":" << (report.printWindowSucceeded ? "true" : "false")
        << ",\n  \"invisibleFrameTrimmed\":" << (report.invisibleFrameTrimmed ? "true" : "false")
        << ",\n  \"uniqueColors\":" << report.uniqueColors
        << ",\n  \"inkFraction\":" << std::setprecision(8) << report.inkFraction
        << ",\n  \"unpaintedFraction\":" << report.unpaintedFraction
        << ",\n  \"pixelInspection\":{\"bounds\":";
    rectJson(stream,report.pixelInspectionBounds);
    stream << ",\"uniqueColors\":" << report.inspectionUniqueColors
        << ",\"inkFraction\":" << report.inspectionInkFraction << '}'
        << ",\n  \"visibleChildren\":" << report.visibleChildren << ",\n  \"clientBounds\":";
    rectJson(stream, report.clientBounds);
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
            << ",\"nativeReadHresult\":" << static_cast<long>(provider.nativeRead)
            << ",\"nativeState\":" << provider.nativeState << '}';
    }
    stream << ']';
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
