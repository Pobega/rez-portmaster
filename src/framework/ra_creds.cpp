#include "ra_creds.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <string_view>
#include <vector>

namespace dreamcomp::ra {
namespace {

std::string trim_whitespace(std::string_view s) {
    while (!s.empty() && (std::isspace(static_cast<unsigned char>(s.front())) || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (std::isspace(static_cast<unsigned char>(s.back())) || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return std::string(s);
}

std::string unquote_value(std::string_view s) {
    std::string trimmed = trim_whitespace(s);
    if (trimmed.size() >= 2) {
        const char front = trimmed.front();
        const char back = trimmed.back();
        if ((front == '"' && back == '"') || (front == '\'' && back == '\'')) {
            return trimmed.substr(1, trimmed.size() - 2);
        }
    }
    return trimmed;
}

std::filesystem::path resolve_path(const std::filesystem::path& base_path,
                                   const std::filesystem::path& root_prefix) {
    if (root_prefix.empty()) {
        return base_path;
    }
    if (base_path.is_absolute()) {
        return root_prefix / base_path.relative_path();
    }
    return root_prefix / base_path;
}

bool parse_key_value_line(std::string_view line, std::string_view& key, std::string_view& value) {
    const auto eq_pos = line.find('=');
    if (eq_pos == std::string_view::npos) {
        return false;
    }
    key = line.substr(0, eq_pos);
    value = line.substr(eq_pos + 1);
    return true;
}

bool try_read_nextui_directory(const std::filesystem::path& dir_path, Credentials& creds) {
    std::error_code ec;
    const std::filesystem::path minui_settings = dir_path / "minuisettings.txt";
    if (std::filesystem::exists(minui_settings, ec)) {
        std::ifstream file(minui_settings);
        if (file.is_open()) {
            std::string line;
            std::string ra_user;
            std::string ra_token;
            std::string ra_pass;
            std::string ra_server_user;
            while (std::getline(file, line)) {
                std::string_view raw_key;
                std::string_view raw_val;
                if (!parse_key_value_line(line, raw_key, raw_val)) {
                    continue;
                }
                const std::string key = trim_whitespace(raw_key);
                std::string val = unquote_value(raw_val);
                if (key == "raUsername") {
                    ra_user = std::move(val);
                } else if (key == "raToken") {
                    ra_token = std::move(val);
                } else if (key == "raPassword") {
                    ra_pass = std::move(val);
                } else if (key == "raServerUsername") {
                    ra_server_user = std::move(val);
                }
            }
            if (!ra_user.empty() && (!ra_token.empty() || !ra_pass.empty())) {
                creds.source = CredentialSource::NextUi;
                creds.source_label = minui_settings.string();
                creds.username = std::move(ra_user);
                creds.server_username = std::move(ra_server_user);
                creds.token = std::move(ra_token);
                creds.password = std::move(ra_pass);
                return true;
            }
        }
    }

    const std::filesystem::path ra_auth = dir_path / "ra_auth.txt";
    if (std::filesystem::exists(ra_auth, ec)) {
        std::ifstream file(ra_auth);
        if (file.is_open()) {
            std::string user_line;
            std::string token_line;
            if (std::getline(file, user_line) && std::getline(file, token_line)) {
                std::string user = unquote_value(user_line);
                std::string token = unquote_value(token_line);
                if (!user.empty() && !token.empty()) {
                    creds.source = CredentialSource::NextUi;
                    creds.source_label = ra_auth.string();
                    creds.username = std::move(user);
                    creds.token = std::move(token);
                    return true;
                }
            }
        }
    }

    return false;
}

bool try_read_retroarch_config(const std::filesystem::path& file_path, Credentials& creds) {
    std::error_code ec;
    if (!std::filesystem::exists(file_path, ec)) {
        return false;
    }

    std::ifstream file(file_path);
    if (!file.is_open()) {
        return false;
    }

    const bool is_batocera_conf = (file_path.filename() == "batocera.conf");
    const std::string_view user_key = is_batocera_conf ? "global.retroachievements.username" : "cheevos_username";
    const std::string_view token_key = is_batocera_conf ? "global.retroachievements.token" : "cheevos_token";
    const std::string_view pass_key = is_batocera_conf ? "global.retroachievements.password" : "cheevos_password";

    std::string line;
    std::string user;
    std::string token;
    std::string pass;

    while (std::getline(file, line)) {
        std::string_view raw_key;
        std::string_view raw_val;
        if (!parse_key_value_line(line, raw_key, raw_val)) {
            continue;
        }
        const std::string key = trim_whitespace(raw_key);
        if (key == user_key) {
            user = unquote_value(raw_val);
        } else if (key == token_key) {
            token = unquote_value(raw_val);
        } else if (key == pass_key) {
            pass = unquote_value(raw_val);
        }
    }

    if (!user.empty() && (!token.empty() || !pass.empty())) {
        creds.source = CredentialSource::RetroArchCfw;
        creds.source_label = file_path.string();
        creds.username = std::move(user);
        creds.token = std::move(token);
        creds.password = std::move(pass);
        return true;
    }

    return false;
}

}  // namespace

Credentials resolve_credentials(const Settings& settings, const std::filesystem::path& root_prefix) {
    const std::string s_user = settings.get("ra_username");
    const std::string s_token = settings.get("ra_token");
    const std::string s_pass = settings.get("ra_password");
    if (!s_user.empty() && (!s_token.empty() || !s_pass.empty())) {
        Credentials creds;
        creds.source = CredentialSource::SettingsIni;
        creds.source_label = settings.file().empty() ? "settings.ini" : settings.file().filename().string();
        creds.username = s_user;
        creds.server_username = settings.get("ra_server_username");
        creds.token = s_token;
        creds.password = s_pass;
        return creds;
    }

    Credentials candidate;

    std::vector<std::filesystem::path> nextui_dirs;
    const char* env_shared = std::getenv("SHARED_USERDATA_PATH");
    if (env_shared && *env_shared) {
        nextui_dirs.emplace_back(env_shared);
    }
    const char* env_sdcard = std::getenv("SDCARD_PATH");
    if (env_sdcard && *env_sdcard) {
        nextui_dirs.push_back(std::filesystem::path(env_sdcard) / ".userdata" / "shared");
    }
    nextui_dirs.emplace_back("/mnt/SDCARD/.userdata/shared");
    nextui_dirs.emplace_back("/mnt/sdcard/.userdata/shared");

    for (const auto& dir : nextui_dirs) {
        const std::filesystem::path target_dir = resolve_path(dir, root_prefix);
        if (try_read_nextui_directory(target_dir, candidate)) {
            break;
        }
    }

    if (candidate.source == CredentialSource::None) {
        std::vector<std::filesystem::path> retroarch_paths = {
            "/run/muos/storage/info/config/retroarch.cfg",
            "/mnt/mmc/MUOS/info/config/retroarch.cfg",
            "/mnt/sdcard/MUOS/info/config/retroarch.cfg",
            "/home/ark/.config/retroarch/retroarch.cfg",
            "/home/ark/.config/retroarch32/retroarch.cfg",
            "/userdata/system/configs/retroarch/retroarchcustom.cfg",
            "/userdata/system/configs/retroarch/retroarch.cfg",
            "/userdata/system/batocera.conf",
            "/storage/.config/retroarch/retroarch.cfg",
            "/storage/.config/system/configs/system.cfg"
        };

        const char* env_xdg = std::getenv("XDG_CONFIG_HOME");
        if (env_xdg && *env_xdg) {
            retroarch_paths.push_back(std::filesystem::path(env_xdg) / "retroarch" / "retroarch.cfg");
        }
        const char* env_home = std::getenv("HOME");
        if (env_home && *env_home) {
            retroarch_paths.push_back(std::filesystem::path(env_home) / ".config" / "retroarch" / "retroarch.cfg");
        }

        for (const auto& file_path : retroarch_paths) {
            const std::filesystem::path target_file = resolve_path(file_path, root_prefix);
            if (try_read_retroarch_config(target_file, candidate)) {
                break;
            }
        }
    }

    if (candidate.source != CredentialSource::None) {
        if (candidate.token.empty() && !candidate.username.empty()) {
            const std::string cached_user = settings.get("ra_cached_user");
            const std::string cached_token = settings.get("ra_cached_token");
            if (cached_user == candidate.username && !cached_token.empty()) {
                candidate.token = cached_token;
            }
        }
        if (candidate.server_username.empty()) {
            const std::string cached_srv = settings.get("ra_server_username");
            const std::string cached_user = settings.get("ra_cached_user");
            if (!cached_srv.empty() && (cached_user.empty() || cached_user == candidate.username)) {
                candidate.server_username = cached_srv;
            }
        }
        return candidate;
    }

    return {};
}

}  // namespace dreamcomp::ra
