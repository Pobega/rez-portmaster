#pragma once

#include <rc_client.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dreamcomp::ra {

struct SyncResult {
    bool online = false;
    unsigned int synced_count = 0;
    unsigned int remaining_count = 0;
    std::string error;
    std::string resolved_token;
};

using SyncCallback = std::function<void(const SyncResult&)>;

std::string normalize_user_key(std::string_view user);

class HttpEngine {
public:
    explicit HttpEngine(const std::filesystem::path& cache_dir);
    ~HttpEngine();

    HttpEngine(const HttpEngine&) = delete;
    HttpEngine& operator=(const HttpEngine&) = delete;

    void post(const rc_api_request_t* request, rc_client_server_callback_t callback, void* callback_data);

    static void server_call_bridge(const rc_api_request_t* request, rc_client_server_callback_t callback,
                                   void* callback_data, rc_client_t* client);

    void sync_now(const std::string& username, const std::string& server_username,
                  const std::string& token, const std::string& password, SyncCallback callback);

    bool offline_mode() const noexcept { return offline_mode_.load(std::memory_order_relaxed); }
    void set_offline_mode(bool offline) noexcept { offline_mode_.store(offline, std::memory_order_relaxed); }

    const std::filesystem::path& cache_dir() const noexcept { return cache_dir_; }

    void dispatch_callbacks();

    bool fetch_badge_image_sync(const std::string& badge_name, std::vector<uint8_t>& out_png);
    bool read_cached_badge_png(const std::string& badge_name, std::vector<std::uint8_t>& out_png) const;
    void prefetch_badges_async(std::vector<std::string> badge_names);

private:
    struct Impl;
    std::filesystem::path cache_dir_;
    std::atomic<bool> offline_mode_{false};
    std::unique_ptr<Impl> impl_;
};

void set_active_http_engine(HttpEngine* engine);
HttpEngine* active_http_engine();

}  // namespace dreamcomp::ra
