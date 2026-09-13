#include "fast/backends/gfx_xbox_window.h"

#include <xtl.h>
#include <d3d8.h>
#include <cstdio>
#include <memory>

#include "fast/Fast3dGui.h"
#include "fast/Fast3dWindow.h"
#include "ship/Context.h"
#include "ship/window/Window.h"

namespace Fast {

static inline IDirect3DDevice8* Dev(void* p) {
    return reinterpret_cast<IDirect3DDevice8*>(p);
}

void GfxWindowBackendXbox::Init(const char* gameName, const char* apiName, bool startFullScreen, uint32_t width,
                                uint32_t height, int32_t posX, int32_t posY) {
    (void)gameName;
    (void)apiName;
    (void)startFullScreen;
    (void)posX;
    (void)posY;

    mWidth = width ? width : 640;
    mHeight = height ? height : 480;

    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    mTimerFreq = freq.QuadPart;
    mTimerStart = now.QuadPart;

    LPDIRECT3D8 d3d = Direct3DCreate8(D3D_SDK_VERSION);
    mD3d = d3d;
    if (!d3d) {
        return;
    }

    D3DPRESENT_PARAMETERS d3dpp;
    ZeroMemory(&d3dpp, sizeof(d3dpp));
    d3dpp.BackBufferWidth = mWidth;
    d3dpp.BackBufferHeight = mHeight;
    d3dpp.BackBufferFormat = D3DFMT_X8R8G8B8;
    d3dpp.BackBufferCount = 1;
    d3dpp.Windowed = FALSE;
    d3dpp.EnableAutoDepthStencil = TRUE;
    d3dpp.AutoDepthStencilFormat = D3DFMT_D24S8;
    d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    d3dpp.FullScreen_RefreshRateInHz = 60;
    d3dpp.FullScreen_PresentationInterval =
        mVsyncEnabled ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;

    // Fast3D pushes a lot of small draws; give the pushbuffer generous headroom.
    Direct3D_SetPushBufferSize(1024 * 1024, (1024 * 1024) / 16);

    LPDIRECT3DDEVICE8 dev = nullptr;
    HRESULT hr = Direct3D_CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, NULL,
                                       D3DCREATE_HARDWARE_VERTEXPROCESSING, &d3dpp, &dev);
    if (FAILED(hr)) {
        mDevice = nullptr;
        return;
    }
    mDevice = dev;

    // Create the ImGui context (Gui::Init) exactly as the desktop backends do after their
    // window/device is up. The GuiWindows built by SohGui::SetupMenu call ImGui in their
    // InitElement, so the context must exist first. The ImGui *render* backend stays a no-op
    // on Xbox (Fast3dGui::ImGuiBackendInit/NewFrame/RenderDrawData fall through), so nothing
    // rasterizes the menus yet -- the game still renders via Fast3D to the backbuffer.
    GuiWindowInitData windowImpl;
    windowImpl.Backend = WindowBackend::FAST3D_XBOX_D3D8;
    auto gui = Ship::Context::GetRawInstance()->GetWindow()->GetGui();
    if (gui != nullptr) {
        std::dynamic_pointer_cast<Fast3dGui>(gui)->Init(windowImpl);
    }
}

void GfxWindowBackendXbox::Close() {
    Destroy();
}

void GfxWindowBackendXbox::Destroy() {
    if (mDevice) {
        Dev(mDevice)->Release();
        mDevice = nullptr;
    }
    if (mD3d) {
        reinterpret_cast<IDirect3D8*>(mD3d)->Release();
        mD3d = nullptr;
    }
}

// --- Input/window stubs (no OS window or mouse on the console; input is XInput, Phase 2). ---
void GfxWindowBackendXbox::SetKeyboardCallbacks(bool (*onKeyDown)(int), bool (*onKeyUp)(int), void (*onAllKeysUp)()) {
    mOnKeyDown = onKeyDown;
    mOnKeyUp = onKeyUp;
}
void GfxWindowBackendXbox::SetMouseCallbacks(bool (*onMouseButtonDown)(int), bool (*onMouseButtonUp)(int)) {
    mOnMouseButtonDown = onMouseButtonDown;
    mOnMouseButtonUp = onMouseButtonUp;
}
void GfxWindowBackendXbox::SetFullscreenChangedCallback(void (*onFullscreenChanged)(bool)) {
    mOnFullscreenChanged = onFullscreenChanged;
}
void GfxWindowBackendXbox::SetFullscreen(bool fullscreen) {
    (void)fullscreen;
}
void GfxWindowBackendXbox::GetActiveWindowRefreshRate(uint32_t* refreshRate) {
    *refreshRate = 60;
}
void GfxWindowBackendXbox::SetCursorVisibility(bool visible) {
    (void)visible;
}
void GfxWindowBackendXbox::SetMousePos(int32_t x, int32_t y) {
    (void)x;
    (void)y;
}
void GfxWindowBackendXbox::GetMousePos(int32_t* x, int32_t* y) {
    *x = 0;
    *y = 0;
}
void GfxWindowBackendXbox::GetMouseDelta(int32_t* x, int32_t* y) {
    *x = 0;
    *y = 0;
}
void GfxWindowBackendXbox::GetMouseWheel(float* x, float* y) {
    *x = 0;
    *y = 0;
}
bool GfxWindowBackendXbox::GetMouseState(uint32_t btn) {
    (void)btn;
    return false;
}
void GfxWindowBackendXbox::SetMouseCapture(bool capture) {
    (void)capture;
}
bool GfxWindowBackendXbox::IsMouseCaptured() {
    return false;
}

void GfxWindowBackendXbox::GetDimensions(uint32_t* width, uint32_t* height, int32_t* posX, int32_t* posY) {
    *width = mWidth;
    *height = mHeight;
    *posX = 0;
    *posY = 0;
}
void GfxWindowBackendXbox::SetDimensions(uint32_t width, uint32_t height, int32_t posX, int32_t posY) {
    (void)width;
    (void)height;
    (void)posX;
    (void)posY;
}
Ship::WindowRect GfxWindowBackendXbox::GetPrimaryMonitorRect() {
    Ship::WindowRect r;
    r.Left = 0;
    r.Top = 0;
    r.Right = (int32_t)mWidth;
    r.Bottom = (int32_t)mHeight;
    return r;
}

void GfxWindowBackendXbox::HandleEvents() {
    // Input pumped separately (Phase 2 XInput). Nothing to poll here.
}
bool GfxWindowBackendXbox::IsFrameReady() {
    return true;
}

void GfxWindowBackendXbox::SwapBuffersBegin() {
    // Present happens in SwapBuffersEnd.
}
void GfxWindowBackendXbox::SwapBuffersEnd() {
    if (mDevice) {
        Dev(mDevice)->Present(NULL, NULL, NULL, NULL);
    }
}

double GfxWindowBackendXbox::GetTime() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (mTimerFreq == 0) {
        return 0.0;
    }
    return (double)(now.QuadPart - mTimerStart) / (double)mTimerFreq;
}

int GfxWindowBackendXbox::GetTargetFps() {
    return (int)mTargetFps;
}
void GfxWindowBackendXbox::SetTargetFps(int fps) {
    mTargetFps = (uint32_t)fps;
}
void GfxWindowBackendXbox::SetMaxFrameLatency(int latency) {
    (void)latency;
}
const char* GfxWindowBackendXbox::GetKeyName(int scancode) {
    (void)scancode;
    return "";
}
bool GfxWindowBackendXbox::CanDisableVsync() {
    return true;
}
bool GfxWindowBackendXbox::IsRunning() {
    return mIsRunning;
}
bool GfxWindowBackendXbox::IsFullscreen() {
    return true;
}
} // namespace Fast
