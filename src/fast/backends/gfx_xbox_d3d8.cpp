#include "fast/backends/gfx_xbox_d3d8.h"
#include "fast/backends/gfx_xbox_window.h"
#include "fast/interpreter.h" // gfx_cc_get_features + CCFeatures

#include <xtl.h>
#include <d3d8.h>
#include <xgraphics.h> // XGSwizzleRect
#include <cstring>     // memcpy
#include <vector>

// Phase-1 Xbox/NV2A Fast3D backend. Device-level ops (clear/viewport/present-adjacent
// state) and texture upload are implemented; DrawTriangles + the combiner->register-
// combiner shader lowering are being brought up incrementally (see docs/XBOX_PORT_PLAN.md).

namespace Fast {

static inline IDirect3DDevice8* Dev(void* p) {
    return reinterpret_cast<IDirect3DDevice8*>(p);
}

void* GfxRenderingAPIXbox::Device() const {
    return mWindow ? mWindow->GetDevice() : nullptr;
}

// --- NV2A register-combiner (pixel shader) construction ---------------------------------------
// The N64 colour combiner computes, per cycle and separately for RGB and alpha, one of:
//   single:   D            multiply: A*C            mix: (A-B)*C + B          general: (A-B)*C + D
// (matches Fast3D's shader generator, gfx_opengl.cpp append_formula). We lower that to the NV2A's
// 8 general combiners + final combiner via D3DPIXELSHADERDEF. Registers: T0/T1 = texels, V0 =
// shade/diffuse, V1 = 2nd vertex input, C0/C1 = constant inputs, R0 = result/scratch.

// Map an N64 combiner input (SHADER_* constant) to an NV2A combiner input word (reg|channel|map)
// for the RGB combiner (alphaComb=false) or alpha combiner (alphaComb=true) of a given cycle.
static DWORD NvCombInput(int item, int cycle, bool alphaComb) {
    DWORD reg;
    bool forceAlpha = false;
    switch (item) {
        case SHADER_0:
            return PS_REGISTER_ZERO;
        case SHADER_1:
            return PS_REGISTER_ONE;
        case SHADER_NOISE:
            return PS_REGISTER_ONE; // no noise source; approximate as 1.0
        case SHADER_INPUT_1:
            reg = PS_REGISTER_V0;
            break; // per-vertex shade (vertex diffuse)
        case SHADER_INPUT_2:
            reg = PS_REGISTER_C0;
            break; // primitive-constant (PRIM/ENV) -> const C0
        case SHADER_INPUT_3:
            reg = PS_REGISTER_C1;
            break;
        case SHADER_INPUT_4:
            return PS_REGISTER_ONE; // rare; not enough constant regs -> approximate
        case SHADER_TEXEL0:
            reg = (cycle == 0) ? PS_REGISTER_T0 : PS_REGISTER_T1;
            break;
        case SHADER_TEXEL1:
            reg = (cycle == 0) ? PS_REGISTER_T1 : PS_REGISTER_T0;
            break;
        case SHADER_TEXEL0A:
            reg = (cycle == 0) ? PS_REGISTER_T0 : PS_REGISTER_T1;
            forceAlpha = true;
            break;
        case SHADER_TEXEL1A:
            reg = (cycle == 0) ? PS_REGISTER_T1 : PS_REGISTER_T0;
            forceAlpha = true;
            break;
        case SHADER_COMBINED:
            reg = PS_REGISTER_R0;
            break;
        default:
            return PS_REGISTER_ZERO;
    }
    DWORD ch = (alphaComb || forceAlpha) ? PS_CHANNEL_ALPHA : PS_CHANNEL_RGB;
    return reg | PS_INPUTMAPPING_UNSIGNED_IDENTITY | ch;
}

// Re-map the input-mapping bits (top 3 bits of the 8-bit input word) of an already-encoded input.
static inline DWORD RemapInput(DWORD in, DWORD mapping) {
    return (in & ~0xE0u) | mapping;
}

// How many NV2A general-combiner stages one N64 cycle+channel needs (general (A-B)*C+D = 2, else 1).
static int ChannelStages(bool single, bool mul, bool mix) {
    return (!single && !mul && !mix) ? 2 : 1;
}

// Fill one stage's inputs+outputs for one channel (RGB or alpha) of one cycle, writing to outReg.
static void FillChannel(DWORD& inWord, DWORD& outWord, bool alphaComb, const int c[4], bool single, bool mul, bool mix,
                        int s, int chanStages, int cycle, DWORD outReg) {
    const DWORD ONE = PS_REGISTER_ONE;
    const DWORD ZERO = PS_REGISTER_ZERO;
    const DWORD ch = alphaComb ? PS_CHANNEL_ALPHA : PS_CHANNEL_RGB;
    const DWORD outAsIn = outReg | PS_INPUTMAPPING_UNSIGNED_IDENTITY | ch;

    if (s >= chanStages) {
        // This channel already produced its result in an earlier stage; preserve it (out = out*1).
        inWord = PS_COMBINERINPUTS(outAsIn, ONE, ZERO, ZERO);
        outWord = PS_COMBINEROUTPUTS(outReg, PS_REGISTER_DISCARD, PS_REGISTER_DISCARD, 0);
        return;
    }
    DWORD A = NvCombInput(c[0], cycle, alphaComb);
    DWORD B = NvCombInput(c[1], cycle, alphaComb);
    DWORD C = NvCombInput(c[2], cycle, alphaComb);
    DWORD D = NvCombInput(c[3], cycle, alphaComb);
    if (single) {
        // out = D  (AB = D*1)
        inWord = PS_COMBINERINPUTS(D, ONE, ZERO, ZERO);
        outWord = PS_COMBINEROUTPUTS(outReg, PS_REGISTER_DISCARD, PS_REGISTER_DISCARD, 0);
    } else if (mul) {
        // out = A*C  (AB = A*C)
        inWord = PS_COMBINERINPUTS(A, C, ZERO, ZERO);
        outWord = PS_COMBINEROUTPUTS(outReg, PS_REGISTER_DISCARD, PS_REGISTER_DISCARD, 0);
    } else if (mix) {
        // out = A*C + B*(1-C) = mix(B,A,C)  (AB=A*C, CD=B*(1-C), SUM)
        inWord = PS_COMBINERINPUTS(A, C, B, RemapInput(C, PS_INPUTMAPPING_UNSIGNED_INVERT));
        outWord = PS_COMBINEROUTPUTS(PS_REGISTER_DISCARD, PS_REGISTER_DISCARD, outReg, 0);
    } else if (s == 0) {
        // general step 1: (A-B)*C  (AB=A*C, CD=(-B)*C, SUM)
        inWord = PS_COMBINERINPUTS(A, C, RemapInput(B, PS_INPUTMAPPING_SIGNED_NEGATE), C);
        outWord = PS_COMBINEROUTPUTS(PS_REGISTER_DISCARD, PS_REGISTER_DISCARD, outReg, 0);
    } else {
        // general step 2: + D  (AB=out*1, CD=D*1, SUM)
        inWord = PS_COMBINERINPUTS(outAsIn, ONE, D, ONE);
        outWord = PS_COMBINEROUTPUTS(PS_REGISTER_DISCARD, PS_REGISTER_DISCARD, outReg, 0);
    }
}

// Build an NV2A register-combiner pixel shader implementing the full N64 colour combiner for `sp`.
// Returns a pixel-shader handle, or 0 on failure (caller falls back to the modulate/fixed path).
static DWORD BuildCombinerPS(IDirect3DDevice8* d, const ShaderProgram& sp) {
    D3DPIXELSHADERDEF def;
    memset(&def, 0, sizeof(def));

    // Sample T0/T1 as plain 2D textures (texcoord set i) where the combiner uses them. A sampler
    // stage with no valid 2D texture bound makes xemu's psh assert on dimensions, so DrawTriangles
    // always binds a real-or-dummy 2D texture to any stage flagged used here.
    DWORD tm0 = sp.usedTextures[0] ? PS_TEXTUREMODES_PROJECT2D : PS_TEXTUREMODES_NONE;
    DWORD tm1 = sp.usedTextures[1] ? PS_TEXTUREMODES_PROJECT2D : PS_TEXTUREMODES_NONE;
    def.PSTextureModes = PS_TEXTUREMODES(tm0, tm1, PS_TEXTUREMODES_NONE, PS_TEXTUREMODES_NONE);
    // Per-stage unique constants; C0<-D3D const 0 (input2), C1<-D3D const 1 (input3), all stages.
    def.PSC0Mapping = PS_CONSTANTMAPPING(0, 0, 0, 0, 0, 0, 0, 0);
    def.PSC1Mapping = PS_CONSTANTMAPPING(1, 1, 1, 1, 1, 1, 1, 1);

    int nCycles = sp.opt2cyc ? 2 : 1;
    int stage = 0;
    for (int cyc = 0; cyc < nCycles && stage < 8; cyc++) {
        DWORD outReg = (cyc == 0) ? PS_REGISTER_R0 : PS_REGISTER_R1;
        int rgbN = ChannelStages(sp.doSingle[cyc][0], sp.doMultiply[cyc][0], sp.doMix[cyc][0]);
        int aN = ChannelStages(sp.doSingle[cyc][1], sp.doMultiply[cyc][1], sp.doMix[cyc][1]);
        int n = rgbN > aN ? rgbN : aN;
        for (int s = 0; s < n && stage < 8; s++, stage++) {
            FillChannel(def.PSRGBInputs[stage], def.PSRGBOutputs[stage], false, sp.cc[cyc][0],
                        sp.doSingle[cyc][0], sp.doMultiply[cyc][0], sp.doMix[cyc][0], s, rgbN, cyc, outReg);
            FillChannel(def.PSAlphaInputs[stage], def.PSAlphaOutputs[stage], true, sp.cc[cyc][1],
                        sp.doSingle[cyc][1], sp.doMultiply[cyc][1], sp.doMix[cyc][1], s, aN, cyc, outReg);
        }
    }
    if (stage == 0) {
        stage = 1; // safety: at least one combiner stage
        def.PSRGBInputs[0] = PS_COMBINERINPUTS(PS_REGISTER_V0 | PS_INPUTMAPPING_UNSIGNED_IDENTITY | PS_CHANNEL_RGB,
                                               PS_REGISTER_ONE, PS_REGISTER_ZERO, PS_REGISTER_ZERO);
        def.PSRGBOutputs[0] = PS_COMBINEROUTPUTS(PS_REGISTER_R0, PS_REGISTER_DISCARD, PS_REGISTER_DISCARD, 0);
        def.PSAlphaInputs[0] = PS_COMBINERINPUTS(PS_REGISTER_V0 | PS_INPUTMAPPING_UNSIGNED_IDENTITY | PS_CHANNEL_ALPHA,
                                                 PS_REGISTER_ONE, PS_REGISTER_ZERO, PS_REGISTER_ZERO);
        def.PSAlphaOutputs[0] = PS_COMBINEROUTPUTS(PS_REGISTER_R0, PS_REGISTER_DISCARD, PS_REGISTER_DISCARD, 0);
    }
    def.PSCombinerCount = PS_COMBINERCOUNT(stage, PS_COMBINERCOUNT_UNIQUE_C0 | PS_COMBINERCOUNT_UNIQUE_C1);

    // Final combiner: fragment.rgb = last cycle's result register (rgb), fragment.a = its alpha.
    DWORD lastReg = (nCycles == 2) ? PS_REGISTER_R1 : PS_REGISTER_R0;
    def.PSFinalCombinerInputsABCD = PS_COMBINERINPUTS(PS_REGISTER_ZERO, PS_REGISTER_ZERO,
                                                      lastReg | PS_INPUTMAPPING_UNSIGNED_IDENTITY | PS_CHANNEL_RGB,
                                                      PS_REGISTER_ZERO);
    def.PSFinalCombinerInputsEFG = PS_COMBINERINPUTS(PS_REGISTER_ZERO, PS_REGISTER_ZERO,
                                                     lastReg | PS_INPUTMAPPING_UNSIGNED_IDENTITY | PS_CHANNEL_ALPHA, 0);

    DWORD handle = 0;
    if (FAILED(d->CreatePixelShader(&def, &handle))) {
        return 0;
    }
    return handle;
}

const char* GfxRenderingAPIXbox::GetName() {
    return "Xbox D3D8";
}

int GfxRenderingAPIXbox::GetMaxTextureSize() {
    // Policy clamp (also sizes the interpreter's upload staging buffer = size^2*4).
    // 512 -> 1 MB staging; scale with detected RAM later (see plan §5/§6).
    return 512;
}

GfxClipParameters GfxRenderingAPIXbox::GetClipParameters() {
    // D3D-style depth [0,1]; Xbox framebuffer is top-left origin (no Y invert needed).
    return { true, false };
}

void GfxRenderingAPIXbox::Init() {
    IDirect3DDevice8* d = Dev(Device());
    mTextures.clear();
    mTextures.push_back(nullptr); // texId 0 reserved / invalid

    // Framebuffer 0 = the window backbuffer. The present chain uses a rotating pair of frame
    // buffers (SwapEffect DISCARD), so the *color* surface is fetched live per bind via
    // GetBackBuffer(0) in StartDrawToFramebuffer — caching it here would go stale after Present.
    // The auto depth-stencil is a single persistent surface, so we borrow it once.
    mFramebuffers.clear();
    mFramebuffers.resize(1);
    {
        XboxFramebuffer& fb0 = mFramebuffers[0];
        fb0.isWindow = true;
        fb0.hasDepth = true;
        fb0.colorSurf = nullptr; // fetched live (backbuffer rotates)
        if (mWindow) {
            mWindow->GetFramebufferSize(&fb0.width, &fb0.height);
        }
        if (d) {
            IDirect3DSurface8* depth = nullptr;
            d->GetDepthStencilSurface(&depth);
            fb0.depthSurf = depth;
        }
    }
    mCurrentFb = 0;

    if (!d) {
        return;
    }

    // 1x1 white 2D texture for combiner sampler stages that lack a real texture.
    {
        IDirect3DTexture8* dummy = nullptr;
        if (SUCCEEDED(d->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &dummy)) && dummy) {
            D3DLOCKED_RECT lr;
            if (SUCCEEDED(dummy->LockRect(0, &lr, NULL, 0))) {
                *reinterpret_cast<uint32_t*>(lr.pBits) = 0xFFFFFFFFu; // opaque white
                dummy->UnlockRect(0);
            }
            mDummyTex = dummy;
        }
    }

    d->SetRenderState(D3DRS_ZENABLE, TRUE);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
}

void GfxRenderingAPIXbox::OnResize() {
}
void GfxRenderingAPIXbox::StartFrame() {
    mFrameCount++;
    IDirect3DDevice8* d = Dev(Device());
    if (d) {
        d->BeginScene();
    }
}
void GfxRenderingAPIXbox::EndFrame() {
    IDirect3DDevice8* d = Dev(Device());
    if (d) {
        d->EndScene();
    }
}
void GfxRenderingAPIXbox::FinishRender() {
}

// --- Framebuffers: render-to-texture on the NV2A. fb 0 is the window backbuffer (set up in
//     Init); fb >= 1 are D3DUSAGE_RENDERTARGET textures the game draws into and samples back. ---
int GfxRenderingAPIXbox::CreateFramebuffer() {
    mFramebuffers.emplace_back();
    return (int)(mFramebuffers.size() - 1);
}

void GfxRenderingAPIXbox::UpdateFramebufferParameters(int fbId, uint32_t width, uint32_t height, uint32_t msaaLevel,
                                                      bool openglInvertY, bool renderTarget, bool hasDepthBuffer,
                                                      bool canExtractDepth) {
    (void)msaaLevel;
    (void)renderTarget;
    (void)canExtractDepth;
    if (fbId < 0 || (size_t)fbId >= mFramebuffers.size()) {
        return;
    }
    XboxFramebuffer& fb = mFramebuffers[fbId];
    fb.invertY = openglInvertY;

    // fb 0 is the backbuffer: just track the window size, never (re)allocate.
    if (fb.isWindow) {
        if (width) {
            fb.width = width;
        }
        if (height) {
            fb.height = height;
        }
        return;
    }

    if (width == 0 || height == 0) {
        return;
    }
    // Already sized correctly — nothing to do.
    if (fb.colorTex && fb.width == width && fb.height == height && fb.hasDepth == hasDepthBuffer) {
        return;
    }

    IDirect3DDevice8* d = Dev(Device());
    if (!d) {
        return;
    }

    // Release any previous allocation (resize/reparametrise).
    if (fb.colorSurf) {
        reinterpret_cast<IDirect3DSurface8*>(fb.colorSurf)->Release();
        fb.colorSurf = nullptr;
    }
    if (fb.colorTex) {
        reinterpret_cast<IDirect3DTexture8*>(fb.colorTex)->Release();
        fb.colorTex = nullptr;
    }
    if (fb.depthSurf) {
        reinterpret_cast<IDirect3DSurface8*>(fb.depthSurf)->Release();
        fb.depthSurf = nullptr;
    }

    // Linear A8R8G8B8: directly renderable and sampleable with no swizzle step (the surface is
    // read back by SelectTextureFb as a plain clamp-addressed texture). Pow2-swizzled render
    // targets would need a resolve before sampling; the linear format avoids that entirely.
    IDirect3DTexture8* tex = nullptr;
    if (FAILED(d->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_LIN_A8R8G8B8, D3DPOOL_DEFAULT, &tex)) ||
        !tex) {
        return;
    }
    IDirect3DSurface8* colorSurf = nullptr;
    if (FAILED(tex->GetSurfaceLevel(0, &colorSurf)) || !colorSurf) {
        tex->Release();
        return;
    }
    IDirect3DSurface8* depthSurf = nullptr;
    if (hasDepthBuffer) {
        d->CreateDepthStencilSurface(width, height, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, &depthSurf);
    }

    fb.colorTex = tex;
    fb.colorSurf = colorSurf;
    fb.depthSurf = depthSurf;
    fb.width = width;
    fb.height = height;
    fb.hasDepth = hasDepthBuffer;
}

void GfxRenderingAPIXbox::StartDrawToFramebuffer(int fbId, float noiseScale) {
    (void)noiseScale;
    IDirect3DDevice8* d = Dev(Device());
    if (!d || fbId < 0 || (size_t)fbId >= mFramebuffers.size()) {
        return;
    }
    XboxFramebuffer& fb = mFramebuffers[fbId];
    if (fb.isWindow) {
        // The backbuffer rotates through the present chain; bind whichever surface is current.
        // GetBackBuffer(0) AddRefs it and SetRenderTarget takes its own reference, so release ours.
        IDirect3DSurface8* bb = nullptr;
        if (SUCCEEDED(d->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
            d->SetRenderTarget(bb, reinterpret_cast<IDirect3DSurface8*>(fb.depthSurf));
            bb->Release();
        }
    } else {
        // An auxiliary fb whose surfaces aren't allocated yet (no UpdateFramebufferParameters): stay
        // on the current target rather than unbinding everything.
        if (!fb.colorSurf) {
            return;
        }
        d->SetRenderTarget(reinterpret_cast<IDirect3DSurface8*>(fb.colorSurf),
                           reinterpret_cast<IDirect3DSurface8*>(fb.depthSurf));
    }
    mCurrentFb = fbId;

    // SetRenderTarget resets the viewport to the full surface; establish a matching full-surface
    // viewport. The interpreter issues an explicit SetViewport for the game's sub-rect next.
    D3DVIEWPORT8 vp;
    vp.X = 0;
    vp.Y = 0;
    vp.Width = fb.width;
    vp.Height = fb.height;
    vp.MinZ = 0.0f;
    vp.MaxZ = 1.0f;
    d->SetViewport(&vp);
    mVpX = 0;
    mVpY = 0;
    mVpW = (float)fb.width;
    mVpH = (float)fb.height;
}

void GfxRenderingAPIXbox::CopyFramebuffer(int fbDstId, int fbSrcId, int srcX0, int srcY0, int srcX1, int srcY1,
                                          int dstX0, int dstY0, int dstX1, int dstY1) {
    IDirect3DDevice8* d = Dev(Device());
    if (!d || fbDstId < 0 || (size_t)fbDstId >= mFramebuffers.size() || fbSrcId < 0 ||
        (size_t)fbSrcId >= mFramebuffers.size()) {
        return;
    }
    XboxFramebuffer& dst = mFramebuffers[fbDstId];
    XboxFramebuffer& src = mFramebuffers[fbSrcId];
    IDirect3DSurface8* srcSurf = reinterpret_cast<IDirect3DSurface8*>(src.colorSurf);
    IDirect3DSurface8* dstSurf = reinterpret_cast<IDirect3DSurface8*>(dst.colorSurf);
    if (!srcSurf || !dstSurf) {
        return;
    }

    // Clamp and normalise the rect (Fast3D may pass y inverted / out of range).
    auto clampi = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };
    int sx0 = clampi(srcX0 < srcX1 ? srcX0 : srcX1, 0, (int)src.width);
    int sy0 = clampi(srcY0 < srcY1 ? srcY0 : srcY1, 0, (int)src.height);
    int sx1 = clampi(srcX0 < srcX1 ? srcX1 : srcX0, 0, (int)src.width);
    int sy1 = clampi(srcY0 < srcY1 ? srcY1 : srcY0, 0, (int)src.height);
    int dx0 = clampi(dstX0 < dstX1 ? dstX0 : dstX1, 0, (int)dst.width);
    int dy0 = clampi(dstY0 < dstY1 ? dstY0 : dstY1, 0, (int)dst.height);
    int w = sx1 - sx0;
    int h = sy1 - sy0;
    if (w <= 0 || h <= 0) {
        return;
    }
    // Xbox CopyRects does not scale; fit the copy to whatever both surfaces can hold.
    w = clampi(w, 0, (int)dst.width - dx0);
    h = clampi(h, 0, (int)dst.height - dy0);
    if (w <= 0 || h <= 0) {
        return;
    }

    RECT srcRect;
    srcRect.left = sx0;
    srcRect.top = sy0;
    srcRect.right = sx0 + w;
    srcRect.bottom = sy0 + h;
    POINT dstPoint;
    dstPoint.x = dx0;
    dstPoint.y = dy0;
    d->CopyRects(srcSurf, &srcRect, 1, dstSurf, &dstPoint);
}

void GfxRenderingAPIXbox::ClearFramebuffer(bool color, bool depth) {
    IDirect3DDevice8* d = Dev(Device());
    if (!d) {
        return;
    }
    DWORD flags = 0;
    if (color) {
        flags |= D3DCLEAR_TARGET;
    }
    if (depth) {
        flags |= D3DCLEAR_ZBUFFER;
    }
    if (flags) {
        d->Clear(0, NULL, flags, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
    }
}
void GfxRenderingAPIXbox::ClearDepthRegion(int, int, int, int) {
    ClearFramebuffer(false, true);
}
void GfxRenderingAPIXbox::ReadFramebufferToCPU(int, uint32_t, uint32_t, uint16_t*) {
}
void GfxRenderingAPIXbox::ResolveMSAAColorBuffer(int, int) {
}
std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff>
GfxRenderingAPIXbox::GetPixelDepth(int, const std::set<std::pair<float, float>>& coordinates) {
    std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff> out;
    for (const auto& c : coordinates) {
        out[c] = 0;
    }
    return out;
}
void* GfxRenderingAPIXbox::GetFramebufferTextureId(int fbId) {
    if (fbId < 0 || (size_t)fbId >= mFramebuffers.size()) {
        return nullptr;
    }
    return mFramebuffers[fbId].colorTex; // null for fb 0 (backbuffer isn't sampleable)
}
void GfxRenderingAPIXbox::SelectTextureFb(int fbId) {
    IDirect3DDevice8* d = Dev(Device());
    if (!d || fbId < 0 || (size_t)fbId >= mFramebuffers.size()) {
        return;
    }
    IDirect3DTexture8* tex = reinterpret_cast<IDirect3DTexture8*>(mFramebuffers[fbId].colorTex);
    if (!tex) {
        return;
    }
    // Bind on tile 0 (the combiner that samples an fb reads TEXEL0) and mark the draw textured.
    mActiveTile = 0;
    mBoundTexture[0] = kFbBoundSentinel;
    d->SetTexture(0, tex);
    // Linear RT textures are clamp-addressed; match that so sampling the composite doesn't wrap.
    d->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
    d->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
}

// --- Viewport / scissor / depth / alpha ---
void GfxRenderingAPIXbox::SetViewport(int x, int y, int width, int height) {
    IDirect3DDevice8* d = Dev(Device());
    if (!d) {
        return;
    }
    // Flip GL bottom-left viewport origin to D3D top-left, relative to the *current* render
    // target's height — an auxiliary framebuffer can be a different size than the 480 window.
    uint32_t fbw = 640, fbh = 480;
    if (mCurrentFb >= 0 && (size_t)mCurrentFb < mFramebuffers.size() && mFramebuffers[mCurrentFb].height) {
        fbw = mFramebuffers[mCurrentFb].width;
        fbh = mFramebuffers[mCurrentFb].height;
    } else if (mWindow) {
        mWindow->GetFramebufferSize(&fbw, &fbh);
    }
    int flippedY = (int)fbh - y - height;
    D3DVIEWPORT8 vp;
    vp.X = (DWORD)x;
    // Fast3D uses GL bottom-left viewport origin; flip to D3D top-left.
    vp.Y = (DWORD)flippedY;
    vp.Width = (DWORD)width;
    vp.Height = (DWORD)height;
    vp.MinZ = 0.0f;
    vp.MaxZ = 1.0f;
    d->SetViewport(&vp);

    // Keep the (top-left) viewport for the CPU clip-space -> screen transform in DrawTriangles.
    mVpX = (float)x;
    mVpY = (float)flippedY;
    mVpW = (float)width;
    mVpH = (float)height;
}
void GfxRenderingAPIXbox::SetScissor(int x, int y, int width, int height) {
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    // NV2A scissor via D3D clip rect — wired with the draw path.
}
void GfxRenderingAPIXbox::SetDepthTestAndMask(bool depthTest, bool zUpd) {
    IDirect3DDevice8* d = Dev(Device());
    if (!d) {
        return;
    }
    d->SetRenderState(D3DRS_ZENABLE, depthTest ? TRUE : FALSE);
    d->SetRenderState(D3DRS_ZWRITEENABLE, zUpd ? TRUE : FALSE);
}
void GfxRenderingAPIXbox::SetZmodeDecal(bool decal) {
    IDirect3DDevice8* d = Dev(Device());
    if (!d) {
        return;
    }
    d->SetRenderState(D3DRS_ZFUNC, decal ? D3DCMP_LESSEQUAL : D3DCMP_LESS);
}
void GfxRenderingAPIXbox::SetUseAlpha(bool useAlpha) {
    mUseAlpha = useAlpha;
    IDirect3DDevice8* d = Dev(Device());
    if (d) {
        d->SetRenderState(D3DRS_ALPHABLENDENABLE, useAlpha ? TRUE : FALSE);
    }
}
void GfxRenderingAPIXbox::SetCurrentPrimDepth(float depth) {
    mCurrentPrimDepth = depth;
}

// --- Textures ---
uint32_t GfxRenderingAPIXbox::NewTexture() {
    mTextures.push_back(nullptr);
    return (uint32_t)(mTextures.size() - 1);
}
void GfxRenderingAPIXbox::SelectTexture(int tile, uint32_t textureId) {
    if (tile < 0 || tile > 1) {
        return;
    }
    mActiveTile = (uint32_t)tile;
    mBoundTexture[tile] = textureId;
    IDirect3DDevice8* d = Dev(Device());
    if (d && textureId < mTextures.size()) {
        d->SetTexture((DWORD)tile, reinterpret_cast<IDirect3DBaseTexture8*>(mTextures[textureId]));
    }
}
// Box-filter a tightly-packed BGRA8 image (srcW x srcH) down to (srcW/2 x srcH/2), averaging each
// 2x2 block. Used to build mip chains on upload: SoH hands Fast3D a single base level, but tiled
// ground/water planes at grazing angles alias badly (moiré rings + shimmering tile seams) without
// mips. Dimensions are power-of-two here, so they halve cleanly to 1.
static void DownsampleBGRA(const uint8_t* src, uint32_t srcW, uint32_t srcH, uint8_t* dst, uint32_t dstW,
                           uint32_t dstH) {
    for (uint32_t y = 0; y < dstH; ++y) {
        const uint8_t* r0 = src + (size_t)(y * 2) * srcW * 4;
        const uint8_t* r1 = (y * 2 + 1 < srcH) ? r0 + (size_t)srcW * 4 : r0;
        uint8_t* o = dst + (size_t)y * dstW * 4;
        for (uint32_t x = 0; x < dstW; ++x) {
            const uint8_t* a = r0 + (size_t)(x * 2) * 4;
            const uint8_t* b = (x * 2 + 1 < srcW) ? a + 4 : a;
            const uint8_t* c = r1 + (size_t)(x * 2) * 4;
            const uint8_t* e = (x * 2 + 1 < srcW) ? c + 4 : c;
            for (int ch = 0; ch < 4; ++ch) {
                o[ch] = (uint8_t)(((uint32_t)a[ch] + b[ch] + c[ch] + e[ch] + 2) >> 2);
            }
            o += 4;
        }
    }
}

void GfxRenderingAPIXbox::UploadTexture(const uint8_t* rgba32Buf, uint32_t width, uint32_t height) {
    IDirect3DDevice8* d = Dev(Device());
    if (!d || width == 0 || height == 0) {
        return;
    }
    uint32_t texId = mBoundTexture[mActiveTile];
    if (texId == 0 || texId >= mTextures.size()) {
        return;
    }
    if (mTextures[texId]) {
        reinterpret_cast<IDirect3DTexture8*>(mTextures[texId])->Release();
        mTextures[texId] = nullptr;
    }
    // Convert RGBA8888 -> A8R8G8B8 (LE byte order B,G,R,A) into a tightly-packed linear buffer.
    static std::vector<uint8_t> lin;
    lin.resize((size_t)width * height * 4);
    {
        const uint8_t* src = rgba32Buf;
        uint8_t* dst = lin.data();
        const size_t n = (size_t)width * height;
        for (size_t i = 0; i < n; ++i) {
            dst[0] = src[2]; // B
            dst[1] = src[1]; // G
            dst[2] = src[0]; // R
            dst[3] = src[3]; // A
            dst += 4;
            src += 4;
        }
    }

    // The NV2A stores A8R8G8B8 textures *swizzled* (Morton order): uploading linear rows to a
    // swizzled surface scrambles the texels. Swizzle via XGSwizzleRect when the dimensions are
    // power-of-two (a swizzled surface requirement); otherwise fall back to the linear format
    // (D3DFMT_LIN_A8R8G8B8), which stores rows verbatim but only supports clamp addressing.
    auto isPow2 = [](uint32_t v) { return v != 0 && (v & (v - 1)) == 0; };
    const bool swizzled = isPow2(width) && isPow2(height);
    const D3DFORMAT fmt = swizzled ? D3DFMT_A8R8G8B8 : D3DFMT_LIN_A8R8G8B8;

    // Mip chain: swizzled (pow2) textures get a full chain (box-filtered on upload) so distant/
    // grazing-angle tiled surfaces minify to the average colour instead of aliasing into moiré.
    // Linear (non-pow2) textures stay single-level (LIN surfaces don't take swizzled mips).
    uint32_t levels = 1;
    if (swizzled) {
        for (uint32_t m = (width > height ? width : height); m > 1; m >>= 1) {
            levels++;
        }
    }

    IDirect3DTexture8* tex = nullptr;
    if (FAILED(d->CreateTexture(width, height, levels, 0, fmt, D3DPOOL_MANAGED, &tex)) || !tex) {
        return;
    }
    if (!swizzled) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(tex->LockRect(0, &lr, NULL, 0))) {
            for (uint32_t row = 0; row < height; ++row) {
                memcpy((uint8_t*)lr.pBits + row * lr.Pitch, lin.data() + (size_t)row * width * 4, width * 4);
            }
            tex->UnlockRect(0);
        }
    } else {
        // Level 0 straight from the source; each subsequent level box-filtered from the previous.
        static std::vector<uint8_t> cur, next;
        cur.assign(lin.begin(), lin.end());
        uint32_t lw = width, lh = height;
        for (uint32_t lvl = 0; lvl < levels; ++lvl) {
            D3DLOCKED_RECT lr;
            if (SUCCEEDED(tex->LockRect(lvl, &lr, NULL, 0))) {
                XGSwizzleRect(cur.data(), lw * 4, NULL, lr.pBits, lw, lh, NULL, 4);
                tex->UnlockRect(lvl);
            }
            if (lvl + 1 < levels) {
                uint32_t nw = lw > 1 ? lw >> 1 : 1;
                uint32_t nh = lh > 1 ? lh >> 1 : 1;
                next.resize((size_t)nw * nh * 4);
                DownsampleBGRA(cur.data(), lw, lh, next.data(), nw, nh);
                cur.swap(next);
                lw = nw;
                lh = nh;
            }
        }
    }
    mTextures[texId] = tex;
    d->SetTexture((DWORD)mActiveTile, tex);
}
void GfxRenderingAPIXbox::SetSamplerParameters(int sampler, bool linearFilter, uint32_t cms, uint32_t cmt) {
    IDirect3DDevice8* d = Dev(Device());
    if (!d || sampler < 0 || sampler > 1) {
        return;
    }
    bool linear = linearFilter && (mTextureFilter != FILTER_NONE);
    d->SetTextureStageState((DWORD)sampler, D3DTSS_MAGFILTER, linear ? D3DTEXF_LINEAR : D3DTEXF_POINT);
    d->SetTextureStageState((DWORD)sampler, D3DTSS_MINFILTER, linear ? D3DTEXF_LINEAR : D3DTEXF_POINT);
    // Trilinear when filtering linearly (blends mip levels → no visible mip seams on the tiled
    // ground); point mip otherwise. Single-level textures ignore this.
    d->SetTextureStageState((DWORD)sampler, D3DTSS_MIPFILTER, linear ? D3DTEXF_LINEAR : D3DTEXF_POINT);
    auto mapWrap = [](uint32_t cm) -> DWORD {
        // G_TX_ enum: 0 wrap, 1 mirror, 2 clamp (bit flags in Fast3D's cms/cmt)
        if (cm & 0x2) {
            return D3DTADDRESS_CLAMP;
        }
        if (cm & 0x1) {
            return D3DTADDRESS_MIRROR;
        }
        return D3DTADDRESS_WRAP;
    };
    mAddrU[sampler] = (uint32_t)mapWrap(cms);
    mAddrV[sampler] = (uint32_t)mapWrap(cmt);
    d->SetTextureStageState((DWORD)sampler, D3DTSS_ADDRESSU, mAddrU[sampler]);
    d->SetTextureStageState((DWORD)sampler, D3DTSS_ADDRESSV, mAddrV[sampler]);
}
void GfxRenderingAPIXbox::DeleteTexture(uint32_t texId) {
    if (texId < mTextures.size() && mTextures[texId]) {
        reinterpret_cast<IDirect3DTexture8*>(mTextures[texId])->Release();
        mTextures[texId] = nullptr;
    }
}
void GfxRenderingAPIXbox::SetTextureFilter(FilteringMode mode) {
    mTextureFilter = mode;
}
FilteringMode GfxRenderingAPIXbox::GetTextureFilter() {
    return mTextureFilter;
}
void GfxRenderingAPIXbox::SetSrgbMode() {
}
ImTextureID GfxRenderingAPIXbox::GetTextureById(int id) {
    if (id >= 0 && (uint32_t)id < mTextures.size()) {
        return (ImTextureID)mTextures[id];
    }
    return (ImTextureID) nullptr;
}

// --- Shaders (combiner). Phase-1 skeleton: cache by id, decode enough for vertex layout.
//     Full combiner->NV2A register-combiner lowering is the next Phase-1 step. ---
ShaderProgram* GfxRenderingAPIXbox::CreateAndLoadNewShader(uint64_t shaderId0, uint64_t shaderId1) {
    CCFeatures f;
    gfx_cc_get_features(shaderId0, shaderId1, &f);

    ShaderProgram sp;
    sp.shaderId0 = shaderId0;
    sp.shaderId1 = shaderId1;
    sp.numInputs = (uint8_t)f.numInputs;
    sp.usedTextures[0] = f.usedTextures[0];
    sp.usedTextures[1] = f.usedTextures[1];
    sp.usesAlpha = f.opt_alpha;
    sp.usesFog = f.opt_fog;
    sp.inputSize = f.opt_alpha ? 4u : 3u;
    sp.clampS[0] = f.clamp[0][0];
    sp.clampT[0] = f.clamp[0][1];
    sp.clampS[1] = f.clamp[1][0];
    sp.clampT[1] = f.clamp[1][1];

    // Walk the buf_vbo attribute layout exactly as the interpreter builds it:
    // pos(4) + per-tex[ uv(2) + clampS?(1) + clampT?(1) ] + fog(4) + grayscale(4) + inputs*(3|4).
    uint32_t off = 4;
    sp.uv0Off = -1;
    sp.uv1Off = -1;
    for (int i = 0; i < 2; i++) {
        if (f.usedTextures[i]) {
            if (i == 0) {
                sp.uv0Off = (int32_t)off;
            } else {
                sp.uv1Off = (int32_t)off;
            }
            off += 2;
            if (f.clamp[i][0]) {
                off += 1;
            }
            if (f.clamp[i][1]) {
                off += 1;
            }
        }
    }
    if (f.opt_fog) {
        off += 4;
    }
    if (f.opt_grayscale) {
        off += 4;
    }
    sp.input0Off = (f.numInputs > 0) ? (int32_t)off : -1;
    sp.numFloats = off + (uint32_t)f.numInputs * sp.inputSize;

    // Combiner definition for the NV2A pixel-shader lowering (see BuildCombinerPS).
    sp.opt2cyc = f.opt_2cyc;
    sp.textureEdge = f.opt_texture_edge;
    sp.alphaThreshold = f.opt_alpha_threshold;
    for (int cyc = 0; cyc < 2; cyc++) {
        for (int ch = 0; ch < 2; ch++) {
            for (int k = 0; k < 4; k++) {
                sp.cc[cyc][ch][k] = f.c[cyc][ch][k];
            }
            sp.doSingle[cyc][ch] = f.do_single[cyc][ch];
            sp.doMultiply[cyc][ch] = f.do_multiply[cyc][ch];
            sp.doMix[cyc][ch] = f.do_mix[cyc][ch];
        }
    }
    // Colour inputs are consecutive in buf_vbo at input0Off + k*inputSize. Input 1 -> vertex
    // diffuse (V0); inputs 2/3 -> combiner constants C0/C1 (assumed primitive-constant).
    sp.input2Off = (f.numInputs >= 2) ? sp.input0Off + (int32_t)sp.inputSize : -1;
    sp.input3Off = (f.numInputs >= 3) ? sp.input0Off + 2 * (int32_t)sp.inputSize : -1;

    auto key = std::make_pair(shaderId0, shaderId1);
    mShaderCache[key] = sp;
    mCurrentShader = &mShaderCache[key];
    return mCurrentShader;
}
ShaderProgram* GfxRenderingAPIXbox::LookupShader(uint64_t shaderId0, uint64_t shaderId1) {
    auto it = mShaderCache.find(std::make_pair(shaderId0, shaderId1));
    if (it == mShaderCache.end()) {
        return nullptr;
    }
    return &it->second;
}
void GfxRenderingAPIXbox::LoadShader(ShaderProgram* newPrg) {
    mCurrentShader = newPrg;
}
void GfxRenderingAPIXbox::UnloadShader(ShaderProgram* oldPrg) {
    (void)oldPrg;
}
void GfxRenderingAPIXbox::ClearShaderCache() {
    mShaderCache.clear();
    mCurrentShader = nullptr;
}
void GfxRenderingAPIXbox::ShaderGetInfo(ShaderProgram* prg, uint8_t* numInputs, bool usedTextures[2]) {
    if (!prg) {
        *numInputs = 0;
        usedTextures[0] = usedTextures[1] = false;
        return;
    }
    *numInputs = prg->numInputs;
    usedTextures[0] = prg->usedTextures[0];
    usedTextures[1] = prg->usedTextures[1];
}

namespace {
struct XVtxRHW {
    float x, y, z, rhw;
    DWORD color;
    float u, v;   // texcoord set 0 (samples T0)
    float u1, v1; // texcoord set 1 (samples T1)
};
constexpr DWORD kXVtxFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX2;

inline DWORD PackColor(const float* c, uint32_t n) {
    auto clamp8 = [](float f) -> DWORD {
        int v = (int)(f * 255.0f + 0.5f);
        if (v < 0)
            v = 0;
        if (v > 255)
            v = 255;
        return (DWORD)v;
    };
    DWORD r = clamp8(c[0]), g = clamp8(c[1]), b = clamp8(c[2]);
    DWORD a = (n == 4) ? clamp8(c[3]) : 255;
    return (a << 24) | (r << 16) | (g << 8) | b; // A8R8G8B8
}
} // namespace

void GfxRenderingAPIXbox::DrawTriangles(float bufVbo[], size_t bufVboLen, size_t bufVboNumTris) {
    IDirect3DDevice8* d = Dev(Device());
    if (!d || !mCurrentShader || bufVboNumTris == 0) {
        return;
    }
    const ShaderProgram* sp = mCurrentShader;
    const uint32_t stride = sp->numFloats;
    const size_t numVerts = bufVboNumTris * 3;
    if (stride < 4 || numVerts * stride > bufVboLen) {
        return; // layout mismatch guard
    }

    // Shading: lower the N64 colour combiner to an NV2A register-combiner pixel shader (cached
    // per ShaderProgram). Textured draws sample T0 (+ T1); untextured draws still run the combiner
    // (its inputs just don't reference a texel). Falls back to fixed-function if the PS build fails.
    ShaderProgram* spm = mCurrentShader;
    const bool textured = sp->usedTextures[0] && sp->uv0Off >= 0 && mBoundTexture[0] != 0;

    // Alpha test: Fast3D bakes a "discard low-alpha texel" into its shader for cutout modes
    // (texture_edge = leaves/fences/glow discs; alpha_threshold). The register combiner can't
    // discard, so approximate with fixed-function alpha test. Without it these draws (often with
    // blending disabled) paint the whole textured quad opaque instead of just the covered texels.
    if (spm->textureEdge || spm->alphaThreshold) {
        d->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
        d->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
        d->SetRenderState(D3DRS_ALPHAREF, spm->alphaThreshold ? 0x80u : 0x4Du);
    } else {
        d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    }

    if (!spm->psTried) {
        spm->psTried = true;
        spm->psHandle = BuildCombinerPS(d, *spm);
    }
    if (spm->psHandle) {
        d->SetPixelShader(spm->psHandle);
        // Feed combiner constants C0/C1 from inputs 2/3 of the first vertex (primitive-constant
        // PRIM/ENV colours). Read inputSize floats; broadcast alpha=1 for RGB-only inputs.
        auto setConst = [&](DWORD reg, int32_t inOff) {
            float c[4] = { 1.f, 1.f, 1.f, 1.f };
            if (inOff >= 0 && (size_t)inOff + sp->inputSize <= stride) {
                c[0] = bufVbo[inOff];
                c[1] = bufVbo[inOff + 1];
                c[2] = bufVbo[inOff + 2];
                c[3] = (sp->inputSize == 4) ? bufVbo[inOff + 3] : 1.0f;
            }
            d->SetPixelShaderConstant(reg, c, 1);
        };
        if (sp->input2Off >= 0) {
            setConst(0, sp->input2Off);
        }
        if (sp->input3Off >= 0) {
            setConst(1, sp->input3Off);
        }
        if (textured) {
            d->SetTextureStageState(0, D3DTSS_ADDRESSU, sp->clampS[0] ? D3DTADDRESS_CLAMP : mAddrU[0]);
            d->SetTextureStageState(0, D3DTSS_ADDRESSV, sp->clampT[0] ? D3DTADDRESS_CLAMP : mAddrV[0]);
        } else if (sp->usedTextures[0]) {
            // Combiner samples texture 0 but no real texture is bound: use the dummy so the
            // sampler stage has a valid 2D texture (xemu asserts on an unbound sampler).
            d->SetTexture(0, reinterpret_cast<IDirect3DBaseTexture8*>(mDummyTex));
        } else {
            d->SetTexture(0, NULL);
        }
        // Texture 1 (two-texture blends: terrain detail, water). Bind the real T1 if selected,
        // else the dummy, so the stage-1 sampler always has a valid 2D texture.
        if (sp->usedTextures[1]) {
            if (mBoundTexture[1] != 0 && mBoundTexture[1] < mTextures.size() && mTextures[mBoundTexture[1]]) {
                d->SetTexture(1, reinterpret_cast<IDirect3DBaseTexture8*>(mTextures[mBoundTexture[1]]));
                d->SetTextureStageState(1, D3DTSS_ADDRESSU, sp->clampS[1] ? D3DTADDRESS_CLAMP : mAddrU[1]);
                d->SetTextureStageState(1, D3DTSS_ADDRESSV, sp->clampT[1] ? D3DTADDRESS_CLAMP : mAddrV[1]);
            } else {
                d->SetTexture(1, reinterpret_cast<IDirect3DBaseTexture8*>(mDummyTex));
            }
        }
    } else if (textured) {
        // Fallback: fixed-function modulate.
        d->SetPixelShader(0);
        d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        d->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        d->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        d->SetTextureStageState(0, D3DTSS_ADDRESSU, sp->clampS[0] ? D3DTADDRESS_CLAMP : mAddrU[0]);
        d->SetTextureStageState(0, D3DTSS_ADDRESSV, sp->clampT[0] ? D3DTADDRESS_CLAMP : mAddrV[0]);
    } else {
        d->SetPixelShader(0); // fixed-function shade-only
        d->SetTexture(0, NULL);
        d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    }

    // Clip-space vertex (pre perspective divide) with interpolatable attributes. D3D8 does NOT
    // clip pretransformed (XYZRHW) vertices, so triangles crossing the near plane must be clipped
    // here: a vertex with w <= 0 is at/behind the eye and its 1/w projects to a garbage screen
    // position, tearing the triangle apart. Clip each triangle against w >= kWNear first.
    struct ClipV {
        float x, y, z, w;    // clip space
        float r, g, b, a;    // shade colour (linear, interpolatable)
        float u, v;          // tex0 uv
        float u1, v1;        // tex1 uv
    };
    const bool textured1 = sp->usedTextures[1] && sp->uv1Off >= 0;
    // Clip against the near plane. With z_is_from_0_to_1 (D3D depth), NDC z in [0,1] means the
    // near plane is clip-space z == 0, i.e. a vertex is in front of the near plane when z >= 0
    // (and w > 0 there). Clipping at the eye plane (w ~= 0) instead keeps behind-near geometry
    // that stretches to infinity; the near plane is the correct cutoff.
    constexpr float kWNear = 0.00001f; // tiny w guard for the final divide only

    auto readV = [&](size_t idx) -> ClipV {
        const float* v = &bufVbo[idx * stride];
        ClipV c;
        c.x = v[0]; c.y = v[1]; c.z = v[2]; c.w = v[3];
        if (sp->input0Off >= 0) {
            c.r = v[sp->input0Off]; c.g = v[sp->input0Off + 1]; c.b = v[sp->input0Off + 2];
            c.a = (sp->inputSize == 4) ? v[sp->input0Off + 3] : 1.0f;
        } else {
            c.r = c.g = c.b = c.a = 1.0f;
        }
        if (textured) {
            c.u = v[sp->uv0Off]; c.v = v[sp->uv0Off + 1];
        } else {
            c.u = c.v = 0.0f;
        }
        if (textured1) {
            c.u1 = v[sp->uv1Off]; c.v1 = v[sp->uv1Off + 1];
        } else {
            c.u1 = c.v1 = 0.0f;
        }
        return c;
    };
    auto lerpV = [](const ClipV& a, const ClipV& b, float t) -> ClipV {
        ClipV o;
        o.x = a.x + (b.x - a.x) * t; o.y = a.y + (b.y - a.y) * t;
        o.z = a.z + (b.z - a.z) * t; o.w = a.w + (b.w - a.w) * t;
        o.r = a.r + (b.r - a.r) * t; o.g = a.g + (b.g - a.g) * t;
        o.b = a.b + (b.b - a.b) * t; o.a = a.a + (b.a - a.a) * t;
        o.u = a.u + (b.u - a.u) * t; o.v = a.v + (b.v - a.v) * t;
        o.u1 = a.u1 + (b.u1 - a.u1) * t; o.v1 = a.v1 + (b.v1 - a.v1) * t;
        return o;
    };

    static std::vector<XVtxRHW> verts;
    verts.clear();
    verts.reserve(numVerts);

    ClipV poly[8];
    for (size_t t = 0; t < bufVboNumTris; ++t) {
        // Sutherland-Hodgman clip of the triangle against the single plane w >= kWNear.
        ClipV in[4] = { readV(t * 3 + 0), readV(t * 3 + 1), readV(t * 3 + 2) };
        int inN = 3;
        int outN = 0;
        for (int i = 0; i < inN; ++i) {
            const ClipV& cur = in[i];
            const ClipV& prev = in[(i + inN - 1) % inN];
            // Near plane: clip-space z >= 0 (D3D [0,1] depth). Interpolate where z crosses 0.
            float dCur = cur.z;
            float dPrev = prev.z;
            bool curIn = dCur >= 0.0f;
            bool prevIn = dPrev >= 0.0f;
            if (curIn != prevIn) {
                float denom = dPrev - dCur;
                float s = (denom != 0.0f) ? dPrev / denom : 0.0f;
                poly[outN++] = lerpV(prev, cur, s);
            }
            if (curIn) {
                poly[outN++] = cur;
            }
        }
        if (outN < 3) {
            continue; // fully clipped
        }
        // Fan-triangulate the clipped polygon; perspective-divide + screen map each vertex.
        auto emit = [&](const ClipV& c) {
            float rhw = (c.w != 0.0f) ? 1.0f / c.w : 1.0f;
            XVtxRHW o;
            o.x = mVpX + (c.x * rhw * 0.5f + 0.5f) * mVpW;
            o.y = mVpY + (0.5f - 0.5f * c.y * rhw) * mVpH;
            o.z = c.z * rhw;
            o.rhw = rhw;
            float col[4] = { c.r, c.g, c.b, c.a };
            o.color = PackColor(col, 4);
            o.u = c.u;
            o.v = c.v;
            o.u1 = c.u1;
            o.v1 = c.v1;
            verts.push_back(o);
        };
        for (int i = 1; i + 1 < outN; ++i) {
            // Drop any triangle with a vertex at/behind the eye plane (w <= ~0): its 1/w blows the
            // vertex to a garbage screen position and tears the triangle across the frame.
            if (poly[0].w <= kWNear || poly[i].w <= kWNear || poly[i + 1].w <= kWNear) {
                continue;
            }
            emit(poly[0]);
            emit(poly[i]);
            emit(poly[i + 1]);
        }
    }

    if (verts.empty()) {
        return;
    }
    d->SetVertexShader(kXVtxFvf);
    d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)(verts.size() / 3), verts.data(), sizeof(XVtxRHW));
}
} // namespace Fast
