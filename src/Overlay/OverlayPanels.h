// SPDX-License-Identifier: AGPL-3.0-or-later

// Individual overlay windows (internal to the Overlay module).

#pragma once

union SDL_Event;

void Overlay_DrawJournal(bool *open, float scale);
void Overlay_DrawSettings(bool *open, float scale);
void Overlay_DrawMacros(bool *open, float scale);
void Overlay_DrawTarget(bool *open, float scale);

// Macro editor key capture: returns true if the event was used to bind a key.
bool Overlay_MacroCaptureEvent(const SDL_Event &e);

// Visibility of each window survives restarts.
void Overlay_LoadState();
void Overlay_SaveState();
