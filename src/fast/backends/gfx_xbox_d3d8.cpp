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
    if (!d) {
        return;
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

// --- Framebuffers: Phase 1 renders everything to the backbuffer (fb 0). Render-to-texture
//     framebuffers come later; the calls are accepted so the interpreter runs. ---
int GfxRenderingAPIXbox::CreateFramebuffer() {
    return 0;
}
void GfxRenderingAPIXbox::UpdateFramebufferParameters(int, uint32_t, uint32_t, uint32_t, bool, bool, bool, bool) {
}
void GfxRenderingAPIXbox::StartDrawToFramebuffer(int, float) {
}
void GfxRenderingAPIXbox::CopyFramebuffer(int, int, int, int, int, int, int, int, int, int) {
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
void* GfxRenderingAPIXbox::GetFramebufferTextureId(int) {
    return nullptr;
}
void GfxRenderingAPIXbox::SelectTextureFb(int) {
}

// --- Viewport / scissor / depth / alpha ---
void GfxRenderingAPIXbox::SetViewport(int x, int y, int width, int height) {
    IDirect3DDevice8* d = Dev(Device());
    if (!d) {
        return;
    }
    uint32_t fbw = 640, fbh = 480;
    if (mWindow) {
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

    IDirect3DTexture8* tex = nullptr;
    if (FAILED(d->CreateTexture(width, height, 1, 0, fmt, D3DPOOL_MANAGED, &tex)) || !tex) {
        return;
    }
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(tex->LockRect(0, &lr, NULL, 0))) {
        if (swizzled) {
            XGSwizzleRect(lin.data(), width * 4, NULL, lr.pBits, width, height, NULL, 4);
        } else {
            for (uint32_t row = 0; row < height; ++row) {
                memcpy((uint8_t*)lr.pBits + row * lr.Pitch, lin.data() + (size_t)row * width * 4, width * 4);
            }
        }
        tex->UnlockRect(0);
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
    for (int i = 0; i < 2; i++) {
        if (f.usedTextures[i]) {
            if (i == 0) {
                sp.uv0Off = (int32_t)off;
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
    float u, v;
};
constexpr DWORD kXVtxFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

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
    // DIAG: the interpreter wrote `bufVboLen` floats for `numVerts` vertices, so the true stride
    // is bufVboLen/numVerts. If it disagrees with my computed numFloats, positions for verts past
    // the first are read at the wrong offset -> spikes. Log the offending shader config once.
    {
        size_t actualStride = bufVboLen / numVerts;
        if (actualStride != stride) {
            static int n = 0;
            if (n++ < 24) {
                std::printf("[xbox] STRIDE MISMATCH mine=%u actual=%zu tex=%d,%d fog=%d gray=%d "
                            "alpha=%d numIn=%u id0=%llx id1=%llx\n",
                            stride, actualStride, (int)sp->usedTextures[0], (int)sp->usedTextures[1],
                            (int)sp->usesFog, 0, (int)sp->usesAlpha, (unsigned)sp->numInputs,
                            (unsigned long long)sp->shaderId0, (unsigned long long)sp->shaderId1);
                std::fflush(stdout);
            }
        }
    }

    // Phase-1 shading: fixed-function. Textured -> modulate(texture, shade); else shade only.
    const bool textured = sp->usedTextures[0] && sp->uv0Off >= 0 && mBoundTexture[0] != 0;
    if (textured) {
        d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        d->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        d->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        // N64 defers non-edge clamping to the shader (strips G_TX_CLAMP from cms/cmt, passes
        // clamp bounds per vertex). Approximate with sampler CLAMP on the shader-clamped axes
        // (correct when the clamp bound is at the texture edge, e.g. billboards like the moon)
        // -- otherwise those textures wrap and tile across the surface.
        d->SetTextureStageState(0, D3DTSS_ADDRESSU, sp->clampS[0] ? D3DTADDRESS_CLAMP : mAddrU[0]);
        d->SetTextureStageState(0, D3DTSS_ADDRESSV, sp->clampT[0] ? D3DTADDRESS_CLAMP : mAddrV[0]);
    } else {
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
    };
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
        return c;
    };
    auto lerpV = [](const ClipV& a, const ClipV& b, float t) -> ClipV {
        ClipV o;
        o.x = a.x + (b.x - a.x) * t; o.y = a.y + (b.y - a.y) * t;
        o.z = a.z + (b.z - a.z) * t; o.w = a.w + (b.w - a.w) * t;
        o.r = a.r + (b.r - a.r) * t; o.g = a.g + (b.g - a.g) * t;
        o.b = a.b + (b.b - a.b) * t; o.a = a.a + (b.a - a.a) * t;
        o.u = a.u + (b.u - a.u) * t; o.v = a.v + (b.v - a.v) * t;
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
