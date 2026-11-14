#include "dream/runtime/gdrom/disc.h"
#include "dreamcomp/settings.h"
#include "ra_client.h"
#include "ra_creds.h"
#include "ra_http.h"

#include <rc_api_runtime.h>
#include <rc_api_user.h>
#include <rc_client.h>
#include <rc_hash.h>
extern "C" {
#include "rhash/md5.h"
}

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

dream::gdrom::Disc* g_test_disc = nullptr;

struct TestTrackHandle {
    dream::gdrom::Disc* disc = nullptr;
    const dream::gdrom::Track* track = nullptr;
};

void* test_open_track(const char* path, uint32_t track) {
    (void)path;
    if (!g_test_disc) {
        return nullptr;
    }
    const auto& tracks = g_test_disc->tracks();
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
    return new TestTrackHandle{g_test_disc, target};
}

uint32_t test_first_track_sector(void* handle) {
    auto* h = static_cast<TestTrackHandle*>(handle);
    return h ? h->track->lba : 0;
}

size_t test_read_sector(void* handle, uint32_t sector, void* buffer, size_t requested_bytes) {
    auto* h = static_cast<TestTrackHandle*>(handle);
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

void test_close_track(void* handle) {
    delete static_cast<TestTrackHandle*>(handle);
}

std::string md5_hex(const uint8_t* ip256, const std::vector<uint8_t>& exe) {
    md5_state_t md5;
    md5_init(&md5);
    md5_append(&md5, ip256, 256);
    md5_append(&md5, exe.data(), static_cast<int>(exe.size()));
    uint8_t digest[16];
    md5_finish(&md5, digest);

    char hex[33];
    for (int i = 0; i < 16; ++i) {
        std::snprintf(&hex[i * 2], 3, "%02x", digest[i]);
    }
    hex[32] = '\0';
    return std::string(hex);
}

bool rc_hash_sh4_track(dream::gdrom::Disc& disc, std::string& out_hash) {
    g_test_disc = &disc;
    rc_hash_cdreader_t reader{};
    reader.open_track = &test_open_track;
    reader.first_track_sector = &test_first_track_sector;
    reader.read_sector = &test_read_sector;
    reader.close_track = &test_close_track;
    rc_hash_init_custom_cdreader(&reader);

    rc_hash_iterator_t iterator;
    rc_hash_initialize_iterator(&iterator, disc.path().c_str(), nullptr, 0);
    iterator.callbacks.cdreader = reader;
    char gen_hash[33] = {0};
    int hash_res = rc_hash_generate(gen_hash, RC_CONSOLE_DREAMCAST, &iterator);
    rc_hash_destroy_iterator(&iterator);
    g_test_disc = nullptr;

    if (hash_res != 0) {
        out_hash = gen_hash;
        return true;
    }
    return false;
}

}  // namespace

int main() {
    std::cout << "==> Running RA End-to-End Verification Tests..." << std::endl;

    const std::filesystem::path test_sandbox = std::filesystem::temp_directory_path() / "ra_test_e2e_sandbox";
    std::filesystem::remove_all(test_sandbox);
    std::filesystem::create_directories(test_sandbox / "conf");
    std::filesystem::create_directories(test_sandbox / "mnt/SDCARD/.userdata/shared");
    std::filesystem::create_directories(test_sandbox / "userdata/system");

    // Test 1: HttpEngine construction, cache_dir path check, user-agent verification
    {
        dreamcomp::ra::HttpEngine http(test_sandbox);
        assert(http.cache_dir() == test_sandbox / "ra_cache");
        assert(std::string(dreamcomp::ra::user_agent()).find("rez-recompiled/") != std::string::npos);
        assert(std::string(dreamcomp::ra::user_agent()).find("rcheevos") != std::string::npos);
        std::cout << "[PASS] Test 1: HttpEngine cache_dir and user_agent verified" << std::endl;
    }

    // Test 2: Local cache mock server: mock login, startsession, patchdata, badges, offline awards
    const std::string mock_hash = "0123456789abcdef0123456789abcdef";
    const std::string user = "Pobega";
    const std::string ukey = dreamcomp::ra::normalize_user_key(user);
    assert(ukey == "pobega");

    const std::filesystem::path offline_dir = test_sandbox / "offline_test";
    const std::filesystem::path cache_dir = offline_dir / "ra_cache";
    const std::filesystem::path badges_dir = cache_dir / "badges";
    std::filesystem::create_directories(badges_dir);

    {
        // Seed offline cache files
        {
            std::ofstream f(cache_dir / ("login_" + ukey + ".json"));
            f << "{\"Success\":true,\"User\":\"" << user << "\",\"Token\":\"mock_token\",\"Score\":100,\"SoftcoreScore\":50}";
        }
        {
            std::ofstream f(cache_dir / ("gameid_" + mock_hash + ".json"));
            f << "{\"Success\":true,\"GameID\":42}";
        }
        {
            const std::string ach_sets_json =
                "{\"Success\":true,\"GameId\":42,\"Title\":\"Rez\",\"ConsoleId\":40,"
                "\"ImageIconUrl\":\"/Images/000042.png\",\"Sets\":[{\"AchievementSetId\":1,\"GameId\":42,"
                "\"Title\":\"Rez\",\"Type\":\"core\",\"ImageIconUrl\":\"/Images/000042.png\","
                "\"Achievements\":[{\"ID\":99,\"Title\":\"Boot\",\"Description\":\"Boot\","
                "\"Points\":1,\"Type\":\"\",\"Author\":\"Dev\",\"BadgeName\":\"12345\",\"Flags\":3,"
                "\"Created\":0,\"Modified\":0,"
                "\"MemAddr\":\"0xH0001=1\"},{\"ID\":101,\"Title\":\"Area 1\",\"Description\":\"Clear\","
                "\"Points\":5,\"Type\":\"progression\",\"Author\":\"Dev\",\"BadgeName\":\"12346\",\"Flags\":3,"
                "\"Created\":0,\"Modified\":0,"
                "\"MemAddr\":\"0xH0002=1\"}],\"Leaderboards\":[]}]}";
            std::ofstream fh(cache_dir / ("achievementsets_h_" + mock_hash + ".json"));
            fh << ach_sets_json;
            std::ofstream fg(cache_dir / "achievementsets_g_42.json");
            fg << ach_sets_json;
        }
        {
            std::ofstream f(cache_dir / ("startsession_" + ukey + "_42.json"));
            f << "{\"Success\":true,\"Unlocks\":[{\"ID\":99,\"When\":1700000000}],\"HardcoreUnlocks\":[],\"ServerNow\":1700001000}";
        }
        {
            std::ofstream f(badges_dir / "12345.png");
            f << "MOCK_PNG_DATA";
        }

        dreamcomp::ra::HttpEngine http(offline_dir);
        http.set_offline_mode(true);

        std::vector<std::uint8_t> badge_bytes;
        assert(http.read_cached_badge_png("12345", badge_bytes));
        assert(!badge_bytes.empty());

        rc_client_t* client = rc_client_create(
            [](uint32_t, uint8_t*, uint32_t, rc_client_t*) -> uint32_t { return 0; },
            &dreamcomp::ra::HttpEngine::server_call_bridge);
        dreamcomp::ra::set_active_http_engine(&http);

        bool login_done = false;
        rc_client_begin_login_with_token(client, user.c_str(), "mock_token",
            [](int res, const char*, rc_client_t*, void* ud) {
                (void)res;
                assert(res == RC_OK);
                *static_cast<bool*>(ud) = true;
            }, &login_done);

        while (!login_done) {
            http.dispatch_callbacks();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        const rc_client_user_t* uinfo = rc_client_get_user_info(client);
        (void)uinfo;
        assert(uinfo != nullptr);
        assert(std::string(uinfo->username) == user);

        bool load_done = false;
        rc_client_begin_load_game(client, mock_hash.c_str(),
            [](int res, const char* err, rc_client_t*, void* ud) {
                if (res != RC_OK) {
                    std::cerr << "load_game err (" << res << "): " << (err ? err : "null") << std::endl;
                }
                assert(res == RC_OK);
                *static_cast<bool*>(ud) = true;
            }, &load_done);

        while (!load_done) {
            http.dispatch_callbacks();
            rc_client_idle(client);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        // Test queuing an offline award via post
        rc_api_award_achievement_request_t award_params{};
        award_params.username = user.c_str();
        award_params.api_token = "mock_token";
        award_params.achievement_id = 101;
        award_params.hardcore = 0;
        award_params.game_hash = mock_hash.c_str();
        rc_api_request_t req{};
        rc_api_init_award_achievement_request(&req, &award_params);

        bool award_done = false;
        http.post(&req, [](const rc_api_server_response_t* resp, void* ud) {
            (void)resp;
            assert(resp->http_status_code == 200);
            assert(std::string(resp->body).find("\"Success\":true") != std::string::npos);
            *static_cast<bool*>(ud) = true;
        }, &award_done);
        rc_api_destroy_request(&req);

        while (!award_done) {
            http.dispatch_callbacks();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        // Verify pending awards and unlocked cache are written
        assert(std::filesystem::exists(cache_dir / ("pending_awards_" + ukey + ".txt")));
        assert(std::filesystem::exists(cache_dir / ("unlocked_cache_" + ukey + ".txt")));

        // Verify startsession offline merges both unlock 99 and newly awarded 101
        rc_api_start_session_request_t sess_params{};
        sess_params.username = user.c_str();
        sess_params.api_token = "mock_token";
        sess_params.game_id = 42;
        rc_api_request_t sess_req{};
        rc_api_init_start_session_request(&sess_req, &sess_params);

        bool sess_done = false;
        http.post(&sess_req, [](const rc_api_server_response_t* resp, void* ud) {
            assert(resp->http_status_code == 200);
            const std::string b(resp->body, resp->body_length);
            assert(b.find("\"ID\":99") != std::string::npos);
            assert(b.find("\"ID\":101") != std::string::npos);
            *static_cast<bool*>(ud) = true;
        }, &sess_done);
        rc_api_destroy_request(&sess_req);

        while (!sess_done) {
            http.dispatch_callbacks();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        dreamcomp::ra::set_active_http_engine(nullptr);
        rc_client_destroy(client);
        std::cout << "[PASS] Test 2: Local cache mock server and offline queuing" << std::endl;
    }

    // Test 3: Online sync invocation and offline fallback safety
    {
        dreamcomp::ra::HttpEngine http(offline_dir);
        bool sync_done = false;
        http.sync_now("Pobega", "", "invalid_token", "", [&](const dreamcomp::ra::SyncResult& res) {
            (void)res;
            assert(!res.online);
            assert(res.remaining_count >= 1);
            sync_done = true;
        });

        const auto start_wait = std::chrono::steady_clock::now();
        while (!sync_done && (std::chrono::steady_clock::now() - start_wait < std::chrono::seconds(7))) {
            http.dispatch_callbacks();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        assert(sync_done);
        std::cout << "[PASS] Test 3: Online sync invocation and graceful fallback" << std::endl;
    }

    // Test 3b: Server username aliasing regression (login="Pobega", server="SerPbone")
    {
        const std::filesystem::path alias_dir = test_sandbox / "alias_test";
        const std::filesystem::path alias_cache = alias_dir / "ra_cache";
        std::filesystem::create_directories(alias_cache);

        dreamcomp::ra::HttpEngine http(alias_dir);
        http.set_offline_mode(true);

        // Offline award under u=SerPbone for achievement 101
        rc_api_award_achievement_request_t award101{};
        award101.username = "SerPbone";
        award101.api_token = "mock_token";
        award101.achievement_id = 101;
        award101.hardcore = 0;
        award101.game_hash = mock_hash.c_str();
        rc_api_request_t req101{};
        rc_api_init_award_achievement_request(&req101, &award101);

        bool award_done = false;
        http.post(&req101, [](const rc_api_server_response_t* resp, void* ud) {
            (void)resp;
            *static_cast<bool*>(ud) = true;
        }, &award_done);
        rc_api_destroy_request(&req101);

        while (!award_done) {
            http.dispatch_callbacks();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        // Post warning achievement 101000001 and verify it is ignored
        rc_api_award_achievement_request_t warn_award{};
        warn_award.username = "SerPbone";
        warn_award.api_token = "mock_token";
        warn_award.achievement_id = 101000001u;
        warn_award.hardcore = 0;
        warn_award.game_hash = mock_hash.c_str();
        rc_api_request_t warn_req{};
        rc_api_init_award_achievement_request(&warn_req, &warn_award);

        bool warn_done = false;
        http.post(&warn_req, [](const rc_api_server_response_t* resp, void* ud) {
            (void)resp;
            *static_cast<bool*>(ud) = true;
        }, &warn_done);
        rc_api_destroy_request(&warn_req);

        while (!warn_done) {
            http.dispatch_callbacks();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        assert(std::filesystem::exists(alias_cache / "pending_awards_serpbone.txt"));
        {
            std::ifstream f(alias_cache / "pending_awards_serpbone.txt");
            std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            assert(content.find("101") != std::string::npos);
            assert(content.find("101000001") == std::string::npos);
        }

        // Mock online transport hook to simulate login returning canonical User: SerPbone
        http.set_offline_mode(false);
        http.set_transport_hook([](const std::string& url, const std::string& post_data) -> std::pair<int, std::string> {
            (void)url;
            if (post_data.find("r=login") != std::string::npos) {
                return {200, "{\"Success\":true,\"User\":\"SerPbone\",\"Token\":\"srv_token\",\"Score\":100}"};
            }
            if (post_data.find("r=awardachievement") != std::string::npos) {
                assert(post_data.find("u=SerPbone") != std::string::npos);
                return {200, "{\"Success\":true,\"AchievementID\":101,\"Score\":100}"};
            }
            return {200, "{\"Success\":true}"};
        });

        bool sync_done = false;
        http.sync_now("Pobega", "", "srv_token", "", [&](const dreamcomp::ra::SyncResult& res) {
            (void)res;
            assert(res.online);
            assert(res.synced_count == 1);
            assert(res.remaining_count == 0);
            assert(res.resolved_server_username == "SerPbone");
            sync_done = true;
        });

        while (!sync_done) {
            http.dispatch_callbacks();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        assert(std::filesystem::exists(alias_cache / "unlocked_cache_serpbone.txt"));
        {
            std::ifstream f(alias_cache / "unlocked_cache_serpbone.txt");
            std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            assert(content.find("101") != std::string::npos);
            assert(content.find("101000001") == std::string::npos);
        }
        std::cout << "[PASS] Test 3b: Server username aliasing and warning achievement filtering" << std::endl;
    }

    // Test 4: Online startsession cache reconciliation removes stale local unlocks
    {
        // Populate unlocked cache with 99, 101, and stale 999
        {
            std::ofstream f(cache_dir / ("unlocked_cache_" + ukey + ".txt"));
            f << "99 1700000000\n101 1700000500\n999 1700000999\n";
        }
        // pending awards only contains 101
        {
            std::ofstream f(cache_dir / ("pending_awards_" + ukey + ".txt"));
            f << "101 1700000500 " << mock_hash << "\n";
        }

        // Server returns startsession with only unlock 99
        const std::string server_sess =
            "{\"Success\":true,\"Unlocks\":[{\"ID\":99,\"When\":1700000000}],\"HardcoreUnlocks\":[],\"ServerNow\":1700001000}";
        dreamcomp::ra::reconcile_unlocked_cache_from_session(cache_dir, ukey, server_sess);

        std::ifstream f(cache_dir / ("unlocked_cache_" + ukey + ".txt"));
        std::string line;
        bool has_99 = false;
        bool has_101 = false;
        bool has_999 = false;
        while (std::getline(f, line)) {
            if (line.find("99 ") == 0) has_99 = true;
            if (line.find("101 ") == 0) has_101 = true;
            if (line.find("999 ") == 0) has_999 = true;
        }
        (void)has_99;
        (void)has_101;
        (void)has_999;
        assert(has_99 && "Unlock 99 from server must be retained");
        assert(has_101 && "Pending award 101 must be retained");
        assert(!has_999 && "Stale unlock 999 must be removed during reconciliation");
        std::cout << "[PASS] Test 4: Online startsession cache reconciliation" << std::endl;
    }

    // Test 5: Disc hash calculation against CHD via rc_hash_sh4_track
    const std::filesystem::path chd_path = "Rez (Japan) (En,Ja,Fr,De,Es,It).chd";
    if (std::filesystem::exists(chd_path)) {
        std::string err;
        auto disc = dream::gdrom::open_disc(chd_path, err);
        assert(disc != nullptr && "Failed to open Rez CHD");

        std::vector<uint8_t> exe_bytes;
        std::string resolved_exe;
        bool read_ok = dream::gdrom::read_root_file(*disc, "1ST_READ.BIN", exe_bytes, &resolved_exe);
        (void)read_ok;
        assert(read_ok && "Failed to read 1ST_READ.BIN from CHD");
        assert(!exe_bytes.empty());

        uint8_t sec45000[2048];
        bool ip_ok = disc->read_user(45000, sec45000);
        (void)ip_ok;
        assert(ip_ok && "Failed to read LBA 45000 (IP.BIN)");

        const std::string expected_hash = md5_hex(sec45000, exe_bytes);
        std::string gen_hash;
        bool hash_ok = rc_hash_sh4_track(*disc, gen_hash);
        (void)hash_ok;
        assert(hash_ok && "rc_hash_sh4_track failed");
        assert(gen_hash == expected_hash);

        std::cout << "[PASS] Test 5: Disc hash calculation: " << gen_hash << std::endl;
    } else {
        std::cout << "[INFO] Test 5: Rez CHD not in working directory, skipping CHD hash verification" << std::endl;
    }

    // Test 6: 3-tier credentials resolution (SettingsIni > NextUi > RetroArchCfw)
    {
        dreamcomp::Settings s;
        s.load(test_sandbox / "conf" / "settings.ini");
        s.set("ra_username", "pobega_ini");
        s.set("ra_token", "token_ini_123");
        auto c1 = dreamcomp::ra::resolve_credentials(s, test_sandbox);
        assert(c1.source == dreamcomp::ra::CredentialSource::SettingsIni);
        assert(c1.username == "pobega_ini");
        assert(c1.token == "token_ini_123");

        dreamcomp::Settings empty_s;
        empty_s.load(test_sandbox / "conf" / "empty.ini");
        std::ofstream minui(test_sandbox / "mnt/SDCARD/.userdata/shared/minuisettings.txt");
        minui << "raUsername = \"pobega_nextui\"\n";
        minui << "raToken = \"token_nextui_456\"\n";
        minui.close();

        auto c2 = dreamcomp::ra::resolve_credentials(empty_s, test_sandbox);
        assert(c2.source == dreamcomp::ra::CredentialSource::NextUi);
        assert(c2.username == "pobega_nextui");
        assert(c2.token == "token_nextui_456");

        std::filesystem::remove(test_sandbox / "mnt/SDCARD/.userdata/shared/minuisettings.txt");
        std::ofstream bato(test_sandbox / "userdata/system/batocera.conf");
        bato << "global.retroachievements.username = pobega_cfw\n";
        bato << "global.retroachievements.token = token_cfw_789\n";
        bato.close();

        auto c3 = dreamcomp::ra::resolve_credentials(empty_s, test_sandbox);
        assert(c3.source == dreamcomp::ra::CredentialSource::RetroArchCfw);
        assert(c3.username == "pobega_cfw");
        assert(c3.token == "token_cfw_789");
        std::cout << "[PASS] Test 6: 3-tier credentials resolution verified" << std::endl;
    }

    // Test 7: Menu string helpers
    {
        assert(dreamcomp::ra::menu_ra_status_label() == "RA: No account setup");
        assert(dreamcomp::ra::menu_sync_progress_header() == "Progress (0/0)");
        assert(dreamcomp::ra::menu_sync_unsynced_label() == "Unsynced: 0");
        std::cout << "[PASS] Test 7: RA menu string helpers verified" << std::endl;
    }

    std::filesystem::remove_all(test_sandbox);
    std::cout << "==> All RA End-to-End Tests Passed!" << std::endl;
    return 0;
}
