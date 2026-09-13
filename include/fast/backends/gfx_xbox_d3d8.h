#pragma once

// Original-Xbox Direct3D8 (NV2A) rendering backend for Fast3D, built on RXDK libd3d8.
// Implements Fast::GfxRenderingAPI. Device is owned by GfxWindowBackendXbox and borrowed
// here. D3D8 types are kept opaque (void*) so this header pulls no <d3d8.h> into the tree.

#include "fast/backends/gfx_rendering_api.h"
#include <stdint.h>
#include <unordered_map>
#include <map>
#include <utility>
#include <vector>

namespace Fast {

class GfxWindowBackendXbox;

// Backend-private shader handle. Fast3D holds these opaquely and queries them via
// ShaderGetInfo. Decoded combiner features drive the vertex layout and stage setup.
struct ShaderProgram {
    uint64_t shaderId0 = 0;
    uint64_t shaderId1 = 0;
    uint8_t numInputs = 0;
    bool usedTextures[2] = { false, false };
    bool usesAlpha = false;
    bool usesFog = false;
    // Derived buf_vbo layout (floats). See gfx_cc_get_features / the GL backend attrib walk.
    uint32_t numFloats = 4;   // per-vertex stride in floats
    int32_t uv0Off = -1;      // float offset of tex0 UV, or -1
    int32_t uv1Off = -1;      // float offset of tex1 UV, or -1
    int32_t input0Off = -1;   // float offset of the first colour input, or -1
    uint32_t inputSize = 4;   // 3 (RGB) or 4 (RGBA) per input
    // Per-texture shader-clamp flags (S,T). Fast3D defers non-edge clamping to the shader (it
    // strips G_TX_CLAMP from cms/cmt and passes clamp bounds per vertex). The fixed-function
    // path can't clamp per-pixel, so approximate it with sampler CLAMP on these axes at draw.
    bool clampS[2] = { false, false };
    bool clampT[2] = { false, false };

    // N64 colour-combiner definition (from CCFeatures), lowered to an NV2A register-combiner
    // pixel shader. c[cycle][channel(0=rgb,1=alpha)][A,B,C,D] holds SHADER_* input constants; the
    // do_* flags pick the per-cycle form (single=D, multiply=A*C, mix=(A-B)*C+B, general=(A-B)*C+D).
    int cc[2][2][4] = {};
    bool doSingle[2][2] = {};
    bool doMultiply[2][2] = {};
    bool doMix[2][2] = {};
    bool opt2cyc = false;
    // Alpha-test (Fast3D bakes these into the shader as a discard; the fixed-function combiner
    // path can't, so approximate with D3D alpha test). texture_edge = discard near-zero alpha
    // (cutout edges: leaves/fences/the moon disc); alpha_threshold = discard below a threshold.
    bool textureEdge = false;
    bool alphaThreshold = false;
    // buf_vbo float offsets of combiner colour inputs 2/3 (SHADER_INPUT_2/3), or -1. Input 1 is
    // the per-vertex shade (input0Off, fed as vertex diffuse -> V0); inputs 2/3 are treated as
    // primitive-constant (PRIM/ENV) and fed as combiner constants C0/C1 from the first vertex.
    int32_t input2Off = -1;
    int32_t input3Off = -1;
    // Cached NV2A pixel-shader handle for this combiner (0 = not built; built lazily at draw).
    uint32_t psHandle = 0;
    bool psTried = false;
};

class GfxRenderingAPIXbox : public GfxRenderingAPI {
  public:
    explicit GfxRenderingAPIXbox(GfxWindowBackendXbox* window) : mWindow(window) {
    }
    ~GfxRenderingAPIXbox() override = default;

    const char* GetName() override;
    int GetMaxTextureSize() override;
    GfxClipParameters GetClipParameters() override;
    void UnloadShader(ShaderProgram* oldPrg) override;
    void LoadShader(ShaderProgram* newPrg) override;
    void ClearShaderCache() override;
    ShaderProgram* CreateAndLoadNewShader(uint64_t shaderId0, uint64_t shaderId1) override;
    ShaderProgram* LookupShader(uint64_t shaderId0, uint64_t shaderId1) override;
    void ShaderGetInfo(ShaderProgram* prg, uint8_t* numInputs, bool usedTextures[2]) override;
    uint32_t NewTexture() override;
    void SelectTexture(int tile, uint32_t textureId) override;
    void UploadTexture(const uint8_t* rgba32Buf, uint32_t width, uint32_t height) override;
    void SetSamplerParameters(int sampler, bool linearFilter, uint32_t cms, uint32_t cmt) override;
    void SetDepthTestAndMask(bool depthTest, bool zUpd) override;
    void SetZmodeDecal(bool decal) override;
    void SetViewport(int x, int y, int width, int height) override;
    void SetScissor(int x, int y, int width, int height) override;
    void SetUseAlpha(bool useAlpha) override;
    void DrawTriangles(float bufVbo[], size_t bufVboLen, size_t bufVboNumTris) override;
    void Init() override;
    void OnResize() override;
    void StartFrame() override;
    void EndFrame() override;
    void FinishRender() override;
    int CreateFramebuffer() override;
    void UpdateFramebufferParameters(int fbId, uint32_t width, uint32_t height, uint32_t msaaLevel, bool openglInvertY,
                                     bool renderTarget, bool hasDepthBuffer, bool canExtractDepth) override;
    void StartDrawToFramebuffer(int fbId, float noiseScale) override;
    void CopyFramebuffer(int fbDstId, int fbSrcId, int srcX0, int srcY0, int srcX1, int srcY1, int dstX0, int dstY0,
                         int dstX1, int dstY1) override;
    void ClearFramebuffer(bool color, bool depth) override;
    void ClearDepthRegion(int x, int y, int w, int h) override;
    void ReadFramebufferToCPU(int fbId, uint32_t width, uint32_t height, uint16_t* rgba16Buf) override;
    void ResolveMSAAColorBuffer(int fbIdTarget, int fbIdSrc) override;
    std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff>
    GetPixelDepth(int fbId, const std::set<std::pair<float, float>>& coordinates) override;
    void* GetFramebufferTextureId(int fbId) override;
    void SelectTextureFb(int fbId) override;
    void DeleteTexture(uint32_t texId) override;
    void SetTextureFilter(FilteringMode mode) override;
    FilteringMode GetTextureFilter() override;
    void SetSrgbMode() override;
    ImTextureID GetTextureById(int id) override;
    void SetCurrentPrimDepth(float depth) override;

  private:
    void* Device() const; // IDirect3DDevice8* (cast in .cpp)

    GfxWindowBackendXbox* mWindow = nullptr;
    uint32_t mFrameCount = 0;
    FilteringMode mTextureFilter = FILTER_LINEAR;

    // Shader cache keyed by the 128-bit combiner id.
    // Keyed by (shaderId0, shaderId1): the interpreter creates a distinct ShaderProgram per
    // combiner+clamp(tm) config and stores the raw pointer (comb->prg[tm]). Keying by id0 alone
    // would let a second config for the same id0 overwrite the first, corrupting the stride the
    // interpreter reads back (spikes). std::map keeps element addresses stable across inserts.
    std::map<std::pair<uint64_t, uint64_t>, ShaderProgram> mShaderCache;
    ShaderProgram* mCurrentShader = nullptr;

    // Texture pool: texId (1-based) -> IDirect3DTexture8* (void*). Slot 0 unused.
    std::vector<void*> mTextures;
    uint32_t mBoundTexture[2] = { 0, 0 }; // per-tile bound texId
    uint32_t mActiveTile = 0;

    // Render-to-texture framebuffers. Index 0 is the window backbuffer (its color/depth
    // surfaces are borrowed from the device in Init; colorTex is null — the backbuffer can't
    // be sampled as a texture). Indices >= 1 are real RTTs: a D3DUSAGE_RENDERTARGET texture
    // plus its surface-level-0 render surface and an optional depth-stencil surface. The
    // interpreter draws the game's auxiliary effects (Link in the pause menu, lens of truth,
    // the MSAA/scale game buffer, ...) into these and then samples them back via SelectTextureFb.
    struct XboxFramebuffer {
        void* colorTex = nullptr;  // IDirect3DTexture8*  (null for fb 0)
        void* colorSurf = nullptr; // IDirect3DSurface8*  (render target)
        void* depthSurf = nullptr; // IDirect3DSurface8*  (depth-stencil, may be null)
        uint32_t width = 0;
        uint32_t height = 0;
        bool hasDepth = false;
        bool isWindow = false; // fb 0: surfaces are borrowed, never released
        bool invertY = false;  // openglInvertY flag (unused on D3D top-left, tracked for parity)
    };
    std::vector<XboxFramebuffer> mFramebuffers; // [0] = backbuffer, created in Init
    int mCurrentFb = 0;                         // fb currently bound as render target
    // Sentinel mBoundTexture value meaning "an fb color texture is bound" (not an mTextures
    // slot). DrawTriangles only tests mBoundTexture[0] != 0 and does not index mTextures in the
    // textured path, so any non-zero, out-of-range value marks the draw as textured safely.
    static const uint32_t kFbBoundSentinel = 0xF0000000u;

    // The N64 colour combiner ((A-B)*C+D per cycle) is lowered to an NV2A register-combiner pixel
    // shader (D3DPIXELSHADERDEF) and cached per ShaderProgram; fixed-function MODULATE can't
    // express PRIM/ENV/texel-on-texel/2-cycle blends (wrong colour + over-opaque effect sprites).
    // 1x1 white 2D texture bound to a combiner sampler stage that lacks a real texture, so xemu's
    // pixel-shader path never sees an unbound sampler (it asserts on texture dimensions otherwise).
    void* mDummyTex = nullptr; // IDirect3DTexture8*

    bool mUseAlpha = false;
    // Base sampler address mode resolved by SetSamplerParameters (wrap/mirror/clamp), per sampler.
    // 1 == D3DTADDRESS_WRAP. DrawTriangles ORs in shader-clamp (CLAMP) for the current shader.
    uint32_t mAddrU[2] = { 1, 1 };
    uint32_t mAddrV[2] = { 1, 1 };
    // Current D3D viewport (top-left origin, as submitted to the device) for the CPU
    // clip-space -> screen transform used by the XYZRHW draw path.
    float mVpX = 0, mVpY = 0, mVpW = 640, mVpH = 480;
};
} // namespace Fast
