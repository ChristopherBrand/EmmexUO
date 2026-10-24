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
#include "../Renderer/RenderAPI.h"
#include <xuocore/enumlist.h>
#include "OverlayPanels.h"
#include "../Application.h"
#include "../Config.h"
#include "../GameWindow.h"
#include "../Globals.h"
#include "../Managers/ColorManager.h"
#include "../Managers/ConfigManager.h"
#include "../ScreenStages/ConnectionScreen.h"

static SDL_Window *s_window = nullptr;
static bool s_initialized = false;
static bool s_visible[4] = {};
static std::string s_iniPath;
static float s_fontScale = 0.f; // UI scale the fonts and style were built for
static float s_fontRetina = 0.f;
static ImFont *s_loginFonts[3] = {};

ImFont *Overlay_LoginFont(LoginFont font)
{
    ImFont *f = s_loginFonts[int(font)];
    return f != nullptr ? f : ImGui::GetIO().Fonts->Fonts[0];
}

// The modern login screen stands in for the classic main gump.
bool Overlay_LoginActive()
{
    if (!s_initialized)
    {
        return false;
    }
    // connecting to the login server (and "connection lost") show in the login card too
    const bool connecting = g_GameState == GS_MAIN_CONNECT &&
                            (g_ConnectionScreen.GetType() == CST_LOGIN ||
                             g_ConnectionScreen.GetType() == CST_CONLOST);
    return g_GameState == GS_MAIN || connecting;
}

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
    return Overlay_LoginActive() || shown(OverlayWindow::Journal) || shown(OverlayWindow::Settings) ||
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
    const float scale = std::max(1.f, Wisp::g_WispWindow != nullptr ? Wisp::g_WispWindow->UIScale : 1.f);
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

    // login screen typefaces (bundled, SIL OFL); missing files fall back to the default font
    struct
    {
        const char *file;
        float size;
        bool latinOnly;
    } loginFonts[] = {
        { "fonts/Cinzel.ttf", 50.f, true },
        { "fonts/Inter.ttf", 15.f, false },
        { "fonts/Inter.ttf", 12.f, false },
    };
    // rasterized at twice the base size: the login screen scales with its window, up to 2x
    static const ImWchar latin[] = { 0x0020, 0x007E, 0 };
    static const ImWchar uiRanges[] = { 0x0020, 0x017F, 0x2010, 0x2027, 0 };
    for (int i = 0; i < 3; i++)
    {
        const std::string path = Overlay_ResourcePath(loginFonts[i].file);
        s_loginFonts[i] = path.empty() ? nullptr :
                                         io.Fonts->AddFontFromFileTTF(
                                             path.c_str(),
                                             loginFonts[i].size * 2.f * retina,
                                             nullptr,
                                             loginFonts[i].latinOnly ? latin : uiRanges);
    }
    io.FontGlobalScale = 1.f / retina;
    Render_OverlayRebuildFonts();
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
    Render_OverlayInit();
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
    Overlay_LoginShutdown();
    Render_OverlayShutdown();
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
    if (Overlay_LoginActive())
    {
        // the login screen is entirely overlay; the classic gump underneath gets no input,
        // except the window shortcuts (Cmd+= / Cmd+- scale, Cmd+Ctrl+F full screen)
        if (e.type == SDL_KEYDOWN && (e.key.keysym.mod & KMOD_GUI) != 0)
        {
            const auto sym = e.key.keysym.sym;
            if (sym == SDLK_EQUALS || sym == SDLK_PLUS || sym == SDLK_KP_PLUS ||
                sym == SDLK_MINUS || sym == SDLK_KP_MINUS ||
                (sym == SDLK_f && (e.key.keysym.mod & KMOD_CTRL) != 0))
            {
                return false;
            }
        }
        switch (e.type)
        {
            case SDL_MOUSEMOTION:
            case SDL_MOUSEWHEEL:
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
            case SDL_KEYDOWN:
            case SDL_KEYUP:
            case SDL_TEXTINPUT:
                return true;
            default:
                return false;
        }
    }
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
    static bool s_loginWasActive = false;
    const bool loginActive = Overlay_LoginActive();
    if (s_loginWasActive && !loginActive)
    {
        Overlay_LoginHidden();
        Render_SetVSync(false);
        // back to the game's own cursor: hide the system one again
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        SDL_ShowCursor(SDL_DISABLE);
    }
    if (loginActive && !s_loginWasActive)
    {
        // the drifting backdrop needs frames in step with the display
        Render_SetVSync(true);
    }
    if (loginActive)
    {
        // the login screen uses the native pointer (arrow, I-beam, hand) instead of drawing one
        ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    }
    s_loginWasActive = loginActive;

    if (!s_initialized || !any_visible())
    {
        return;
    }
    rebuild_fonts_if_needed();
    Render_OverlayNewFrame();
    ImGui_ImplSDL2_NewFrame(s_window);
    ImGui::NewFrame();
    // In game, a software cursor over overlay windows (the game draws its own underneath);
    // the login screen uses the native pointer instead.
    ImGui::GetIO().MouseDrawCursor = !loginActive && ImGui::GetIO().WantCaptureMouse;

    const float scale = float(s_fontScale);
    if (loginActive)
    {
        Overlay_DrawLogin();
    }
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
    Render_OverlayDraw(ImGui::GetDrawData());
}

#endif
