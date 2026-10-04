#include "explorer/headless_visual.hpp"

#include <commctrl.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void succeeded(HRESULT hr, const char* message) {
    if (FAILED(hr)) throw std::runtime_error(std::string(message) + " HRESULT=" + std::to_string(static_cast<unsigned long>(hr)));
}
struct Window {
    HWND value = nullptr;
    ~Window() { if (value) DestroyWindow(value); }
};
void paintMessages() {
    const auto end = GetTickCount64() + 350;
    do {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    } while (GetTickCount64() < end);
}
std::vector<unsigned char> read(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
void decode(const std::filesystem::path& path, unsigned width, unsigned height) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    succeeded(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), "WIC factory");
    ComPtr<IWICBitmapDecoder> decoder;
    succeeded(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder), "PNG decode");
    GUID container{};
    succeeded(decoder->GetContainerFormat(&container), "container format");
    require(container == GUID_ContainerFormatPng, "Snapshot is not PNG");
    ComPtr<IWICBitmapFrameDecode> frame;
    succeeded(decoder->GetFrame(0, &frame), "PNG frame");
    UINT decodedWidth = 0, decodedHeight = 0;
    succeeded(frame->GetSize(&decodedWidth, &decodedHeight), "PNG geometry");
    require(decodedWidth == width && decodedHeight == height, "PNG dimensions differ from native window");
}
}

int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    // This executable has no COM or HWND before the desktop transition.
    if (explorer::PrivateDesktop::current()) return 3;
    explorer::PrivateDesktop desktop;
    auto hr = desktop.initialize();
    if (FAILED(hr)) {
        std::cerr << "FAIL: private desktop initialization HRESULT=" << static_cast<unsigned long>(hr) << '\n';
        return 1;
    }
    hr = OleInitialize(nullptr);
    if (FAILED(hr)) return 2;
    int result = 0;
    try {
        bool unchanged = false;
        succeeded(desktop.verifyIsolation(&unchanged), "private desktop isolation");
        require(unchanged, "Input desktop changed");
        bool visibleInput = true;
        succeeded(desktop.visibleWindowsOnInputDesktop(visibleInput), "input desktop observation");
        require(!visibleInput, "Process has visible input desktop windows");
        require(explorer::PrivateDesktop::current() == &desktop, "Private rendering phase lost its thread guard");
        require(desktop.initialize() == HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED), "Double desktop initialization accepted");
        explorer::PrivateDesktop nested;
        require(nested.initialize() == HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED), "Nested private desktop replaced the guard");
        HRESULT otherThreadResult = S_OK;
        bool wrongThreadHasGuard = true;
        std::thread differentThread([&] { otherThreadResult = desktop.verifyIsolation(); wrongThreadHasGuard = explorer::PrivateDesktop::current() != nullptr; });
        differentThread.join();
        require(otherThreadResult == E_ACCESSDENIED, "Desktop guard accepted the wrong thread");
        require(!wrongThreadHasGuard, "Private render guard leaked across threads");
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES};
        require(InitCommonControlsEx(&controls) != FALSE, "Common control initialization failed");
        WNDCLASSW klass{};
        klass.lpszClassName = L"WindowsExplorer.Visual.Tests";
        klass.lpfnWndProc = DefWindowProcW;
        klass.hInstance = GetModuleHandleW(nullptr);
        klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        require(RegisterClassW(&klass) != 0, "Native window class registration failed");
        {
            Window host{CreateWindowExW(0, klass.lpszClassName, L"Private desktop native capture fixture",
                WS_OVERLAPPEDWINDOW, 20, 20, 735, 503, nullptr, nullptr, klass.hInstance, nullptr)};
            require(host.value != nullptr, "Native capture host failed");
            const auto button = CreateWindowExW(0, WC_BUTTONW, L"Copy", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                12, 12, 92, 44, host.value, reinterpret_cast<HMENU>(101), klass.hInstance, nullptr);
            const auto edit = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, L"Search owned fixture", WS_CHILD | WS_VISIBLE,
                120, 20, 250, 28, host.value, reinterpret_cast<HMENU>(102), klass.hInstance, nullptr);
            const auto list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT,
                12, 80, 650, 300, host.value, reinterpret_cast<HMENU>(103), klass.hInstance, nullptr);
            require(button && edit && list, "Native widgets were not created");
            LVCOLUMNW column{};
            column.mask = LVCF_TEXT | LVCF_WIDTH;
            column.cx = 280;
            wchar_t title[] = L"Name";
            column.pszText = title;
            require(ListView_InsertColumn(list, 0, &column) == 0, "Native column creation failed");
            std::array<std::wstring, 3> names{L"Documents", L"Résumé.txt", L"Photo.png"};
            for (size_t i = 0; i < names.size(); ++i) {
                LVITEMW item{};
                item.mask = LVIF_TEXT;
                item.iItem = static_cast<int>(i);
                item.pszText = names[i].data();
                require(ListView_InsertItem(list, &item) >= 0, "Native list item creation failed");
            }
            // WS_VISIBLE is safe only after verifyIsolation and on this private desktop.
            succeeded(desktop.verifyIsolation(), "before noninteractive ShowWindow");
            ShowWindow(host.value, SW_SHOWNOACTIVATE);
            UpdateWindow(host.value);
            paintMessages();
            auto directory = std::filesystem::absolute(std::filesystem::temp_directory_path() /
                (L"windows-explorer-visual-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
            require(std::filesystem::create_directory(directory), "Owned snapshot directory was not created");
            explorer::VisualCaptureOptions options;
            RECT buttonPixels{},framePixels{};GetWindowRect(button,&buttonPixels);GetWindowRect(host.value,&framePixels);
            OffsetRect(&buttonPixels,-framePixels.left,-framePixels.top);InflateRect(&buttonPixels,-4,-4);
            options.pixelInspectionBounds=buttonPixels;
            explorer::VisualCaptureReport report;
            succeeded(explorer::captureWindowPng(desktop, host.value, directory / L"native.png", options, report), "Native PrintWindow snapshot");
            require(report.width == 735 && report.height == 503, "Outer native geometry is wrong");
            require(report.inputDesktopUnchanged && !report.visibleInputDesktopWindows, "Input desktop visibility changed during snapshot");
            require(report.visibleChildren >= 4 && report.uniqueColors > 12 && report.inkFraction > 0.002,
                "Actual native widgets were not painted");
            require(report.unpaintedFraction < 0.001, "Snapshot contains unpainted sentinel pixels");
            require(EqualRect(&report.pixelInspectionBounds,&buttonPixels)&&report.inspectionUniqueColors>=2&&
                report.inspectionInkFraction>0.01,"Native button central pixels are unpainted");
            auto invalidInspection=options;invalidInspection.pixelInspectionBounds={0,0,736,504};
            require(explorer::captureWindowPng(desktop,host.value,directory/L"invalid-region.png",invalidInspection,report)==E_INVALIDARG&&
                !std::filesystem::exists(directory/L"invalid-region.png"),"Out-of-bounds pixel inspection was accepted");
            options.pixelInspectionBounds={};
            const auto find = [&](int id, const wchar_t* text) {
                return std::any_of(report.widgets.begin(), report.widgets.end(), [&](const auto& widget) {
                    return widget.id == id && widget.text == text && widget.visible;
                });
            };
            require(find(101, L"Copy") && find(102, L"Search owned fixture"), "Widget inventory does not reflect real native controls");
            decode(directory / L"native.png", report.width, report.height);
            succeeded(explorer::writeVisualCaptureReport(directory / L"native.json", report), "Native capture inventory write");
            require(read(directory / L"native.json").size() > 500, "Capture inventory is empty");
            const auto firstPixels = read(directory / L"native.png");
            const auto existingReport = report.width;
            const auto collision = explorer::captureWindowPng(desktop, host.value, directory / L"native.png", options, report);
            require(collision == HRESULT_FROM_WIN32(ERROR_FILE_EXISTS) || collision == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS),
                "Snapshot overwrite was not rejected");
            require(read(directory / L"native.png") == firstPixels && report.width == existingReport, "Failed snapshot changed existing file/output");
            SetWindowTextW(button, L"PAINTED CONTROL CHANGED");
            InvalidateRect(button, nullptr, TRUE);
            UpdateWindow(button);
            paintMessages();
            succeeded(explorer::captureWindowPng(desktop, host.value, directory / L"changed.png", options, report), "Changed native widget capture");
            require(read(directory / L"changed.png") != firstPixels, "Native control state/text changes do not affect screenshot pixels");
            require(find(101, L"PAINTED CONTROL CHANGED"), "Changed widget inventory is stale");
            options.trimInvisibleFrame = true;
            succeeded(explorer::captureWindowPng(desktop, host.value, directory / L"visible-frame.png", options, report), "Visible native frame capture");
            require(report.width <= 735 && report.height <= 503 && report.width >= 703 && report.height >= 471,
                "DWM visible frame bounds are invalid");
            options.includeFrame = false;
            succeeded(explorer::captureWindowPng(desktop, host.value, directory / L"client.png", options, report), "Client-only native capture");
            RECT client{};
            GetClientRect(host.value, &client);
            require(report.width == static_cast<unsigned>(client.right) && report.height == static_cast<unsigned>(client.bottom),
                "Client-only geometry includes nonclient chrome");
            const auto beforeInvalid = report.width;
            require(explorer::captureWindowPng(desktop, nullptr, directory / L"invalid.png", options, report) == E_INVALIDARG,
                "Invalid HWND was accepted");
            require(report.width == beforeInvalid && !std::filesystem::exists(directory / L"invalid.png"), "Invalid capture mutated outputs");
            require(explorer::captureWindowPng(desktop, host.value, L"relative.png", options, report) == E_INVALIDARG,
                "Relative capture output was accepted");
            const auto gdiBefore = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
            for (unsigned i = 0; i < 8; ++i)
                succeeded(explorer::captureWindowPng(desktop, host.value,
                    directory / (L"repeat-" + std::to_wstring(i) + L".png"), options, report), "Repeated native capture");
            const auto gdiAfter = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
            require(gdiAfter <= gdiBefore + 1, "Repeated captures leaked GDI objects");
            succeeded(desktop.visibleWindowsOnInputDesktop(visibleInput), "final input desktop observation");
            require(!visibleInput, "Private capture leaked a visible input desktop window");
            std::cout << "Headless native visual capture: private desktop, WIC PNG, geometry, rendered mutations, create-new, invalid-input checks passed\n";
        }
        require(UnregisterClassW(klass.lpszClassName, klass.hInstance) != FALSE, "Window class cleanup failed");
        succeeded(desktop.verifyIsolation(), "post-window cleanup isolation");
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        result = 1;
    }
    OleUninitialize();
    return result;
}
