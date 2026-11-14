#pragma once

#include <string>

namespace dreamcomp::ra {

enum class ToastStyle : unsigned {
    PauseTab = 0,
    Options = 1,
    KProject = 2,
    Count = 3
};

bool is_enabled();
void set_enabled(bool enabled);
bool can_sync_achievements();
unsigned int pending_sync_count();
void sync_achievements_now();

ToastStyle toast_style();
void set_toast_style(ToastStyle style);
const char* toast_style_label(ToastStyle style);

std::string menu_ra_status_label();
std::string menu_sync_progress_header();
std::string menu_sync_unsynced_label();

}  // namespace dreamcomp::ra
