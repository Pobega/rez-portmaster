#pragma once

#include "dreamcomp/settings.h"

#include <filesystem>
#include <string>

namespace dreamcomp::ra {

enum class CredentialSource {
    None,
    SettingsIni,
    NextUi,
    RetroArchCfw
};

struct Credentials {
    CredentialSource source = CredentialSource::None;
    std::string source_label;
    std::string username;
    std::string server_username;
    std::string token;
    std::string password;
};

Credentials resolve_credentials(const Settings& settings, const std::filesystem::path& root_prefix = {});

}  // namespace dreamcomp::ra
