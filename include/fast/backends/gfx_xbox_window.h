#pragma once

// Original-Xbox windowing/present backend for Fast3D, built on RXDK libd3d8.
// Owns the Direct3D8 device (there is no OS window on the console — the "window" is the
// framebuffer). The rendering API (GfxRenderingAPIXbox) borrows the device via GetDevice().
// D3D8 types are kept opaque here so this header pulls no <d3d8.h> into the include tree.

#include "fast/backends/gfx_window_manager_api.h"
#include <stdint.h>

namespace Fast {

class GfxWindowBackendXbox : public GfxWindowBackend {
  public:
    GfxWindowBackendXbox() = default;
    ~GfxWindowBackendXbox() override = default;

    // Returns the LPDIRECT3DDEVICE8 as an opaque pointer (cast in the .cpp consumers).
    void* GetDevice() const {
        return mDevice;
    }
    void GetFramebufferSize(uint32_t* width, uint32_t* height) const {
        *width = mWidth;
        *height = mHeight;
    }

    void Init(const char* gameName, const char* apiName, bool startFullScreen, uint32_t width, uint32_t height,
              int32_t posX, int32_t posY) override;
    void Close() override;
    void SetKeyboardCallbacks(bool (*onKeyDown)(int), bool (*onKeyUp)(int), void (*onAllKeysUp)()) override;
    void SetMouseCallbacks(bool (*onMouseButtonDown)(int), bool (*onMouseButtonUp)(int)) override;
    void SetFullscreenChangedCallback(void (*onFullscreenChanged)(bool)) override;
    void SetFullscreen(bool fullscreen) override;
    void GetActiveWindowRefreshRate(uint32_t* refreshRate) override;
    void SetCursorVisibility(bool visible) override;
    void SetMousePos(int32_t x, int32_t y) override;
    void GetMousePos(int32_t* x, int32_t* y) override;
    void GetMouseDelta(int32_t* x, int32_t* y) override;
    void GetMouseWheel(float* x, float* y) override;
    bool GetMouseState(uint32_t btn) override;
    void SetMouseCapture(bool capture) override;
    bool IsMouseCaptured() override;
    void GetDimensions(uint32_t* width, uint32_t* height, int32_t* posX, int32_t* posY) override;
    void SetDimensions(uint32_t width, uint32_t height, int32_t posX, int32_t posY) override;
    Ship::WindowRect GetPrimaryMonitorRect() override;
    void HandleEvents() override;
    bool IsFrameReady() override;
    void SwapBuffersBegin() override;
    void SwapBuffersEnd() override;
    double GetTime() override;
    int GetTargetFps() override;
    void SetTargetFps(int fps) override;
    void SetMaxFrameLatency(int latency) override;
    const char* GetKeyName(int scancode) override;
    bool CanDisableVsync() override;
    bool IsRunning() override;
    void Destroy() override;
    bool IsFullscreen() override;

  private:
    void* mD3d = nullptr;    // LPDIRECT3D8
    void* mDevice = nullptr; // LPDIRECT3DDEVICE8
    uint32_t mWidth = 640;
    uint32_t mHeight = 480;
    int64_t mTimerFreq = 0;
    int64_t mTimerStart = 0;
};
} // namespace Fast
