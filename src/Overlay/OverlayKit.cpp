// SPDX-License-Identifier: AGPL-3.0-or-later

#include "Overlay.h"

#if defined(NEW_RENDERER_ENABLED) && defined(USE_GL3)

#include "OverlayKit.h"
#include "OverlayPanels.h"
#include <algorithm>
#include <cmath>
#include <vector>
#include <external/gfx/imgui/imgui_stdlib.h>
#include "../Application.h"
#include "../Renderer/RenderAPI.h"
#include <xuocore/uodata.h>
#include "../Managers/ColorManager.h"
#include "../Managers/UOFileReader.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include <external/stb_image.h>
#pragma clang diagnostic pop

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#define PL_MPEG_IMPLEMENTATION
#include <external/pl_mpeg.h>
#pragma clang diagnostic pop

// backdrop layers: a looping cloud strip drifting behind the castle and mountains

// login fonts are rasterized at 2x (see Overlay.cpp); this maps them to the current layout
static float s_textScale = 0.5f;

void Overlay_SetTextScale(float u)
{
    s_textScale = ImGui::GetIO().FontGlobalScale * u / 2.f;
}

float Overlay_px(const ImFont *font)
{
    return font->FontSize * s_textScale;
}


std::string Overlay_ResourcePath(const char *name)
{
    // next to the client in a build tree, or in Contents/Resources inside EmmexUO.app
    // (the executable's own folder: apps opened from Finder start in '/')
    const char *dirs[] = { "resources", "../Resources" };
    for (const auto dir : dirs)
    {
        const fs_path path = fs_path_join(fs_path_process(), dir, name);
        if (fs_path_exists(path))
        {
            return fs_path_ascii(path);
        }
    }
    return {};
}

// Loads an image, optionally taking its alpha channel from a separate greyscale file
// (keeps the painted foreground a small JPEG instead of a large RGBA PNG).
OverlayImage Overlay_LoadImage(const char *name, const char *alphaName)
{
    OverlayImage image;
    const std::string path = Overlay_ResourcePath(name);
    int w = 0, h = 0, n = 0;
    uint8_t *pixels = path.empty() ? nullptr : stbi_load(path.c_str(), &w, &h, &n, 4);
    if (pixels == nullptr)
    {
        return image;
    }
    if (alphaName != nullptr)
    {
        const std::string alphaPath = Overlay_ResourcePath(alphaName);
        int aw = 0, ah = 0, an = 0;
        uint8_t *alpha = alphaPath.empty() ? nullptr : stbi_load(alphaPath.c_str(), &aw, &ah, &an, 1);
        if (alpha != nullptr && aw == w && ah == h)
        {
            for (int i = 0; i < w * h; i++)
            {
                pixels[i * 4 + 3] = alpha[i];
            }
        }
        if (alpha != nullptr)
        {
            stbi_image_free(alpha);
        }
    }
    image.texture = Render_OverlayCreateImage(w, h, pixels);
    image.width = w;
    image.height = h;
    stbi_image_free(pixels);
    return image;
}

void Overlay_TextCentered(
    ImDrawList *dl, ImFont *font, float y, float x0, float width, ImU32 col, const char *text)
{
    const ImVec2 ts = font->CalcTextSizeA(Overlay_px(font), FLT_MAX, 0.f, text);
    const ImVec2 pos(floorf(x0 + (width - ts.x) / 2), y);
    dl->AddText(font, Overlay_px(font), ImVec2(pos.x + 1, pos.y + 2), IM_COL32(0, 0, 0, 160), text);
    dl->AddText(font, Overlay_px(font), pos, col, text);
}

// Screen changes requested from the login scene run from the main loop, between frames:
// connecting renders a frame of its own, which can't happen inside an overlay frame.
static std::function<void()> s_pending;

void Overlay_Later(std::function<void()> action)
{
    s_pending = std::move(action);
}

void Overlay_LoginRunPending()
{
    if (s_pending)
    {
        auto action = std::move(s_pending);
        s_pending = nullptr;
        action();
    }
}

// Text link: muted until hovered, then gold and underlined.
bool Overlay_Link(const char *label, const char *tooltip)
{
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 size = ImGui::CalcTextSize(label);
    const bool pressed = ImGui::InvisibleButton(label, size);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddText(pos, hovered ? COL_GOLD : COL_MUTED, label);
    if (hovered)
    {
        dl->AddLine(
            ImVec2(pos.x, pos.y + size.y), ImVec2(pos.x + size.x, pos.y + size.y), COL_GOLD);
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (tooltip != nullptr)
        {
            ImGui::SetTooltip("%s", tooltip);
        }
    }
    return pressed;
}

// Draws text wrapped to a width, each line centred; returns the height used.
float Overlay_TextWrappedCentered(ImDrawList *dl, ImFont *font, float x0, float width, float y, ImU32 col, const char *text)
{
    const float size = Overlay_px(font);
    const char *line = text;
    const char *end = text + strlen(text);
    float h = 0;
    while (line < end)
    {
        const char *wrap = font->CalcWordWrapPositionA(size / font->FontSize, line, end, width);
        if (wrap == line)
        {
            wrap++;
        }
        const char *next = wrap;
        while (next < end && (*next == ' ' || *next == '\n'))
        {
            next++;
        }
        const float lw = font->CalcTextSizeA(size, FLT_MAX, 0.f, line, wrap).x;
        dl->AddText(font, size, ImVec2(floorf(x0 + (width - lw) / 2), y + h), col, line, wrap);
        h += size * 1.35f;
        line = next;
    }
    return h;
}

void Overlay_Spinner(ImDrawList *dl, const ImVec2 &c, float r, float thick)
{
    const float t = float(ImGui::GetTime());
    dl->AddCircle(c, r, IM_COL32(255, 255, 255, 30), 48, thick);
    const float a0 = t * 4.2f;
    const float sweep = 1.6f + 0.9f * sinf(t * 2.1f);
    dl->PathArcTo(c, r, a0, a0 + sweep, 32);
    dl->PathStroke(COL_GOLD, false, thick);
}

void Overlay_AlertIcon(ImDrawList *dl, const ImVec2 &c, float r, ImU32 col, float u)
{
    dl->AddCircle(c, r, col, 48, 2.f * u);
    dl->AddLine(ImVec2(c.x, c.y - r * 0.45f), ImVec2(c.x, c.y + r * 0.12f), col, 2.4f * u);
    dl->AddCircleFilled(ImVec2(c.x, c.y + r * 0.42f), 1.8f * u, col, 12);
}

bool Overlay_CardButton(const char *label, const ImVec2 &size, bool primary)
{
    if (primary)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(COL_GOLD));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.91f, 0.77f, 0.50f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.74f, 0.59f, 0.33f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.10f, 0.08f, 0.05f, 1.f));
    }
    else
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.f, 1.f, 1.f, 0.08f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.f, 1.f, 1.f, 0.14f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.f, 1.f, 1.f, 0.20f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(COL_TEXT));
    }
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return pressed;
}


void Overlay_Chevron(ImDrawList *dl, const ImVec2 &c, float s, Chevron dir, ImU32 col, float thick)
{
    ImVec2 p[3];
    switch (dir)
    {
        case Chevron::Right:
            p[0] = ImVec2(c.x - s / 2, c.y - s);
            p[1] = ImVec2(c.x + s / 2, c.y);
            p[2] = ImVec2(c.x - s / 2, c.y + s);
            break;
        case Chevron::Down:
            p[0] = ImVec2(c.x - s, c.y - s / 2);
            p[1] = ImVec2(c.x, c.y + s / 2);
            p[2] = ImVec2(c.x + s, c.y - s / 2);
            break;
        case Chevron::Left:
            p[0] = ImVec2(c.x + s / 2, c.y - s);
            p[1] = ImVec2(c.x - s / 2, c.y);
            p[2] = ImVec2(c.x + s / 2, c.y + s);
            break;
    }
    dl->AddPolyline(p, 3, col, false, thick);
}

// A labelled text field in the server editor; returns true once an edit is finished.
bool Overlay_Field(const char *label, const char *hint, std::string *value, float width, bool focus,
                   ImGuiInputTextFlags flags)
{
    ImFont *small = Overlay_LoginFont(LoginFont::Small);
    ImGui::BeginGroup();
    ImGui::PushFont(small);
    ImGui::TextDisabled("%s", label);
    ImGui::PopFont();
    ImGui::PushItemWidth(width);
    if (focus)
    {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::InputTextWithHint((std::string("##") + label).c_str(), hint, value, flags);
    const bool done = ImGui::IsItemDeactivatedAfterEdit();
    ImGui::PopItemWidth();
    ImGui::EndGroup();
    return done;
}

OverlayImage Overlay_CreateImage(int width, int height, const uint8_t *rgba)
{
    OverlayImage image;
    image.texture = Render_OverlayCreateImage(width, height, rgba);
    if (image.texture != nullptr)
    {
        image.width = width;
        image.height = height;
    }
    return image;
}

void Overlay_FreeImage(OverlayImage &image)
{
    Render_OverlayDestroyImage(image.texture);
    image = {};
}

void Overlay_DrawCover(ImDrawList *dl, const OverlayImage &image, const ImVec2 &min, const ImVec2 &max, ImU32 tint)
{
    if (image.texture == nullptr || image.width == 0 || image.height == 0)
    {
        return;
    }
    const float w = max.x - min.x;
    const float h = max.y - min.y;
    const float imageAspect = float(image.width) / float(image.height);
    const float rectAspect = w / std::max(1.f, h);
    float u = 1.f, v = 1.f; // visible fraction of the image
    if (rectAspect > imageAspect)
    {
        v = imageAspect / rectAspect;
    }
    else
    {
        u = rectAspect / imageAspect;
    }
    dl->AddImage(image.texture, min, max, ImVec2(0.5f - u / 2, 0.5f - v / 2), ImVec2(0.5f + u / 2, 0.5f + v / 2), tint);
}

void Overlay_GlassPanel(ImDrawList *dl, const ImVec2 &min, const ImVec2 &max, float r, float u)
{
    dl->AddRectFilled(ImVec2(min.x, min.y + 6.f * u), ImVec2(max.x, max.y + 10.f * u), IM_COL32(0, 0, 0, 70), r);
    dl->AddRectFilled(min, max, IM_COL32(18, 20, 28, 178), r);
    dl->AddRectFilledMultiColor(
        ImVec2(min.x + r, min.y + 1),
        ImVec2(max.x - r, min.y + 40.f * u),
        IM_COL32(255, 255, 255, 14),
        IM_COL32(255, 255, 255, 14),
        IM_COL32(255, 255, 255, 0),
        IM_COL32(255, 255, 255, 0));
    dl->AddRect(min, max, IM_COL32(255, 255, 255, 38), r, ImDrawCornerFlags_All, 1.f);
}

// ---------------------------------------------------------------------------------------
// Video

struct VideoFrameTarget
{
    std::vector<uint8_t> rgba;
    bool fresh = false;
};

static void on_video_frame(plm_t *, plm_frame_t *frame, void *user)
{
    auto *target = (VideoFrameTarget *)user;
    plm_frame_to_rgba(frame, target->rgba.data(), int(frame->width) * 4);
    target->fresh = true;
}

static VideoFrameTarget s_videoTarget; // one video plays at a time

bool Overlay_VideoOpen(OverlayVideo &video, const char *name)
{
    video.tried = true;
    const std::string path = Overlay_ResourcePath(name);
    if (path.empty())
    {
        return false;
    }
    video.plm = plm_create_with_filename(path.c_str());
    if (video.plm == nullptr)
    {
        return false;
    }
    plm_set_audio_enabled(video.plm, 0);
    plm_set_loop(video.plm, 1);
    video.width = plm_get_width(video.plm);
    video.height = plm_get_height(video.plm);
    s_videoTarget.rgba.assign(size_t(video.width) * video.height * 4, 255);
    s_videoTarget.fresh = false;
    plm_set_video_decode_callback(video.plm, on_video_frame, &s_videoTarget);
    // a few textures in rotation, so a frame isn't rewritten while the GPU still draws it
    for (auto &texture : video.textures)
    {
        texture = Render_OverlayCreateImage(video.width, video.height, s_videoTarget.rgba.data());
    }
    plm_decode(video.plm, 1.0 / 60.0); // first frame
    return true;
}

void *Overlay_VideoUpdate(OverlayVideo &video, double dt)
{
    if (video.plm == nullptr)
    {
        return nullptr;
    }
    // after a stall (window hidden), skip ahead instead of decoding a backlog
    plm_decode(video.plm, std::min(dt, 0.1));
    if (s_videoTarget.fresh)
    {
        s_videoTarget.fresh = false;
        video.current = (video.current + 1) % 3;
        Render_OverlayUpdateImage(
            video.textures[video.current], video.width, video.height, s_videoTarget.rgba.data());
    }
    return video.textures[video.current];
}

void Overlay_VideoClose(OverlayVideo &video)
{
    if (video.plm != nullptr)
    {
        plm_destroy(video.plm);
    }
    for (auto &texture : video.textures)
    {
        Render_OverlayDestroyImage(texture);
    }
    video = {};
}

// ---------------------------------------------------------------------------------------
// UO gumps composed into an image


// Draws UO gumps on top of each other (all from the same origin, as the paperdoll does),
// hued like the game's shader: the red channel picks a step of the hue's colour ramp.
OverlayImage Overlay_ComposeGumps(const std::vector<GumpLayer> &layers, bool crop)
{
    struct Layer
    {
        std::vector<uint16_t> pixels;
        int w, h;
        uint16_t hue;
        bool partial;
    };
    std::vector<Layer> loaded;
    int w = 0, h = 0;
    for (const auto &l : layers)
    {
        if (l.gump <= 0 || l.gump >= MAX_GUMP_DATA_INDEX_COUNT || g_Index.m_Gump[l.gump].Address == 0)
        {
            continue;
        }
        CIndexObject &io = g_Index.m_Gump[l.gump];
        auto pixels = g_UOFileReader.GetGumpPixels(io);
        if (pixels.empty())
        {
            continue;
        }
        loaded.push_back({ std::move(pixels), io.Width, io.Height, l.hue, l.partial });
        w = std::max(w, io.Width);
        h = std::max(h, io.Height);
    }
    if (w == 0 || h == 0)
    {
        return {};
    }
    std::vector<uint32_t> canvas(size_t(w) * h, 0);
    for (auto &l : loaded)
    {
        for (int y = 0; y < l.h; y++)
        {
            for (int x = 0; x < l.w; x++)
            {
                uint16_t c = l.pixels[size_t(y) * l.w + x];
                if (c == 0)
                {
                    continue;
                }
                const uint32_t rgb = l.hue == 0 ? g_ColorManager.Color16To32(c) :
                                     l.partial ? g_ColorManager.GetPartialHueColor(c, l.hue) :
                                                 g_ColorManager.GetColor(c, l.hue);
                canvas[size_t(y) * w + x] = (rgb & 0x00FFFFFF) | 0xFF000000;
            }
        }
    }
    // crop to what's drawn: the paperdoll gumps carry a lot of empty space
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = 0; y < h; y++)
    {
        for (int x = 0; x < w; x++)
        {
            if (canvas[size_t(y) * w + x] != 0)
            {
                x0 = std::min(x0, x);
                x1 = std::max(x1, x);
                y0 = std::min(y0, y);
                y1 = std::max(y1, y);
            }
        }
    }
    if (!crop || x1 < 0)
    {
        return Overlay_CreateImage(w, h, (const uint8_t *)canvas.data());
    }
    const int cw = x1 - x0 + 1;
    const int ch = y1 - y0 + 1;
    std::vector<uint32_t> cropped(size_t(cw) * ch);
    for (int y = 0; y < ch; y++)
    {
        std::copy_n(&canvas[size_t(y + y0) * w + x0], cw, &cropped[size_t(y) * cw]);
    }
    return Overlay_CreateImage(cw, ch, (const uint8_t *)cropped.data());
}

#endif
