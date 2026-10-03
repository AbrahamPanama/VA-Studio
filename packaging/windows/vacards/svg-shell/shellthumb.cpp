// SPDX-License-Identifier: GPL-2.0-or-later
// VIEW-1 prototype: asks the Windows shell thumbnail cache (the path Explorer
// uses) for thumbnails, forcing extraction through whatever provider is
// registered for the file type, and saves them as PNG.
#include <windows.h>
#include <objbase.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <thumbcache.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdio>
#include <string>

using Microsoft::WRL::ComPtr;

static const CLSID VA_CLSID_LocalThumbnailCache = {0x50ef4544, 0xac9f, 0x4a8e, {0xb2, 0x1b, 0x8a, 0x26, 0x18, 0x0d, 0xb1, 0x3f}};
static const IID VA_IID_IThumbnailCache = {0xf676c15d, 0x596a, 0x4ce2, {0x82, 0x34, 0x33, 0x99, 0x6f, 0x44, 0x5d, 0xb1}};
static const IID VA_IID_IShellItem = {0x43826d1e, 0xe718, 0x42ee, {0xbc, 0x55, 0xa1, 0xe2, 0x61, 0xc3, 0x7b, 0xfe}};

static bool save_png(HBITMAP bitmap, std::wstring const &path)
{
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmap> source;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
           SUCCEEDED(factory->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapIgnoreAlpha, &source)) &&
           SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
           SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
           SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
           SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
           SUCCEEDED(frame->WriteSource(source.Get(), nullptr)) && SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 4) {
        std::fwprintf(stderr, L"usage: shellthumb <out-dir> <size> <file>...\n");
        return 64;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::wstring const out = argv[1];
    UINT const size = static_cast<UINT>(std::wcstoul(argv[2], nullptr, 10));
    ComPtr<IThumbnailCache> cache;
    HRESULT hr = CoCreateInstance(VA_CLSID_LocalThumbnailCache, nullptr, CLSCTX_INPROC_SERVER, VA_IID_IThumbnailCache,
                                  reinterpret_cast<void **>(cache.GetAddressOf()));
    if (FAILED(hr)) { std::printf("thumbnail cache 0x%08lx\n", static_cast<unsigned long>(hr)); return 2; }
    int failures = 0;
    for (int i = 3; i < argc; ++i) {
        std::wstring const path = argv[i];
        std::wstring const name = path.substr(path.find_last_of(L"\\/") + 1);
        ComPtr<IShellItem> item;
        hr = SHCreateItemFromParsingName(path.c_str(), nullptr, VA_IID_IShellItem, reinterpret_cast<void **>(item.GetAddressOf()));
        ComPtr<ISharedBitmap> shared;
        WTS_CACHEFLAGS flags{};
        WTS_THUMBNAILID id{};
        auto const started = std::chrono::steady_clock::now();
        if (SUCCEEDED(hr))
            hr = cache->GetThumbnail(item.Get(), size, static_cast<WTS_FLAGS>(WTS_EXTRACT | WTS_FORCEEXTRACTION | WTS_SCALETOREQUESTEDSIZE),
                                     &shared, &flags, &id);
        auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        bool saved = false;
        HBITMAP bitmap = nullptr;
        if (SUCCEEDED(hr) && shared && SUCCEEDED(shared->GetSharedBitmap(&bitmap)) && bitmap)
            saved = save_png(bitmap, out + L"\\" + name + L".png");
        std::printf("%ls hr=0x%08lx ms=%lld flags=0x%x saved=%d\n", name.c_str(), static_cast<unsigned long>(hr),
                    static_cast<long long>(ms), static_cast<unsigned>(flags), saved ? 1 : 0);
        if (!saved) ++failures;
    }
    return failures ? 1 : 0;
}
