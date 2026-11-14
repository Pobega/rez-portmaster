// "recomp settings" in Rez's main menu: a page built from Rez's own options screen offering
// aspect ratio (4:3, 3:2, 16:9, 1:1), render resolution ladders, and frame rate (30 vs 60 fps).
// The game's tables are changed only while the page runs, and restored when closed.
#include "recomp_settings.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>

#include "dream/runtime/sh4/abi.h"
#include "dream/runtime/sh4/cost.h"
#include "dream/runtime/sh4/ctx.h"
#include "dreamcomp/hook.h"
#include "dreamcomp/port.h"
#include "dreamcomp/settings.h"
#include "native_sys.h"
#include "ra_client.h"

namespace rez {

constexpr std::uint32_t kSet0 = 0x8C303B78u;
constexpr std::uint32_t kSet2 = 0x8C303C18u;
constexpr unsigned kSetWords = 20;
constexpr std::uint32_t kBoxSelect0 = 0x8C303F0Cu;
constexpr std::uint32_t kBoxSelect1 = 0x8C303F38u;
constexpr std::uint32_t kBoxSelect3 = 0x8C303F90u;
constexpr std::uint32_t kBoxSelect4 = 0x8C303FBCu;
constexpr std::uint32_t kBoxSelectItems = 0x8C30460Cu;
constexpr std::uint32_t kBoxSelectArrowLeftIdx = 0x8C303ED4u;
constexpr std::uint32_t kBoxSelectArrowRightIdx = 0x8C303EE4u;
constexpr unsigned kBoxSelectWords = 11;
constexpr unsigned kLanguageCount = 6;
constexpr std::uint32_t kRowTypeNone = 0u;
constexpr std::uint32_t kRowTypeBoxOnOff = 1u;
constexpr std::uint32_t kRowTypeBoxSelect = 3u;
constexpr std::uint32_t kSet0Getter = 0x8C301464u, kSet0Setter = 0x8C30146Au;
constexpr std::uint32_t kSyncActionAddr = 0x8C301094u;
constexpr std::uint32_t kBoxSelectInputGetReturnAddr = 0x8C301A1Au;
constexpr std::uint32_t kBoxSelectSetLeftReturnAddr = 0x8C301A44u;
constexpr std::uint32_t kBoxSelectSetRightReturnAddr = 0x8C301A70u;
constexpr std::uint32_t kBoxSelectDrawReturnAddr = 0x8C301BC0u;
constexpr std::uint32_t kBoxOnOffDrawLeftReturnAddr = 0x8C3017F2u;
constexpr std::uint32_t kBoxOnOffDrawRightReturnAddr = 0x8C30181Au;

struct Text {
    std::uint32_t at;
    const char* original;
    const char* page;
};

// Row 0: aspect ratio (replaces "overdrive button" at 0x8C3038E8)
constexpr Text kAspectRow{0x8C3038E8u, "overdrive button", "aspect ratio"};
constexpr Text kSelectValue{0x8C3038FCu, "overdrive-taste", ""};

// Row 1: resolution (replaces "sound mode" at 0x8C303784 / 0x8C303D1C)
constexpr Text kResRow{0x8C303784u, "sound mode", "resolution"};
constexpr Text kResHead{0x8C303D1Cu, "sound mode", "resolution"};

// Titles and headings
constexpr Text kTitle{0x8C303804u, "sound effects vol.", "recomp settings"};
constexpr Text kHead{0x8C30385Cu, "up/down", "recomp"};

// Row 2: frame rate (replaces "shot button" at 0x8C303898, subpanel heading at 0x8C303E20)
constexpr Text kFpsRow{0x8C303898u, "shot button", "frame rate"};
constexpr Text kFpsHead{0x8C303E20u, "heads up display", "frame rate"};
constexpr Text k30Fps{0x8C303DACu, "normal", "30 fps"};
constexpr Text k60Fps{0x8C303DBCu, "reverse", "60 fps"};

// Row 6: toast style (replaces "point display" at 0x8C3039C8, subpanel heading at 0x8C3039E8)
constexpr Text kToastRow{0x8C3039C8u, "point display", "toast style"};
constexpr Text kToastHead{0x8C3039E8u, "affich. des pts", "toast style"};

// Dynamic strings for Box 2 (0x8C303FE8..0x8C304097, after kBoxSelect4)
constexpr std::uint32_t kDynStringsBase = 0x8C303FE8u;
constexpr std::uint32_t kAchRowDynAddr = 0x8C303FE8u;
constexpr std::uint32_t kAchHeadDynAddr = 0x8C304008u;
constexpr std::uint32_t kRaStatusDynAddr = 0x8C304028u;
constexpr std::uint32_t kSyncRowDynAddr = 0x8C304058u;
constexpr std::uint32_t kSyncHeadDynAddr = 0x8C304078u;
constexpr unsigned kDynStringsTotalSize = 176;

void write_text(dream::Memory& m, std::uint32_t a, const char* s) {
    for (std::size_t i = 0;; ++i) {
        m.write8(a + static_cast<std::uint32_t>(i), static_cast<std::uint8_t>(s[i]));
        if (!s[i])
            return;
    }
}

namespace {

constexpr std::uint32_t kMenu = 0x8C3042BCu;  // nine 32-byte entries
constexpr std::uint32_t kReturnOne = 0x8C014EA4u;  // `rts; mov #1,r0` in the main program
constexpr unsigned kOrder[9] = {1, 2, 3, 4, 0, 5, 6, 7, 8};  // shown slot -> table slot
constexpr unsigned kItem = 4;                                  // where slot 0 is shown
constexpr std::uint32_t kItemId = 2, kOptionsId = 6;

constexpr std::uint32_t kRows = 0x8C3034A4u, kRowBytes = 40;  // 17 rows; the type at +24
constexpr std::uint32_t kHeading = 0x8C303B00u;               // six title pointers

constexpr unsigned kRowCount = 17, kRowWords = kRowBytes / 4;

bool g_on = true, g_page = false, g_menu = false;
std::uint32_t g_menu_state = 0;              // the menu's state, from recomp_menu_begin to _end
std::uint32_t g_table[9][8];                 // the menu table as the game has it, while reordered
std::uint32_t g_rows[kRowCount][kRowWords];  // the options rows, while the page is open
std::uint32_t g_set0[kSetWords], g_set2[kSetWords], g_box0[kBoxSelectWords], g_box1[kBoxSelectWords], g_box3[kBoxSelectWords], g_box4[kBoxSelectWords], g_box_items[kLanguageCount], g_heading[kLanguageCount];
char g_dyn_strings[kDynStringsTotalSize];

bool text_is(dream::Memory& m, std::uint32_t a, const char* s) {
    for (std::size_t i = 0;; ++i) {
        const char ch = static_cast<char>(m.read8(a + static_cast<std::uint32_t>(i)));
        if (ch != s[i])
            return false;
        if (!ch)
            return true;
    }
}

// The title overlay as on the European and Japanese discs: its menu table's ids, the options
// screen's labels, rows and heading. Anything else and the item is not offered.
bool overlay_as_read(dream::Memory& m) {
    for (unsigned i = 0; i < 9; ++i)
        if (m.read32(kMenu + 32 * i + 24) != 2 + i)
            return false;
    for (const Text& t : {kAspectRow, kSelectValue, kResRow, kResHead,
                          kHead, kFpsRow, kFpsHead, k30Fps, k60Fps,
                          kToastRow, kToastHead}) {
        if (!text_is(m, t.at, t.original))
            return false;
    }
    if (!text_is(m, kTitle.at, kTitle.original) && !text_is(m, kTitle.at, kTitle.page))
        return false;
    return m.read32(kRows + 24) == 1 && m.read32(kRows + 36) == 0 && m.read32(kRows + 16 * kRowBytes + 24) == 4;
}

void update_ra_rows(dream::Memory& m) {
    const std::string ra_status = dreamcomp::ra::menu_ra_status_label();
    const std::string sync_head = dreamcomp::ra::menu_sync_progress_header();

    write_text(m, kAchRowDynAddr, "RetroAchievements");
    write_text(m, kAchHeadDynAddr, "RetroAchievements");
    write_text(m, kRaStatusDynAddr, ra_status.c_str());
    write_text(m, kSyncRowDynAddr, "Sync RetroAchievements");
    write_text(m, kSyncHeadDynAddr, sync_head.c_str());

    constexpr std::uint32_t row5_addr = kRows + 5 * kRowBytes;
    constexpr std::uint32_t row6_addr = kRows + 6 * kRowBytes;
    constexpr std::uint32_t row7_addr = kRows + 7 * kRowBytes;

    if (dreamcomp::ra::is_enabled()) {
        for (unsigned i = 0; i < kLanguageCount; ++i) {
            m.write32(row5_addr + 4 * i, kRaStatusDynAddr);
            m.write32(row6_addr + 4 * i, kToastRow.at);
            m.write32(row7_addr + 4 * i, kSyncRowDynAddr);
        }
        m.write32(row5_addr + 24, kRowTypeNone);
        m.write32(row5_addr + 28, 0u);
        m.write32(row5_addr + 32, 0u);
        m.write32(row5_addr + 36, 0u);

        m.write32(row6_addr + 24, kRowTypeBoxSelect);
        m.write32(row6_addr + 28, 0u);
        m.write32(row6_addr + 32, 0u);
        m.write32(row6_addr + 36, 3u);

        m.write32(row7_addr + 24, kRowTypeBoxSelect);
        m.write32(row7_addr + 28, 0u);
        m.write32(row7_addr + 32, kSyncActionAddr);
        m.write32(row7_addr + 36, 4u);
    } else {
        for (unsigned w = 0; w < kRowWords; ++w) {
            m.write32(row5_addr + 4 * w, 0u);
            m.write32(row6_addr + 4 * w, 0u);
            m.write32(row7_addr + 4 * w, 0u);
        }
    }
}

// The page is the options screen with heading "recomp", Box 1 rows 0..2 ("aspect ratio",
// "resolution", "frame rate"), Box 2 rows 4..7 ("Retroachievements", "RA: <login_status>",
// "toast style", "Sync Retroachievements"), and row 16 "return".
void page_open(dream::Memory& m) {
    for (unsigned r = 0; r < kRowCount; ++r)
        for (unsigned w = 0; w < kRowWords; ++w)
            g_rows[r][w] = m.read32(kRows + r * kRowBytes + 4 * w);
    for (unsigned i = 0; i < kLanguageCount; ++i) {
        g_heading[i] = m.read32(kHeading + 4 * i);
        g_box_items[i] = m.read32(kBoxSelectItems + 24 * i);
    }
    for (unsigned i = 0; i < kSetWords; ++i) {
        g_set0[i] = m.read32(kSet0 + 4 * i);
        g_set2[i] = m.read32(kSet2 + 4 * i);
    }
    for (unsigned i = 0; i < kBoxSelectWords; ++i) {
        g_box0[i] = m.read32(kBoxSelect0 + 4 * i);
        g_box1[i] = m.read32(kBoxSelect1 + 4 * i);
        g_box3[i] = m.read32(kBoxSelect3 + 4 * i);
        g_box4[i] = m.read32(kBoxSelect4 + 4 * i);
    }
    for (unsigned i = 0; i < kDynStringsTotalSize; ++i)
        g_dyn_strings[i] = static_cast<char>(m.read8(kDynStringsBase + i));

    for (const Text& t : {kAspectRow, kResRow, kResHead, kTitle, kHead, kFpsRow, kFpsHead, k30Fps, k60Fps, kToastRow, kToastHead})
        write_text(m, t.at, t.page);

    for (unsigned r = 1; r + 1 < kRowCount; ++r)
        for (unsigned word = 0; word < kRowWords; ++word)
            m.write32(kRows + r * kRowBytes + 4 * word, 0);

    for (unsigned i = 0; i < kLanguageCount; ++i) {
        m.write32(kRows + 4 * i, kAspectRow.at);
        m.write32(kRows + kRowBytes + 4 * i, kResRow.at);
        m.write32(kRows + 2 * kRowBytes + 4 * i, kFpsRow.at);
        m.write32(kRows + 4 * kRowBytes + 4 * i, kAchRowDynAddr);

        m.write32(kHeading + 4 * i, kHead.at);

        m.write32(kBoxSelect0 + 4 * i, kAspectRow.at);
        m.write32(kBoxSelect1 + 4 * i, kResHead.at);
        m.write32(kBoxSelect3 + 4 * i, kToastHead.at);
        m.write32(kBoxSelect4 + 4 * i, kSyncHeadDynAddr);
        m.write32(kBoxSelectItems + 24 * i, kSelectValue.at);

        m.write32(kSet0 + 4 * i, kFpsHead.at);
        m.write32(kSet0 + 24 + 4 * i, k30Fps.at);
        m.write32(kSet0 + 48 + 4 * i, k60Fps.at);

        m.write32(kSet2 + 4 * i, kAchHeadDynAddr);
    }

    m.write32(kRows + 24, kRowTypeBoxSelect);
    m.write32(kRows + 36, 0u);
    m.write32(kRows + kRowBytes + 24, kRowTypeBoxSelect);
    m.write32(kRows + kRowBytes + 36, 1u);
    m.write32(kRows + 2 * kRowBytes + 24, kRowTypeBoxOnOff);
    m.write32(kRows + 2 * kRowBytes + 36, 0u);
    m.write32(kRows + 4 * kRowBytes + 24, kRowTypeBoxOnOff);
    m.write32(kRows + 4 * kRowBytes + 36, 2u);

    const auto fam = dreamcomp::current_aspect_family();
    const auto& res_opts = dreamcomp::resolution_options(fam);

    m.write32(kBoxSelect0 + 24, kBoxSelectItems);
    m.write32(kBoxSelect0 + 28, 0u);
    m.write32(kBoxSelect0 + 32, static_cast<std::uint32_t>(dreamcomp::AspectFamily::Count) - 1u);
    m.write32(kBoxSelect0 + 36, kSet0Getter);
    m.write32(kBoxSelect0 + 40, kSet0Setter);

    m.write32(kBoxSelect1 + 24, kBoxSelectItems);
    m.write32(kBoxSelect1 + 28, 0u);
    m.write32(kBoxSelect1 + 32, static_cast<std::uint32_t>(res_opts.size()) - 1u);
    m.write32(kBoxSelect1 + 36, kSet0Getter);
    m.write32(kBoxSelect1 + 40, kSet0Setter);

    m.write32(kBoxSelect3 + 24, kBoxSelectItems);
    m.write32(kBoxSelect3 + 28, 0u);
    m.write32(kBoxSelect3 + 32, static_cast<std::uint32_t>(dreamcomp::ra::ToastStyle::Count) - 1u);
    m.write32(kBoxSelect3 + 36, kSet0Getter);
    m.write32(kBoxSelect3 + 40, kSet0Setter);

    m.write32(kBoxSelect4 + 24, kBoxSelectItems);
    m.write32(kBoxSelect4 + 28, 0u);
    m.write32(kBoxSelect4 + 32, 0u);
    m.write32(kBoxSelect4 + 36, kSet0Getter);
    m.write32(kBoxSelect4 + 40, kSet0Setter);

    m.write32(kSet0 + 72, kSet0Getter);
    m.write32(kSet0 + 76, kSet0Setter);

    m.write32(kSet2 + 72, kSet0Getter);
    m.write32(kSet2 + 76, kSet0Setter);

    update_ra_rows(m);

    g_page = true;

    const unsigned cur_res = dreamcomp::current_resolution_index();
    const char* label = (cur_res < res_opts.size()) ? res_opts[cur_res].label : "unknown";
    std::printf("rez: recomp settings opened (aspect %s, res %s, %u fps, achievements %s, toast %s)\n",
                dreamcomp::aspect_family_label(fam), label, rez::native_sys_get_frame_rate(),
                dreamcomp::ra::is_enabled() ? "on" : "off",
                dreamcomp::ra::toast_style_label(dreamcomp::ra::toast_style()));
}

void page_close(dream::Memory& m) {
    m.write32(kBoxSelectArrowLeftIdx, 1u);
    m.write32(kBoxSelectArrowRightIdx, 2u);
    for (unsigned r = 0; r < kRowCount; ++r)
        for (unsigned w = 0; w < kRowWords; ++w)
            m.write32(kRows + r * kRowBytes + 4 * w, g_rows[r][w]);
    for (unsigned i = 0; i < kLanguageCount; ++i) {
        m.write32(kHeading + 4 * i, g_heading[i]);
        m.write32(kBoxSelectItems + 24 * i, g_box_items[i]);
    }
    for (unsigned i = 0; i < kSetWords; ++i) {
        m.write32(kSet0 + 4 * i, g_set0[i]);
        m.write32(kSet2 + 4 * i, g_set2[i]);
    }
    for (unsigned i = 0; i < kBoxSelectWords; ++i) {
        m.write32(kBoxSelect0 + 4 * i, g_box0[i]);
        m.write32(kBoxSelect1 + 4 * i, g_box1[i]);
        m.write32(kBoxSelect3 + 4 * i, g_box3[i]);
        m.write32(kBoxSelect4 + 4 * i, g_box4[i]);
    }
    for (unsigned i = 0; i < kDynStringsTotalSize; ++i)
        m.write8(kDynStringsBase + i, static_cast<std::uint8_t>(g_dyn_strings[i]));
    for (const Text& t : {kAspectRow, kSelectValue, kResRow, kResHead, kTitle, kHead, kFpsRow, kFpsHead, k30Fps, k60Fps, kToastRow, kToastHead})
        write_text(m, t.at, t.original);
    g_page = false;
}

}  // namespace

void recomp_settings_start(bool on) {
    g_on = on;
    std::printf("rez: recomp settings item %s\n", on ? "on" : "off");
}

// The main menu's table slot 0, "play" (id 2), is never shown (its availability function returns
// 0). While the menu's input and drawing run, the table is in the order travelling, play, score
// attack, options, slot 0 as "recomp settings", how to play, beyond, exit, DEMO, and the cursor
// follows that order; both are put back afterwards, so the places the game sets the cursor keep
// their meaning. Slot 8 stays DEMO: the drawing calls its availability function after its loop.
void recomp_menu_begin(dream::Memory& m, std::uint32_t menu) {
    if (!g_on || g_menu || !menu)
        return;
    if (g_page) {  // back from the page: its tables put back, the cursor on the item
        page_close(m);
        m.write32(menu + 32, 0);
    }
    if (!overlay_as_read(m))
        return;
    for (unsigned s = 0; s < 9; ++s)
        for (unsigned w = 0; w < 8; ++w)
            g_table[s][w] = m.read32(kMenu + 32 * s + 4 * w);
    write_text(m, kTitle.at, kTitle.page);
    for (unsigned i = 0; i < 9; ++i)
        for (unsigned w = 0; w < 8; ++w)
            m.write32(kMenu + 32 * i + 4 * w, g_table[kOrder[i]][w]);
    for (unsigned l = 0; l < 6; ++l)
        m.write32(kMenu + 32 * kItem + 4 * l, kTitle.at);
    m.write32(kMenu + 32 * kItem + 28, kReturnOne);
    const std::uint32_t cursor = m.read32(menu + 32);
    for (unsigned i = 0; i < 9; ++i)
        if (kOrder[i] == cursor) {
            m.write32(menu + 32, i);
            break;
        }
    g_menu = true;
    g_menu_state = menu;
}

void recomp_menu_chosen(dream::Memory& m) {
    if (!g_menu || m.read32(g_menu_state + 12) != kItemId)
        return;
    m.write32(g_menu_state + 12, kOptionsId);  // the options screen opens, as the page
    page_open(m);
}

void recomp_menu_end(dream::Memory& m) {
    if (!g_menu)
        return;
    const std::uint32_t cursor = m.read32(g_menu_state + 32);
    if (cursor < 9)
        m.write32(g_menu_state + 32, kOrder[cursor]);
    for (unsigned s = 0; s < 9; ++s)
        for (unsigned w = 0; w < 8; ++w)
            m.write32(kMenu + 32 * s + 4 * w, g_table[s][w]);
    if (!g_page)
        write_text(m, kTitle.at, kTitle.original);
    g_menu = false;
}

}  // namespace rez

// 0x8C301464: Choice getter.
DC_HOOK_ENTRY(recomp_get) {
    if (!rez::g_page)
        return false;
    rez::update_ra_rows(m);
    if (c.cycles >= c.next_event) {
        c.pc = 0x8C301464u;
        dream::sh4::deliver_irq(c, m);
    }
    if (c.pr == rez::kBoxSelectInputGetReturnAddr) {
        if (c.r[13] == rez::kBoxSelect4) {
            m.write32(rez::kBoxSelectArrowLeftIdx, 4u);
            m.write32(rez::kBoxSelectArrowRightIdx, 4u);
            c.r[0] = 0u;
        } else {
            m.write32(rez::kBoxSelectArrowLeftIdx, 1u);
            m.write32(rez::kBoxSelectArrowRightIdx, 2u);
            if (c.r[13] == rez::kBoxSelect0) {
                c.r[0] = static_cast<std::uint32_t>(dreamcomp::current_aspect_family());
            } else if (c.r[13] == rez::kBoxSelect1) {
                c.r[0] = dreamcomp::current_resolution_index();
            } else if (c.r[13] == rez::kBoxSelect3) {
                c.r[0] = static_cast<std::uint32_t>(dreamcomp::ra::toast_style());
            } else {
                c.r[0] = 0u;
            }
        }
    } else if (c.pr == rez::kBoxSelectDrawReturnAddr) {
        std::string dyn_label;
        const char* label = "";
        if (c.r[14] == rez::kBoxSelect0) {
            label = dreamcomp::aspect_family_label(dreamcomp::current_aspect_family());
        } else if (c.r[14] == rez::kBoxSelect1) {
            const auto& res_opts = dreamcomp::resolution_options(dreamcomp::current_aspect_family());
            const unsigned idx = dreamcomp::current_resolution_index();
            label = (idx < res_opts.size()) ? res_opts[idx].label : "";
        } else if (c.r[14] == rez::kBoxSelect3) {
            label = dreamcomp::ra::toast_style_label(dreamcomp::ra::toast_style());
        } else if (c.r[14] == rez::kBoxSelect4) {
            dyn_label = dreamcomp::ra::menu_sync_unsynced_label();
            label = dyn_label.c_str();
        }
        rez::write_text(m, rez::kSelectValue.at, label);
        c.r[0] = 0u;
    } else {
        const std::uint32_t set_addr =
            (c.pr == rez::kBoxOnOffDrawLeftReturnAddr || c.pr == rez::kBoxOnOffDrawRightReturnAddr) ? c.r[4] : c.r[14];
        if (set_addr == rez::kSet2) {
            c.r[0] = dreamcomp::ra::is_enabled() ? 1u : 0u;
        } else {
            c.r[0] = (rez::native_sys_get_frame_rate() == 60) ? 1u : 0u;
        }
    }
    c.cycles += dream::sh4::cost(8);
    return true;
}

// 0x8C30146A: Choice setter.
DC_HOOK_ENTRY(recomp_set) {
    if (!rez::g_page)
        return false;
    rez::update_ra_rows(m);
    if (c.cycles >= c.next_event) {
        c.pc = 0x8C30146Au;
        dream::sh4::deliver_irq(c, m);
    }
    if (c.pr == rez::kBoxSelectSetLeftReturnAddr || c.pr == rez::kBoxSelectSetRightReturnAddr) {
        if (c.r[13] == rez::kBoxSelect0) {
            dreamcomp::set_aspect_family(static_cast<dreamcomp::AspectFamily>(c.r[4]));
            m.write32(rez::kBoxSelect1 + 32,
                      static_cast<std::uint32_t>(dreamcomp::resolution_options(dreamcomp::current_aspect_family()).size()) - 1u);
            c.r[0] = 1u;
        } else if (c.r[13] == rez::kBoxSelect1) {
            dreamcomp::set_resolution_index(c.r[4]);
            c.r[0] = 1u;
        } else if (c.r[13] == rez::kBoxSelect3) {
            dreamcomp::ra::set_toast_style(static_cast<dreamcomp::ra::ToastStyle>(c.r[4]));
            c.r[0] = 1u;
        }
    } else if (c.r[14] == rez::kSet0) {
        const unsigned fps = c.r[4] ? 60u : 30u;
        rez::native_sys_frame_rate(fps);
        dreamcomp::settings().set("frame_rate", std::to_string(fps));
        dreamcomp::settings().save();
    } else if (c.r[14] == rez::kSet2) {
        dreamcomp::ra::set_enabled(c.r[4] != 0u);
        rez::update_ra_rows(m);
    }
    c.cycles += dream::sh4::cost(8);
    return true;
}

// 0x8C301094: Sync achievements action button.
DC_HOOK_ENTRY(recomp_sync_achievements) {
    if (!rez::g_page)
        return false;
    if (c.cycles >= c.next_event) {
        c.pc = 0x8C301094u;
        dream::sh4::deliver_irq(c, m);
    }
    dreamcomp::ra::sync_achievements_now();
    rez::update_ra_rows(m);
    c.cycles += dream::sh4::cost(4);
    return true;
}
