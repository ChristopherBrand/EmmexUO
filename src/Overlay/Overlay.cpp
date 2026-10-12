// SPDX-License-Identifier: AGPL-3.0-or-later

#include "Overlay.h"

#if defined(NEW_RENDERER_ENABLED) && defined(USE_GL3)

#include <algorithm>
#include <deque>
#include <string>
#include <SDL.h>
#include <external/gfx/gfx.h>
#include <external/gfx/imgui/imgui.h>
#include <external/gfx/imgui/imgui_impl_sdl.h>
#include <external/gfx/imgui/imgui_impl_opengl3.h>
#include <xuocore/enumlist.h>
#include "OverlayPanels.h"
#include "../Application.h"
#include "../Config.h"
#include "../GameWindow.h"
#include "../Globals.h"
#include "../Managers/ColorManager.h"
#include "../Managers/ConfigManager.h"

static SDL_Window *s_window = nullptr;
static bool s_initialized = false;
static bool s_visible[4] = {};
static std::string s_iniPath;
static int s_fontScale = 0; // UI scale the fonts and style were built for
static float s_fontRetina = 0.f;

bool &Overlay_Visible(OverlayWindow w)
{
    return s_visible[int(w)];
}

// Journal, macros and vitals only make sense in the world; settings work anywhere.
static bool shown(OverlayWindow w)
{
    return s_visible[int(w)] && (w == OverlayWindow::Settings || g_GameState >= GS_GAME);
}

static bool any_visible()
{
    return shown(OverlayWindow::Journal) || shown(OverlayWindow::Settings) ||
           shown(OverlayWindow::Macros) || shown(OverlayWindow::Target);
}

static float retina_factor()
{
    int w = 0, h = 0, dw = 0, dh = 0;
    SDL_GetWindowSize(s_window, &w, &h);
    SDL_GL_GetDrawableSize(s_window, &dw, &dh);
    return w > 0 ? float(dw) / float(w) : 1.f;
}

static void apply_style(float scale)
{
    ImGuiStyle &style = ImGui::GetStyle();
    style = ImGuiStyle();
    ImGui::StyleColorsDark(&style);
    style.WindowRounding = 6.f;
    style.FrameRounding = 4.f;
    style.GrabRounding = 4.f;
    style.ScrollbarRounding = 6.f;
    style.WindowBorderSize = 1.f;
    style.Colors[ImGuiCol_WindowBg].w = 0.92f;
    style.ScaleAllSizes(scale);
}

// Fonts are rasterized at the real pixel size (UI scale x Retina) so text stays crisp.
static void rebuild_fonts_if_needed()
{
    const int scale = std::max(1, Wisp::g_WispWindow != nullptr ? Wisp::g_WispWindow->UIScale : 1);
    const float retina = retina_factor();
    if (scale == s_fontScale && retina == s_fontRetina)
    {
        return;
    }
    s_fontScale = scale;
    s_fontRetina = retina;

    ImGuiIO &io = ImGui::GetIO();
    io.Fonts->Clear();
    const float px = 14.f * float(scale) * retina;
    static const char *candidates[] = {
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/System/Library/Fonts/Supplemental/Verdana.ttf",
    };
    ImFont *font = nullptr;
    for (const auto path : candidates)
    {
        FILE *f = fopen(path, "rb");
        if (f == nullptr)
        {
            continue;
        }
        fclose(f);
        // Latin + Latin-1/Latin Extended-A covers UO's common speech text
        static const ImWchar ranges[] = { 0x0020, 0x017F, 0x2010, 0x2027, 0 };
        font = io.Fonts->AddFontFromFileTTF(path, px, nullptr, ranges);
        if (font != nullptr)
        {
            break;
        }
    }
    if (font == nullptr)
    {
        ImFontConfig cfg;
        cfg.SizePixels = 13.f * float(scale) * retina;
        io.Fonts->AddFontDefault(&cfg);
    }
    io.FontGlobalScale = 1.f / retina;
    ImGui_ImplOpenGL3_DestroyFontsTexture();
    ImGui_ImplOpenGL3_CreateFontsTexture();
    apply_style(float(scale));
}

void Overlay_Init(SDL_Window *window, void *glContext)
{
    if (s_initialized)
    {
        return;
    }
    s_window = window;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    s_iniPath = fs_path_ascii(g_App.FilePath("overlay.ini"));
    io.IniFilename = s_iniPath.c_str();
    // The game hides the OS cursor and draws its own; don't let the back-end fight it.
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.ConfigWindowsMoveFromTitleBarOnly = true;

    ImGui_ImplSDL2_InitForOpenGL(window, glContext);
    ImGui_ImplOpenGL3_Init("#version " GL_SHADER_VERSION);
    s_initialized = true;
    Overlay_LoadState();
}

void Overlay_Shutdown()
{
    if (!s_initialized)
    {
        return;
    }
    Overlay_SaveState();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    s_initialized = false;
}

void Overlay_Toggle(OverlayWindow w)
{
    s_visible[int(w)] = !s_visible[int(w)];
}

static bool handle_shortcut(const SDL_Event &e)
{
    if (e.type != SDL_KEYDOWN || (e.key.keysym.mod & KMOD_GUI) == 0 || e.key.repeat != 0)
    {
        return false;
    }
    switch (e.key.keysym.sym)
    {
        case SDLK_COMMA: // the standard macOS Settings shortcut
            Overlay_Toggle(OverlayWindow::Settings);
            return true;
        case SDLK_j:
            if (g_GameState >= GS_GAME)
            {
                Overlay_Toggle(OverlayWindow::Journal);
                return true;
            }
            break;
        case SDLK_k:
            if (g_GameState >= GS_GAME)
            {
                Overlay_Toggle(OverlayWindow::Macros);
                return true;
            }
            break;
        case SDLK_i:
            if (g_GameState >= GS_GAME)
            {
                Overlay_Toggle(OverlayWindow::Target);
                return true;
            }
            break;
        default:
            break;
    }
    return false;
}

bool Overlay_ProcessEvent(const SDL_Event &e)
{
    if (!s_initialized)
    {
        return false;
    }
    if (Overlay_Visible(OverlayWindow::Macros) && Overlay_MacroCaptureEvent(e))
    {
        return true;
    }
    if (handle_shortcut(e))
    {
        return true;
    }
    ImGui_ImplSDL2_ProcessEvent(&e);
    if (!any_visible())
    {
        return false;
    }

    // A press that started in the game finishes in the game, even over a window.
    static uint32_t s_gameButtons = 0;
    const ImGuiIO &io = ImGui::GetIO();
    switch (e.type)
    {
        case SDL_MOUSEMOTION:
        case SDL_MOUSEWHEEL:
            return io.WantCaptureMouse && s_gameButtons == 0;
        case SDL_MOUSEBUTTONDOWN:
            if (io.WantCaptureMouse)
            {
                return true;
            }
            s_gameButtons |= 1u << e.button.button;
            return false;
        case SDL_MOUSEBUTTONUP:
            if ((s_gameButtons & (1u << e.button.button)) != 0)
            {
                s_gameButtons &= ~(1u << e.button.button);
                return false;
            }
            return io.WantCaptureMouse;
        case SDL_KEYDOWN:
        case SDL_KEYUP:
        case SDL_TEXTINPUT:
            return io.WantCaptureKeyboard || io.WantTextInput;
        default:
            return false;
    }
}

void Overlay_Render()
{
    if (!s_initialized || !any_visible())
    {
        return;
    }
    rebuild_fonts_if_needed();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame(s_window);
    ImGui::NewFrame();
    // Software cursor over overlay windows; the game's own cursor is drawn underneath them.
    ImGui::GetIO().MouseDrawCursor = ImGui::GetIO().WantCaptureMouse;

    const float scale = float(s_fontScale);
    if (shown(OverlayWindow::Journal))
    {
        Overlay_DrawJournal(&s_visible[int(OverlayWindow::Journal)], scale);
    }
    if (shown(OverlayWindow::Settings))
    {
        Overlay_DrawSettings(&s_visible[int(OverlayWindow::Settings)], scale);
    }
    if (shown(OverlayWindow::Macros))
    {
        Overlay_DrawMacros(&s_visible[int(OverlayWindow::Macros)], scale);
    }
    if (shown(OverlayWindow::Target))
    {
        Overlay_DrawTarget(&s_visible[int(OverlayWindow::Target)], scale);
    }

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

#endif
