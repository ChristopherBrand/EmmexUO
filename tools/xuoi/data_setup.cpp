// SPDX-License-Identifier: AGPL-3.0-or-later

// First-run "Game Data" view: fetch a shard's client files (or point at an existing
// folder) and create a ready-to-use launcher profile for it.

#define LOGGER_MODULE Launcher

#include "data_setup.h"
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <external/miniz.h>
#include <external/tinyfiledialogs.h>
#include <xuocore/client_info.h>
#include <xuocore/http.h>
#include <xuocore/common.h>
#include "accounts.h"
#include "widgets.h"
#include "ui_model.h"

struct data_preset
{
    const char *name;
    const char *description;
    const char *url;
    const char *loginserver;
    const char *clientversion;
};

// Only shards that publish a plain archive of their client files can be listed here.
static const data_preset s_presets[] = {
    { "UOAlive",
      "Free shard with automatic account creation. Downloads about 1.8 GB.",
      "https://files.uoalive.com/UOAlive_Package.zip",
      "login.uoalive.com,2593",
      "7.0.117.0" },
};

enum class setup_state
{
    idle,
    downloading,
    extracting,
    done,
    failed,
};

static std::atomic<setup_state> s_state{ setup_state::idle };
static std::atomic<uint64_t> s_now{ 0 };
static std::atomic<uint64_t> s_total{ 0 };
static std::atomic<bool> s_cancel{ false };
static std::mutex s_message_lock;
static std::string s_message;
static std::thread s_worker;
static int s_preset = -1;
static fs_path s_target;

static void set_message(const std::string &msg)
{
    std::lock_guard<std::mutex> lock(s_message_lock);
    s_message = msg;
}

static std::string get_message()
{
    std::lock_guard<std::mutex> lock(s_message_lock);
    return s_message;
}

static fs_path data_root()
{
    return fs_path_join(fs_path_appdata(), "crossuo", "gamedata");
}

// A folder is usable if it has the files every client era needs.
static bool data_folder_valid(const fs_path &dir)
{
    const bool has_art =
        fs_path_exists(fs_path_join(dir, "artLegacyMUL.uop")) ||
        fs_path_exists(fs_path_join(dir, "art.mul"));
    return has_art && fs_path_exists(fs_path_join(dir, "tiledata.mul")) &&
           fs_path_exists(fs_path_join(dir, "Cliloc.enu"));
}

static bool download_progress(void *, uint64_t now, uint64_t total)
{
    s_now = now;
    s_total = total;
    return !s_cancel;
}

// Extract the folder holding tiledata.mul (archives wrap the client in a versioned folder).
static bool extract_client(const fs_path &zipfile, const fs_path &target)
{
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, fs_path_ascii(zipfile), 0))
    {
        set_message("The download is not a valid zip file.");
        return false;
    }

    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    std::string prefix;
    bool found = false;
    for (mz_uint i = 0; i < count && !found; ++i)
    {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st))
            continue;
        const std::string name = st.m_filename;
        const auto slash = name.find_last_of('/');
        const auto base = slash == std::string::npos ? name : name.substr(slash + 1);
        if (strcasecmp(base.c_str(), "tiledata.mul") == 0)
        {
            prefix = slash == std::string::npos ? "" : name.substr(0, slash + 1);
            found = true;
        }
    }
    if (!found)
    {
        mz_zip_reader_end(&zip);
        set_message("The download does not contain Ultima Online client files.");
        return false;
    }

    s_now = 0;
    s_total = count;
    bool ok = true;
    for (mz_uint i = 0; i < count && ok && !s_cancel; ++i)
    {
        s_now = i;
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st) || st.m_is_directory)
            continue;
        const std::string name = st.m_filename;
        if (name.compare(0, prefix.size(), prefix) != 0 || name.find("..") != std::string::npos)
            continue;
        const auto out = fs_path_join(target, name.substr(prefix.size()).c_str());
        fs_path_create(fs_directory(out));
        if (!mz_zip_reader_extract_to_file(&zip, i, fs_path_ascii(out), 0))
        {
            set_message("Could not extract " + name);
            ok = false;
        }
    }
    mz_zip_reader_end(&zip);
    return ok && !s_cancel;
}

static void install_worker(int preset_index)
{
    const auto &preset = s_presets[preset_index];
    fs_path_create(data_root());
    const auto zipfile = fs_path_join(data_root(), (std::string(preset.name) + ".zip").c_str());

    s_state = setup_state::downloading;
    set_message(std::string("Downloading ") + preset.name + " client files...");
    if (!http_get_file(preset.url, fs_path_ascii(zipfile), download_progress, nullptr))
    {
        fs_del(zipfile);
        set_message(s_cancel ? "Cancelled." : "Download failed. Check your connection and try again.");
        s_state = setup_state::failed;
        return;
    }

    s_state = setup_state::extracting;
    set_message("Unpacking...");
    const bool ok = extract_client(zipfile, s_target);
    fs_del(zipfile);
    if (!ok || !data_folder_valid(s_target))
    {
        if (s_cancel)
            set_message("Cancelled.");
        else if (ok)
            set_message("The unpacked files are incomplete.");
        s_state = setup_state::failed;
        return;
    }
    set_message("Ready.");
    s_state = setup_state::done;
}

static void start_install(int preset_index)
{
    if (s_worker.joinable())
        s_worker.join();
    s_preset = preset_index;
    s_target = fs_path_join(data_root(), s_presets[preset_index].name);
    s_cancel = false;
    s_now = 0;
    s_total = 0;
    if (data_folder_valid(s_target))
    {
        set_message("Already installed.");
        s_state = setup_state::done;
        return;
    }
    s_worker = std::thread(install_worker, preset_index);
}

static void finish(ui_model &m, const char *profile, const char *loginserver, const char *version, const fs_path &dir)
{
    account_add(profile, loginserver, version, fs_path_ascii(dir));
    s_state = setup_state::idle;
    m.view = ui_view::accounts;
}

void data_setup_shutdown()
{
    s_cancel = true;
    if (s_worker.joinable())
        s_worker.join();
}

static std::string human_size(uint64_t bytes)
{
    char buf[32];
    if (bytes >= (1ull << 30))
        snprintf(buf, sizeof(buf), "%.2f GB", double(bytes) / double(1ull << 30));
    else
        snprintf(buf, sizeof(buf), "%.0f MB", double(bytes) / double(1ull << 20));
    return buf;
}

void ui_data_setup(ui_model &m)
{
    const setup_state state = s_state;
    ImGui::BeginChild("##data_setup", m.area, false);
    ImGui::TextWrapped(
        "CrossUO needs the Ultima Online game files. Download them for a shard, or use a folder "
        "you already have.");
    ImGui::Separator();

    if (state == setup_state::downloading || state == setup_state::extracting)
    {
        ImGui::TextUnformatted(get_message().c_str());
        const uint64_t now = s_now, total = s_total;
        const float f = total ? float(double(now) / double(total)) : 0.f;
        std::string overlay;
        if (state == setup_state::downloading && total)
            overlay = human_size(now) + " / " + human_size(total);
        ImGui::ProgressBar(f, ImVec2(-1, 0), overlay.empty() ? nullptr : overlay.c_str());
        if (ImGui::Button("Cancel"))
            s_cancel = true;
        ImGui::EndChild();
        return;
    }

    if (state == setup_state::done)
    {
        if (s_worker.joinable())
            s_worker.join();
        const auto &p = s_presets[s_preset];
        finish(m, p.name, p.loginserver, p.clientversion, s_target);
        ImGui::EndChild();
        return;
    }

    if (state == setup_state::failed)
    {
        ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1.f), "%s", get_message().c_str());
    }

    for (int i = 0; i < int(countof(s_presets)); ++i)
    {
        const auto &p = s_presets[i];
        ImGui::PushID(i);
        if (ImGui::Button(p.name, ImVec2(110, 0)))
            start_install(i);
        ImGui::SameLine();
        ImGui::TextWrapped("%s", p.description);
        ImGui::PopID();
    }

    if (ImGui::Button("Use folder...", ImVec2(110, 0)))
    {
        const auto picked = tinyfd_selectFolderDialog("Ultima Online data folder", nullptr);
        if (picked)
        {
            const auto dir = fs_path_from(picked);
            if (!data_folder_valid(dir))
            {
                set_message("That folder doesn't contain Ultima Online client files (tiledata.mul, Cliloc.enu, art).");
                s_state = setup_state::failed;
            }
            else
            {
                char version[32] = {};
                client_info info;
                const auto exe = fs_path_join(dir, "client.exe");
                if (fs_path_exists(exe) && client_version(fs_path_ascii(exe), info) == 0 && info.version)
                    client_version_string(info.version, version, sizeof(version));
                finish(m, "My Shard", "", version, dir);
            }
        }
    }
    ImGui::SameLine();
    ImGui::TextWrapped("Pick a folder with the files for your shard, then set its login server.");

    if (!accounts_empty())
    {
        ImGui::NewLine();
        if (ImGui::Button("Back to accounts"))
            m.view = ui_view::accounts;
    }
    ImGui::EndChild();
}
