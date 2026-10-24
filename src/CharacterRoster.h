// SPDX-License-Identifier: AGPL-3.0-or-later

// What EmmexUO remembers about the characters it has played (the server's character list
// only has names): their class, top skills, time played and a paperdoll snapshot. Kept in
// characters.cfg in the app data folder.

#pragma once

#include <ctime>
#include <string>
#include <vector>

struct RosterLayer
{
    int Gump = 0;
    unsigned short Hue = 0;
    bool PartialHue = false;
};

struct RosterSkill
{
    std::string Name;
    float Value = 0.f;
};

struct RosterCharacter
{
    std::string Key; // login server, shard, account and character name
    std::string Name;
    std::string Profession; // warrior, mage, ... or advanced
    std::vector<RosterSkill> TopSkills;
    long long PlayedSeconds = 0;
    time_t LastPlayed = 0;
    std::vector<RosterLayer> Layers; // paperdoll, back to front
};

// The record for a character on the current login server/shard/account, or null.
const RosterCharacter *Roster_Find(const std::string &characterName);
void Roster_Forget(const std::string &characterName);

// In-game hooks: start the play-time clock after entering the world; capture skills,
// appearance and time (periodically and before leaving the world).
void Roster_BeginSession();
void Roster_Capture();
void Roster_Tick(); // call from the main loop; captures every ~30 s while in game
void Roster_EndSession();

// Display name for a profession key ("mage" -> "Mage", "advanced" -> "Adventurer").
std::string Roster_ProfessionTitle(const std::string &key);
