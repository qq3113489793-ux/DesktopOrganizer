#include <windows.h>

#include "desktop_capture_target.h"
#include <d3d11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/base.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace winrt;
namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;
namespace wgd11 = winrt::Windows::Graphics::DirectX::Direct3D11;

wgd11::IDirect3DDevice CreateDevice(com_ptr<ID3D11Device>& d3dDevice,
                                    com_ptr<ID3D11DeviceContext>& context) {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL featureLevel{};
    check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                    nullptr, 0, D3D11_SDK_VERSION, d3dDevice.put(),
                                    &featureLevel, context.put()));
    auto dxgiDevice = d3dDevice.as<IDXGIDevice>();
    com_ptr<IInspectable> inspectable;
    check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put()));
    return inspectable.as<wgd11::IDirect3DDevice>();
}

wgc::GraphicsCaptureItem ItemForWindow(HWND window) {
    auto interop = get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    wgc::GraphicsCaptureItem item{nullptr};
    check_hresult(interop->CreateForWindow(window, guid_of<wgc::GraphicsCaptureItem>(), put_abi(item)));
    return item;
}

std::vector<uint32_t> ReadFrame(const wgc::Direct3D11CaptureFrame& frame,
                                ID3D11Device* device, ID3D11DeviceContext* context,
                                int& width, int& height) {
    auto access = frame.Surface().as<
        ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    com_ptr<ID3D11Texture2D> texture;
    check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), texture.put_void()));
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    width = static_cast<int>(desc.Width);
    height = static_cast<int>(desc.Height);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    com_ptr<ID3D11Texture2D> staging;
    check_hresult(device->CreateTexture2D(&desc, nullptr, staging.put()));
    context->CopyResource(staging.get(), texture.get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check_hresult(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
    std::vector<uint32_t> pixels(static_cast<size_t>(width) * height);
    for (int y = 0; y < height; ++y) {
        memcpy(pixels.data() + static_cast<size_t>(y) * width,
               static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
               static_cast<size_t>(width) * sizeof(uint32_t));
    }
    context->Unmap(staging.get(), 0);
    return pixels;
}

void WriteBmp(const std::filesystem::path& path, const std::vector<uint32_t>& pixels,
              int width, int height) {
    BITMAPFILEHEADER file{};
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = width;
    info.biHeight = -height;
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    info.biSizeImage = static_cast<DWORD>(pixels.size() * sizeof(uint32_t));
    file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + info.biSizeImage;
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(&file), sizeof(file));
    stream.write(reinterpret_cast<const char*>(&info), sizeof(info));
    stream.write(reinterpret_cast<const char*>(pixels.data()), info.biSizeImage);
}

bool Probe(HWND window, const std::filesystem::path& outputDirectory,
           const wchar_t* label, DWORD intervalMs) {
    try {
        com_ptr<ID3D11Device> d3dDevice;
        com_ptr<ID3D11DeviceContext> context;
        auto device = CreateDevice(d3dDevice, context);
        auto item = ItemForWindow(window);
        auto size = item.Size();
        auto pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
            device, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
        auto session = pool.CreateCaptureSession(item);
        session.IsCursorCaptureEnabled(false);
        if (session.IsBorderRequired()) session.IsBorderRequired(false);
        HANDLE arrived = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        auto token = pool.FrameArrived([arrived](auto&&, auto&&) { SetEvent(arrived); });
        session.StartCapture();
        if (WaitForSingleObject(arrived, 5000) != WAIT_OBJECT_0) throw hresult_error(E_FAIL, L"first frame timeout");
        ResetEvent(arrived);
        auto firstFrame = pool.TryGetNextFrame();
        int widthA = 0, heightA = 0;
        auto first = ReadFrame(firstFrame, d3dDevice.get(), context.get(), widthA, heightA);
        firstFrame.Close();
        WriteBmp(outputDirectory / (std::wstring(label) + L"-a.bmp"), first, widthA, heightA);
        {
            std::ofstream ready(outputDirectory / L"ready.flag", std::ios::binary | std::ios::trunc);
            ready << "ready";
        }
        Sleep(intervalMs);
        auto secondFrame = pool.TryGetNextFrame();
        if (!secondFrame) {
            ResetEvent(arrived);
            if (WaitForSingleObject(arrived, 5000) != WAIT_OBJECT_0)
                throw hresult_error(E_FAIL, L"second frame timeout");
            secondFrame = pool.TryGetNextFrame();
        }
        int widthB = 0, heightB = 0;
        auto second = ReadFrame(secondFrame, d3dDevice.get(), context.get(), widthB, heightB);
        secondFrame.Close();
        pool.FrameArrived(token);
        session.Close();
        pool.Close();
        CloseHandle(arrived);
        if (widthA != widthB || heightA != heightB || first.size() != second.size()) return false;
        double energy = 0.0;
        double mean = 0.0;
        for (size_t i = 0; i < first.size(); ++i) {
            const auto channel = [](uint32_t pixel, int shift) { return static_cast<int>((pixel >> shift) & 0xff); };
            const int a = (channel(first[i], 0) + channel(first[i], 8) + channel(first[i], 16)) / 3;
            const int b = (channel(second[i], 0) + channel(second[i], 8) + channel(second[i], 16)) / 3;
            energy += std::abs(a - b);
            mean += b;
        }
        energy /= std::max<size_t>(1, first.size());
        mean /= std::max<size_t>(1, second.size());
        WriteBmp(outputDirectory / (std::wstring(label) + L"-b.bmp"), second, widthB, heightB);
        wprintf(L"%ls hwnd=%p size=%dx%d mean=%.4f temporal_energy=%.4f\n",
                label, window, widthA, heightA, mean, energy);
        return mean > 1.0 && energy > 0.001;
    } catch (const hresult_error& error) {
        wprintf(L"%ls hwnd=%p failed: 0x%08X %ls\n", label, window,
                static_cast<unsigned>(error.code().value), error.message().c_str());
        return false;
    }
}

int wmain(int argc, wchar_t** argv) {
    init_apartment(apartment_type::multi_threaded);
    const auto output = argc >= 2
        ? std::filesystem::path(argv[1])
        : std::filesystem::path(L"build-visual/artifacts/wgc-wallpaper-probe");
    const DWORD intervalMs = argc >= 3
        ? static_cast<DWORD>(std::clamp(_wtoi(argv[2]), 100, 10000)) : 3000;
    std::filesystem::create_directories(output);
    HWND desktop = desktop_capture_target::Find();
    if (!desktop) {
        wchar_t shellClass[128]{};
        HWND shell = GetShellWindow();
        if (shell) GetClassNameW(shell, shellClass, static_cast<int>(std::size(shellClass)));
        wprintf(L"Desktop capture host not found; shell=%p class=%ls\n", shell, shellClass);
        return 2;
    }
    return Probe(desktop, output, L"desktop", intervalMs) ? 0 : 1;
}
