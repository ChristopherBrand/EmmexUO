// SPDX-License-Identifier: AGPL-3.0-or-later

// Metal back-end for the new renderer (macOS). Implements the same Render_* / RenderState_* /
// RenderBackend_* surface as RenderInternal.cpp + RenderState.cpp do for OpenGL; RenderDraw.cpp
// and RenderAdd.cpp are shared. Compiled instead of those two when XUO_METAL is set.
//
// Model: immediate mode, like the GL path. State is recorded on RenderState and applied when a
// draw is encoded. One command buffer per frame; a render pass (encoder) is opened lazily on
// the current target and replaced when the target changes or it is cleared.
//
// Orientation: GL framebuffer objects store row 0 at the bottom, and the game samples them that
// way. Offscreen targets therefore get a Y-flipped clip space so their memory layout matches GL;
// window-like targets (the drawable, the smooth-scaling scene) are drawn top-down as usual.

#if defined(XUO_METAL)

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

// AppKit pulls in Apple's OpenGL types; gl3w (via gfx.h) declares GLhandleARB differently.
// Nothing here uses either, so rename gl3w's copy for this file.
#define GLhandleARB xuo_gl3w_GLhandleARB
#include "../Renderer/RenderAPI.h"
#define RENDERER_INTERNAL
#include "../Renderer/RenderInternal.h"
#undef GLhandleARB
#include "../Overlay/Overlay.h"
#include "../Config.h"
#include "../Wisp/WispWindow.h"
#include <SDL_syswm.h>
#include <external/gfx/imgui/imgui.h>
#include <external/gfx/imgui/imgui_impl_metal.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <cmath>
#include <string.h>
#include <unordered_map>
#include <vector>

#define countof(xarray) (sizeof(xarray) / sizeof(xarray[0]))

// ---------------------------------------------------------------------------------------
// Globals shared with RenderDraw.cpp / RenderInternal.h

texture_handle_t _defaultTex = RENDER_TEXTUREHANDLE_INVALID;
texture_handle_t _whiteTex = RENDER_TEXTUREHANDLE_INVALID;
bool g_rendererDebugForceStateReset = false;
uint32_t g_drawVAO = 0;
uint32_t g_drawVBO = 0;
size_t g_vboSize = 0;

// ---------------------------------------------------------------------------------------
// Shaders (Metal port of src/ShaderData.h)

static const char *s_shaderSource = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct VIn
{
    float2 pos [[attribute(0)]];
    float2 uv [[attribute(1)]];
    float4 col [[attribute(2)]];
    float3 normal [[attribute(3)]];
};

struct VOut
{
    float4 pos [[position]];
    float2 uv;
    float4 col;
    float3 normal;
};

struct VUniforms
{
    float4x4 projection;
    float4x4 model;
    float flipY;
};

struct FUniforms
{
    int drawMode;
    int alphaTestEnabled;
    float alphaRef;
    float pad;
    float colors[96];
};

vertex VOut vs_main(VIn in [[stage_in]], constant VUniforms &u [[buffer(1)]])
{
    VOut o;
    float4 p = u.projection * u.model * float4(in.pos, 0.0, 1.0);
    p.y *= u.flipY;
    p.z = (p.z + p.w) * 0.5; // GL clip depth [-w, w] -> Metal [0, w]
    o.pos = p;
    o.uv = in.uv;
    o.col = in.col;
    o.normal = in.normal;
    return o;
}

fragment float4 fs_main(
    VOut in [[stage_in]],
    constant FUniforms &u [[buffer(0)]],
    texture2d<float> tex [[texture(0)]],
    sampler smp [[sampler(0)]])
{
    float4 textureColor = tex.sample(smp, in.uv);
    if (u.alphaTestEnabled != 0 && textureColor.a <= u.alphaRef)
    {
        discard_fragment();
    }
    if (textureColor.a == 0.0)
    {
        return textureColor * in.col;
    }
    const int drawMode = u.drawMode;
    if (drawMode == 6 || drawMode == 7) // SDM_LAND, SDM_LAND_COLORED: lighting
    {
        float3 lightDir = normalize(float3(-1.0, -1.0, 0.5));
        float3 normal = normalize(in.normal);
        float light = max(dot(normal, lightDir) + 0.5, 0.0);
        if (drawMode == 7)
        {
            int index = int(textureColor.r * 31.875) * 3;
            return (float4(u.colors[index], u.colors[index + 1], u.colors[index + 2], textureColor.a) * in.col) * light;
        }
        return (textureColor * in.col) * light;
    }
    // SDM_COLORED = 1 || SDM_PARTIAL_HUE = 2 (only grey pixels)
    if (drawMode == 1 || (drawMode == 2 && textureColor.r == textureColor.g && textureColor.r == textureColor.b))
    {
        int index = int(textureColor.r * 31.875) * 3;
        return float4(u.colors[index], u.colors[index + 1], u.colors[index + 2], textureColor.a) * in.col;
    }
    // SDM_TEXT_COLORED = 4 || SDM_TEXT_COLORED_NO_BLACK = 3
    if (drawMode == 4 || (drawMode == 3 && textureColor.r > 0.04))
    {
        return float4(u.colors[90], u.colors[91], u.colors[92], textureColor.a) * in.col;
    }
    // SDM_SPECTRAL = 10, SDM_SPECIAL_SPECTRAL = 11, SDM_SHADOW = 12
    if (drawMode > 9)
    {
        float red = textureColor.r;
        if (drawMode > 10)
        {
            red = drawMode > 11 ? 0.6 : red * 0.5;
        }
        else
        {
            red *= 1.5;
        }
        return float4(red, red, red, textureColor.a) * in.col;
    }
    return textureColor * in.col;
}

// Full-screen copy used to present the smooth-scaling scene buffer.
struct BlitOut
{
    float4 pos [[position]];
    float2 uv;
};

vertex BlitOut vs_blit(uint vid [[vertex_id]])
{
    const float2 p[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };
    BlitOut o;
    o.pos = float4(p[vid], 0.0, 1.0);
    o.uv = float2((p[vid].x + 1.0) * 0.5, 1.0 - (p[vid].y + 1.0) * 0.5);
    return o;
}

fragment float4 fs_blit(BlitOut in [[stage_in]], texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]])
{
    return tex.sample(smp, in.uv);
}
)MSL";

struct VUniforms
{
    float projection[16];
    float model[16];
    float flipY;
    float pad[3];
};

struct FUniforms
{
    int32_t drawMode;
    int32_t alphaTestEnabled;
    float alphaRef;
    float pad;
    float colors[96];
};

// ---------------------------------------------------------------------------------------
// Device state

struct MetalTexture
{
    id<MTLTexture> texture = nil;
    bool linear = false; // framebuffer textures are sampled with linear filtering, like GL
};

struct MetalFramebuffer
{
    texture_handle_t color = RENDER_TEXTUREHANDLE_INVALID;
};

static const int FRAMES_IN_FLIGHT = 3;
static const size_t VERTEX_BUFFER_SIZE = 8 * 1024 * 1024;

static struct
{
    SDL_Window *window = nullptr;
    bool shutdown = false;

    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    CAMetalLayer *layer = nil;
    NSView *view = nil;
    id<MTLLibrary> library = nil;
    MTLVertexDescriptor *vertexDescriptor = nil;
    id<MTLSamplerState> nearest = nil;
    id<MTLSamplerState> linear = nil;
    id<MTLRenderPipelineState> blitPipeline = nil;

    std::unordered_map<uint32_t, id<MTLRenderPipelineState>> pipelines;
    std::unordered_map<uint64_t, id<MTLDepthStencilState>> depthStencil;

    std::vector<MetalTexture> textures; // handle = index (0 unused)
    std::vector<uint32_t> freeTextures;
    std::vector<MetalFramebuffer> framebuffers; // handle = index (0 unused)
    std::vector<uint32_t> freeFramebuffers;

    // frame
    dispatch_semaphore_t inflight = nullptr;
    id<MTLCommandBuffer> cmd = nil;
    id<CAMetalDrawable> drawable = nil;

    // vertices
    id<MTLBuffer> vertexBuffers[FRAMES_IN_FLIGHT] = {};
    int frameIndex = 0;
    id<MTLBuffer> currentVB = nil;
    size_t vbOffset = 0;
    size_t vbCapacity = 0;

    // pass
    id<MTLRenderCommandEncoder> encoder = nil;
    uint32_t encoderTarget = 0; // framebuffer handle, 0 = default target
    int targetW = 0, targetH = 0;
    bool targetOffscreen = false;
    bool targetHasStencil = false;
    id<MTLRenderPipelineState> boundPipeline = nil;
    id<MTLDepthStencilState> boundDepthStencil = nil;
    id<MTLBuffer> boundVB = nil;
    id<MTLTexture> boundTexture = nil;
    id<MTLSamplerState> boundSampler = nil;
    bool viewportDirty = true;
    bool fragmentDirty = true;

    // GL-style view state (pixels, bottom-left origin) and matrices
    int vpX = 0, vpY = 0, vpW = 1, vpH = 1;
    float projection[16] = {};
    float model[16] = {};
    bool scissorEnabled = false;
    int scX = 0, scY = 0, scW = 0, scH = 0;
    float clearColor[4] = { 0.f, 0.f, 0.f, 1.f };

    // smooth scaling scene buffer
    bool smoothScaling = false;
    id<MTLTexture> sceneColor = nil;
    id<MTLTexture> sceneStencil = nil;
    int sceneW = 0, sceneH = 0;

    // overlay
    MTLRenderPassDescriptor *overlayPass = nil;
} m;

// ---------------------------------------------------------------------------------------
// Helpers

@interface XUOMetalView : NSView
@end

@implementation XUOMetalView
- (BOOL)wantsUpdateLayer
{
    return YES;
}
- (CALayer *)makeBackingLayer
{
    return [CAMetalLayer layer];
}
- (NSView *)hitTest:(NSPoint)point
{
    (void)point;
    return nil; // mouse events go to SDL's view underneath
}
@end

static id<MTLTexture> texture_of(texture_handle_t handle, bool *linear = nullptr)
{
    if (handle == RENDER_TEXTUREHANDLE_INVALID || handle >= m.textures.size())
    {
        return nil;
    }
    if (linear != nullptr)
    {
        *linear = m.textures[handle].linear;
    }
    return m.textures[handle].texture;
}

static texture_handle_t texture_add(id<MTLTexture> tex, bool linear)
{
    uint32_t handle;
    if (!m.freeTextures.empty())
    {
        handle = m.freeTextures.back();
        m.freeTextures.pop_back();
    }
    else
    {
        handle = uint32_t(m.textures.size());
        m.textures.emplace_back();
    }
    m.textures[handle].texture = tex;
    m.textures[handle].linear = linear;
    return handle;
}

static void drawable_size(int *w, int *h)
{
    SDL_GL_GetDrawableSize(m.window, w, h); // reads the view's backing size; no GL involved
}

// Window pixels per logical game pixel.
static float window_pixel_ratio()
{
    int windowW = 0, windowH = 0, drawableW = 0, drawableH = 0;
    SDL_GetWindowSize(m.window, &windowW, &windowH);
    drawable_size(&drawableW, &drawableH);
    const float retina = (windowW > 0 && drawableW > 0) ? float(drawableW) / float(windowW) : 1.0f;
    const float uiScale = Wisp::g_WispWindow != nullptr ? Wisp::g_WispWindow->UIScale : 1.f;
    return retina * uiScale;
}

static bool smooth_active()
{
    if (!m.smoothScaling)
    {
        return false;
    }
    const float r = window_pixel_ratio();
    return r != std::floor(r);
}

static id<MTLTexture> make_stencil(int w, int h)
{
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatStencil8
                                                           width:NSUInteger(std::max(w, 1))
                                                          height:NSUInteger(std::max(h, 1))
                                                       mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget;
    d.storageMode = MTLStorageModePrivate;
    return [m.device newTextureWithDescriptor:d];
}

// The default target is always this persistent scene texture, copied to the drawable at
// present. It keeps the last frame for readback (screenshots happen after present) and is
// drawn at the next whole pixel ratio when smooth scaling, then scaled down linearly.
static void scene_buffer_prepare()
{
    int w = 0, h = 0;
    if (smooth_active())
    {
        int windowW = 0, windowH = 0;
        SDL_GetWindowSize(m.window, &windowW, &windowH);
        const float uiScale = Wisp::g_WispWindow != nullptr ? Wisp::g_WispWindow->UIScale : 1.f;
        const float ratio = std::ceil(window_pixel_ratio());
        w = int(int(windowW / uiScale) * ratio);
        h = int(int(windowH / uiScale) * ratio);
    }
    else
    {
        drawable_size(&w, &h);
    }
    w = std::max(w, 1);
    h = std::max(h, 1);
    if (m.sceneColor != nil && w == m.sceneW && h == m.sceneH)
    {
        return;
    }
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:NSUInteger(w)
                                                          height:NSUInteger(h)
                                                       mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    d.storageMode = MTLStorageModePrivate;
    m.sceneColor = [m.device newTextureWithDescriptor:d];
    m.sceneStencil = make_stencil(w, h);
    m.sceneW = w;
    m.sceneH = h;
}

static void begin_frame()
{
    if (m.cmd != nil)
    {
        return;
    }
    dispatch_semaphore_wait(m.inflight, DISPATCH_TIME_FOREVER);
    m.cmd = [m.queue commandBuffer];
    m.frameIndex = (m.frameIndex + 1) % FRAMES_IN_FLIGHT;
    m.currentVB = m.vertexBuffers[m.frameIndex];
    m.vbCapacity = VERTEX_BUFFER_SIZE;
    m.vbOffset = 0;
}

static void ensure_drawable()
{
    begin_frame();
    if (m.drawable != nil)
    {
        return;
    }
    int w = 0, h = 0;
    drawable_size(&w, &h);
    w = std::max(w, 1);
    h = std::max(h, 1);
    const CGSize size = CGSizeMake(w, h);
    if (!CGSizeEqualToSize(m.layer.drawableSize, size))
    {
        m.layer.drawableSize = size;
    }
    NSWindow *nsw = m.view.window;
    if (nsw != nil && m.layer.contentsScale != nsw.backingScaleFactor)
    {
        m.layer.contentsScale = nsw.backingScaleFactor;
    }
    @autoreleasepool
    {
        m.drawable = [m.layer nextDrawable];
    }
}

static void end_encoder()
{
    if (m.encoder != nil)
    {
        [m.encoder endEncoding];
        m.encoder = nil;
    }
}

// Opens a pass on `target` (framebuffer handle, 0 = default). Color/stencil are cleared when
// asked; the drawable is cleared on its first pass of the frame since its contents are undefined.
static void begin_pass(uint32_t target, bool clearColor, bool clearStencil)
{
    end_encoder();
    begin_frame();

    id<MTLTexture> color = nil;
    id<MTLTexture> stencil = nil;
    bool offscreen = false;
    if (target != 0)
    {
        if (target < m.framebuffers.size())
        {
            color = texture_of(m.framebuffers[target].color);
        }
        offscreen = true;
    }
    else
    {
        scene_buffer_prepare();
        color = m.sceneColor;
        stencil = m.sceneStencil;
    }
    if (color == nil)
    {
        return;
    }

    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = color;
    pass.colorAttachments[0].loadAction = clearColor ? MTLLoadActionClear : MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor =
        MTLClearColorMake(m.clearColor[0], m.clearColor[1], m.clearColor[2], m.clearColor[3]);
    if (stencil != nil)
    {
        pass.stencilAttachment.texture = stencil;
        pass.stencilAttachment.loadAction = clearStencil ? MTLLoadActionClear : MTLLoadActionLoad;
        pass.stencilAttachment.storeAction = MTLStoreActionStore;
        pass.stencilAttachment.clearStencil = 0;
    }

    m.encoder = [m.cmd renderCommandEncoderWithDescriptor:pass];
    m.encoderTarget = target;
    m.targetW = int(color.width);
    m.targetH = int(color.height);
    m.targetOffscreen = offscreen;
    m.targetHasStencil = stencil != nil;
    m.boundPipeline = nil;
    m.boundDepthStencil = nil;
    m.boundVB = nil;
    m.boundTexture = nil;
    m.boundSampler = nil;
    m.viewportDirty = true;
    m.fragmentDirty = true;
}

static uint32_t current_target(const RenderState *state)
{
    return state->framebuffer.handle != RENDER_FRAMEBUFFER_INVALID ? state->framebuffer.handle : 0;
}

static void ensure_encoder(const RenderState *state)
{
    const uint32_t target = current_target(state);
    if (m.encoder != nil && m.encoderTarget == target)
    {
        return;
    }
    begin_pass(target, false, false);
}

// ---------------------------------------------------------------------------------------
// Pipeline / depth-stencil state caches

static MTLBlendFactor blend_factor(BlendFactor f)
{
    static const MTLBlendFactor s_map[] = {
        MTLBlendFactorZero,
        MTLBlendFactorOne,
        MTLBlendFactorOneMinusSourceAlpha,
        MTLBlendFactorOneMinusSourceColor,
        MTLBlendFactorSourceColor,
        MTLBlendFactorSourceAlpha,
        MTLBlendFactorDestinationColor,
    };
    static_assert(countof(s_map) == BlendFactor::BlendFactor_Count, "missing blend factor");
    return f < BlendFactor::BlendFactor_Count ? s_map[f] : MTLBlendFactorOne;
}

static id<MTLRenderPipelineState> pipeline_for(const RenderState *state, bool hasStencil)
{
    const bool blend = state->blend.enabled;
    uint32_t key = (blend ? 1u : 0u) | (uint32_t(state->colorMask & 0xF) << 1) | ((hasStencil ? 1u : 0u) << 5);
    if (blend)
    {
        key |= (uint32_t(state->blend.src) << 8) | (uint32_t(state->blend.dst) << 12) |
               (uint32_t(state->blend.equation) << 16);
    }
    auto it = m.pipelines.find(key);
    if (it != m.pipelines.end())
    {
        return it->second;
    }

    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [m.library newFunctionWithName:@"vs_main"];
    d.fragmentFunction = [m.library newFunctionWithName:@"fs_main"];
    d.vertexDescriptor = m.vertexDescriptor;
    d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    MTLColorWriteMask mask = MTLColorWriteMaskNone;
    if (state->colorMask & ColorMask::ColorMask_Red)
    {
        mask |= MTLColorWriteMaskRed;
    }
    if (state->colorMask & ColorMask::ColorMask_Green)
    {
        mask |= MTLColorWriteMaskGreen;
    }
    if (state->colorMask & ColorMask::ColorMask_Blue)
    {
        mask |= MTLColorWriteMaskBlue;
    }
    if (state->colorMask & ColorMask::ColorMask_Alpha)
    {
        mask |= MTLColorWriteMaskAlpha;
    }
    d.colorAttachments[0].writeMask = mask;
    if (blend)
    {
        // glBlendFunc/glBlendEquation apply to color and alpha alike
        auto a = d.colorAttachments[0];
        a.blendingEnabled = YES;
        a.sourceRGBBlendFactor = a.sourceAlphaBlendFactor = blend_factor(state->blend.src);
        a.destinationRGBBlendFactor = a.destinationAlphaBlendFactor = blend_factor(state->blend.dst);
        const MTLBlendOperation op = state->blend.equation == BlendEquation::BlendEquation_ReverseSubtract ?
                                         MTLBlendOperationReverseSubtract :
                                         MTLBlendOperationAdd;
        a.rgbBlendOperation = a.alphaBlendOperation = op;
    }
    if (hasStencil)
    {
        d.stencilAttachmentPixelFormat = MTLPixelFormatStencil8;
    }
    NSError *err = nil;
    id<MTLRenderPipelineState> p = [m.device newRenderPipelineStateWithDescriptor:d error:&err];
    if (p == nil)
    {
        Error(Renderer, "metal pipeline: %s", err.localizedDescription.UTF8String);
    }
    m.pipelines[key] = p;
    return p;
}

static MTLCompareFunction compare_func(StencilFunc f)
{
    static const MTLCompareFunction s_map[] = {
        MTLCompareFunctionNever,     MTLCompareFunctionAlways,       MTLCompareFunctionLess,
        MTLCompareFunctionLessEqual, MTLCompareFunctionGreater,      MTLCompareFunctionGreaterEqual,
        MTLCompareFunctionEqual,     MTLCompareFunctionNotEqual,
    };
    static_assert(countof(s_map) == StencilFunc::StencilFunc_Count, "missing stencil func");
    return f < StencilFunc::StencilFunc_Count ? s_map[f] : MTLCompareFunctionAlways;
}

static MTLStencilOperation stencil_op(StencilOp o)
{
    static const MTLStencilOperation s_map[] = {
        MTLStencilOperationKeep,          MTLStencilOperationZero,
        MTLStencilOperationReplace,       MTLStencilOperationIncrementClamp,
        MTLStencilOperationIncrementWrap, MTLStencilOperationDecrementClamp,
        MTLStencilOperationDecrementWrap, MTLStencilOperationInvert,
    };
    static_assert(countof(s_map) == StencilOp::StencilOp_Count, "missing stencil op");
    return o < StencilOp::StencilOp_Count ? s_map[o] : MTLStencilOperationKeep;
}

static id<MTLDepthStencilState> depth_stencil_for(const RenderState *state)
{
    const auto &s = state->stencil;
    uint64_t key = 0;
    if (s.enabled)
    {
        key = 1ull | (uint64_t(s.func) << 1) | (uint64_t(s.stencilFail) << 5) |
              (uint64_t(s.depthFail) << 9) | (uint64_t(s.bothFail) << 13) |
              (uint64_t(s.mask & 0xFF) << 17);
    }
    auto it = m.depthStencil.find(key);
    if (it != m.depthStencil.end())
    {
        return it->second;
    }
    MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new];
    if (s.enabled)
    {
        MTLStencilDescriptor *sd = [MTLStencilDescriptor new];
        sd.stencilCompareFunction = compare_func(s.func);
        sd.stencilFailureOperation = stencil_op(s.stencilFail);
        sd.depthFailureOperation = stencil_op(s.depthFail);
        sd.depthStencilPassOperation = stencil_op(s.bothFail); // glStencilOp's 3rd = dppass
        sd.readMask = s.mask & 0xFF;
        sd.writeMask = 0xFF; // glStencilMask default
        d.frontFaceStencil = sd;
        d.backFaceStencil = sd;
    }
    id<MTLDepthStencilState> st = [m.device newDepthStencilStateWithDescriptor:d];
    m.depthStencil[key] = st;
    return st;
}

// ---------------------------------------------------------------------------------------
// Init / shutdown

static id<MTLTexture> make_texture(int w, int h, const void *bgra, bool renderTarget)
{
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:NSUInteger(std::max(w, 1))
                                                          height:NSUInteger(std::max(h, 1))
                                                       mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead | (renderTarget ? MTLTextureUsageRenderTarget : 0);
    d.storageMode = renderTarget ? MTLStorageModePrivate : MTLStorageModeShared;
    id<MTLTexture> t = [m.device newTextureWithDescriptor:d];
    if (bgra != nullptr && !renderTarget)
    {
        [t replaceRegion:MTLRegionMake2D(0, 0, NSUInteger(w), NSUInteger(h))
             mipmapLevel:0
               withBytes:bgra
             bytesPerRow:NSUInteger(w) * 4];
    }
    return t;
}

// 8x8 RGBA byte textures from RenderInternal.cpp, converted to BGRA
static texture_handle_t make_solid_8x8(const uint32_t *rgba, bool linear)
{
    uint32_t bgra[64];
    for (int i = 0; i < 64; ++i)
    {
        const uint32_t v = rgba[i]; // bytes R,G,B,A
        bgra[i] = (v & 0xFF00FF00u) | ((v & 0xFFu) << 16) | ((v >> 16) & 0xFFu);
    }
    return texture_add(make_texture(8, 8, bgra, false), linear);
}

bool Render_Init(SDL_Window *window)
{
    atexit([] { m.shutdown = true; });
    m.window = window;
    m.device = MTLCreateSystemDefaultDevice();
    if (m.device == nil)
    {
        Error(Renderer, "no Metal device");
        return false;
    }
    m.queue = [m.device newCommandQueue];
    m.inflight = dispatch_semaphore_create(FRAMES_IN_FLIGHT);

    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window, &info))
    {
        Error(Renderer, "SDL_GetWindowWMInfo: %s", SDL_GetError());
        return false;
    }
    NSWindow *nsw = info.info.cocoa.window;
    NSView *content = nsw.contentView;
    m.view = [[XUOMetalView alloc] initWithFrame:content.bounds];
    m.view.wantsLayer = YES;
    m.view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [content addSubview:m.view];
    m.layer = (CAMetalLayer *)m.view.layer;
    m.layer.device = m.device;
    m.layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    m.layer.framebufferOnly = NO; // screenshots read the drawable back
    m.layer.contentsScale = nsw.backingScaleFactor;
    m.layer.displaySyncEnabled = NO; // paced by the game loop, like the GL swap interval 0
    m.layer.maximumDrawableCount = 3;

    NSError *err = nil;
    m.library = [m.device newLibraryWithSource:@(s_shaderSource) options:nil error:&err];
    if (m.library == nil)
    {
        Error(Renderer, "metal shader: %s", err.localizedDescription.UTF8String);
        return false;
    }

    m.vertexDescriptor = [MTLVertexDescriptor vertexDescriptor];
    m.vertexDescriptor.attributes[0].format = MTLVertexFormatFloat2;
    m.vertexDescriptor.attributes[0].offset = OFFSETOF(GenericVertex, pos);
    m.vertexDescriptor.attributes[1].format = MTLVertexFormatFloat2;
    m.vertexDescriptor.attributes[1].offset = OFFSETOF(GenericVertex, uv);
    m.vertexDescriptor.attributes[2].format = MTLVertexFormatUChar4Normalized;
    m.vertexDescriptor.attributes[2].offset = OFFSETOF(GenericVertex, col);
    m.vertexDescriptor.attributes[3].format = MTLVertexFormatFloat3;
    m.vertexDescriptor.attributes[3].offset = OFFSETOF(GenericVertex, normal);
    for (int i = 0; i < 4; ++i)
    {
        m.vertexDescriptor.attributes[i].bufferIndex = 0;
    }
    m.vertexDescriptor.layouts[0].stride = sizeof(GenericVertex);

    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
    m.nearest = [m.device newSamplerStateWithDescriptor:sd];
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
    m.linear = [m.device newSamplerStateWithDescriptor:sd];

    MTLRenderPipelineDescriptor *bd = [MTLRenderPipelineDescriptor new];
    bd.vertexFunction = [m.library newFunctionWithName:@"vs_blit"];
    bd.fragmentFunction = [m.library newFunctionWithName:@"fs_blit"];
    bd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    m.blitPipeline = [m.device newRenderPipelineStateWithDescriptor:bd error:&err];

    for (int i = 0; i < FRAMES_IN_FLIGHT; ++i)
    {
        m.vertexBuffers[i] = [m.device newBufferWithLength:VERTEX_BUFFER_SIZE
                                                   options:MTLResourceStorageModeShared |
                                                           MTLResourceCPUCacheModeWriteCombined];
    }
    g_vboSize = VERTEX_BUFFER_SIZE;

    m.textures.emplace_back(); // handle 0 unused
    m.framebuffers.emplace_back();

    // clang-format off
    static const uint32_t missing[64] = {
        0x00000000, 0xffff00ff, 0x00000000, 0xffff00ff, 0x00000000, 0xffff00ff, 0x00000000, 0xffff00ff,
        0xffff00ff, 0x00000000, 0xffff00ff, 0x00000000, 0xffff00ff, 0x00000000, 0xffff00ff, 0x00000000,
        0x00000000, 0xffff00ff, 0x00000000, 0xffff00ff, 0x00000000, 0xffff00ff, 0x00000000, 0xffff00ff,
        0xffff00ff, 0x00000000, 0xffff00ff, 0x00000000, 0xffff00ff, 0x00000000, 0xffff00ff, 0x00000000,
        0x00000000, 0xffff00ff, 0x00000000, 0xffff00ff, 0x00000000, 0xffff00ff, 0x00000000, 0xffff00ff,
        0xffff00ff, 0x00000000, 0xffff00ff, 0x00000000, 0xffff00ff, 0x00000000, 0xffff00ff, 0x00000000,
        0x00000000, 0xffff00ff, 0x00000000, 0xffff00ff, 0x00000000, 0xffff00ff, 0x00000000, 0xffff00ff,
        0xffff00ff, 0x00000000, 0xffff00ff, 0x00000000, 0xffff00ff, 0x00000000, 0xffff00ff, 0x00000000,
    };
    // clang-format on
    static uint32_t white[64];
    for (auto &v : white)
    {
        v = 0xffffffffu;
    }
    _defaultTex = make_solid_8x8(missing, true); // GL used LINEAR for it
    _whiteTex = make_solid_8x8(white, false);

    glm::mat4 identity(1.0f);
    memcpy(m.model, glm::value_ptr(identity), sizeof(m.model));
    memcpy(m.projection, glm::value_ptr(identity), sizeof(m.projection));

    Info(Renderer, "Graphics Successfully Initialized");
    Info(Renderer, "    Metal device: %s", m.device.name.UTF8String);

    Overlay_Init(window, nullptr);
    Render_SetSmoothScaling(g_Config.SmoothScaling);
    return true;
}

void Render_Shutdown()
{
    if (m.cmd != nil)
    {
        end_encoder();
        [m.cmd commit];
        [m.cmd waitUntilCompleted];
        m.cmd = nil;
    }
    Overlay_Shutdown();
    m.shutdown = true;
    m.pipelines.clear();
    m.depthStencil.clear();
    m.textures.clear();
    m.framebuffers.clear();
}

bool Render_InitVertexBuffers()
{
    return true;
}

void Render_CleanupVertexBuffers()
{
}

// ---------------------------------------------------------------------------------------
// Present

void Render_SetSmoothScaling(bool smooth)
{
    m.smoothScaling = smooth;
}

void Render_SwapBuffers()
{
    end_encoder();
    ensure_drawable();
    if (m.drawable == nil)
    {
        return;
    }
    {
        // scene -> drawable (linear when smooth scaling shrinks a larger scene)
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = m.drawable.texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        id<MTLRenderCommandEncoder> enc = [m.cmd renderCommandEncoderWithDescriptor:pass];
        if (m.sceneColor != nil)
        {
            [enc setRenderPipelineState:m.blitPipeline];
            [enc setFragmentTexture:m.sceneColor atIndex:0];
            [enc setFragmentSamplerState:(smooth_active() ? m.linear : m.nearest) atIndex:0];
            [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        }
        [enc endEncoding];
    }

    MTLRenderPassDescriptor *overlay = [MTLRenderPassDescriptor renderPassDescriptor];
    overlay.colorAttachments[0].texture = m.drawable.texture;
    overlay.colorAttachments[0].loadAction = MTLLoadActionLoad;
    overlay.colorAttachments[0].storeAction = MTLStoreActionStore;
    m.overlayPass = overlay;
    Overlay_Render();
    m.overlayPass = nil;

    [m.cmd presentDrawable:m.drawable];
    dispatch_semaphore_t sem = m.inflight;
    [m.cmd addCompletedHandler:^(id<MTLCommandBuffer>) {
      dispatch_semaphore_signal(sem);
    }];
    [m.cmd commit];
    m.cmd = nil;
    m.drawable = nil;
}

// ---------------------------------------------------------------------------------------
// Overlay (ImGui Metal back-end)

bool Render_OverlayInit()
{
    return ImGui_ImplMetal_Init(m.device);
}

void Render_OverlayShutdown()
{
    ImGui_ImplMetal_Shutdown();
}

void Render_OverlayRebuildFonts()
{
    ImGui_ImplMetal_DestroyFontsTexture();
    ImGui_ImplMetal_CreateFontsTexture(m.device);
}

void Render_OverlayNewFrame()
{
    if (m.overlayPass != nil)
    {
        ImGui_ImplMetal_NewFrame(m.overlayPass);
    }
}

void Render_OverlayDraw(ImDrawData *drawData)
{
    if (m.overlayPass == nil || m.cmd == nil)
    {
        return;
    }
    id<MTLRenderCommandEncoder> enc = [m.cmd renderCommandEncoderWithDescriptor:m.overlayPass];
    ImGui_ImplMetal_RenderDrawData(drawData, m.cmd, enc);
    [enc endEncoding];
}

void Render_SetVSync(bool enabled)
{
    if (m.layer != nil)
    {
        m.layer.displaySyncEnabled = enabled;
    }
}

void *Render_OverlayCreateImage(int width, int height, const uint8_t *rgba)
{
    MTLTextureDescriptor *desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:NSUInteger(width)
                                                          height:NSUInteger(height)
                                                       mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> tex = [m.device newTextureWithDescriptor:desc];
    if (tex == nil)
    {
        return nullptr;
    }
    [tex replaceRegion:MTLRegionMake2D(0, 0, NSUInteger(width), NSUInteger(height))
           mipmapLevel:0
             withBytes:rgba
           bytesPerRow:NSUInteger(width) * 4];
    // ImGui only holds a raw pointer; the reference is released in Render_OverlayDestroyImage
    return (__bridge_retained void *)tex;
}

void Render_OverlayUpdateImage(void *image, int width, int height, const uint8_t *rgba)
{
    if (image != nullptr)
    {
        id<MTLTexture> tex = (__bridge id<MTLTexture>)image;
        [tex replaceRegion:MTLRegionMake2D(0, 0, NSUInteger(width), NSUInteger(height))
               mipmapLevel:0
                 withBytes:rgba
               bytesPerRow:NSUInteger(width) * 4];
    }
}

void Render_OverlayDestroyImage(void *image)
{
    if (image != nullptr)
    {
        id<MTLTexture> tex = (__bridge_transfer id<MTLTexture>)image;
        tex = nil;
    }
}

// ---------------------------------------------------------------------------------------
// Resources

bool Render_CreateShaderPipeline(const char *, const char *, ShaderPipeline *pipeline)
{
    // The GL "extra" programs (CGLShader) are never bound; Metal draws everything with fs_main.
    *pipeline = ShaderPipeline{};
    pipeline->program = 1;
    return true;
}

bool Render_DestroyShaderPipeline(ShaderPipeline *pipeline)
{
    *pipeline = ShaderPipeline{};
    return true;
}

uint32_t Render_GetUniformId(ShaderPipeline *pipeline, const char *)
{
    if (pipeline->uniformCount + 1 > countof(pipeline->uniforms))
    {
        return RENDER_SHADERUNIFORMID_INVALID;
    }
    const auto id = pipeline->uniformCount++;
    pipeline->uniforms[id].location = id;
    return id;
}

uint32_t Render_ShaderUniformTypeToSize(ShaderUniformType type)
{
    return type == ShaderUniformType::ShaderUniformType_Int1 ? uint32_t(sizeof(uint32_t)) : 0xffffffff;
}

texture_handle_t Render_CreateTexture2D(
    uint32_t width, uint32_t height, TextureGPUFormat gpuFormat, void *pixels, TextureFormat pixelsFormat)
{
    (void)gpuFormat;
    if (m.shutdown || m.device == nil || width == 0 || height == 0)
    {
        return RENDER_TEXTUREHANDLE_INVALID;
    }
    std::vector<uint32_t> bgra(size_t(width) * height);
    if (pixels != nullptr)
    {
        const size_t n = bgra.size();
        if (pixelsFormat == TextureFormat::TextureFormat_Unsigned_A1_BGR5)
        {
            // GL_BGRA + GL_UNSIGNED_SHORT_1_5_5_5_REV: B bits 0-4, G 5-9, R 10-14, A bit 15
            const uint16_t *src = (const uint16_t *)pixels;
            for (size_t i = 0; i < n; ++i)
            {
                const uint32_t v = src[i];
                uint32_t b = v & 0x1F, g = (v >> 5) & 0x1F, r = (v >> 10) & 0x1F;
                b = (b << 3) | (b >> 2);
                g = (g << 3) | (g >> 2);
                r = (r << 3) | (r >> 2);
                const uint32_t a = (v & 0x8000) ? 0xFF : 0;
                bgra[i] = b | (g << 8) | (r << 16) | (a << 24);
            }
        }
        else
        {
            // GL_BGRA + GL_UNSIGNED_INT_8_8_8_8: B in bits 24-31, G 16-23, R 8-15, A 0-7
            const uint32_t *src = (const uint32_t *)pixels;
            for (size_t i = 0; i < n; ++i)
            {
                const uint32_t v = src[i];
                const uint32_t a = v & 0xFF, r = (v >> 8) & 0xFF, g = (v >> 16) & 0xFF, b = v >> 24;
                bgra[i] = b | (g << 8) | (r << 16) | (a << 24);
            }
        }
    }
    return texture_add(make_texture(int(width), int(height), bgra.data(), false), false);
}

bool Render_DestroyTexture(texture_handle_t texture)
{
    if (m.shutdown || texture == RENDER_TEXTUREHANDLE_INVALID || texture >= m.textures.size() ||
        m.textures[texture].texture == nil)
    {
        return false;
    }
    m.textures[texture] = MetalTexture{}; // in-flight command buffers keep their own reference
    m.freeTextures.push_back(texture);
    return true;
}

frame_buffer_t Render_CreateFrameBuffer(uint32_t width, uint32_t height)
{
    if (m.shutdown || m.device == nil)
    {
        return frame_buffer_t{};
    }
    frame_buffer_t fb;
    fb.texture = texture_add(make_texture(int(width), int(height), nullptr, true), true);
    uint32_t handle;
    if (!m.freeFramebuffers.empty())
    {
        handle = m.freeFramebuffers.back();
        m.freeFramebuffers.pop_back();
    }
    else
    {
        handle = uint32_t(m.framebuffers.size());
        m.framebuffers.emplace_back();
    }
    m.framebuffers[handle].color = fb.texture;
    fb.handle = handle;
    return fb;
}

bool Render_DestroyFrameBuffer(frame_buffer_t fb)
{
    if (m.shutdown || fb.handle == RENDER_FRAMEBUFFER_INVALID || fb.handle >= m.framebuffers.size())
    {
        return false;
    }
    if (m.encoder != nil && m.encoderTarget == fb.handle)
    {
        end_encoder();
    }
    Render_DestroyTexture(fb.texture);
    m.framebuffers[fb.handle] = MetalFramebuffer{};
    m.freeFramebuffers.push_back(fb.handle);
    return true;
}

// ---------------------------------------------------------------------------------------
// Command list plumbing (same as RenderInternal.cpp)

bool Render_AppendCmd(RenderCmdList *cmdList, const void *cmd, uint32_t cmdSize)
{
    if (cmdList->immediateMode)
    {
        return true;
    }
    if (cmdList->remainingSize >= cmdSize)
    {
        memcpy(cmdList->data + cmdList->size - cmdList->remainingSize, cmd, cmdSize);
        cmdList->remainingSize -= cmdSize;
        return true;
    }
    Error(Renderer, "%s render cmd list capacity reached. skipping render cmd %p", __FUNCTION__, cmd);
    return false;
}

bool Render_AppendCmdType(RenderCmdList *cmdList, RenderCommandType type, const void *cmd, uint32_t cmdSize)
{
    if (cmdList->immediateMode)
    {
        return true;
    }
    if (cmdList->remainingSize >= sizeof(type) + cmdSize)
    {
        auto ptr = cmdList->data + cmdList->size - cmdList->remainingSize;
        *ptr = (uint8_t)type;
        ptr++;
        memcpy(ptr, cmd, cmdSize);
        cmdList->remainingSize -= sizeof(type) + cmdSize;
        return true;
    }
    Error(Renderer, "%s render cmd list capacity reached. skipping render cmd %p", __FUNCTION__, cmd);
    return false;
}

RenderState Render_DefaultState()
{
    return RenderState();
}

void Render_ResetCmdList(RenderCmdList *cmdList, RenderState state)
{
    cmdList->remainingSize = cmdList->size;
    cmdList->state = state;
}

uint32_t Render_DefaultFramebuffer()
{
    return 0;
}

float Render_PixelRatio(const RenderState *state)
{
    if (state != nullptr && state->framebuffer.handle != RENDER_FRAMEBUFFER_INVALID)
    {
        return 1.0f;
    }
    const float r = window_pixel_ratio();
    return smooth_active() ? std::ceil(r) : r;
}

int RenderDraw_StreamVertices(const GenericVertex *vertices, int count)
{
    begin_frame();
    const size_t bytes = size_t(count) * sizeof(GenericVertex);
    if (m.vbOffset + bytes > m.vbCapacity)
    {
        // a very busy frame: continue in a fresh buffer (kept alive by the command buffer)
        m.vbCapacity = std::max(VERTEX_BUFFER_SIZE, bytes);
        m.currentVB = [m.device newBufferWithLength:m.vbCapacity
                                            options:MTLResourceStorageModeShared |
                                                    MTLResourceCPUCacheModeWriteCombined];
        m.vbOffset = 0;
    }
    memcpy((uint8_t *)m.currentVB.contents + m.vbOffset, vertices, bytes);
    const int first = int(m.vbOffset / sizeof(GenericVertex));
    m.vbOffset += bytes;
    return first;
}

// ---------------------------------------------------------------------------------------
// Encoding

static void apply_viewport_scissor()
{
    // GL rectangles are bottom-left based. Offscreen targets keep GL's memory layout (flipped
    // clip space), so their rows map 1:1; window-like targets are flipped to top-left.
    MTLViewport vp;
    vp.originX = m.vpX;
    vp.width = m.vpW;
    vp.height = m.vpH;
    vp.originY = m.targetOffscreen ? m.vpY : m.targetH - (m.vpY + m.vpH);
    vp.znear = 0.0;
    vp.zfar = 1.0;
    [m.encoder setViewport:vp];

    int x = 0, y = 0, w = m.targetW, h = m.targetH;
    if (m.scissorEnabled)
    {
        x = m.scX;
        w = m.scW;
        h = m.scH;
        y = m.targetOffscreen ? m.scY : m.targetH - (m.scY + m.scH);
    }
    // Metal requires the rectangle to lie inside the target
    const int x0 = std::max(0, std::min(x, m.targetW));
    const int y0 = std::max(0, std::min(y, m.targetH));
    const int x1 = std::max(x0, std::min(x + w, m.targetW));
    const int y1 = std::max(y0, std::min(y + h, m.targetH));
    MTLScissorRect r = { NSUInteger(x0), NSUInteger(y0), NSUInteger(x1 - x0), NSUInteger(y1 - y0) };
    [m.encoder setScissorRect:r];
    m.viewportDirty = false;
}

void RenderBackend_Draw(RenderPrimitive primitive, int first, int count, RenderState *state)
{
    ensure_encoder(state);
    if (m.encoder == nil)
    {
        return;
    }
    if (m.scissorEnabled && (m.scW <= 0 || m.scH <= 0))
    {
        return; // GL draws nothing through an empty scissor box
    }

    id<MTLRenderPipelineState> p = pipeline_for(state, m.targetHasStencil);
    if (p != m.boundPipeline)
    {
        [m.encoder setRenderPipelineState:p];
        m.boundPipeline = p;
    }
    if (m.targetHasStencil)
    {
        id<MTLDepthStencilState> ds = depth_stencil_for(state);
        if (ds != m.boundDepthStencil)
        {
            [m.encoder setDepthStencilState:ds];
            m.boundDepthStencil = ds;
        }
        [m.encoder setStencilReferenceValue:state->stencil.ref];
    }
    if (m.viewportDirty)
    {
        apply_viewport_scissor();
    }
    if (m.currentVB != m.boundVB)
    {
        [m.encoder setVertexBuffer:m.currentVB offset:0 atIndex:0];
        m.boundVB = m.currentVB;
    }

    VUniforms vu;
    memcpy(vu.projection, m.projection, sizeof(vu.projection));
    memcpy(vu.model, m.model, sizeof(vu.model));
    vu.flipY = m.targetOffscreen ? -1.f : 1.f;
    [m.encoder setVertexBytes:&vu length:sizeof(vu) atIndex:1];

    FUniforms fu;
    fu.drawMode = state->currentDrawMode;
    fu.alphaTestEnabled = state->alphaTest.enabled ? 1 : 0;
    fu.alphaRef = state->alphaTest.alphaRef;
    fu.pad = 0.f;
    memcpy(fu.colors, state->currentColors, sizeof(fu.colors));
    [m.encoder setFragmentBytes:&fu length:sizeof(fu) atIndex:0];

    bool linear = false;
    id<MTLTexture> tex = texture_of(state->texture.texture, &linear);
    if (tex == nil)
    {
        tex = texture_of(_defaultTex, &linear);
    }
    if (tex != m.boundTexture)
    {
        [m.encoder setFragmentTexture:tex atIndex:0];
        m.boundTexture = tex;
    }
    id<MTLSamplerState> smp = linear ? m.linear : m.nearest;
    if (smp != m.boundSampler)
    {
        [m.encoder setFragmentSamplerState:smp atIndex:0];
        m.boundSampler = smp;
    }

    static const MTLPrimitiveType s_types[] = {
        MTLPrimitiveTypeTriangleStrip, MTLPrimitiveTypeTriangle, MTLPrimitiveTypeLine
    };
    [m.encoder drawPrimitives:s_types[int(primitive)] vertexStart:NSUInteger(first) vertexCount:NSUInteger(count)];
}

void RenderBackend_Clear(uint8_t clearMask, RenderState *state)
{
    const uint32_t target = current_target(state);
    begin_pass(
        target,
        (clearMask & ClearRT::ClearRT_Color) != 0,
        (clearMask & ClearRT::ClearRT_Stencil) != 0);
}

bool RenderBackend_ReadPixels(const GetFrameBufferPixelsCmd &cmd, RenderState *state)
{
    // finish what has been drawn so far, then copy the rectangle out
    ensure_encoder(state);
    if (m.encoder == nil)
    {
        return false;
    }
    const int targetH = m.targetH;
    const int targetW = m.targetW;
    const bool offscreen = m.targetOffscreen;
    id<MTLTexture> src = nil;
    const uint32_t target = current_target(state);
    if (target != 0)
    {
        src = texture_of(m.framebuffers[target].color);
    }
    else
    {
        src = m.sceneColor;
    }
    end_encoder();
    if (src == nil)
    {
        return false;
    }

    const float r = Render_PixelRatio(state);
    const int bottom = int(cmd.window_height) - (cmd.y + int(cmd.height));
    const int w = int(cmd.width), h = int(cmd.height);
    const int x = std::max(0, std::min(int(cmd.x * r), targetW - 1));
    const int glY = int(bottom * r);
    int y = offscreen ? glY : targetH - (glY + h); // top row of the rectangle in memory
    y = std::max(0, std::min(y, targetH - 1));
    const int cw = std::min(w, targetW - x);
    const int ch = std::min(h, targetH - y);
    if (cw <= 0 || ch <= 0)
    {
        return false;
    }

    id<MTLBuffer> out = [m.device newBufferWithLength:NSUInteger(cw * ch * 4) options:MTLResourceStorageModeShared];
    id<MTLBlitCommandEncoder> blit = [m.cmd blitCommandEncoder];
    [blit copyFromTexture:src
                 sourceSlice:0
                 sourceLevel:0
                sourceOrigin:MTLOriginMake(NSUInteger(x), NSUInteger(y), 0)
                  sourceSize:MTLSizeMake(NSUInteger(cw), NSUInteger(ch), 1)
                    toBuffer:out
           destinationOffset:0
      destinationBytesPerRow:NSUInteger(cw * 4)
    destinationBytesPerImage:NSUInteger(cw * ch * 4)];
    [blit endEncoding];

    // run everything so far and wait; the frame continues in a new command buffer
    dispatch_semaphore_t sem = m.inflight;
    [m.cmd addCompletedHandler:^(id<MTLCommandBuffer>) {
      dispatch_semaphore_signal(sem);
    }];
    [m.cmd commit];
    [m.cmd waitUntilCompleted];
    m.cmd = nil;
    begin_frame();

    // GL returns RGBA rows bottom-up; window targets are stored top-down
    const uint8_t *px = (const uint8_t *)out.contents;
    uint8_t *dst = (uint8_t *)cmd.data;
    memset(dst, 0, size_t(w) * h * 4);
    for (int row = 0; row < ch; ++row)
    {
        const int srcRow = offscreen ? row : (ch - 1 - row);
        const uint8_t *s = px + size_t(srcRow) * cw * 4;
        uint8_t *d = dst + size_t(row) * w * 4;
        for (int col = 0; col < cw; ++col)
        {
            d[col * 4 + 0] = s[col * 4 + 2];
            d[col * 4 + 1] = s[col * 4 + 1];
            d[col * 4 + 2] = s[col * 4 + 0];
            d[col * 4 + 3] = s[col * 4 + 3];
        }
    }
    return true;
}

void RenderBackend_PushDebugGroup(const char *label)
{
    (void)label;
}

void RenderBackend_PopDebugGroup()
{
}

// ---------------------------------------------------------------------------------------
// View / projection

static void set_view(int px, int py, int pw, int ph, const glm::mat4 &projection)
{
    if (px != m.vpX || py != m.vpY || pw != m.vpW || ph != m.vpH)
    {
        m.vpX = px;
        m.vpY = py;
        m.vpW = pw;
        m.vpH = ph;
        m.viewportDirty = true;
    }
    memcpy(m.projection, glm::value_ptr(projection), sizeof(m.projection));
}

bool HACKRender_SetViewParams(const SetViewParamsCmd &cmd)
{
    const int bottom = cmd.window_height - (cmd.scene_y + cmd.scene_height);
    const float r = Render_PixelRatio(nullptr);
    const auto projection = glm::ortho(
        float(cmd.scene_x),
        float(cmd.scene_x + cmd.scene_width),
        float(cmd.scene_y + cmd.scene_height),
        float(cmd.scene_y),
        float(cmd.camera_nearZ),
        float(cmd.camera_farZ));
    set_view(
        int(cmd.scene_x * r), int(bottom * r), int(cmd.scene_width * r), int(cmd.scene_height * r), projection);
    return true;
}

bool HACKRender_GetFrameBuffer(RenderCmdList *cmdList, frame_buffer_t *currFb)
{
    *currFb = cmdList->state.framebuffer;
    return true;
}

static bool f32eq(float a, float b)
{
    uint32_t ba, bb;
    memcpy(&ba, &a, sizeof(ba));
    memcpy(&bb, &b, sizeof(bb));
    return ba == bb;
}

bool RenderState_SetViewParams(
    RenderState *state,
    int scene_x,
    int scene_y,
    int scene_width,
    int scene_height,
    int window_width,
    int window_height,
    int camera_nearZ,
    int camera_farZ,
    float scene_scale,
    bool proj_flipped_y,
    bool forced)
{
    (void)window_width;
    const int right = scene_x + scene_width;
    const int bottom = window_height - (scene_y + scene_height);
    static float s_lastPixelRatio = 0.0f;
    const float pixelRatio = Render_PixelRatio(state);
    const bool ratioChanged = !f32eq(pixelRatio, s_lastPixelRatio);
    s_lastPixelRatio = pixelRatio;
    if (!(forced || ratioChanged || state->viewport.left != scene_x || state->viewport.right != right ||
          state->viewport.bottom != bottom || state->viewport.top != scene_y ||
          state->viewport.nearZ != camera_nearZ || state->viewport.farZ != camera_farZ ||
          !f32eq(state->viewport.scale, scene_scale) || state->viewport.proj_flipped_y != proj_flipped_y))
    {
        return false;
    }
    state->viewport.left = scene_x;
    state->viewport.right = right;
    state->viewport.bottom = bottom;
    state->viewport.top = scene_y;
    state->viewport.nearZ = camera_nearZ;
    state->viewport.farZ = camera_farZ;
    state->viewport.scale = scene_scale;
    state->viewport.proj_flipped_y = proj_flipped_y;

    const float scaledRight = right * scene_scale;
    const float scaledLeft = scene_x * scene_scale - (scaledRight - right);
    const float scaledBottom = (scene_y + scene_height) * scene_scale;
    const float scaledTop = scene_y * scene_scale - (scaledBottom - (scene_y + scene_height));
    const auto projection = glm::ortho(
        scaledLeft,
        scaledRight,
        proj_flipped_y ? scaledTop : scaledBottom,
        proj_flipped_y ? scaledBottom : scaledTop,
        float(camera_nearZ),
        float(camera_farZ));
    set_view(
        int(scene_x * pixelRatio),
        int(bottom * pixelRatio),
        int(scene_width * pixelRatio),
        int(scene_height * pixelRatio),
        projection);
    return true;
}

// ---------------------------------------------------------------------------------------
// RenderState: recorded here, applied when a draw is encoded

bool RenderState_SetupCachedState(RenderState *state, const glm::mat4 &modelMatrix)
{
    (void)state;
    memcpy(m.model, glm::value_ptr(modelMatrix), sizeof(m.model));
    return true;
}

void RenderState_ResetAllStates(RenderState *)
{
}

bool RenderState_FlushState(RenderState *state)
{
    m.viewportDirty = true;
    glm::mat4 identity(1.0f);
    memcpy(m.model, glm::value_ptr(identity), sizeof(m.model));
    RenderState_SetClearColor(state, state->clearColor, true);
    RenderState_SetScissor(
        state,
        state->scissor.enabled,
        state->scissor.x,
        state->scissor.y,
        state->scissor.width,
        state->scissor.height,
        true);
    RenderState_SetDrawMode(state, 0, true);
    return true;
}

bool RenderState_SetAlphaTest(RenderState *state, bool enabled, AlphaTestFunc func, float ref, bool forced)
{
    bool changed = forced || state->alphaTest.enabled != enabled;
    state->alphaTest.enabled = enabled;
    if (enabled && func != AlphaTestFunc::AlphaTestFunc_Invalid &&
        (forced || state->alphaTest.func != func || !f32eq(state->alphaTest.alphaRef, ref)))
    {
        state->alphaTest.func = func;
        state->alphaTest.alphaRef = ref;
        changed = true;
    }
    return changed;
}

bool RenderState_SetBlend(
    RenderState *state, bool enabled, BlendFactor src, BlendFactor dst, BlendEquation equation, bool forced)
{
    if (forced || state->blend.enabled != enabled || state->blend.src != src || state->blend.dst != dst ||
        state->blend.equation != equation)
    {
        state->blend.enabled = enabled;
        if (enabled)
        {
            state->blend.src = src;
            state->blend.dst = dst;
            state->blend.equation = equation;
        }
        return true;
    }
    return false;
}

bool RenderState_SetBlendEnabled(RenderState *state, bool enabled, bool forced)
{
    if (forced || state->blend.enabled != enabled)
    {
        state->blend.enabled = enabled;
        return true;
    }
    return false;
}

bool RenderState_SetDepth(RenderState *state, bool enabled, DepthFunc func, bool forced)
{
    // no depth buffer: depth testing is never used for drawing in this client
    if (forced || state->depth.enabled != enabled || state->depth.func != func)
    {
        state->depth.enabled = enabled;
        state->depth.func = func;
        return true;
    }
    return false;
}

bool RenderState_SetDepthEnabled(RenderState *state, bool enabled, bool forced)
{
    if (forced || state->depth.enabled != enabled)
    {
        state->depth.enabled = enabled;
        return true;
    }
    return false;
}

bool RenderState_SetStencilEnabled(RenderState *state, bool enabled, bool forced)
{
    if (forced || state->stencil.enabled != enabled)
    {
        state->stencil.enabled = enabled;
        return true;
    }
    return false;
}

bool RenderState_SetStencil(
    RenderState *state,
    bool enabled,
    StencilFunc func,
    uint32_t ref,
    uint32_t mask,
    StencilOp stencilFail,
    StencilOp depthFail,
    StencilOp bothFail,
    bool forced)
{
    (void)forced;
    state->stencil.enabled = enabled;
    state->stencil.func = func;
    state->stencil.stencilFail = stencilFail;
    state->stencil.depthFail = depthFail;
    state->stencil.bothFail = bothFail;
    state->stencil.ref = ref;
    state->stencil.mask = mask;
    return true;
}

bool RenderState_SetColorMask(RenderState *state, ColorMask mask, bool forced)
{
    if (forced || state->colorMask != mask)
    {
        state->colorMask = mask;
        return true;
    }
    return false;
}

bool RenderState_SetDrawMode(RenderState *state, int drawMode, bool forced)
{
    if (forced || state->currentDrawMode != drawMode)
    {
        state->currentDrawMode = drawMode;
        return true;
    }
    return false;
}

bool RenderState_SetColor(RenderState *state, float4 color, bool forced)
{
    if (forced || state->color != color)
    {
        state->color = color;
        return true;
    }
    return false;
}

bool RenderState_SetClearColor(RenderState *state, float4 color, bool forced)
{
    if (forced || state->clearColor != color)
    {
        state->clearColor = color;
        memcpy(m.clearColor, color.rgba, sizeof(m.clearColor));
        return true;
    }
    return false;
}

bool RenderState_SetColorPalette(RenderState *state, const float *colors, bool forced)
{
    (void)forced;
    if (memcmp(state->currentColors, colors, sizeof(state->currentColors)) != 0)
    {
        memcpy(state->currentColors, colors, sizeof(state->currentColors));
        return true;
    }
    return false;
}

bool RenderState_SetShaderUniform(RenderState *, uint32_t, const void *, ShaderUniformType, bool)
{
    return false; // only used by the never-bound CGLShader programs
}

bool RenderState_SetShaderLargeUniform(RenderState *, uint32_t, void *, uint32_t, ShaderUniformType, bool)
{
    return true;
}

bool RenderState_SetShaderPipeline(RenderState *state, ShaderPipeline *pipeline, bool forced)
{
    (void)forced;
    state->pipeline = *pipeline;
    return true;
}

bool RenderState_DisableShaderPipeline(RenderState *state, bool forced)
{
    (void)forced;
    state->pipeline = ShaderPipeline{};
    return true;
}

bool RenderState_SetTexture(RenderState *state, TextureType type, texture_handle_t texture, bool forced)
{
    if (forced || state->texture.texture != texture || state->texture.type != type)
    {
        state->texture.texture = texture;
        state->texture.type = type;
        return true;
    }
    return false;
}

bool RenderState_SetFrameBuffer(RenderState *state, frame_buffer_t fb, bool forced)
{
    if (forced || state->framebuffer.handle != fb.handle || state->framebuffer.texture != fb.texture)
    {
        state->framebuffer = fb;
        return true; // the pass switches on the next draw
    }
    return false;
}

bool RenderState_SetModelViewTranslation(RenderState *state, float3 pos, bool forced)
{
    (void)forced;
    state->modelTranslation.rgb[0] += pos[0];
    state->modelTranslation.rgb[1] += pos[1];
    state->modelTranslation.rgb[2] += pos[2];
    return true;
}

bool RenderState_SetScissor(RenderState *state, bool enabled, int x, int y, uint32_t width, uint32_t height, bool forced)
{
    bool changed = forced || state->scissor.enabled != enabled;
    state->scissor.enabled = enabled;
    if (forced || state->scissor.x != x || state->scissor.y != y || state->scissor.width != width ||
        state->scissor.height != height)
    {
        state->scissor.x = x;
        state->scissor.y = y;
        state->scissor.width = width;
        state->scissor.height = height;
        changed = true;
    }
    const float r = Render_PixelRatio(state);
    m.scissorEnabled = enabled;
    m.scX = int(x * r);
    m.scY = int(y * r);
    m.scW = int(width * r);
    m.scH = int(height * r);
    m.viewportDirty = true;
    return changed;
}

#endif // XUO_METAL
