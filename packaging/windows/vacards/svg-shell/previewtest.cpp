// SPDX-License-Identifier: GPL-2.0-or-later
#include "vasvgthumb.h"
#include <objbase.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <propsys.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <cstdio>
#include <chrono>

using Microsoft::WRL::ComPtr;

static bool save_png(HBITMAP bitmap, wchar_t const *path)
{
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmap> source;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapIgnoreAlpha, &source)) &&
        SUCCEEDED(factory->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromFilename(path, GENERIC_WRITE)) &&
        SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
        SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
        SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
        SUCCEEDED(frame->Initialize(nullptr)) &&
        SUCCEEDED(frame->WriteSource(source.Get(), nullptr)) &&
        SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

int wmain(int argc, wchar_t **argv)
{
    if (argc != 4) { std::fwprintf(stderr, L"usage: previewtest <vasvgthumb.dll> <file.svg> <out.png>\n"); return 64; }
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) return 2;
    HWND window = CreateWindowExW(0, L"STATIC", L"VA Studio SVG preview test", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 480, 360, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    HMODULE dll = LoadLibraryW(argv[1]);
    using GetClassObject = HRESULT(STDAPICALLTYPE *)(REFCLSID, REFIID, LPVOID *);
    auto get_class_object = dll ? reinterpret_cast<GetClassObject>(reinterpret_cast<void *>(GetProcAddress(dll, "DllGetClassObject"))) : nullptr;
    ComPtr<IClassFactory> factory;
    ComPtr<IInitializeWithStream> init;
    ComPtr<IPreviewHandler> preview;
    ComPtr<IStream> stream;
    if (!window || !get_class_object) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = get_class_object(CLSID_VASvgPreview, IID_IClassFactory, reinterpret_cast<void **>(factory.GetAddressOf()));
    if (SUCCEEDED(hr)) hr = factory->CreateInstance(nullptr, VA_IID_IInitializeWithStream, reinterpret_cast<void **>(init.GetAddressOf()));
    if (SUCCEEDED(hr)) hr = SHCreateStreamOnFileEx(argv[2], STGM_READ | STGM_SHARE_DENY_WRITE, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &stream);
    if (SUCCEEDED(hr)) hr = init->Initialize(stream.Get(), STGM_READ);
    if (SUCCEEDED(hr)) hr = init->QueryInterface(VA_IID_IPreviewHandler, reinterpret_cast<void **>(preview.GetAddressOf()));
    RECT rect{};
    if (SUCCEEDED(hr)) { GetClientRect(window, &rect); hr = preview->SetWindow(window, &rect); }
    if (SUCCEEDED(hr)) hr = preview->DoPreview();
    HRESULT const preview_hr = hr;
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    MSG message{};
    while (std::chrono::steady_clock::now() < end) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 30, QS_ALLINPUT);
    }
    HDC screen = GetDC(window);
    HDC dc = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = rect.right;
    info.bmiHeader.biHeight = -rect.bottom;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void *pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    HGDIOBJ old = bitmap ? SelectObject(dc, bitmap) : nullptr;
    BOOL captured = bitmap ? PrintWindow(window, dc, PW_CLIENTONLY | PW_RENDERFULLCONTENT) : FALSE;
    unsigned long long nonwhite = 0;
    if (captured) {
        auto *bytes = static_cast<unsigned char *>(pixels);
        for (LONG i = 0; i < rect.right * rect.bottom; ++i)
            if (bytes[i * 4] != 255 || bytes[i * 4 + 1] != 255 || bytes[i * 4 + 2] != 255) ++nonwhite;
    }
    bool saved = captured && save_png(bitmap, argv[3]);
    if (preview) preview->Unload();
    std::printf("DoPreview hr=0x%08lx saved=%d nonwhite=%llu\n", static_cast<unsigned long>(preview_hr), saved ? 1 : 0, nonwhite);
    if (old) SelectObject(dc, old);
    if (bitmap) DeleteObject(bitmap);
    DeleteDC(dc); ReleaseDC(window, screen);
    preview.Reset(); init.Reset(); factory.Reset(); stream.Reset();
    if (window) DestroyWindow(window);
    if (dll) FreeLibrary(dll);
    CoUninitialize();
    return SUCCEEDED(preview_hr) && saved ? 0 : 3;
}
