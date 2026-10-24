// SPDX-License-Identifier: AGPL-3.0-or-later

// Modern Dear ImGui windows drawn on top of the classic client UI (journal, settings,
// macros, target info). Only built with the GL3 renderer.

#pragma once

#include <stdint.h>
#include <common/str.h>

union SDL_Event;
struct SDL_Window;

enum class OverlayWindow
{
    Journal,
    Settings,
    Macros,
    Target,
};

#if defined(NEW_RENDERER_ENABLED) && defined(USE_GL3)

void Overlay_Init(SDL_Window *window, void *glContext);
void Overlay_Shutdown();
// Feeds ImGui; returns true when the event belongs to an overlay window and the game
// should not see it.
bool Overlay_ProcessEvent(const SDL_Event &event);
// Draws the overlay into the current back buffer. Call right before swapping.
void Overlay_Render();
void Overlay_Toggle(OverlayWindow w);
// Mirrors a journal line (UTF-8) into the overlay's longer, searchable history.
void Overlay_JournalAdd(int textType, uint16_t hue, const astr_t &utf8);
// True while the overlay draws the login screen in place of the classic main gump.
bool Overlay_LoginActive();
// True while the overlay draws character creation (profession, look, starting city).
bool Overlay_CreateActive();
// Either full-window overlay screen (login or character creation).
bool Overlay_SceneActive();
// Runs a screen change the login screen asked for; call from the main loop between frames.
void Overlay_LoginRunPending();

#else

inline void Overlay_Init(SDL_Window *, void *) {}
inline void Overlay_Shutdown() {}
inline bool Overlay_ProcessEvent(const SDL_Event &) { return false; }
inline void Overlay_Render() {}
inline void Overlay_Toggle(OverlayWindow) {}
inline void Overlay_JournalAdd(int, uint16_t, const astr_t &) {}
inline bool Overlay_LoginActive() { return false; }
inline void Overlay_LoginRunPending() {}
inline bool Overlay_CreateActive() { return false; }
inline bool Overlay_SceneActive() { return false; }

#endif
