// SPDX-License-Identifier: AGPL-3.0-or-later

// The servers you can log in to, kept in servers.cfg next to the client config. The active
// server's settings are mirrored into the client config (crossuo.cfg), which stays what the
// rest of the client reads.

#pragma once

#include <string>
#include <vector>

struct LoginServer
{
    std::string Name;
    std::string Host;
    int Port = 2593;
    std::string ClientVersion;
    std::string DataPath;
    bool Crypt = false;

    // the login card's state for this server
    std::string Login;
    std::string Password;
    bool SavePassword = false;
    bool AutoLogin = false;
};

enum class ServerStatus
{
    Unknown,
    Online,
    Offline,
};

// Loads servers.cfg, importing the current config and launcher profiles the first time.
void LoginServers_Init();
void LoginServers_Shutdown();
void LoginServers_Save();

std::vector<LoginServer> &LoginServers_List();
int LoginServers_Active();

// Makes a server the active one: stores the login card into the previous server, then loads
// this server's account into the card and its address into the client config.
void LoginServers_Activate(int index);
// Applies live edits of the active server (name, address, port) to the client config.
void LoginServers_ApplyActive();
// True when the active server needs settings that are only read at startup.
bool LoginServers_NeedsRestart();
// Writes the active server into the client config and quits; the client starts again.
void LoginServers_Restart();
// Starts the launcher next to the client (game data setup); false if it is missing.
bool LoginServers_OpenLauncher();
// Called after the main loop ends: starts the new client if a restart was requested.
void LoginServers_RunPendingRelaunch();

int LoginServers_Add();
void LoginServers_Remove(int index);

ServerStatus LoginServers_Status(const LoginServer &server);
// Ask the status checker to look again soon (after an address edit).
void LoginServers_Recheck();

const char *LoginServers_DisplayName(const LoginServer &server);
