#include "ra_client.h"
#include "ra_creds.h"
#include "ra_http.h"

#include "dreamcomp/port.h"
#include "dreamcomp/settings.h"

#include "dream/render/overlay.h"
#include "dream/render/png.h"
#include "dream/runtime/gdrom/disc.h"
#include "dream/runtime/host_ext.h"
#include "dream/runtime/system.h"

#include <rc_api_user.h>
#include <rc_client.h>
#include <rc_consoles.h>
#include <rc_hash.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dreamcomp::ra {
namespace {

constexpr std::uint32_t kToastWidth = 320;
constexpr std::uint32_t kToastHeight = 48;

constexpr std::uint32_t make_premul_rgba(unsigned r, unsigned g, unsigned b, unsigned a) {
    const unsigned pr = (r * a + 127u) / 255u;
    const unsigned pg = (g * a + 127u) / 255u;
    const unsigned pb = (b * a + 127u) / 255u;
    return static_cast<std::uint32_t>(pr | (pg << 8) | (pb << 16) | (a << 24));
}

struct ToastItem {
    std::string header;
    std::string line1;
    std::string line2;
    std::string badge_name;
    bool is_amber = false;
    std::chrono::milliseconds duration{4000};
    std::string tag;
    bool is_preview = false;
};

struct CdTrackHandle {
    dream::gdrom::Disc* disc = nullptr;
    const dream::gdrom::Track* track = nullptr;
};

dream::gdrom::Disc* g_active_disc = nullptr;
dream::System* g_active_system = nullptr;

void* custom_open_track(const char* path, uint32_t track) {
    (void)path;
    if (!g_active_disc) {
        return nullptr;
    }
    const auto& tracks = g_active_disc->tracks();
    if (tracks.empty()) {
        return nullptr;
    }

    const dream::gdrom::Track* target = nullptr;
    if (track == RC_HASH_CDTRACK_FIRST_DATA) {
        for (const auto& t : tracks) {
            if (t.data) {
                target = &t;
                break;
            }
        }
    } else if (track == RC_HASH_CDTRACK_LAST) {
        target = &tracks.back();
    } else if (track == RC_HASH_CDTRACK_LARGEST) {
        uint32_t max_sectors = 0;
        for (const auto& t : tracks) {
            if (t.sectors > max_sectors) {
                max_sectors = t.sectors;
                target = &t;
            }
        }
    } else {
        for (const auto& t : tracks) {
            if (t.number == track) {
                target = &t;
                break;
            }
        }
    }

    if (!target) {
        return nullptr;
    }
    return new CdTrackHandle{g_active_disc, target};
}

uint32_t custom_first_track_sector(void* handle) {
    auto* h = static_cast<CdTrackHandle*>(handle);
    return h ? h->track->lba : 0;
}

size_t custom_read_sector(void* handle, uint32_t sector, void* buffer, size_t requested_bytes) {
    auto* h = static_cast<CdTrackHandle*>(handle);
    if (!h || !h->disc) {
        return 0;
    }
    size_t total_copied = 0;
    uint32_t current_sector = sector;
    auto* dst = static_cast<uint8_t*>(buffer);

    while (total_copied < requested_bytes) {
        const dream::gdrom::Track* t = h->disc->track_for(current_sector);
        if (!t || !t->data) {
            break;
        }
        uint8_t sec2048[2048];
        if (!h->disc->read_user(current_sector, sec2048)) {
            break;
        }
        const size_t to_copy = std::min(requested_bytes - total_copied, static_cast<size_t>(2048));
        std::memcpy(dst + total_copied, sec2048, to_copy);
        total_copied += to_copy;
        ++current_sector;
    }
    return total_copied;
}

void custom_close_track(void* handle) {
    delete static_cast<CdTrackHandle*>(handle);
}

rc_clock_t RC_CCONV get_time_millisecs_callback(const rc_client_t* client) {
    (void)client;
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<rc_clock_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

uint32_t RC_CCONV read_memory_callback(uint32_t address, uint8_t* buffer, uint32_t num_bytes, rc_client_t* client) {
    (void)client;
    if (g_active_system && g_active_system->memory.fast_ram && address < 0x01000000u) {
        const uint32_t available = 0x01000000u - address;
        const uint32_t to_copy = std::min(num_bytes, available);
        std::memcpy(buffer, g_active_system->memory.fast_ram + address, to_copy);
        return to_copy;
    }
    return 0;
}

void fill_box(std::uint32_t* rgba, unsigned pitch, int x, int y, int w, int h, std::uint32_t color) {
    if (!rgba || w <= 0 || h <= 0) return;
    const int x0 = std::max(0, x), y0 = std::max(0, y);
    const int x1 = std::min(static_cast<int>(kToastWidth), x + w);
    const int y1 = std::min(static_cast<int>(kToastHeight), y + h);
    for (int py = y0; py < y1; ++py) {
        for (int px = x0; px < x1; ++px) {
            rgba[py * pitch + px] = color;
        }
    }
}

void stroke_box(std::uint32_t* rgba, unsigned pitch, int x, int y, int w, int h, std::uint32_t color) {
    fill_box(rgba, pitch, x, y, w, 1, color);
    fill_box(rgba, pitch, x, y + h - 1, w, 1, color);
    fill_box(rgba, pitch, x, y, 1, h, color);
    fill_box(rgba, pitch, x + w - 1, y, 1, h, color);
}

void draw_triangle_right(std::uint32_t* rgba, unsigned pitch, int x, int y, std::uint32_t color) {
    for (int row = 0; row < 9; ++row) {
        const int w = 5 - std::abs(row - 4);
        for (int col = 0; col < w; ++col) {
            const int px = x + col;
            const int py = y + row;
            if (px >= 0 && px < static_cast<int>(kToastWidth) && py >= 0 && py < static_cast<int>(kToastHeight)) {
                rgba[py * pitch + px] = color;
            }
        }
    }
}

void draw_tracked_text(std::uint32_t* rgba, unsigned pitch, unsigned w, unsigned h,
                       int x, int y, std::string_view text, std::uint32_t fg, int spacing) {
    dream::render::TextStyle style;
    style.bg = 0;
    style.fg = fg;
    style.scale = 1;
    int cur_x = x;
    for (char c : text) {
        const char s[2] = {c, '\0'};
        dream::render::draw_text(rgba, pitch, w, h, cur_x, y, s, style);
        cur_x += spacing;
    }
}

std::string to_lower_str(std::string_view s) {
    std::string res;
    res.reserve(s.size());
    for (char c : s) {
        res.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return res;
}

std::string to_terminal_str(std::string_view s) {
    std::string res;
    res.reserve(s.size());
    for (char c : s) {
        if (c == ' ') {
            res.push_back('_');
        } else {
            res.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        }
    }
    return res;
}

void draw_rez_target_icon(std::uint32_t* rgba, unsigned pitch, int start_x, int start_y, std::uint32_t color) {
    auto set_pixel = [&](int px, int py) {
        if (px >= 0 && px < 28 && py >= 0 && py < 28) {
            const int tx = start_x + px;
            const int ty = start_y + py;
            if (tx >= 0 && tx < static_cast<int>(kToastWidth) && ty >= 0 && ty < static_cast<int>(kToastHeight)) {
                rgba[ty * pitch + tx] = color;
            }
        }
    };

    for (int i = 7; i <= 20; ++i) {
        set_pixel(i, 0);
        set_pixel(i, 27);
        set_pixel(0, i);
        set_pixel(27, i);
    }
    for (int i = 0; i < 7; ++i) {
        set_pixel(i, 7 - i);
        set_pixel(20 + i, i);
        set_pixel(27 - i, 20 + i);
        set_pixel(7 - i, 27 - i);
    }

    for (int i = 0; i <= 5; ++i) {
        set_pixel(14 + i, 9 + i);
        set_pixel(19 - i, 14 + i);
        set_pixel(14 - i, 19 - i);
        set_pixel(9 + i, 14 - i);
    }
    set_pixel(13, 13);
    set_pixel(14, 13);
    set_pixel(13, 14);
    set_pixel(14, 14);
}

void blit_badge_scaled(std::uint32_t* toast_rgba, unsigned pitch, int start_x, int start_y,
                       const dream::render::png::Image& img) {
    if (img.width == 0 || img.height == 0) {
        return;
    }
    constexpr int kBadgeSize = 28;
    for (int dy = 0; dy < kBadgeSize; ++dy) {
        for (int dx = 0; dx < kBadgeSize; ++dx) {
            const unsigned sx0 = static_cast<unsigned>(dx * img.width / kBadgeSize);
            const unsigned sy0 = static_cast<unsigned>(dy * img.height / kBadgeSize);
            const unsigned sx1 = std::min(sx0 + 1, img.width - 1);
            const unsigned sy1 = std::min(sy0 + 1, img.height - 1);

            const std::uint32_t p00 = img.pixels[sy0 * img.width + sx0];
            const std::uint32_t p01 = img.pixels[sy0 * img.width + sx1];
            const std::uint32_t p10 = img.pixels[sy1 * img.width + sx0];
            const std::uint32_t p11 = img.pixels[sy1 * img.width + sx1];

            auto get_r = [](std::uint32_t p) { return p & 0xFFu; };
            auto get_g = [](std::uint32_t p) { return (p >> 8) & 0xFFu; };
            auto get_b = [](std::uint32_t p) { return (p >> 16) & 0xFFu; };
            auto get_a = [](std::uint32_t p) { return (p >> 24) & 0xFFu; };

            const unsigned r = (get_r(p00) + get_r(p01) + get_r(p10) + get_r(p11)) / 4u;
            const unsigned g = (get_g(p00) + get_g(p01) + get_g(p10) + get_g(p11)) / 4u;
            const unsigned b = (get_b(p00) + get_b(p01) + get_b(p10) + get_b(p11)) / 4u;
            const unsigned a = (get_a(p00) + get_a(p01) + get_a(p10) + get_a(p11)) / 4u;

            if (a == 0) {
                continue;
            }

            const std::uint32_t pr = (r * a + 127u) / 255u;
            const std::uint32_t pg = (g * a + 127u) / 255u;
            const std::uint32_t pb = (b * a + 127u) / 255u;
            toast_rgba[(start_y + dy) * pitch + (start_x + dx)] = pr | (pg << 8) | (pb << 16) | (a << 24);
        }
    }
}

void render_pause_tab_toast(const ToastItem& item, std::uint32_t* rgba, bool has_badge) {
    constexpr std::uint32_t kKatanaOrange = make_premul_rgba(255, 142, 0, 255);
    constexpr std::uint32_t kMainBg = make_premul_rgba(4, 3, 2, 218);
    constexpr std::uint32_t kTabBg = make_premul_rgba(8, 6, 4, 245);
    constexpr std::uint32_t kFrameBg = make_premul_rgba(18, 10, 4, 255);
    constexpr std::uint32_t kFrameBorder = make_premul_rgba(205, 115, 20, 255);
    constexpr std::uint32_t kSoftAmber = make_premul_rgba(205, 145, 60, 255);
    constexpr std::uint32_t kWhite = make_premul_rgba(255, 255, 255, 255);

    fill_box(rgba, kToastWidth, 0, 6, 320, 42, kMainBg);
    stroke_box(rgba, kToastWidth, 0, 6, 320, 42, kKatanaOrange);

    const std::string lower_header = to_lower_str(item.header);
    const int tab_w = std::min(std::max(static_cast<int>(lower_header.size()) * 7 + 14, 80), 220);
    fill_box(rgba, kToastWidth, 10, 0, tab_w, 12, kTabBg);
    stroke_box(rgba, kToastWidth, 10, 0, tab_w, 12, kKatanaOrange);
    draw_tracked_text(rgba, kToastWidth, kToastWidth, kToastHeight, 17, 2, lower_header, kKatanaOrange, 7);

    fill_box(rgba, kToastWidth, 8, 14, 30, 30, kFrameBg);
    stroke_box(rgba, kToastWidth, 8, 14, 30, 30, kFrameBorder);
    if (!has_badge) {
        draw_rez_target_icon(rgba, kToastWidth, 9, 15, kKatanaOrange);
    }

    draw_triangle_right(rgba, kToastWidth, 44, 15, kKatanaOrange);
    draw_tracked_text(rgba, kToastWidth, kToastWidth, kToastHeight, 53, 16, to_lower_str(item.line1), kWhite, 7);

    if (!item.tag.empty()) {
        const std::uint32_t pill_bg = item.is_amber ? make_premul_rgba(110, 65, 10, 245)
                                                    : make_premul_rgba(138, 36, 12, 245);
        fill_box(rgba, kToastWidth, 262, 13, 52, 13, pill_bg);
        fill_box(rgba, kToastWidth, 262, 13, 52, 1, make_premul_rgba(195, 145, 120, 255));
        fill_box(rgba, kToastWidth, 262, 13, 1, 13, make_premul_rgba(195, 145, 120, 255));
        fill_box(rgba, kToastWidth, 262, 25, 52, 1, make_premul_rgba(60, 15, 5, 255));
        fill_box(rgba, kToastWidth, 313, 13, 1, 13, make_premul_rgba(60, 15, 5, 255));
        draw_tracked_text(rgba, kToastWidth, kToastWidth, kToastHeight, 268, 16, to_lower_str(item.tag),
                          make_premul_rgba(255, 235, 220, 255), 6);
    }

    if (!item.line2.empty()) {
        draw_tracked_text(rgba, kToastWidth, kToastWidth, kToastHeight, 44, 31, to_lower_str(item.line2), kSoftAmber, 6);
    }
}

void render_options_toast(const ToastItem& item, std::uint32_t* rgba, bool has_badge) {
    constexpr std::uint32_t kBronze = make_premul_rgba(118, 76, 18, 255);
    constexpr std::uint32_t kOrange = make_premul_rgba(255, 142, 0, 255);
    constexpr std::uint32_t kWhite = make_premul_rgba(255, 255, 255, 255);

    fill_box(rgba, kToastWidth, 0, 0, 157, 13, make_premul_rgba(10, 10, 10, 228));
    stroke_box(rgba, kToastWidth, 0, 0, 157, 13, kBronze);
    fill_box(rgba, kToastWidth, 1, 1, 154, 1, make_premul_rgba(165, 108, 28, 255));
    draw_tracked_text(rgba, kToastWidth, kToastWidth, kToastHeight, 8, 3, to_lower_str(item.header),
                      make_premul_rgba(220, 160, 50, 255), 8);

    stroke_box(rgba, kToastWidth, 0, 13, 320, 35, kBronze);

    fill_box(rgba, kToastWidth, 2, 15, 36, 31, make_premul_rgba(6, 6, 6, 240));
    stroke_box(rgba, kToastWidth, 2, 15, 36, 31, kBronze);
    if (!has_badge) {
        draw_rez_target_icon(rgba, kToastWidth, 6, 17, kOrange);
    }

    fill_box(rgba, kToastWidth, 39, 15, 279, 31, make_premul_rgba(6, 6, 6, 240));
    stroke_box(rgba, kToastWidth, 39, 15, 279, 31, kBronze);

    draw_triangle_right(rgba, kToastWidth, 45, 17, kOrange);
    draw_tracked_text(rgba, kToastWidth, kToastWidth, kToastHeight, 53, 19, to_lower_str(item.line1), kWhite, 7);

    if (!item.tag.empty()) {
        const std::uint32_t tag_bg = item.is_amber ? make_premul_rgba(120, 75, 12, 245)
                                                   : make_premul_rgba(146, 26, 24, 245);
        fill_box(rgba, kToastWidth, 266, 17, 49, 12, tag_bg);
        draw_tracked_text(rgba, kToastWidth, kToastWidth, kToastHeight, 271, 19, to_lower_str(item.tag),
                          make_premul_rgba(255, 235, 220, 255), 6);
    }

    fill_box(rgba, kToastWidth, 40, 31, 277, 1, make_premul_rgba(65, 42, 10, 255));

    if (!item.line2.empty()) {
        draw_tracked_text(rgba, kToastWidth, kToastWidth, kToastHeight, 45, 35, to_lower_str(item.line2),
                          make_premul_rgba(180, 140, 80, 255), 6);
    }
}

void render_kproject_toast(const ToastItem& item, std::uint32_t* rgba, bool has_badge) {
    constexpr std::uint32_t kKatanaOrange = make_premul_rgba(255, 142, 0, 255);
    constexpr std::uint32_t kAmber = make_premul_rgba(255, 185, 40, 255);

    fill_box(rgba, kToastWidth, 0, 0, 320, 48, make_premul_rgba(4, 3, 2, 198));
    fill_box(rgba, kToastWidth, 0, 0, 320, 1, kKatanaOrange);
    fill_box(rgba, kToastWidth, 0, 47, 320, 1, kKatanaOrange);
    fill_box(rgba, kToastWidth, 0, 0, 1, 8, kKatanaOrange);
    fill_box(rgba, kToastWidth, 0, 40, 1, 8, kKatanaOrange);
    fill_box(rgba, kToastWidth, 319, 0, 1, 8, kKatanaOrange);
    fill_box(rgba, kToastWidth, 319, 40, 1, 8, kKatanaOrange);

    stroke_box(rgba, kToastWidth, 6, 6, 30, 30, make_premul_rgba(180, 75, 10, 255));
    stroke_box(rgba, kToastWidth, 9, 9, 30, 30, kAmber);
    for (int k = 0; k <= 3; ++k) {
        fill_box(rgba, kToastWidth, 6 + k, 6 + k, 1, 1, make_premul_rgba(200, 110, 20, 255));
        fill_box(rgba, kToastWidth, 35 + k, 6 + k, 1, 1, make_premul_rgba(200, 110, 20, 255));
        fill_box(rgba, kToastWidth, 6 + k, 35 + k, 1, 1, make_premul_rgba(200, 110, 20, 255));
        fill_box(rgba, kToastWidth, 35 + k, 35 + k, 1, 1, make_premul_rgba(200, 110, 20, 255));
    }
    if (!has_badge) {
        draw_rez_target_icon(rgba, kToastWidth, 10, 10, kKatanaOrange);
    }

    std::string line1_term = to_terminal_str(item.header);
    if (!item.tag.empty()) {
        line1_term += "_[" + to_terminal_str(item.tag) + "]";
    }
    draw_tracked_text(rgba, kToastWidth, kToastWidth, kToastHeight, 46, 5, line1_term, kKatanaOrange, 6);

    draw_tracked_text(rgba, kToastWidth, kToastWidth, kToastHeight, 46, 16, to_terminal_str(item.line1), kAmber, 7);

    if (!item.line2.empty()) {
        draw_tracked_text(rgba, kToastWidth, kToastWidth, kToastHeight, 46, 27, to_lower_str(item.line2),
                          make_premul_rgba(195, 110, 20, 255), 6);
    }

    stroke_box(rgba, kToastWidth, 46, 38, 267, 5, make_premul_rgba(110, 95, 75, 240));
    fill_box(rgba, kToastWidth, 47, 39, 265, 3, make_premul_rgba(185, 75, 15, 255));
}

struct ActiveToast {
    std::uint64_t seq = 0;
    std::chrono::steady_clock::time_point end_time;
    std::vector<std::uint32_t> pixels;
    bool started = false;
    std::chrono::milliseconds duration{0};
};

class RaExtension;
extern RaExtension g_ra_extension;

class RaExtension final : public dream::host::Extension {
public:
    const char* name() const override { return "RetroAchievements"; }

    void on_start(dream::System& sys) override {
        sys_ = &sys;
        g_active_system = &sys;
        g_active_disc = sys.disc;

        enabled_.store(dreamcomp::settings().get_bool("ra_enabled", true), std::memory_order_relaxed);

        const std::string style_str = dreamcomp::settings().get("ra_toast_style");
        if (style_str == "options") {
            toast_style_ = ToastStyle::Options;
        } else if (style_str == "k_project" || style_str == "kproject") {
            toast_style_ = ToastStyle::KProject;
        } else {
            toast_style_ = ToastStyle::PauseTab;
        }

        if (!sys.disc) {
            return;
        }

        rc_hash_cdreader_t reader{};
        reader.open_track = &custom_open_track;
        reader.first_track_sector = &custom_first_track_sector;
        reader.read_sector = &custom_read_sector;
        reader.close_track = &custom_close_track;
        rc_hash_init_custom_cdreader(&reader);

        rc_hash_iterator_t iter{};
        rc_hash_initialize_iterator(&iter, sys.disc->path().c_str(), nullptr, 0);
        iter.callbacks.cdreader = reader;
        char hash_buf[33] = {0};
        const int hash_res = rc_hash_generate(hash_buf, RC_CONSOLE_DREAMCAST, &iter);
        rc_hash_destroy_iterator(&iter);

        if (hash_res != 0) {
            game_hash_ = hash_buf;
            std::printf("rez: RetroAchievements disc hash %s\n", game_hash_.c_str());
        }

        if (is_enabled()) {
            start_session_if_configured();
        }
    }

    void on_vblank(dream::System& sys) override {
        (void)sys;
        if (http_) {
            http_->dispatch_callbacks();
        }
        if (client_) {
            rc_client_do_frame(client_);
        }
    }

    void on_paused_tick() override {
        if (http_) {
            http_->dispatch_callbacks();
        }
        if (client_) {
            rc_client_idle(client_);
        }
    }

    void teardown_session() {
        if (client_) {
            rc_client_destroy(client_);
            client_ = nullptr;
        }
        if (http_) {
            set_active_http_engine(nullptr);
            http_.reset();
        }
        logged_in_ = false;
        game_loaded_ = false;
        connecting_ = false;
        syncing_ = false;
    }

    void on_stop(dream::System& sys, const char* why) override {
        (void)sys;
        (void)why;
        teardown_session();
        g_active_system = nullptr;
        g_active_disc = nullptr;
    }

    std::uint64_t toast_sequence() override {
        std::lock_guard<std::mutex> lock(toast_mutex_);
        check_toast_expiry();
        return active_toast_ ? active_toast_->seq : 0;
    }

    bool query_toast(unsigned view_w, unsigned view_h, std::uint64_t last_uploaded_seq,
                     dream::host::ToastView& out_view, std::vector<std::uint32_t>& out_pixels) override {
        std::lock_guard<std::mutex> lock(toast_mutex_);
        check_toast_expiry();
        if (!active_toast_) {
            return false;
        }

        if (!active_toast_->started) {
            active_toast_->started = true;
            active_toast_->end_time = std::chrono::steady_clock::now() + active_toast_->duration;
        }

        const std::uint32_t scale = (view_h >= 720) ? 2u : 1u;
        const int margin = static_cast<int>(10 * scale);

        out_view.seq = active_toast_->seq;
        out_view.width = kToastWidth;
        out_view.height = kToastHeight;
        out_view.dst_w = kToastWidth * scale;
        out_view.dst_h = kToastHeight * scale;
        out_view.dst_x = std::max(0, static_cast<int>(view_w) - static_cast<int>(out_view.dst_w) - margin);
        out_view.dst_y = margin;

        if (last_uploaded_seq != active_toast_->seq) {
            out_pixels = active_toast_->pixels;
        }
        return true;
    }

    ToastStyle toast_style() const {
        return toast_style_;
    }

    void set_toast_style(ToastStyle style) {
        toast_style_ = style;
        const char* s = "pause_tab";
        if (style == ToastStyle::Options) {
            s = "options";
        } else if (style == ToastStyle::KProject) {
            s = "k_project";
        }
        dreamcomp::settings().set("ra_toast_style", s);
        dreamcomp::settings().save();
        show_toast_immediate("toast preview", std::string("style: ") + toast_style_label(style),
                             "retroachievements notification", 2.5f, "", false, "10 pts", true);
    }

    void set_enabled(bool enabled) {
        enabled_.store(enabled, std::memory_order_relaxed);
        dreamcomp::settings().set("ra_enabled", enabled ? "true" : "false");
        dreamcomp::settings().save();
        if (!enabled) {
            teardown_session();
            std::lock_guard<std::mutex> lock(toast_mutex_);
            toast_queue_.clear();
            active_toast_.reset();
        } else if (sys_) {
            start_session_if_configured();
        }
    }

    bool is_enabled() const {
        return enabled_.load(std::memory_order_relaxed);
    }

    bool can_sync_achievements() const {
        return is_enabled() && creds_.source != CredentialSource::None;
    }

    unsigned int pending_sync_count() const {
        if (!is_enabled()) {
            return 0;
        }
        const std::string user = creds_.username;
        if (user.empty()) {
            return 0;
        }
        const std::string ukey = normalize_user_key(user);
        const std::filesystem::path base_cache =
            http_ ? http_->cache_dir() : (dreamcomp::settings().file().parent_path() / "ra_cache");
        const std::filesystem::path awards_file = base_cache / ("pending_awards_" + ukey + ".txt");
        std::ifstream file(awards_file);
        if (!file.is_open()) {
            return 0;
        }
        std::string line;
        unsigned int count = 0;
        while (std::getline(file, line)) {
            if (!line.empty() && line.front() != '#') {
                ++count;
            }
        }
        return count;
    }

    void sync_achievements_now() {
        if (!is_enabled()) {
            return;
        }
        if (creds_.source == CredentialSource::None) {
            creds_ = resolve_credentials(dreamcomp::settings());
            if (creds_.source == CredentialSource::None) {
                show_toast_immediate("retroachievements", "not logged in", "set ra_username in settings.ini",
                                     3.0f, "", true, "setup", false);
                return;
            }
        }
        if (!http_ || !client_ || !logged_in_ || !game_loaded_) {
            teardown_session();
            show_toast_immediate("retroachievements", "connecting & syncing...", "", 2.5f, "", false, "sync", false);
            start_session_if_configured();
            return;
        }

        show_toast_immediate("retroachievements", "syncing awards...", "", 2.0f, "", false, "sync", false);
        syncing_ = true;

        http_->sync_now(creds_.username, server_username_, creds_.token, creds_.password,
                        [this](const SyncResult& result) {
                            syncing_ = false;
                            if (!result.resolved_token.empty()) {
                                creds_.token = result.resolved_token;
                            }
                            if (result.online) {
                                const std::string detail = (result.synced_count > 0)
                                                               ? ("synced " + std::to_string(result.synced_count) + " awards")
                                                               : "all awards up to date";
                                show_toast_immediate("sync complete", detail, "", 3.0f, "", false, "sync", false);
                            } else {
                                const std::string err = result.error.empty() ? "server unavailable" : result.error;
                                show_toast_immediate("sync failed", err, "", 3.0f, "", true, "sync", false);
                            }
                        });
    }

    void show_toast_immediate(const std::string& header, const std::string& line1, const std::string& line2,
                              float duration_sec, const std::string& badge_name, bool is_amber,
                              const std::string& tag, bool is_preview) {
        std::lock_guard<std::mutex> lock(toast_mutex_);
        if (is_preview) {
            std::deque<ToastItem> kept;
            for (auto& item : toast_queue_) {
                if (!item.is_preview) {
                    kept.push_back(std::move(item));
                }
            }
            toast_queue_ = std::move(kept);
        }
        ToastItem item;
        item.header = header;
        item.line1 = line1;
        item.line2 = line2;
        item.badge_name = badge_name;
        item.is_amber = is_amber;
        item.duration = std::chrono::milliseconds(static_cast<int>(duration_sec * 1000.0f));
        item.tag = tag;
        item.is_preview = is_preview;

        toast_queue_.push_front(std::move(item));
        active_toast_.reset();
        advance_toast_locked();
    }

    void enqueue_toast(const std::string& header, const std::string& line1, const std::string& line2,
                       float duration_sec, const std::string& badge_name, bool is_amber,
                       const std::string& tag = "") {
        std::lock_guard<std::mutex> lock(toast_mutex_);
        ToastItem item;
        item.header = header;
        item.line1 = line1;
        item.line2 = line2;
        item.badge_name = badge_name;
        item.is_amber = is_amber;
        item.duration = std::chrono::milliseconds(static_cast<int>(duration_sec * 1000.0f));
        item.tag = tag;

        toast_queue_.push_back(std::move(item));
        if (!active_toast_) {
            advance_toast_locked();
        }
    }

    std::string menu_ra_status_label() const {
        if (creds_.source == CredentialSource::None) {
            return "RA: No account setup";
        }
        if (login_failed_ || !logged_in_ || (http_ && http_->offline_mode())) {
            return "RA: Offline";
        }
        std::string user = display_username_.empty() ? creds_.username : display_username_;
        if (user.size() > 20) {
            user = user.substr(0, 20);
        }
        return "RA: " + user;
    }

    std::string menu_sync_progress_header() const {
        return "Progress (" + std::to_string(unlocked_count_) + "/" + std::to_string(total_count_) + ")";
    }

    std::string menu_sync_unsynced_label() const {
        return "Unsynced: " + std::to_string(pending_sync_count());
    }

private:
    std::atomic<bool> enabled_{true};
    ToastStyle toast_style_{ToastStyle::PauseTab};
    bool logged_in_ = false;
    bool game_loaded_ = false;
    bool connecting_ = false;
    bool login_failed_ = false;
    bool syncing_ = false;
    std::uint32_t unlocked_count_ = 0;
    std::uint32_t total_count_ = 0;
    dream::System* sys_ = nullptr;
    std::string game_hash_;
    Credentials creds_;
    std::string server_username_;
    std::string display_username_;
    std::unique_ptr<HttpEngine> http_;
    rc_client_t* client_ = nullptr;

    std::mutex toast_mutex_;
    std::deque<ToastItem> toast_queue_;
    std::unique_ptr<ActiveToast> active_toast_;
    std::uint64_t next_seq_ = 1;

    void check_toast_expiry() {
        if (!active_toast_) {
            if (!toast_queue_.empty()) {
                advance_toast_locked();
            }
            return;
        }
        if (active_toast_->started && std::chrono::steady_clock::now() >= active_toast_->end_time) {
            active_toast_.reset();
            if (!toast_queue_.empty()) {
                advance_toast_locked();
            }
        }
    }

    void advance_toast_locked() {
        if (toast_queue_.empty()) {
            return;
        }
        ToastItem item = std::move(toast_queue_.front());
        toast_queue_.pop_front();

        auto toast = std::make_unique<ActiveToast>();
        toast->seq = next_seq_++;
        toast->duration = item.duration;
        toast->started = false;
        toast->pixels.assign(static_cast<std::size_t>(kToastWidth) * kToastHeight, 0);

        render_toast_to_buffer(item, toast->pixels.data());
        active_toast_ = std::move(toast);
    }

    void render_toast_to_buffer(const ToastItem& item, std::uint32_t* rgba) {
        int badge_x = 9, badge_y = 15;
        if (toast_style_ == ToastStyle::Options) {
            badge_x = 6;
            badge_y = 17;
        } else if (toast_style_ == ToastStyle::KProject) {
            badge_x = 10;
            badge_y = 10;
        }

        dream::render::png::Image badge_img;
        bool has_badge = false;
        if (!item.badge_name.empty() && http_) {
            std::vector<std::uint8_t> png_bytes;
            if (http_->read_cached_badge_png(item.badge_name, png_bytes)) {
                if (dream::render::png::decode(png_bytes.data(), png_bytes.size(), badge_img)) {
                    has_badge = true;
                }
            }
        }

        switch (toast_style_) {
            case ToastStyle::Options:
                render_options_toast(item, rgba, has_badge);
                break;
            case ToastStyle::KProject:
                render_kproject_toast(item, rgba, has_badge);
                break;
            case ToastStyle::PauseTab:
            default:
                render_pause_tab_toast(item, rgba, has_badge);
                break;
        }

        if (has_badge) {
            blit_badge_scaled(rgba, kToastWidth, badge_x, badge_y, badge_img);
        }
    }

    void start_session_if_configured() {
        login_failed_ = false;
        unlocked_count_ = 0;
        total_count_ = 0;
        creds_ = resolve_credentials(dreamcomp::settings());
        if (creds_.source == CredentialSource::None) {
            std::printf("rez: RetroAchievements no credentials configured\n");
            return;
        }
        connecting_ = true;

        const auto cache_dir = dreamcomp::settings().file().parent_path();
        http_ = std::make_unique<HttpEngine>(cache_dir);
        set_active_http_engine(http_.get());

        client_ = rc_client_create(&read_memory_callback, &HttpEngine::server_call_bridge);
        rc_client_set_get_time_millisecs_function(client_, &get_time_millisecs_callback);
        rc_client_set_hardcore_enabled(client_, 0);
        rc_client_set_encore_mode_enabled(client_, 0);
        rc_client_set_event_handler(client_, &event_handler_callback);

        if (!creds_.token.empty()) {
            rc_client_begin_login_with_token(client_, creds_.username.c_str(), creds_.token.c_str(),
                                             &login_callback, this);
        } else if (!creds_.password.empty()) {
            rc_client_begin_login_with_password(client_, creds_.username.c_str(), creds_.password.c_str(),
                                                &login_callback, this);
        }
    }

    static void login_callback(int result, const char* error_message, rc_client_t* client, void* userdata) {
        auto* self = static_cast<RaExtension*>(userdata);
        if (self) {
            self->on_login_finished(result, error_message, client, false);
        }
    }

    static void login_callback_retry(int result, const char* error_message, rc_client_t* client, void* userdata) {
        auto* self = static_cast<RaExtension*>(userdata);
        if (self) {
            self->on_login_finished(result, error_message, client, true);
        }
    }

    void on_login_finished(int result, const char* error_message, rc_client_t* client, bool is_retry) {
        if (result != RC_OK) {
            if (!is_retry && !creds_.token.empty() && !creds_.password.empty()) {
                rc_client_begin_login_with_password(client, creds_.username.c_str(), creds_.password.c_str(),
                                                    &login_callback_retry, this);
                return;
            }
            connecting_ = false;
            login_failed_ = true;
            std::printf("rez: RetroAchievements login failed: %s\n", error_message ? error_message : "unknown");
            return;
        }

        const rc_client_user_t* user = rc_client_get_user_info(client);
        if (user) {
            logged_in_ = true;
            if (user->token && *user->token) {
                creds_.token = user->token;
            }
            server_username_ = !creds_.server_username.empty()
                                   ? creds_.server_username
                                   : ((user->username && *user->username) ? user->username : creds_.username);
            display_username_ = (user->display_name && *user->display_name) ? user->display_name : server_username_;

            if (creds_.source == CredentialSource::SettingsIni && !creds_.password.empty()) {
                dreamcomp::settings().set("ra_token", creds_.token);
                dreamcomp::settings().set("ra_password", "");
                dreamcomp::settings().save();
            } else if (creds_.source != CredentialSource::SettingsIni) {
                dreamcomp::settings().set("ra_cached_user", creds_.username);
                dreamcomp::settings().set("ra_cached_token", creds_.token);
                dreamcomp::settings().save();
            }
        }

        if (!game_hash_.empty()) {
            rc_client_begin_load_game(client, game_hash_.c_str(), &load_game_callback, this);
        }
    }

    static void load_game_callback(int result, const char* error_message, rc_client_t* client, void* userdata) {
        auto* self = static_cast<RaExtension*>(userdata);
        if (self) {
            self->on_load_game_finished(result, error_message, client);
        }
    }

    void on_load_game_finished(int result, const char* error_message, rc_client_t* client) {
        connecting_ = false;
        if (result != RC_OK) {
            login_failed_ = true;
            std::printf("rez: RetroAchievements game load failed: %s\n", error_message ? error_message : "unknown");
            return;
        }

        game_loaded_ = true;
        rc_client_achievement_list_t* list =
            rc_client_create_achievement_list(client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE,
                                              RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_PROGRESS);
        std::vector<std::string> badge_names;
        uint32_t unlocked = 0;
        uint32_t total = 0;

        if (list) {
            for (uint32_t b = 0; b < list->num_buckets; ++b) {
                const auto& bucket = list->buckets[b];
                for (uint32_t a = 0; a < bucket.num_achievements; ++a) {
                    const auto* ach = bucket.achievements[a];
                    if (!ach) {
                        continue;
                    }
                    ++total;
                    if (ach->unlocked) {
                        ++unlocked;
                    }
                    if (ach->badge_name[0]) {
                        badge_names.emplace_back(ach->badge_name);
                    }
                }
            }
            rc_client_destroy_achievement_list(list);
        }

        unlocked_count_ = unlocked;
        total_count_ = total;

        if (!badge_names.empty() && http_) {
            http_->prefetch_badges_async(std::move(badge_names));
        }

        const bool offline = http_ ? http_->offline_mode() : false;
        const std::string header = "retroachievements";
        const std::string tag = offline ? "offline" : "softcore";
        const std::string user_label = display_username_.empty() ? creds_.username : display_username_;
        const std::string desc = std::to_string(unlocked) + " / " +
                                 std::to_string(total) + (offline ? " cached" : " unlocked");

        enqueue_toast(header, user_label, desc, 4.0f, "", offline, tag);

        if (!offline && pending_sync_count() > 0) {
            sync_achievements_now();
        }
    }

    static void event_handler_callback(const rc_client_event_t* event, rc_client_t* client) {
        (void)client;
        if (!event) {
            return;
        }
        g_ra_extension.handle_client_event(*event);
    }

    void handle_client_event(const rc_client_event_t& event) {
        if (event.type == RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED && event.achievement) {
            if (unlocked_count_ < total_count_) {
                ++unlocked_count_;
            }
            const bool offline = http_ ? http_->offline_mode() : false;
            const std::string header = offline ? "offline unlock" : "achievement";
            const std::string title = event.achievement->title ? event.achievement->title : "";
            const std::string pts_tag = std::to_string(event.achievement->points) + " pts";
            const std::string desc = event.achievement->description ? event.achievement->description : "";
            const std::string badge = event.achievement->badge_name[0] ? event.achievement->badge_name : "";

            enqueue_toast(header, title, desc, 4.5f, badge, offline, pts_tag);
        } else if ((event.type == RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_SHOW ||
                    event.type == RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_UPDATE) &&
                   event.achievement) {
            const std::string title = event.achievement->title ? event.achievement->title : "";
            const std::string progress = event.achievement->measured_progress;
            const std::string badge = event.achievement->badge_name[0] ? event.achievement->badge_name : "";

            enqueue_toast("progress", title, "", 2.5f, badge, false, progress);
        }
    }
};

RaExtension g_ra_extension;
dream::host::Register reg_ra(g_ra_extension);

}  // namespace

bool is_enabled() {
    return g_ra_extension.is_enabled();
}

void set_enabled(bool enabled) {
    g_ra_extension.set_enabled(enabled);
}

bool can_sync_achievements() {
    return g_ra_extension.can_sync_achievements();
}

unsigned int pending_sync_count() {
    return g_ra_extension.pending_sync_count();
}

void sync_achievements_now() {
    g_ra_extension.sync_achievements_now();
}

ToastStyle toast_style() {
    return g_ra_extension.toast_style();
}

void set_toast_style(ToastStyle style) {
    g_ra_extension.set_toast_style(style);
}

const char* toast_style_label(ToastStyle style) {
    switch (style) {
        case ToastStyle::Options:
            return "options";
        case ToastStyle::KProject:
            return "k-project";
        case ToastStyle::PauseTab:
        default:
            return "pause tab";
    }
}

std::string menu_ra_status_label() {
    return g_ra_extension.menu_ra_status_label();
}

std::string menu_sync_progress_header() {
    return g_ra_extension.menu_sync_progress_header();
}

std::string menu_sync_unsynced_label() {
    return g_ra_extension.menu_sync_unsynced_label();
}

}  // namespace dreamcomp::ra
