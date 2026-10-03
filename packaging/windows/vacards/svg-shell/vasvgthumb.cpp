// SPDX-License-Identifier: GPL-2.0-or-later
// VIEW-1: Windows Explorer SVG thumbnail provider that frames the drawing (its
// bounding box plus 5% margin) instead of the page, on white. Renders with the
// system WebView2 runtime through one shared per-process render service; the
// SVG's own scripts are off and remote requests are refused.
#include "vasvgthumb.h"

#include <objbase.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <thumbcache.h>
#include <propsys.h>
#include <shobjidl.h>
#include <ocidl.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <WebView2.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cwctype>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

HMODULE module_handle = nullptr;
std::atomic<long> live_objects{0}, server_locks{0};

void log_line(char const *format, ...)
{
    // Prototype diagnostics: VA_THUMB_LOG, else a shared folder the build account can read.
    wchar_t path[MAX_PATH];
    if (!GetEnvironmentVariableW(L"VA_THUMB_LOG", path, MAX_PATH))
        wcscpy(path, L"C:\\Users\\Public\\Documents\\VAStudio-view1\\explorer-provider.log");
    FILE *file = _wfopen(path, L"a");
    if (!file) return;
    SYSTEMTIME now;
    GetLocalTime(&now);
    std::fprintf(file, "%02u:%02u:%02u pid=%lu ", now.wHour, now.wMinute, now.wSecond, GetCurrentProcessId());
    va_list args;
    va_start(args, format);
    vfprintf(file, format, args);
    va_end(args);
    fputc('\n', file);
    fclose(file);
}

std::string hex(HRESULT hr)
{
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08lx", static_cast<unsigned long>(hr));
    return text;
}

// Minimal COM completion handler around a std::function.
template <class Interface, class... Args>
class Handler final : public Interface {
public:
    Handler(IID const &iid, std::function<HRESULT(Args...)> callback) : _iid(iid), _callback(std::move(callback)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override
    {
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, _iid)) {
            *object = static_cast<Interface *>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++_refs; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG const refs = --_refs;
        if (!refs) delete this;
        return refs;
    }
    HRESULT STDMETHODCALLTYPE Invoke(Args... args) override { return _callback(args...); }

private:
    std::atomic<ULONG> _refs{1};
    IID _iid;
    std::function<HRESULT(Args...)> _callback;
};

template <class Interface, class... Args, class Callback>
ComPtr<Interface> handler(IID const &iid, Callback &&callback)
{
    ComPtr<Interface> result;
    result.Attach(new Handler<Interface, Args...>(iid, std::function<HRESULT(Args...)>(std::forward<Callback>(callback))));
    return result;
}

std::wstring module_directory()
{
    wchar_t path[MAX_PATH];
    DWORD const length = GetModuleFileNameW(module_handle, path, MAX_PATH);
    std::wstring text(path, length);
    return text.substr(0, text.find_last_of(L"\\/"));
}

std::wstring data_directory()
{
    PWSTR base = nullptr;
    std::wstring directory;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &base)))
        directory = std::wstring(base) + L"\\VAStudio\\SvgThumbnails";
    CoTaskMemFree(base);
    if (!directory.empty()) SHCreateDirectoryExW(nullptr, directory.c_str(), nullptr);
    return directory;
}

// A temporary file name that needs no escaping in a URL: the GUID's hex
// digits only (StringFromGUID2's braces are percent-encoded by the browser, so
// the URL it requests would no longer equal the one we allow).
std::wstring temporary_name()
{
    GUID id{};
    if (FAILED(CoCreateGuid(&id))) return {};
    wchar_t text[64];
    StringFromGUID2(id, text, ARRAYSIZE(text));
    std::wstring name;
    for (wchar_t const *c = text; *c; ++c) {
        if (iswxdigit(*c)) name += *c;
    }
    return name;
}

// Frames the drawing: the root's content bounds grown so they fill 90% of the
// limiting side (5% margin each side), centred, on white.
wchar_t const *const frame_script = LR"JS((() => {
  const svg = document.documentElement;
  if (!svg || svg.localName !== 'svg') return 'not-svg';
  let box;
  try { box = svg.getBBox(); } catch (e) { return 'bbox-error'; }
  if (!box || !(box.width > 0) || !(box.height > 0)) return 'empty';
  const w = box.width / 0.9, h = box.height / 0.9;
  const x = box.x + box.width / 2 - w / 2, y = box.y + box.height / 2 - h / 2;
  svg.setAttribute('viewBox', x + ' ' + y + ' ' + w + ' ' + h);
  svg.setAttribute('width', '100%');
  svg.setAttribute('height', '100%');
  svg.setAttribute('preserveAspectRatio', 'xMidYMid meet');
  svg.style.background = 'white';
  return [box.x, box.y, box.width, box.height].join(',');
})())JS";

struct RenderResult {
    std::vector<BYTE> png;
    std::string error;
    std::wstring bounds;
    unsigned blocked = 0;
};

using CreateEnvironment = HRESULT(STDAPICALLTYPE *)(PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions *,
                                                    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *);

// One WebView2 per process, on its own STA thread, serving thumbnail requests
// one at a time. Explorer asks for many thumbnails at once; starting a browser
// per thumbnail costs most of the time. The service closes itself after 30 s
// without work and is restarted by the next request.
class RenderService {
public:
    static RenderService &instance()
    {
        static RenderService service;
        return service;
    }

    bool running() const { return _alive; }

    RenderResult render(std::wstring const &path, UINT size)
    {
        auto job = std::make_shared<Job>();
        job->path = path;
        job->size = size;
        for (int attempt = 0; attempt < 3; ++attempt) {
            {
                std::lock_guard life(_lifecycle);
                bool accepting;
                { std::lock_guard lock(_mutex); accepting = _accepting; }
                if (!accepting) {
                    if (_thread) {
                        WaitForSingleObject(_thread, 10000);
                        CloseHandle(_thread);
                        _thread = nullptr;
                    }
                    HMODULE self_ref = nullptr;
                    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                            reinterpret_cast<LPCWSTR>(&RenderService::thread_proc), &self_ref)) {
                        RenderResult result; result.error = "thread start"; return result;
                    }
                    {
                        std::lock_guard lock(_mutex);
                        _ready = false;
                        _quit = false;
                        _alive = true;
                    }
                    _thread = CreateThread(nullptr, 0, &RenderService::thread_proc, this, 0, &_thread_id);
                    if (!_thread) {
                        _alive = false;
                        FreeLibrary(self_ref);
                        RenderResult result; result.error = "thread start"; return result;
                    }
                    std::unique_lock lock(_mutex);
                    if (!_cv.wait_for(lock, std::chrono::seconds(10), [this] { return _ready; })) {
                        RenderResult result; result.error = "service unavailable"; return result;
                    }
                }
            }
            std::unique_lock lock(_mutex);
            if (!_accepting) continue;
            _queue.push_back(job);
            PostThreadMessageW(_thread_id, WM_APP_JOB, 0, 0);
            if (!_cv.wait_for(lock, std::chrono::seconds(20), [&] { return job->done; })) {
                RenderResult result; result.error = "busy"; return result;
            }
            return job->result;
        }
        RenderResult result; result.error = "service unavailable"; return result;
    }

private:
    ~RenderService() { if (_thread) CloseHandle(_thread); }
    static DWORD WINAPI thread_proc(void *self)
    {
        static_cast<RenderService *>(self)->thread_main();
        FreeLibraryAndExitThread(module_handle, 0);
        return 0;
    }
    static constexpr UINT WM_APP_JOB = WM_APP + 1;
    static constexpr UINT_PTR TIMER_JOB_DEADLINE = 1, TIMER_CAPTURE = 2, TIMER_IDLE = 3;

    struct Job {
        std::wstring path;
        UINT size = 0;
        RenderResult result;
        bool done = false;
    };

    std::mutex _mutex, _lifecycle;
    std::condition_variable _cv;
    std::deque<std::shared_ptr<Job>> _queue;
    HANDLE _thread = nullptr;
    DWORD _thread_id = 0;
    std::atomic<bool> _alive{false};
    bool _accepting = false;
    bool _ready = false;

    HWND _window = nullptr;
    CreateEnvironment _create_environment = nullptr;
    ComPtr<ICoreWebView2Environment> _environment;
    ComPtr<ICoreWebView2Controller> _controller;
    ComPtr<ICoreWebView2> _webview;
    ComPtr<IStream> _capture;
    std::shared_ptr<Job> _current;
    std::wstring _allowed_url;
    unsigned _browser_generation = 0;
    bool _starting = false;
    bool _quit = false;

    void finish(std::string error)
    {
        if (!_current) {
            return;
        }
        KillTimer(_window, TIMER_JOB_DEADLINE);
        KillTimer(_window, TIMER_CAPTURE);
        if (!error.empty()) {
            _current->result.error = std::move(error);
            _current->result.png.clear();
            reset_browser(); // a failed browser is rebuilt for the next job
        }
        {
            std::lock_guard lock(_mutex);
            _current->done = true;
            _current = nullptr;
        }
        _cv.notify_all();
        SetTimer(_window, TIMER_IDLE, 30000, nullptr);
        next();
    }

    void reset_browser()
    {
        if (_controller) {
            _controller->Close();
        }
        _webview.Reset();
        _controller.Reset();
        _environment.Reset();
        _starting = false;
        ++_browser_generation;
    }

    void next()
    {
        if (_current || _starting) {
            return;
        }
        {
            std::lock_guard lock(_mutex);
            if (_queue.empty()) {
                return;
            }
            _current = _queue.front();
            _queue.pop_front();
        }
        KillTimer(_window, TIMER_IDLE);
        SetTimer(_window, TIMER_JOB_DEADLINE, 15000, nullptr);
        if (_webview) {
            navigate();
        } else {
            start_browser();
        }
    }

    void start_browser()
    {
        _starting = true;
        unsigned const generation = _browser_generation;
        std::wstring const user_data = data_directory() + L"\\WebView2";
        auto on_environment = handler<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, HRESULT,
                                      ICoreWebView2Environment *>(
            IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
            [this, generation](HRESULT code, ICoreWebView2Environment *environment) -> HRESULT {
                if (generation != _browser_generation) return S_OK;
                if (FAILED(code) || !environment) {
                    _starting = false;
                    finish("environment " + hex(code));
                    return S_OK;
                }
                _environment = environment;
                auto on_controller = handler<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, HRESULT,
                                             ICoreWebView2Controller *>(
                    IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
                    [this, generation](HRESULT code, ICoreWebView2Controller *created) -> HRESULT {
                        if (generation != _browser_generation) { if (created) created->Close(); return S_OK; }
                        _starting = false;
                        if (FAILED(code) || !created) {
                            finish("controller " + hex(code));
                            return S_OK;
                        }
                        _controller = created;
                        if (!configure_browser()) { finish("webview2 too old"); return S_OK; }
                        if (_current) {
                            navigate();
                        }
                        return S_OK;
                    });
                HRESULT const hr = _environment->CreateCoreWebView2Controller(_window, on_controller.Get());
                if (FAILED(hr)) {
                    _starting = false;
                    finish("create controller " + hex(hr));
                }
                return S_OK;
            });
        HRESULT const hr = _create_environment(nullptr, user_data.c_str(), nullptr, on_environment.Get());
        if (FAILED(hr)) {
            _starting = false;
            finish("create environment " + hex(hr));
        }
    }

    bool configure_browser()
    {
        _controller->put_IsVisible(TRUE);
        ComPtr<ICoreWebView2Controller2> controller2;
        if (SUCCEEDED(_controller->QueryInterface(IID_ICoreWebView2Controller2,
                                                  reinterpret_cast<void **>(controller2.GetAddressOf())))) {
            controller2->put_DefaultBackgroundColor(COREWEBVIEW2_COLOR{255, 255, 255, 255});
        }
        if (FAILED(_controller->get_CoreWebView2(&_webview)) || !_webview) return false;
        ComPtr<ICoreWebView2_3> webview3;
        if (FAILED(_webview->QueryInterface(IID_ICoreWebView2_3,
                reinterpret_cast<void **>(webview3.GetAddressOf()))) || !webview3 ||
            FAILED(webview3->SetVirtualHostNameToFolderMapping(L"vastudio-svg.example",
                (data_directory() + L"\\tmp").c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY))) return false;
        ComPtr<ICoreWebView2Settings> settings;
        if (SUCCEEDED(_webview->get_Settings(&settings))) {
            settings->put_IsScriptEnabled(FALSE); // the SVG's own scripts
            settings->put_AreDefaultContextMenusEnabled(FALSE);
            settings->put_IsStatusBarEnabled(FALSE);
            settings->put_AreDevToolsEnabled(FALSE);
            settings->put_IsWebMessageEnabled(FALSE);
        }
        // Only the current temporary SVG and inline data may load.
        _webview->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
        auto on_request = handler<ICoreWebView2WebResourceRequestedEventHandler, ICoreWebView2 *,
                                  ICoreWebView2WebResourceRequestedEventArgs *>(
            IID_ICoreWebView2WebResourceRequestedEventHandler,
            [this](ICoreWebView2 *, ICoreWebView2WebResourceRequestedEventArgs *args) -> HRESULT {
                ComPtr<ICoreWebView2WebResourceRequest> request;
                LPWSTR uri = nullptr;
                if (SUCCEEDED(args->get_Request(&request)) && SUCCEEDED(request->get_Uri(&uri)) && uri) {
                    if (_wcsicmp(uri, _allowed_url.c_str()) != 0 && _wcsnicmp(uri, L"data:", 5) != 0) {
                        ComPtr<ICoreWebView2WebResourceResponse> response;
                        if (_environment &&
                            SUCCEEDED(_environment->CreateWebResourceResponse(nullptr, 403, L"Blocked", L"", &response)))
                            args->put_Response(response.Get());
                        if (_current) {
                            ++_current->result.blocked;
                        }
                    }
                    CoTaskMemFree(uri);
                }
                return S_OK;
            });
        EventRegistrationToken token{};
        _webview->add_WebResourceRequested(on_request.Get(), &token);
        auto on_starting = handler<ICoreWebView2NavigationStartingEventHandler, ICoreWebView2 *,
                                   ICoreWebView2NavigationStartingEventArgs *>(
            IID_ICoreWebView2NavigationStartingEventHandler,
            [this](ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *args) -> HRESULT {
                LPWSTR uri = nullptr;
                if (FAILED(args->get_Uri(&uri)) || !uri || _wcsicmp(uri, _allowed_url.c_str()) != 0)
                    args->put_Cancel(TRUE);
                CoTaskMemFree(uri);
                return S_OK;
            });
        _webview->add_NavigationStarting(on_starting.Get(), &token);
        auto on_popup = handler<ICoreWebView2NewWindowRequestedEventHandler, ICoreWebView2 *,
                                ICoreWebView2NewWindowRequestedEventArgs *>(
            IID_ICoreWebView2NewWindowRequestedEventHandler,
            [](ICoreWebView2 *, ICoreWebView2NewWindowRequestedEventArgs *args) -> HRESULT {
                args->put_Handled(TRUE); return S_OK;
            });
        _webview->add_NewWindowRequested(on_popup.Get(), &token);
        auto on_navigation = handler<ICoreWebView2NavigationCompletedEventHandler, ICoreWebView2 *,
                                     ICoreWebView2NavigationCompletedEventArgs *>(
            IID_ICoreWebView2NavigationCompletedEventHandler,
            [this](ICoreWebView2 *, ICoreWebView2NavigationCompletedEventArgs *args) -> HRESULT {
                if (!_current) {
                    return S_OK;
                }
                BOOL success = FALSE;
                args->get_IsSuccess(&success);
                if (!success) {
                    finish("navigation failed");
                    return S_OK;
                }
                auto on_script = handler<ICoreWebView2ExecuteScriptCompletedHandler, HRESULT, LPCWSTR>(
                    IID_ICoreWebView2ExecuteScriptCompletedHandler, [this](HRESULT code, LPCWSTR json) -> HRESULT {
                        if (!_current) {
                            return S_OK;
                        }
                        if (json) {
                            _current->result.bounds = json;
                        }
                        if (FAILED(code)) {
                            finish("script " + hex(code));
                            return S_OK;
                        }
                        SetTimer(_window, TIMER_CAPTURE, 300, nullptr); // let the new framing paint
                        return S_OK;
                    });
                _webview->ExecuteScript(frame_script, on_script.Get());
                return S_OK;
            });
        _webview->add_NavigationCompleted(on_navigation.Get(), &token);
        return true;
    }

    void navigate()
    {
        auto const size = static_cast<LONG>(_current->size);
        SetWindowPos(_window, nullptr, 0, 0, size, size, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        _controller->put_Bounds(RECT{0, 0, size, size});
        _allowed_url = L"https://vastudio-svg.example/" + _current->path.substr(_current->path.find_last_of(L"\\/") + 1);
        HRESULT const hr = _webview->Navigate(_allowed_url.c_str());
        if (FAILED(hr)) {
            finish("navigate " + hex(hr));
        }
    }

    void capture()
    {
        if (!_current || !_webview) {
            return;
        }
        _capture.Reset();
        if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &_capture))) {
            finish("capture stream");
            return;
        }
        auto done = handler<ICoreWebView2CapturePreviewCompletedHandler, HRESULT>(
            IID_ICoreWebView2CapturePreviewCompletedHandler, [this](HRESULT code) -> HRESULT {
                if (!_current) {
                    return S_OK;
                }
                if (FAILED(code)) {
                    finish("capture " + hex(code));
                    return S_OK;
                }
                HGLOBAL memory = nullptr;
                STATSTG stat{};
                if (SUCCEEDED(GetHGlobalFromStream(_capture.Get(), &memory)) &&
                    SUCCEEDED(_capture->Stat(&stat, STATFLAG_NONAME))) {
                    auto const *bytes = static_cast<BYTE const *>(GlobalLock(memory));
                    _current->result.png.assign(bytes, bytes + stat.cbSize.QuadPart);
                    GlobalUnlock(memory);
                }
                finish(_current->result.png.empty() ? "empty capture" : "");
                return S_OK;
            });
        HRESULT const hr =
            _webview->CapturePreview(COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, _capture.Get(), done.Get());
        if (FAILED(hr)) {
            finish("capture call " + hex(hr));
        }
    }

    void thread_main()
    {
        bool const com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
        MSG message;
        PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE); // create the queue
        HMODULE loader = LoadLibraryW((module_directory() + L"\\WebView2Loader.dll").c_str());
        _create_environment = loader ? reinterpret_cast<CreateEnvironment>(reinterpret_cast<void *>(
                                           GetProcAddress(loader, "CreateCoreWebView2EnvironmentWithOptions")))
                                     : nullptr;
        WNDCLASSW window_class{};
        window_class.lpfnWndProc = DefWindowProcW;
        window_class.hInstance = module_handle;
        window_class.lpszClassName = L"VASvgThumbnailHost";
        RegisterClassW(&window_class);
        _window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, window_class.lpszClassName, L"", WS_POPUP, 0,
                                  0, 256, 256, nullptr, nullptr, module_handle, nullptr);
        {
            std::lock_guard lock(_mutex);
            _thread_id = GetCurrentThreadId();
            _accepting = true;
            _ready = true;
        }
        _cv.notify_all();
        if (!_window || !_create_environment || !com) {
            { std::lock_guard lock(_mutex); _accepting = false; }
            fail_all(!_create_environment ? "WebView2Loader.dll not found" : "service start failed");
        } else {
            while (!_quit && GetMessageW(&message, nullptr, 0, 0) > 0) {
                if (message.hwnd == nullptr && message.message == WM_APP_JOB) {
                    next();
                    continue;
                }
                if (message.message == WM_TIMER && message.hwnd == _window) {
                    if (message.wParam == TIMER_JOB_DEADLINE) {
                        finish("timeout");
                        continue;
                    }
                    if (message.wParam == TIMER_CAPTURE) {
                        KillTimer(_window, TIMER_CAPTURE);
                        capture();
                        continue;
                    }
                    if (message.wParam == TIMER_IDLE) {
                        KillTimer(_window, TIMER_IDLE);
                        std::lock_guard lock(_mutex);
                        if (!_current && _queue.empty()) {
                            _accepting = false;
                            _quit = true;
                        }
                        continue;
                    }
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        reset_browser();
        if (_window) {
            DestroyWindow(_window);
            _window = nullptr;
        }
        { std::lock_guard lock(_mutex); _accepting = false; }
        fail_all("service stopped");
        if (com) {
            CoUninitialize();
        }
        _alive = false;
    }

    void fail_all(char const *error)
    {
        std::lock_guard lock(_mutex);
        for (auto const &job : _queue) {
            job->result.error = error;
            job->done = true;
        }
        _queue.clear();
        _cv.notify_all();
    }
};

RenderResult render_svg(std::wstring const &svg_path, UINT size)
{
    return RenderService::instance().render(svg_path, size);
}

bool png_to_bitmap(std::vector<BYTE> const &png, UINT size, HBITMAP *out)
{
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE *>(png.data()), static_cast<DWORD>(png.size()))) ||
        FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(factory->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(frame.Get(), size, size, WICBitmapInterpolationModeFant)) ||
        FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom)))
        return false;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = static_cast<LONG>(size);
    info.bmiHeader.biHeight = -static_cast<LONG>(size);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void *bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap) return false;
    if (FAILED(converter->CopyPixels(nullptr, size * 4, size * size * 4, static_cast<BYTE *>(bits)))) {
        DeleteObject(bitmap);
        return false;
    }
    *out = bitmap;
    return true;
}

class SvgThumbnail final : public IInitializeWithStream, public IThumbnailProvider {
public:
    SvgThumbnail() { ++live_objects; }
    ~SvgThumbnail() { --live_objects; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override
    {
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, VA_IID_IInitializeWithStream))
            *object = static_cast<IInitializeWithStream *>(this);
        else if (IsEqualIID(riid, VA_IID_IThumbnailProvider))
            *object = static_cast<IThumbnailProvider *>(this);
        else {
            *object = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++_refs; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG const refs = --_refs;
        if (!refs) delete this;
        return refs;
    }
    HRESULT STDMETHODCALLTYPE Initialize(IStream *stream, DWORD) override
    {
        if (_stream) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        _stream = stream;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetThumbnail(UINT size, HBITMAP *bitmap, WTS_ALPHATYPE *alpha) override
    {
        if (!_stream || !bitmap || !alpha || !size) return E_INVALIDARG;
        auto const started = std::chrono::steady_clock::now();
        // WebView2 loads only this private temporary SVG through the mapped host.
        std::wstring const directory = data_directory() + L"\\tmp";
        SHCreateDirectoryExW(nullptr, directory.c_str(), nullptr);
        std::wstring const name = temporary_name();
        if (name.empty()) return E_FAIL;
        std::wstring const path = directory + L"\\" + name + L".svg";
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
        LARGE_INTEGER zero{};
        _stream->Seek(zero, STREAM_SEEK_SET, nullptr);
        std::vector<BYTE> chunk(1 << 16);
        ULONG read = 0;
        bool copied = true;
        while (SUCCEEDED(_stream->Read(chunk.data(), static_cast<ULONG>(chunk.size()), &read)) && read) {
            DWORD written = 0;
            if (!WriteFile(file, chunk.data(), read, &written, nullptr) || written != read) { copied = false; break; }
        }
        CloseHandle(file);
        RenderResult rendered;
        if (copied) rendered = render_svg(path, size);
        DeleteFileW(path.c_str());
        auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        log_line("size=%u ms=%lld error=%s bounds=%ls png=%zu blocked=%u", size, static_cast<long long>(elapsed),
                 rendered.error.empty() ? "none" : rendered.error.c_str(), rendered.bounds.c_str(), rendered.png.size(),
                 rendered.blocked);
        if (rendered.png.empty() || !png_to_bitmap(rendered.png, size, bitmap)) return E_FAIL;
        *alpha = WTSAT_RGB;
        return S_OK;
    }

private:
    std::atomic<ULONG> _refs{1};
    ComPtr<IStream> _stream;
};


// Preview runs on Explorer's preview-host STA. Its existing message loop drives
// WebView2 callbacks; no worker thread or synchronous wait is needed.
class SvgPreview final : public IPreviewHandler, public IInitializeWithStream, public IObjectWithSite, public IOleWindow {
public:
    SvgPreview() { ++live_objects; }
    ~SvgPreview() { Unload(); --live_objects; } // the WebView2 loader stays loaded, as for thumbnails
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, VA_IID_IPreviewHandler))
            *out = static_cast<IPreviewHandler *>(this);
        else if (IsEqualIID(iid, VA_IID_IInitializeWithStream)) *out = static_cast<IInitializeWithStream *>(this);
        else if (IsEqualIID(iid, VA_IID_IObjectWithSite)) *out = static_cast<IObjectWithSite *>(this);
        else if (IsEqualIID(iid, VA_IID_IOleWindow)) *out = static_cast<IOleWindow *>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++_refs; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG n = --_refs; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE Initialize(IStream *stream, DWORD) override
    {
        if (!stream) return E_INVALIDARG;
        if (_stream) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        _stream = stream;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetWindow(HWND parent, const RECT *rect) override
    {
        if (!parent || !rect) return E_INVALIDARG;
        _parent = parent;
        _rect = *rect;
        if (_window) SetParent(_window, parent);
        resize();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetRect(const RECT *rect) override
    {
        if (!rect) return E_INVALIDARG;
        _rect = *rect;
        resize();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DoPreview() override
    {
        if (!_stream || !_parent) { log_line("preview error=state hr=%s", hex(E_UNEXPECTED).c_str()); return E_UNEXPECTED; }
        if (_window) return S_OK;
        std::wstring directory = data_directory() + L"\\tmp";
        SHCreateDirectoryExW(nullptr, directory.c_str(), nullptr);
        std::wstring const name = temporary_name();
        if (name.empty()) { log_line("preview error=guid hr=%s", hex(E_FAIL).c_str()); return E_FAIL; }
        _path = directory + L"\\" + name + L".svg";
        _url = L"https://vastudio-svg.example/" + name + L".svg";
        HANDLE file = CreateFileW(_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (file == INVALID_HANDLE_VALUE) { HRESULT error = HRESULT_FROM_WIN32(GetLastError()); log_line("preview error=temp file hr=%s", hex(error).c_str()); _path.clear(); return error; }
        LARGE_INTEGER zero{};
        HRESULT hr = _stream->Seek(zero, STREAM_SEEK_SET, nullptr);
        BYTE chunk[1 << 16]; ULONG got = 0; DWORD written = 0;
        while (SUCCEEDED(hr)) {
            hr = _stream->Read(chunk, sizeof(chunk), &got);
            if (FAILED(hr) || !got) break;
            if (!WriteFile(file, chunk, got, &written, nullptr) || written != got) {
                hr = HRESULT_FROM_WIN32(GetLastError()); if (SUCCEEDED(hr)) hr = E_FAIL;
                break;
            }
        }
        CloseHandle(file);
        if (FAILED(hr)) { log_line("preview error=copy hr=%s", hex(hr).c_str()); DeleteFileW(_path.c_str()); _path.clear(); return hr; }
        _window = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, _rect.left, _rect.top,
                                  _rect.right - _rect.left, _rect.bottom - _rect.top,
                                  _parent, nullptr, module_handle, nullptr);
        if (!_window) { hr = HRESULT_FROM_WIN32(GetLastError()); log_line("preview error=window hr=%s", hex(hr).c_str()); Unload(); return hr; }
        HMODULE loader = LoadLibraryW((module_directory() + L"\\WebView2Loader.dll").c_str());
        auto create = loader ? reinterpret_cast<CreateEnvironment>(reinterpret_cast<void *>(
            GetProcAddress(loader, "CreateCoreWebView2EnvironmentWithOptions"))) : nullptr;
        if (!create) { if (loader) FreeLibrary(loader); hr = HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND); log_line("preview error=loader hr=%s", hex(hr).c_str()); Unload(); return hr; }
        _loader = loader;
        unsigned generation = ++_generation;
        AddRef();
        auto complete = handler<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, HRESULT, ICoreWebView2Environment *>(
            IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
            [this, generation](HRESULT code, ICoreWebView2Environment *environment) -> HRESULT {
                if (generation == _generation && SUCCEEDED(code) && environment && _window) {
                    _environment = environment;
                    AddRef();
                    auto controller = handler<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, HRESULT, ICoreWebView2Controller *>(
                        IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
                        [this, generation](HRESULT result, ICoreWebView2Controller *created) -> HRESULT {
                            if (generation == _generation && SUCCEEDED(result) && created && _window) {
                                _controller = created;
                                if (configure()) { resize(); navigate(); }
                                else { log_line("preview error=webview2 too old hr=%s", hex(E_NOINTERFACE).c_str()); Unload(); }
                            } else if (generation == _generation) {
                                log_line("preview error=controller hr=%s", hex(result).c_str());
                                Unload();
                            }
                            Release();
                            return S_OK;
                        });
                    HRESULT result = _environment->CreateCoreWebView2Controller(_window, controller.Get());
                    if (FAILED(result)) { log_line("preview error=create controller hr=%s", hex(result).c_str()); Unload(); Release(); }
                } else if (generation == _generation) {
                    log_line("preview error=environment hr=%s", hex(code).c_str());
                    Unload();
                }
                Release();
                return S_OK;
            });
        hr = create(nullptr, (data_directory() + L"\\WebView2Preview").c_str(), nullptr, complete.Get());
        if (FAILED(hr)) { log_line("preview error=create environment hr=%s", hex(hr).c_str()); Release(); Unload(); return hr; }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Unload() override
    {
        ++_generation;
        if (_controller) _controller->Close();
        _webview.Reset(); _controller.Reset(); _environment.Reset();
        if (_window) { DestroyWindow(_window); _window = nullptr; }
        if (!_path.empty()) { DeleteFileW(_path.c_str()); _path.clear(); }
        _url.clear();
        _stream.Reset();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetFocus() override { if (!_window) return E_FAIL; ::SetFocus(_window); return S_OK; }
    HRESULT STDMETHODCALLTYPE QueryFocus(HWND *focused) override { if (!focused) return E_POINTER; *focused = ::GetFocus(); return S_OK; }
    HRESULT STDMETHODCALLTYPE TranslateAccelerator(MSG *) override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE GetWindow(HWND *window) override { if (!window) return E_POINTER; *window = _window; return S_OK; }
    HRESULT STDMETHODCALLTYPE ContextSensitiveHelp(BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetSite(IUnknown *site) override { _site = site; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetSite(REFIID iid, void **site) override
    { if (!site) return E_POINTER; *site = nullptr; return _site ? _site->QueryInterface(iid, site) : E_FAIL; }
private:
    void resize()
    {
        if (!_window) return;
        LONG width = _rect.right - _rect.left, height = _rect.bottom - _rect.top;
        SetWindowPos(_window, nullptr, _rect.left, _rect.top, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
        if (_controller) _controller->put_Bounds(RECT{0, 0, width, height});
    }
    bool configure()
    {
        _controller->put_IsVisible(TRUE);
        ComPtr<ICoreWebView2Controller2> controller2;
        if (SUCCEEDED(_controller->QueryInterface(IID_ICoreWebView2Controller2,
                reinterpret_cast<void **>(controller2.GetAddressOf()))))
            controller2->put_DefaultBackgroundColor(COREWEBVIEW2_COLOR{255, 255, 255, 255});
        if (FAILED(_controller->get_CoreWebView2(&_webview)) || !_webview) return false;
        ComPtr<ICoreWebView2_3> webview3;
        if (FAILED(_webview->QueryInterface(IID_ICoreWebView2_3,
                reinterpret_cast<void **>(webview3.GetAddressOf()))) || !webview3 ||
            FAILED(webview3->SetVirtualHostNameToFolderMapping(L"vastudio-svg.example",
                (data_directory() + L"\\tmp").c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY))) return false;
        ComPtr<ICoreWebView2Settings> settings;
        if (SUCCEEDED(_webview->get_Settings(&settings))) {
            settings->put_IsScriptEnabled(FALSE);
            settings->put_AreDefaultContextMenusEnabled(FALSE);
            settings->put_IsStatusBarEnabled(FALSE);
            settings->put_AreDevToolsEnabled(FALSE);
            settings->put_IsWebMessageEnabled(FALSE);
        }
        _webview->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
        auto request = handler<ICoreWebView2WebResourceRequestedEventHandler, ICoreWebView2 *, ICoreWebView2WebResourceRequestedEventArgs *>(
            IID_ICoreWebView2WebResourceRequestedEventHandler,
            [this](ICoreWebView2 *, ICoreWebView2WebResourceRequestedEventArgs *args) -> HRESULT {
                ComPtr<ICoreWebView2WebResourceRequest> req; LPWSTR uri = nullptr;
                if (SUCCEEDED(args->get_Request(&req)) && SUCCEEDED(req->get_Uri(&uri)) && uri) {
                    if (_wcsicmp(uri, _url.c_str()) && _wcsnicmp(uri, L"data:", 5)) {
                        ComPtr<ICoreWebView2WebResourceResponse> response;
                        if (_environment && SUCCEEDED(_environment->CreateWebResourceResponse(nullptr, 403, L"Blocked", L"", &response)))
                            args->put_Response(response.Get());
                    }
                    CoTaskMemFree(uri);
                }
                return S_OK;
            });
        EventRegistrationToken token{};
        _webview->add_WebResourceRequested(request.Get(), &token);
        auto starting = handler<ICoreWebView2NavigationStartingEventHandler, ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *>(
            IID_ICoreWebView2NavigationStartingEventHandler,
            [this](ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *args) -> HRESULT {
                LPWSTR uri = nullptr;
                if (FAILED(args->get_Uri(&uri)) || !uri || _wcsicmp(uri, _url.c_str())) args->put_Cancel(TRUE);
                CoTaskMemFree(uri);
                return S_OK;
            });
        _webview->add_NavigationStarting(starting.Get(), &token);
        auto popup = handler<ICoreWebView2NewWindowRequestedEventHandler, ICoreWebView2 *, ICoreWebView2NewWindowRequestedEventArgs *>(
            IID_ICoreWebView2NewWindowRequestedEventHandler,
            [](ICoreWebView2 *, ICoreWebView2NewWindowRequestedEventArgs *args) -> HRESULT {
                args->put_Handled(TRUE); return S_OK;
            });
        _webview->add_NewWindowRequested(popup.Get(), &token);
        auto navigation = handler<ICoreWebView2NavigationCompletedEventHandler, ICoreWebView2 *, ICoreWebView2NavigationCompletedEventArgs *>(
            IID_ICoreWebView2NavigationCompletedEventHandler,
            [this](ICoreWebView2 *, ICoreWebView2NavigationCompletedEventArgs *args) -> HRESULT {
                BOOL success = FALSE; args->get_IsSuccess(&success);
                if (success && _webview) _webview->ExecuteScript(frame_script, nullptr);
                return S_OK;
            });
        _webview->add_NavigationCompleted(navigation.Get(), &token);
        return true;
    }
    void navigate()
    {
        HRESULT hr = _webview->Navigate(_url.c_str());
        if (FAILED(hr)) { log_line("preview error=navigate hr=%s", hex(hr).c_str()); Unload(); }
    }
    std::atomic<ULONG> _refs{1};
    ComPtr<IStream> _stream;
    ComPtr<IUnknown> _site;
    HWND _parent = nullptr, _window = nullptr;
    RECT _rect{};
    std::wstring _path, _url;
    HMODULE _loader = nullptr;
    unsigned _generation = 0;
    ComPtr<ICoreWebView2Environment> _environment;
    ComPtr<ICoreWebView2Controller> _controller;
    ComPtr<ICoreWebView2> _webview;
};

class Factory final : public IClassFactory {
public:
    explicit Factory(bool preview) : _preview(preview) {}
private:
    bool _preview;
public:
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override
    {
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) {
            *object = static_cast<IClassFactory *>(this);
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown *outer, REFIID riid, void **object) override
    {
        if (outer) return CLASS_E_NOAGGREGATION;
        IUnknown *instance = _preview ? static_cast<IUnknown *>(static_cast<IPreviewHandler *>(new SvgPreview))
                                      : static_cast<IUnknown *>(static_cast<IThumbnailProvider *>(new SvgThumbnail));
        HRESULT const hr = instance->QueryInterface(riid, object);
        instance->Release();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override
    {
        lock ? ++server_locks : --server_locks;
        return S_OK;
    }
};

} // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        module_handle = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID *object)
{
    static Factory thumbnail(false), preview(true);
    if (IsEqualCLSID(clsid, CLSID_VASvgThumbnail)) return thumbnail.QueryInterface(riid, object);
    if (IsEqualCLSID(clsid, CLSID_VASvgPreview)) return preview.QueryInterface(riid, object);
    return CLASS_E_CLASSNOTAVAILABLE;
}

STDAPI DllCanUnloadNow()
{
    // The render service thread runs code from this DLL until it stops.
    return live_objects == 0 && server_locks == 0 && !RenderService::instance().running() ? S_OK : S_FALSE;
}
