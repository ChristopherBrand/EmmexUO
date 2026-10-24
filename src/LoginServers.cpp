// SPDX-License-Identifier: AGPL-3.0-or-later

#include "LoginServers.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#include <common/fs.h>
#include <common/logging/logging.h>
#include <common/str.h>
#include <external/process.h>
#include "Application.h"
#include "Config.h"
#include "GUI/GUICheckbox.h"
#include "ScreenStages/MainScreen.h"
#include "TextEngine/EntryText.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

static std::vector<LoginServer> s_servers;
static int s_active = -1;
static bool s_initialized = false;
static bool s_relaunch = false;

// status checker: a background thread that tries a TCP connection to each login server
static std::thread s_checker;
static std::mutex s_mutex;
static std::condition_variable s_wake;
static bool s_stop = false;
static bool s_recheck = false;
static std::vector<std::pair<std::string, int>> s_targets;
static std::map<std::string, ServerStatus> s_status;
static std::map<std::string, int> s_latency; // connect round trip, ms

static std::string endpoint(const std::string &host, int port)
{
    return host + ":" + std::to_string(port);
}

static std::string trim(const std::string &s)
{
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
    {
        return {};
    }
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

static std::string normalized_path(std::string path)
{
    while (path.size() > 1 && (path.back() == '/' || path.back() == '\\'))
    {
        path.pop_back();
    }
    return path;
}

static bool parse_bool(const std::string &v)
{
    return v == "yes" || v == "true" || v == "1" || v == "on";
}

static void split_login_server(const std::string &value, LoginServer &s)
{
    const auto comma = value.find(',');
    s.Host = trim(value.substr(0, comma));
    if (comma != std::string::npos)
    {
        s.Port = std::max(1, std::min(65535, atoi(value.c_str() + comma + 1)));
    }
}

static fs_path servers_file()
{
    return g_App.FilePath("servers.cfg");
}

// --------------------------------------------------------------------------------------
// status checker

static ServerStatus probe(const std::string &host, int port, int &latencyMs)
{
    latencyMs = -1;
    if (host.empty())
    {
        return ServerStatus::Unknown;
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || res == nullptr)
    {
        return ServerStatus::Offline;
    }
    bool online = false;
    for (auto ai = res; ai != nullptr && !online; ai = ai->ai_next)
    {
        const auto started = std::chrono::steady_clock::now();
#if defined(_WIN32)
        SOCKET fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == INVALID_SOCKET)
        {
            continue;
        }
        u_long nonBlocking = 1;
        ioctlsocket(fd, FIONBIO, &nonBlocking);
#else
        int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
        {
            continue;
        }
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
#endif
        connect(fd, ai->ai_addr, int(ai->ai_addrlen));
        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(fd, &writable);
        timeval timeout{ 3, 0 };
        if (select(int(fd) + 1, nullptr, &writable, nullptr, &timeout) > 0)
        {
            int error = 0;
            socklen_t len = sizeof(error);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&error, &len);
            online = error == 0;
            if (online)
            {
                latencyMs = int(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - started)
                                    .count());
            }
        }
#if defined(_WIN32)
        closesocket(fd);
#else
        close(fd);
#endif
    }
    freeaddrinfo(res);
    return online ? ServerStatus::Online : ServerStatus::Offline;
}

static void checker_main()
{
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    std::unique_lock<std::mutex> lock(s_mutex);
    while (!s_stop)
    {
        const auto targets = s_targets;
        s_recheck = false;
        lock.unlock();
        for (const auto &t : targets)
        {
            int latency = -1;
            const auto status = probe(t.first, t.second, latency);
            std::lock_guard<std::mutex> guard(s_mutex);
            s_status[endpoint(t.first, t.second)] = status;
            s_latency[endpoint(t.first, t.second)] = latency;
        }
        lock.lock();
        s_wake.wait_for(lock, std::chrono::seconds(30), [] { return s_stop || s_recheck; });
    }
#if defined(_WIN32)
    WSACleanup();
#endif
}

static void update_targets()
{
    std::lock_guard<std::mutex> guard(s_mutex);
    s_targets.clear();
    for (const auto &s : s_servers)
    {
        const auto t = std::make_pair(s.Host, s.Port);
        if (!s.Host.empty() && std::find(s_targets.begin(), s_targets.end(), t) == s_targets.end())
        {
            s_targets.push_back(t);
        }
    }
}

void LoginServers_Recheck()
{
    update_targets();
    {
        std::lock_guard<std::mutex> guard(s_mutex);
        s_recheck = true;
    }
    s_wake.notify_all();
}

int LoginServers_LatencyMs(const LoginServer &server)
{
    std::lock_guard<std::mutex> guard(s_mutex);
    const auto it = s_latency.find(endpoint(server.Host, server.Port));
    return it != s_latency.end() ? it->second : -1;
}

ServerStatus LoginServers_Status(const LoginServer &server)
{
    std::lock_guard<std::mutex> guard(s_mutex);
    const auto it = s_status.find(endpoint(server.Host, server.Port));
    return it != s_status.end() ? it->second : ServerStatus::Unknown;
}

// --------------------------------------------------------------------------------------
// persistence

static void load_file()
{
    s_servers.clear();
    s_active = -1;
    FILE *fp = fs_open(servers_file(), FS_READ);
    if (fp == nullptr)
    {
        return;
    }
    char buf[2048];
    LoginServer *cur = nullptr;
    while (fgets(buf, sizeof(buf), fp) != nullptr)
    {
        const std::string line = trim(buf);
        if (line.empty() || line[0] == '#' || line[0] == ';')
        {
            continue;
        }
        if (line == "[Server]")
        {
            s_servers.emplace_back();
            cur = &s_servers.back();
            continue;
        }
        const auto eq = line.find('=');
        if (cur == nullptr || eq == std::string::npos)
        {
            continue;
        }
        const std::string key = str_lower(trim(line.substr(0, eq)));
        const std::string value = line.substr(eq + 1); // passwords may have spaces
        if (key == "name")
            cur->Name = trim(value);
        else if (key == "loginserver")
            split_login_server(value, *cur);
        else if (key == "clientversion")
            cur->ClientVersion = trim(value);
        else if (key == "datapath")
            cur->DataPath = trim(value);
        else if (key == "crypt")
            cur->Crypt = parse_bool(trim(value));
        else if (key == "acctid")
            cur->Login = trim(value);
        else if (key == "acctpassword")
            cur->Password = value;
        else if (key == "rememberacctpw")
            cur->SavePassword = parse_bool(trim(value));
        else if (key == "autologin")
            cur->AutoLogin = parse_bool(trim(value));
        else if (key == "active" && parse_bool(trim(value)))
            s_active = int(s_servers.size()) - 1;
    }
    fs_close(fp);
}

static void capture_card();

void LoginServers_Save()
{
    capture_card();
    FILE *fp = fs_open(servers_file(), FS_WRITE);
    if (fp == nullptr)
    {
        Error(Config, "could not write %s", fs_path_ascii(servers_file()));
        return;
    }
    for (size_t i = 0; i < s_servers.size(); i++)
    {
        const auto &s = s_servers[i];
        fprintf(fp, "[Server]\n");
        fprintf(fp, "Name=%s\n", s.Name.c_str());
        fprintf(fp, "LoginServer=%s,%d\n", s.Host.c_str(), s.Port);
        fprintf(fp, "ClientVersion=%s\n", s.ClientVersion.c_str());
        fprintf(fp, "DataPath=%s\n", s.DataPath.c_str());
        fprintf(fp, "Crypt=%s\n", s.Crypt ? "yes" : "no");
        fprintf(fp, "AcctID=%s\n", s.Login.c_str());
        fprintf(fp, "AcctPassword=%s\n", s.SavePassword ? s.Password.c_str() : "");
        fprintf(fp, "RememberAcctPW=%s\n", s.SavePassword ? "yes" : "no");
        fprintf(fp, "AutoLogin=%s\n", s.AutoLogin ? "yes" : "no");
        if (int(i) == s_active)
        {
            fprintf(fp, "Active=yes\n");
        }
        fprintf(fp, "\n");
    }
    fs_close(fp);
    update_targets();
}

// The server the client is configured for right now (crossuo.cfg or --config).
static LoginServer from_current_config()
{
    LoginServer s;
    s.Host = g_Config.ServerAddress;
    s.Port = g_Config.ServerPort;
    s.Name = s.Host;
    s.ClientVersion = g_Config.ClientVersionString;
    s.DataPath = normalized_path(fs_path_str(g_App.m_UOPath));
    s.Crypt = g_Config.UseCrypt;
    s.Login = g_Config.Login;
    s.Password = g_Config.Password;
    s.SavePassword = g_Config.SavePassword;
    s.AutoLogin = g_Config.AutoLogin;
    return s;
}

// Profiles from the old launcher (xuolauncher.cfg): one [Account] section per server.
static std::vector<std::pair<std::string, LoginServer>> launcher_profiles()
{
    std::vector<std::pair<std::string, LoginServer>> out;
    const auto path = fs_path_join(fs_path_appdata(), "xuolauncher", "xuolauncher.cfg");
    FILE *fp = fs_open(path, FS_READ);
    if (fp == nullptr)
    {
        return out;
    }
    char buf[2048];
    LoginServer *cur = nullptr;
    while (fgets(buf, sizeof(buf), fp) != nullptr)
    {
        const std::string line = trim(buf);
        if (line == "[Account]")
        {
            out.emplace_back();
            cur = &out.back().second;
            continue;
        }
        if (!line.empty() && line[0] == '[')
        {
            cur = nullptr;
            continue;
        }
        const auto eq = line.find('=');
        if (cur == nullptr || eq == std::string::npos)
        {
            continue;
        }
        const std::string key = trim(line.substr(0, eq));
        const std::string value = line.substr(eq + 1);
        if (key == "profile")
            out.back().first = cur->Name = trim(value);
        else if (key == "login")
            cur->Login = trim(value);
        else if (key == "password")
            cur->Password = value;
        else if (key == "auto_login")
            cur->AutoLogin = parse_bool(trim(value));
        else if (key == "data_path")
            cur->DataPath = normalized_path(trim(value));
        else if (key == "loginserver")
            split_login_server(value, *cur);
        else if (key == "clientversion")
            cur->ClientVersion = trim(value);
        else if (key == "crypt")
            cur->Crypt = parse_bool(trim(value));
    }
    fs_close(fp);
    for (auto &p : out)
    {
        p.second.SavePassword = !p.second.Password.empty();
    }
    return out;
}

static void import_existing()
{
    const auto profiles = launcher_profiles();
    if (!g_Config.ServerAddress.empty())
    {
        auto current = from_current_config();
        for (const auto &p : profiles)
        {
            if (p.second.Host == current.Host && p.second.Port == current.Port && !p.first.empty())
            {
                current.Name = p.first;
            }
        }
        s_servers.push_back(current);
        s_active = 0;
    }
    for (const auto &p : profiles)
    {
        const bool known = std::any_of(s_servers.begin(), s_servers.end(), [&](const LoginServer &s) {
            return s.Host == p.second.Host && s.Port == p.second.Port;
        });
        if (!known && !p.second.Host.empty())
        {
            s_servers.push_back(p.second);
        }
    }
    Info(Config, "imported %d server(s) into servers.cfg", int(s_servers.size()));
    LoginServers_Save();
}

void LoginServers_Init()
{
    if (s_initialized)
    {
        return;
    }
    s_initialized = true;
    if (fs_path_exists(servers_file()))
    {
        load_file();
    }
    else
    {
        import_existing();
    }
    if (s_active < 0 || s_active >= int(s_servers.size()))
    {
        // the config may have been switched outside the client (e.g. launched with --config)
        s_active = -1;
        for (size_t i = 0; i < s_servers.size(); i++)
        {
            if (s_servers[i].Host == g_Config.ServerAddress && s_servers[i].Port == g_Config.ServerPort)
            {
                s_active = int(i);
            }
        }
    }
    update_targets();
    s_checker = std::thread(checker_main);
}

void LoginServers_Shutdown()
{
    if (!s_initialized)
    {
        return;
    }
    {
        std::lock_guard<std::mutex> guard(s_mutex);
        s_stop = true;
    }
    s_wake.notify_all();
    if (s_checker.joinable())
    {
        s_checker.join();
    }
    s_initialized = false;
}

std::vector<LoginServer> &LoginServers_List()
{
    return s_servers;
}

int LoginServers_Active()
{
    return s_active;
}

const char *LoginServers_DisplayName(const LoginServer &server)
{
    if (!server.Name.empty())
    {
        return server.Name.c_str();
    }
    return server.Host.empty() ? "Unnamed server" : server.Host.c_str();
}

// --------------------------------------------------------------------------------------
// switching

// The login card (g_MainScreen) holds the active server's account while the screen is up.
static void capture_card()
{
    // before LoginServers_Init the card may not hold the saved account yet
    if (!s_initialized || s_active < 0 || s_active >= int(s_servers.size()) || g_MainScreen.m_Account == nullptr)
    {
        return;
    }
    g_MainScreen.Save(); // card -> g_Config
    auto &s = s_servers[s_active];
    s.Login = g_Config.Login;
    s.Password = g_Config.Password;
    s.SavePassword = g_Config.SavePassword;
    s.AutoLogin = g_Config.AutoLogin;
}

void LoginServers_ApplyActive()
{
    if (s_active < 0 || s_active >= int(s_servers.size()))
    {
        return;
    }
    const auto &s = s_servers[s_active];
    g_Config.ServerAddress = s.Host;
    g_Config.ServerPort = uint16_t(s.Port);
    SaveGlobalConfig();
    LoginServers_Save();
}

void LoginServers_Activate(int index)
{
    if (index < 0 || index >= int(s_servers.size()))
    {
        return;
    }
    capture_card();
    s_active = index;
    const auto &s = s_servers[index];
    g_Config.Login = s.Login;
    g_Config.Password = s.SavePassword ? s.Password : std::string();
    g_Config.SavePassword = s.SavePassword;
    g_Config.AutoLogin = s.AutoLogin;
    if (g_MainScreen.m_Account != nullptr)
    {
        g_MainScreen.SetAccounting(s.Login, g_Config.Password);
        g_MainScreen.m_SavePassword->Checked = s.SavePassword;
        g_MainScreen.m_AutoLogin->Checked = s.AutoLogin;
    }
    LoginServers_ApplyActive();
}

bool LoginServers_NeedsRestart()
{
    if (s_active < 0 || s_active >= int(s_servers.size()))
    {
        return false;
    }
    const auto &s = s_servers[s_active];
    const bool data = !s.DataPath.empty() &&
                      normalized_path(s.DataPath) != normalized_path(fs_path_str(g_App.m_UOPath));
    const bool version = !s.ClientVersion.empty() && s.ClientVersion != g_Config.ClientVersionString;
    return data || version || s.Crypt != g_Config.UseCrypt;
}

void LoginServers_Restart()
{
    if (s_active < 0 || s_active >= int(s_servers.size()))
    {
        return;
    }
    capture_card();
    const auto &s = s_servers[s_active];
    if (!s.ClientVersion.empty())
    {
        g_Config.ClientVersionString = s.ClientVersion;
        g_Config.ClientVersionModified = true;
    }
    if (!s.DataPath.empty())
    {
        g_App.m_UOPath = fs_path_from(s.DataPath);
    }
    g_Config.UseCrypt = s.Crypt;
    SaveGlobalConfig();
    LoginServers_Save();
    s_relaunch = true;
    Info(Client, "restarting for server %s", LoginServers_DisplayName(s));
    g_MainScreen.CreateSmoothAction(CMainScreen::ID_SMOOTH_MS_QUIT);
}

static bool spawn(const fs_path &bin, const char *const *extra)
{
    if (!fs_path_is_file(bin))
    {
        return false;
    }
    const astr_t binPath = fs_path_str(bin);
    std::vector<const char *> args = { binPath.c_str() };
    for (; extra != nullptr && *extra != nullptr; ++extra)
    {
        args.push_back(*extra);
    }
    args.push_back(nullptr);
    fs_path_change(fs_directory(bin));
    process_s process;
    const auto options = process_option_inherit_environment | process_option_child_detached |
                         process_option_dont_touch_descriptors;
    if (process_create(args.data(), options, &process) != 0)
    {
        Error(Client, "could not start %s", binPath.c_str());
        return false;
    }
    return true;
}

bool LoginServers_OpenLauncher()
{
#if defined(_WIN32)
    return spawn(fs_path_join(fs_path_process(), "xuolauncher.exe"), nullptr);
#else
    return spawn(fs_path_join(fs_path_process(), "xuolauncher"), nullptr);
#endif
}

void LoginServers_RunPendingRelaunch()
{
    if (!s_relaunch)
    {
        return;
    }
    s_relaunch = false;
#if defined(_WIN32)
    const fs_path bin = fs_path_join(fs_path_process(), "crossuo.exe");
#else
    const fs_path bin = fs_path_join(fs_path_process(), "crossuo");
#endif
    const astr_t cfgPath = fs_path_str(GetConfigFilePath());
    const char *args[] = { "--config", cfgPath.c_str(), nullptr };
    spawn(bin, args);
}

int LoginServers_Add()
{
    LoginServer s;
    if (s_active >= 0 && s_active < int(s_servers.size()))
    {
        // a new server most likely uses the same game data as the current one
        s.ClientVersion = s_servers[s_active].ClientVersion;
        s.DataPath = s_servers[s_active].DataPath;
    }
    else
    {
        s.ClientVersion = g_Config.ClientVersionString;
        s.DataPath = normalized_path(fs_path_str(g_App.m_UOPath));
    }
    s_servers.push_back(s);
    LoginServers_Save();
    return int(s_servers.size()) - 1;
}

void LoginServers_Remove(int index)
{
    if (index < 0 || index >= int(s_servers.size()))
    {
        return;
    }
    s_servers.erase(s_servers.begin() + index);
    if (s_active == index)
    {
        s_active = -1;
        if (!s_servers.empty())
        {
            LoginServers_Activate(0);
            return;
        }
    }
    else if (s_active > index)
    {
        s_active--;
    }
    LoginServers_Save();
}
