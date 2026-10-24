// SPDX-License-Identifier: AGPL-3.0-or-later

// Shared look and building blocks for the full-window overlay screens (login, character
// creation): text scaling, colours, images and video, buttons and small drawn icons.

#pragma once

#include <functional>
#include <string>
#include <external/gfx/imgui/imgui.h>

static const ImU32 COL_TEXT = IM_COL32(236, 230, 218, 255);
static const ImU32 COL_MUTED = IM_COL32(236, 230, 218, 150);
static const ImU32 COL_GOLD = IM_COL32(214, 176, 104, 255);

// The overlay screens' fonts are rasterized at 2x; u is the layout unit (1 at 1280x800).
void Overlay_SetTextScale(float u);
float Overlay_px(const ImFont *font);

// Absolute path of a bundled resource (resources/<name>), or empty if missing.
std::string Overlay_ResourcePath(const char *name);

struct OverlayImage
{
    void *texture = nullptr;
    int width = 0;
    int height = 0;
};
// Loads a bundled image; the alpha channel can come from a separate greyscale file.
OverlayImage Overlay_LoadImage(const char *name, const char *alphaName = nullptr);
OverlayImage Overlay_CreateImage(int width, int height, const uint8_t *rgba);
void Overlay_FreeImage(OverlayImage &image);
// Draws an image scaled to cover the rectangle (cropping the overflow).
void Overlay_DrawCover(ImDrawList *dl, const OverlayImage &image, const ImVec2 &min, const ImVec2 &max, ImU32 tint = IM_COL32_WHITE);

// A looping MPEG-1 video (pl_mpeg) shown as an overlay image.
struct OverlayVideo
{
    struct plm_t *plm = nullptr;
    void *textures[3] = {};
    int current = 0;
    int width = 0;
    int height = 0;
    bool tried = false;
};
bool Overlay_VideoOpen(OverlayVideo &video, const char *name);
// Advances playback by dt seconds; returns the texture of the latest frame (or null).
void *Overlay_VideoUpdate(OverlayVideo &video, double dt);
void Overlay_VideoClose(OverlayVideo &video);

// Screen changes asked for by an overlay screen run from the main loop between frames
// (some, like connecting, render a frame of their own).
void Overlay_Later(std::function<void()> action);

void Overlay_TextCentered(ImDrawList *dl, ImFont *font, float y, float x0, float width, ImU32 col, const char *text);
// Wraps text to a width with each line centred; returns the height used.
float Overlay_TextWrappedCentered(ImDrawList *dl, ImFont *font, float x0, float width, float y, ImU32 col, const char *text);
void Overlay_Spinner(ImDrawList *dl, const ImVec2 &c, float r, float thick);
void Overlay_AlertIcon(ImDrawList *dl, const ImVec2 &c, float r, ImU32 col, float u);
// Gold (primary) or translucent (secondary) button.
bool Overlay_CardButton(const char *label, const ImVec2 &size, bool primary);
// Text link: muted until hovered, then gold and underlined.
bool Overlay_Link(const char *label, const char *tooltip);
// A labelled text field; returns true once an edit is finished.
bool Overlay_Field(const char *label, const char *hint, std::string *value, float width, bool focus = false,
                   ImGuiInputTextFlags flags = 0);
// Frosted glass panel: translucent fill, lighter top edge, hairline border, soft shadow.
void Overlay_GlassPanel(ImDrawList *dl, const ImVec2 &min, const ImVec2 &max, float rounding, float u);

enum class Chevron
{
    Right,
    Down,
    Left,
};
void Overlay_Chevron(ImDrawList *dl, const ImVec2 &c, float s, Chevron dir, ImU32 col, float thick);
