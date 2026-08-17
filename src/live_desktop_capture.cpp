#include "live_desktop_capture.h"

#include <d3d11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/base.h>

#include <atomic>
#include <cstring>
#include <mutex>

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;
namespace wgd11 = winrt::Windows::Graphics::DirectX::Direct3D11;

namespace {

constexpr ULONGLONG kMinimumFrameIntervalMs = 66; // About 15 FPS under animated wallpaper.

wgd11::IDirect3DDevice CreateWinRtDevice(winrt::com_ptr<ID3D11Device>& d3dDevice,
                                         winrt::com_ptr<ID3D11DeviceContext>& context) {
    constexpr UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL featureLevel{};
    winrt::check_hresult(D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
        D3D11_SDK_VERSION, d3dDevice.put(), &featureLevel, context.put()));
    auto dxgiDevice = d3dDevice.as<IDXGIDevice>();
    winrt::com_ptr<IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(
        dxgiDevice.get(), inspectable.put()));
    return inspectable.as<wgd11::IDirect3DDevice>();
}

wgc::GraphicsCaptureItem CaptureItemForWindow(HWND window) {
    auto interop = winrt::get_activation_factory<
        wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    wgc::GraphicsCaptureItem item{nullptr};
    winrt::check_hresult(interop->CreateForWindow(
        window, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item)));
    return item;
}

} // namespace

struct LiveDesktopCapture::Impl {
    std::mutex callbackMutex;
    std::mutex frameMutex;
    std::atomic_bool stopping{false};
    std::atomic_bool running{false};
    std::atomic_bool notificationPending{false};
    HWND sourceWindow = nullptr;
    HWND notifyWindow = nullptr;
    UINT notifyMessage = 0;
    RECT sourceBounds{};
    ULONGLONG lastAcceptedFrame = 0;
    std::uint64_t latestGeneration = 0;
    std::uint64_t takenGeneration = 0;
    std::vector<DWORD> latestPixels;
    int latestWidth = 0;
    int latestHeight = 0;

    winrt::com_ptr<ID3D11Device> d3dDevice;
    winrt::com_ptr<ID3D11DeviceContext> d3dContext;
    winrt::com_ptr<ID3D11Texture2D> stagingTexture;
    UINT stagingWidth = 0;
    UINT stagingHeight = 0;
    wgd11::IDirect3DDevice winrtDevice{nullptr};
    wgc::GraphicsCaptureItem captureItem{nullptr};
    wgc::Direct3D11CaptureFramePool framePool{nullptr};
    wgc::GraphicsCaptureSession session{nullptr};
    winrt::event_token frameToken{};
    winrt::event_token closedToken{};

    void NotifyFrameReady() {
        if (!notifyWindow || !notifyMessage) return;
        if (!notificationPending.exchange(true, std::memory_order_acq_rel))
            PostMessageW(notifyWindow, notifyMessage, 0, 0);
    }

    bool EnsureStaging(UINT width, UINT height) {
        if (stagingTexture && stagingWidth == width && stagingHeight == height) return true;
        stagingTexture = nullptr;
        stagingWidth = 0;
        stagingHeight = 0;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(d3dDevice->CreateTexture2D(&desc, nullptr, stagingTexture.put()))) return false;
        stagingWidth = width;
        stagingHeight = height;
        return true;
    }

    void OnFrameArrived(const wgc::Direct3D11CaptureFramePool& sender) {
        std::scoped_lock callbackLock(callbackMutex);
        if (stopping.load(std::memory_order_acquire)) return;
        try {
            auto frame = sender.TryGetNextFrame();
            if (!frame) return;
            const auto size = frame.ContentSize();
            if (size.Width <= 0 || size.Height <= 0) return;
            const ULONGLONG now = GetTickCount64();
            if (lastAcceptedFrame && now - lastAcceptedFrame < kMinimumFrameIntervalMs) return;

            auto access = frame.Surface().as<
                ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            winrt::com_ptr<ID3D11Texture2D> texture;
            winrt::check_hresult(access->GetInterface(
                __uuidof(ID3D11Texture2D), texture.put_void()));
            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);
            if (!EnsureStaging(desc.Width, desc.Height)) return;

            d3dContext->CopyResource(stagingTexture.get(), texture.get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(d3dContext->Map(stagingTexture.get(), 0, D3D11_MAP_READ, 0, &mapped))) return;
            {
                std::scoped_lock frameLock(frameMutex);
                const size_t pixelCount = static_cast<size_t>(desc.Width) * desc.Height;
                latestPixels.resize(pixelCount);
                for (UINT y = 0; y < desc.Height; ++y) {
                    std::memcpy(latestPixels.data() + static_cast<size_t>(y) * desc.Width,
                                static_cast<const BYTE*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
                                static_cast<size_t>(desc.Width) * sizeof(DWORD));
                }
                latestWidth = static_cast<int>(desc.Width);
                latestHeight = static_cast<int>(desc.Height);
                RECT bounds{};
                if (sourceWindow && GetWindowRect(sourceWindow, &bounds)) sourceBounds = bounds;
                sourceBounds.right = sourceBounds.left + latestWidth;
                sourceBounds.bottom = sourceBounds.top + latestHeight;
                ++latestGeneration;
                lastAcceptedFrame = now;
            }
            d3dContext->Unmap(stagingTexture.get(), 0);
            NotifyFrameReady();

            if (size.Width != static_cast<int>(desc.Width) ||
                size.Height != static_cast<int>(desc.Height)) {
                sender.Recreate(winrtDevice, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                                2, size);
            }
        } catch (...) {
            // Explorer, the wallpaper host, or the graphics device may be in
            // the middle of restarting. The UI thread restarts capture on the
            // corresponding shell/display notification.
        }
    }

    bool Start(HWND desktopWindow, HWND targetWindow, UINT message) {
        Stop();
        if (!desktopWindow || !targetWindow || !message || !wgc::GraphicsCaptureSession::IsSupported())
            return false;
        try {
            sourceWindow = desktopWindow;
            notifyWindow = targetWindow;
            notifyMessage = message;
            GetWindowRect(sourceWindow, &sourceBounds);
            winrtDevice = CreateWinRtDevice(d3dDevice, d3dContext);
            captureItem = CaptureItemForWindow(sourceWindow);
            const auto size = captureItem.Size();
            if (size.Width <= 0 || size.Height <= 0) throw winrt::hresult_error(E_FAIL);
            framePool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
                winrtDevice, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
            session = framePool.CreateCaptureSession(captureItem);
            session.IsCursorCaptureEnabled(false);
            try {
                session.IsBorderRequired(false);
            } catch (...) {
                // Older Windows builds do not allow applications to disable
                // the capture border. Capture itself remains usable.
            }
            frameToken = framePool.FrameArrived(
                [this](const auto& sender, const auto&) { OnFrameArrived(sender); });
            closedToken = captureItem.Closed([this](const auto&, const auto&) {
                running.store(false, std::memory_order_release);
            });
            stopping.store(false, std::memory_order_release);
            running.store(true, std::memory_order_release);
            session.StartCapture();
            return true;
        } catch (...) {
            Stop();
            return false;
        }
    }

    void Stop() {
        stopping.store(true, std::memory_order_release);
        std::scoped_lock callbackLock(callbackMutex);
        running.store(false, std::memory_order_release);
        if (framePool) {
            try { framePool.FrameArrived(frameToken); } catch (...) {}
        }
        if (captureItem) {
            try { captureItem.Closed(closedToken); } catch (...) {}
        }
        if (session) {
            try { session.Close(); } catch (...) {}
        }
        if (framePool) {
            try { framePool.Close(); } catch (...) {}
        }
        session = nullptr;
        framePool = nullptr;
        captureItem = nullptr;
        winrtDevice = nullptr;
        stagingTexture = nullptr;
        d3dContext = nullptr;
        d3dDevice = nullptr;
        stagingWidth = 0;
        stagingHeight = 0;
        sourceWindow = nullptr;
        notifyWindow = nullptr;
        notifyMessage = 0;
        notificationPending.store(false, std::memory_order_release);
        stopping.store(false, std::memory_order_release);
    }

    bool TakeLatestFrame(LiveDesktopFrame& frame) {
        std::scoped_lock lock(frameMutex);
        notificationPending.store(false, std::memory_order_release);
        if (!latestGeneration || latestGeneration == takenGeneration ||
            latestWidth <= 0 || latestHeight <= 0 || latestPixels.empty()) return false;
        frame.bounds = sourceBounds;
        frame.width = latestWidth;
        frame.height = latestHeight;
        frame.generation = latestGeneration;
        frame.pixels.swap(latestPixels);
        takenGeneration = latestGeneration;
        return true;
    }
};

LiveDesktopCapture::LiveDesktopCapture() : impl_(std::make_unique<Impl>()) {}
LiveDesktopCapture::~LiveDesktopCapture() { impl_->Stop(); }

bool LiveDesktopCapture::Start(HWND desktopWindow, HWND notifyWindow, UINT notifyMessage) {
    return impl_->Start(desktopWindow, notifyWindow, notifyMessage);
}

void LiveDesktopCapture::Stop() { impl_->Stop(); }

bool LiveDesktopCapture::TakeLatestFrame(LiveDesktopFrame& frame) {
    return impl_->TakeLatestFrame(frame);
}

bool LiveDesktopCapture::IsRunning() const {
    return impl_->running.load(std::memory_order_acquire);
}
