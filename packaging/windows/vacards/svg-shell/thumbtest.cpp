// SPDX-License-Identifier: GPL-2.0-or-later
// VIEW-1 prototype harness: calls the thumbnail provider the way Explorer does
// (class factory, IInitializeWithStream, IThumbnailProvider) without any
// registration, and saves the returned bitmap as PNG.
#include "vasvgthumb.h"

#include <objbase.h>
#include <shlwapi.h>
#include <thumbcache.h>
#include <propsys.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdio>
#include <cwchar>

using Microsoft::WRL::ComPtr;

static bool save_png(HBITMAP bitmap, wchar_t const *path)
{
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmap> source;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapIgnoreAlpha, &source)) ||
        FAILED(factory->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path, GENERIC_WRITE)) ||
        FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->WriteSource(source.Get(), nullptr)) || FAILED(frame->Commit()) || FAILED(encoder->Commit()))
        return false;
    return true;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 4) {
        std::fwprintf(stderr, L"usage: thumbtest <provider.dll> <file.svg> <out.png> [size]\n");
        return 64;
    }
    UINT const size = argc > 4 ? static_cast<UINT>(std::wcstoul(argv[4], nullptr, 10)) : 256;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HMODULE dll = LoadLibraryW(argv[1]);
    if (!dll) { std::printf("load dll failed %lu\n", GetLastError()); return 2; }
    using GetClassObject = HRESULT(STDAPICALLTYPE *)(REFCLSID, REFIID, LPVOID *);
    auto get_class_object = reinterpret_cast<GetClassObject>(reinterpret_cast<void *>(GetProcAddress(dll, "DllGetClassObject")));
    ComPtr<IClassFactory> factory;
    ComPtr<IInitializeWithStream> init;
    ComPtr<IThumbnailProvider> provider;
    ComPtr<IStream> stream;
    HRESULT hr = get_class_object ? get_class_object(CLSID_VASvgThumbnail, IID_IClassFactory,
                                                     reinterpret_cast<void **>(factory.GetAddressOf())) : E_FAIL;
    if (SUCCEEDED(hr)) hr = factory->CreateInstance(nullptr, VA_IID_IInitializeWithStream, reinterpret_cast<void **>(init.GetAddressOf()));
    if (SUCCEEDED(hr)) hr = SHCreateStreamOnFileEx(argv[2], STGM_READ | STGM_SHARE_DENY_WRITE, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &stream);
    if (SUCCEEDED(hr)) hr = init->Initialize(stream.Get(), STGM_READ);
    if (SUCCEEDED(hr)) hr = init->QueryInterface(VA_IID_IThumbnailProvider, reinterpret_cast<void **>(provider.GetAddressOf()));
    if (FAILED(hr)) { std::printf("setup failed 0x%08lx\n", static_cast<unsigned long>(hr)); return 3; }
    HBITMAP bitmap = nullptr;
    WTS_ALPHATYPE alpha = WTSAT_UNKNOWN;
    auto const started = std::chrono::steady_clock::now();
    hr = provider->GetThumbnail(size, &bitmap, &alpha);
    auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    std::printf("GetThumbnail hr=0x%08lx ms=%lld alpha=%d\n", static_cast<unsigned long>(hr), static_cast<long long>(elapsed), static_cast<int>(alpha));
    if (FAILED(hr) || !bitmap) return 4;
    bool const saved = save_png(bitmap, argv[3]);
    std::printf("saved=%d\n", saved ? 1 : 0);
    DeleteObject(bitmap);
    return saved ? 0 : 5;
}
