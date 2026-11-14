#include "ra_http.h"

#include <rc_api_runtime.h>
#include <rc_api_user.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

namespace dreamcomp::ra {

std::string normalize_user_key(std::string_view user) {
    std::string out;
    out.reserve(user.size());
    for (char ch : user) {
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

namespace {

constexpr int kCurloptUrl = 10002;
constexpr int kCurloptPostfields = 10015;
constexpr int kCurloptPostfieldsize = 60;
constexpr int kCurloptWritefunction = 20011;
constexpr int kCurloptWritedata = 10001;
constexpr int kCurloptUseragent = 10018;
constexpr int kCurloptHttpheader = 10023;
constexpr int kCurloptConnecttimeout = 78;
constexpr int kCurloptTimeout = 13;
constexpr int kCurloptFollowlocation = 52;
constexpr int kCurloptNosignal = 99;
constexpr int kCurloptSslVerifypeer = 64;
constexpr int kCurloptSslVerifyhost = 81;
constexpr int kCurloptHttpGet = 80;
constexpr int kCurlinfoResponseCode = 0x200002;

constexpr long kConnectTimeoutSeconds = 5L;
constexpr long kTransferTimeoutSeconds = 15L;

constexpr int kCurlePeerFailedVerification = 60;
constexpr int kCurleSslCacertBadfile = 77;
constexpr int kHttpOk = 200;
constexpr int kHttpServerErrorThreshold = 500;
constexpr uint32_t kRaWarningAchievementMinId = 101000001u;

using fn_curl_global_init = int (*)(long);
using fn_curl_easy_init = void* (*)();
using fn_curl_easy_setopt = int (*)(void*, int, ...);
using fn_curl_easy_perform = int (*)(void*);
using fn_curl_easy_getinfo = int (*)(void*, int, ...);
using fn_curl_easy_cleanup = void (*)(void*);
using fn_curl_slist_append = void* (*)(void*, const char*);
using fn_curl_slist_free_all = void (*)(void*);

bool system_ca_certificates_exist() {
    std::error_code ec;
    return std::filesystem::exists("/etc/ssl/certs/ca-certificates.crt", ec) ||
           std::filesystem::exists("/etc/pki/tls/certs/ca-bundle.crt", ec);
}

size_t curl_write_memory_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buffer = static_cast<std::string*>(userdata);
    const size_t total_bytes = size * nmemb;
    buffer->append(ptr, total_bytes);
    return total_bytes;
}

std::string url_decode(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '+') {
            out += ' ';
        } else if (in[i] == '%' && i + 2 < in.size()) {
            const char hex_chars[3] = {in[i + 1], in[i + 2], '\0'};
            char* end_ptr = nullptr;
            const long parsed = std::strtol(hex_chars, &end_ptr, 16);
            if (end_ptr == hex_chars + 2) {
                out += static_cast<char>(parsed);
                i += 2;
            } else {
                out += in[i];
            }
        } else {
            out += in[i];
        }
    }
    return out;
}

std::string extract_query_param(std::string_view query, std::string_view key) {
    size_t pos = 0;
    while (pos < query.size()) {
        size_t next_delim = query.find('&', pos);
        if (next_delim == std::string_view::npos) {
            next_delim = query.size();
        }
        std::string_view item = query.substr(pos, next_delim - pos);
        const auto eq_idx = item.find('=');
        if (eq_idx != std::string_view::npos) {
            if (item.substr(0, eq_idx) == key) {
                return url_decode(item.substr(eq_idx + 1));
            }
        }
        pos = next_delim + 1;
    }
    return {};
}

std::string find_request_param(std::string_view post_data, std::string_view url, std::string_view key) {
    std::string val = extract_query_param(post_data, key);
    if (!val.empty()) {
        return val;
    }
    const auto question_idx = url.find('?');
    if (question_idx != std::string_view::npos) {
        return extract_query_param(url.substr(question_idx + 1), key);
    }
    return {};
}

std::string extract_badge_name(std::string_view url) {
    const std::string_view marker = "/Badge/";
    const auto marker_pos = url.find(marker);
    if (marker_pos == std::string_view::npos) {
        return {};
    }
    std::string_view badge = url.substr(marker_pos + marker.size());
    const auto question_pos = badge.find('?');
    if (question_pos != std::string_view::npos) {
        badge = badge.substr(0, question_pos);
    }
    return std::string(badge);
}

bool atomic_write_file(const std::filesystem::path& target_path, std::string_view content) {
    const std::filesystem::path tmp_path = target_path.string() + ".tmp";
    FILE* file = std::fopen(tmp_path.string().c_str(), "wb");
    if (!file) {
        return false;
    }

    if (!content.empty()) {
        const size_t written = std::fwrite(content.data(), 1, content.size(), file);
        if (written != content.size()) {
            std::fclose(file);
            std::error_code ec;
            std::filesystem::remove(tmp_path, ec);
            return false;
        }
    }

    std::fflush(file);
    const int fd = fileno(file);
    if (fd >= 0) {
        fsync(fd);
    }

    if (std::fclose(file) != 0) {
        std::error_code ec;
        std::filesystem::remove(tmp_path, ec);
        return false;
    }

    std::error_code ec;
    std::filesystem::rename(tmp_path, target_path, ec);
    return !ec;
}

bool read_file_string(const std::filesystem::path& file_path, std::string& out_content) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    out_content = ss.str();
    return true;
}

struct PendingAwardEntry {
    uint32_t achievement_id = 0;
    std::time_t unlocked_epoch = 0;
    std::string game_hash;
};

std::vector<PendingAwardEntry> load_pending_awards(const std::filesystem::path& file_path) {
    std::vector<PendingAwardEntry> entries;
    std::ifstream file(file_path);
    if (!file.is_open()) {
        return entries;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line.front() == '#') {
            continue;
        }
        std::istringstream iss(line);
        PendingAwardEntry entry;
        long long epoch = 0;
        if (iss >> entry.achievement_id >> epoch) {
            entry.unlocked_epoch = static_cast<std::time_t>(epoch);
            iss >> entry.game_hash;
            entries.push_back(std::move(entry));
        }
    }
    return entries;
}

bool save_pending_awards(const std::filesystem::path& file_path, const std::vector<PendingAwardEntry>& entries) {
    std::ostringstream oss;
    for (const auto& item : entries) {
        oss << item.achievement_id << " " << static_cast<long long>(item.unlocked_epoch) << " " << item.game_hash << "\n";
    }
    return atomic_write_file(file_path, oss.str());
}

void record_pending_award_entry(const std::filesystem::path& awards_file, uint32_t achievement_id,
                                const std::string& game_hash, std::time_t unlocked_epoch) {
    if (achievement_id == 0 || achievement_id >= kRaWarningAchievementMinId) {
        return;
    }
    std::vector<PendingAwardEntry> entries = load_pending_awards(awards_file);
    for (const auto& item : entries) {
        if (item.achievement_id == achievement_id) {
            return;
        }
    }
    PendingAwardEntry entry;
    entry.achievement_id = achievement_id;
    entry.unlocked_epoch = unlocked_epoch;
    entry.game_hash = game_hash;
    entries.push_back(std::move(entry));
    save_pending_awards(awards_file, entries);
}

struct UnlockedCacheEntry {
    uint32_t achievement_id = 0;
    std::time_t unlocked_epoch = 0;
};

std::vector<UnlockedCacheEntry> load_unlocked_cache(const std::filesystem::path& file_path) {
    std::vector<UnlockedCacheEntry> entries;
    std::ifstream file(file_path);
    if (!file.is_open()) {
        return entries;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line.front() == '#') {
            continue;
        }
        std::istringstream iss(line);
        UnlockedCacheEntry entry;
        long long epoch = 0;
        if (iss >> entry.achievement_id >> epoch) {
            entry.unlocked_epoch = static_cast<std::time_t>(epoch);
            entries.push_back(std::move(entry));
        }
    }
    return entries;
}

bool save_unlocked_cache(const std::filesystem::path& file_path, const std::vector<UnlockedCacheEntry>& entries) {
    std::ostringstream oss;
    for (const auto& item : entries) {
        oss << item.achievement_id << " " << static_cast<long long>(item.unlocked_epoch) << "\n";
    }
    return atomic_write_file(file_path, oss.str());
}

void record_unlocked_cache_entry(const std::filesystem::path& file_path, uint32_t achievement_id,
                                 std::time_t unlocked_epoch) {
    if (achievement_id == 0 || achievement_id >= kRaWarningAchievementMinId) {
        return;
    }
    std::vector<UnlockedCacheEntry> entries = load_unlocked_cache(file_path);
    for (const auto& item : entries) {
        if (item.achievement_id == achievement_id) {
            return;
        }
    }
    UnlockedCacheEntry entry;
    entry.achievement_id = achievement_id;
    entry.unlocked_epoch = unlocked_epoch;
    entries.push_back(std::move(entry));
    save_unlocked_cache(file_path, entries);
}

class CurlTransport {
public:
    struct Result {
        int http_status = -1;
        std::string body;
    };

    CurlTransport() {
        init_dlopen();
    }

    ~CurlTransport() {
        if (curl_ && fn_cleanup_) {
            fn_cleanup_(curl_);
            curl_ = nullptr;
        }
        if (dl_handle_) {
            dlclose(dl_handle_);
            dl_handle_ = nullptr;
        }
    }

    void set_hook(HttpEngine::TransportHook hook) {
        hook_ = std::move(hook);
    }

    Result perform(const std::string& url, const std::string& post_data, const std::string& content_type) {
        if (hook_) {
            auto res = hook_(url, post_data);
            return {res.first, std::move(res.second)};
        }
        if (curl_ && fn_setopt_ && fn_perform_ && fn_getinfo_) {
            return perform_libcurl(url, post_data, content_type);
        }
        return perform_exec(url, post_data, content_type);
    }

private:
    HttpEngine::TransportHook hook_;
    void* dl_handle_ = nullptr;
    void* curl_ = nullptr;
    fn_curl_global_init fn_global_init_ = nullptr;
    fn_curl_easy_init fn_init_ = nullptr;
    fn_curl_easy_setopt fn_setopt_ = nullptr;
    fn_curl_easy_perform fn_perform_ = nullptr;
    fn_curl_easy_getinfo fn_getinfo_ = nullptr;
    fn_curl_easy_cleanup fn_cleanup_ = nullptr;
    fn_curl_slist_append fn_slist_append_ = nullptr;
    fn_curl_slist_free_all fn_slist_free_all_ = nullptr;

    void init_dlopen() {
        const char* const lib_candidates[] = {"libcurl.so.4", "libcurl-gnutls.so.4", "libcurl.so"};
        for (const char* name : lib_candidates) {
            dl_handle_ = dlopen(name, RTLD_LAZY);
            if (dl_handle_) {
                break;
            }
        }
        if (!dl_handle_) {
            return;
        }

        fn_global_init_ = reinterpret_cast<fn_curl_global_init>(dlsym(dl_handle_, "curl_global_init"));
        fn_init_ = reinterpret_cast<fn_curl_easy_init>(dlsym(dl_handle_, "curl_easy_init"));
        fn_setopt_ = reinterpret_cast<fn_curl_easy_setopt>(dlsym(dl_handle_, "curl_easy_setopt"));
        fn_perform_ = reinterpret_cast<fn_curl_easy_perform>(dlsym(dl_handle_, "curl_easy_perform"));
        fn_getinfo_ = reinterpret_cast<fn_curl_easy_getinfo>(dlsym(dl_handle_, "curl_easy_getinfo"));
        fn_cleanup_ = reinterpret_cast<fn_curl_easy_cleanup>(dlsym(dl_handle_, "curl_easy_cleanup"));
        fn_slist_append_ = reinterpret_cast<fn_curl_slist_append>(dlsym(dl_handle_, "curl_slist_append"));
        fn_slist_free_all_ = reinterpret_cast<fn_curl_slist_free_all>(dlsym(dl_handle_, "curl_slist_free_all"));

        if (!fn_init_ || !fn_setopt_ || !fn_perform_ || !fn_getinfo_ || !fn_cleanup_) {
            dlclose(dl_handle_);
            dl_handle_ = nullptr;
            return;
        }

        if (fn_global_init_) {
            fn_global_init_(3L);
        }
        curl_ = fn_init_();
    }

    Result perform_libcurl(const std::string& url, const std::string& post_data, const std::string& content_type) {
        std::string response_body;
        fn_setopt_(curl_, kCurloptUrl, url.c_str());
        fn_setopt_(curl_, kCurloptUseragent, kUserAgent);
        fn_setopt_(curl_, kCurloptConnecttimeout, kConnectTimeoutSeconds);
        fn_setopt_(curl_, kCurloptTimeout, kTransferTimeoutSeconds);
        fn_setopt_(curl_, kCurloptFollowlocation, 1L);
        fn_setopt_(curl_, kCurloptNosignal, 1L);
        fn_setopt_(curl_, kCurloptWritefunction, &curl_write_memory_callback);
        fn_setopt_(curl_, kCurloptWritedata, &response_body);
        fn_setopt_(curl_, kCurloptSslVerifypeer, 1L);
        fn_setopt_(curl_, kCurloptSslVerifyhost, 2L);

        if (!post_data.empty()) {
            fn_setopt_(curl_, kCurloptPostfields, post_data.c_str());
            fn_setopt_(curl_, kCurloptPostfieldsize, static_cast<long>(post_data.size()));
        } else {
            fn_setopt_(curl_, kCurloptHttpGet, 1L);
        }

        void* headers_list = nullptr;
        std::string ct_header;
        if (!content_type.empty() && fn_slist_append_) {
            ct_header = "Content-Type: " + content_type;
            headers_list = fn_slist_append_(headers_list, ct_header.c_str());
            fn_setopt_(curl_, kCurloptHttpheader, headers_list);
        } else {
            fn_setopt_(curl_, kCurloptHttpheader, nullptr);
        }

        int perform_res = fn_perform_(curl_);
        if ((perform_res == kCurlePeerFailedVerification || perform_res == kCurleSslCacertBadfile) &&
            !system_ca_certificates_exist()) {
            response_body.clear();
            fn_setopt_(curl_, kCurloptSslVerifypeer, 0L);
            fn_setopt_(curl_, kCurloptSslVerifyhost, 0L);
            perform_res = fn_perform_(curl_);
        }

        long status_code = 0;
        fn_getinfo_(curl_, kCurlinfoResponseCode, &status_code);

        if (headers_list && fn_slist_free_all_) {
            fn_slist_free_all_(headers_list);
        }

        if (perform_res == 0) {
            return {static_cast<int>(status_code), std::move(response_body)};
        }
        return {-1, {}};
    }

    Result perform_exec(const std::string& url, const std::string& post_data, const std::string& content_type) {
        int stdin_fds[2];
        int stdout_fds[2];
        if (pipe(stdin_fds) != 0) {
            return {-1, {}};
        }
        if (pipe(stdout_fds) != 0) {
            close(stdin_fds[0]);
            close(stdin_fds[1]);
            return {-1, {}};
        }

        const pid_t pid = fork();
        if (pid < 0) {
            close(stdin_fds[0]);
            close(stdin_fds[1]);
            close(stdout_fds[0]);
            close(stdout_fds[1]);
            return {-1, {}};
        }

        if (pid == 0) {
            close(stdin_fds[1]);
            close(stdout_fds[0]);

            dup2(stdin_fds[0], STDIN_FILENO);
            dup2(stdout_fds[1], STDOUT_FILENO);
            close(stdin_fds[0]);
            close(stdout_fds[1]);

            const int dev_null = open("/dev/null", O_WRONLY);
            if (dev_null >= 0) {
                dup2(dev_null, STDERR_FILENO);
                close(dev_null);
            }

            std::vector<const char*> args;
            args.push_back("curl");
            args.push_back("-sS");
            args.push_back("--connect-timeout");
            args.push_back("5");
            args.push_back("--max-time");
            args.push_back("15");
            args.push_back("-L");
            args.push_back("-A");
            args.push_back(kUserAgent);
            args.push_back("-w");
            args.push_back("\n%{http_code}");

            if (!system_ca_certificates_exist()) {
                args.push_back("-k");
            }

            std::string ct_arg;
            if (!content_type.empty()) {
                ct_arg = "Content-Type: " + content_type;
                args.push_back("-H");
                args.push_back(ct_arg.c_str());
            }

            if (!post_data.empty()) {
                args.push_back("--data-binary");
                args.push_back("@-");
            }

            args.push_back(url.c_str());
            args.push_back(nullptr);

            execvp("curl", const_cast<char* const*>(args.data()));
            _exit(127);
        }

        close(stdin_fds[0]);
        close(stdout_fds[1]);

        if (!post_data.empty()) {
            size_t written_total = 0;
            while (written_total < post_data.size()) {
                const ssize_t res = write(stdin_fds[1], post_data.data() + written_total,
                                          post_data.size() - written_total);
                if (res <= 0) {
                    break;
                }
                written_total += static_cast<size_t>(res);
            }
        }
        close(stdin_fds[1]);

        std::string raw_output;
        char chunk[4096];
        ssize_t read_bytes = 0;
        while ((read_bytes = read(stdout_fds[0], chunk, sizeof(chunk))) > 0) {
            raw_output.append(chunk, static_cast<size_t>(read_bytes));
        }
        close(stdout_fds[0]);

        int wait_status = 0;
        waitpid(pid, &wait_status, 0);

        if (!WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0) {
            return {-1, {}};
        }

        const auto split_pos = raw_output.find_last_of('\n');
        if (split_pos == std::string::npos) {
            return {-1, {}};
        }

        std::string status_str = raw_output.substr(split_pos + 1);
        std::string payload = raw_output.substr(0, split_pos);
        int parsed_code = -1;
        try {
            parsed_code = std::stoi(status_str);
        } catch (...) {
            return {-1, {}};
        }

        return {parsed_code, std::move(payload)};
    }
};

HttpEngine* g_active_engine = nullptr;

}  // namespace

void reconcile_unlocked_cache_from_session(const std::filesystem::path& cache_dir, const std::string& ukey,
                                           const std::string& session_body) {
    rc_api_server_response_t srv_resp{};
    srv_resp.body = session_body.data();
    srv_resp.body_length = session_body.size();
    srv_resp.http_status_code = kHttpOk;

    rc_api_start_session_response_t sess_resp{};
    const int parse_res = rc_api_process_start_session_server_response(&sess_resp, &srv_resp);
    if (parse_res == RC_OK && sess_resp.response.succeeded) {
        std::map<uint32_t, std::time_t> unlock_map;
        if (sess_resp.unlocks) {
            for (uint32_t i = 0; i < sess_resp.num_unlocks; ++i) {
                const uint32_t id = sess_resp.unlocks[i].achievement_id;
                if (id > 0 && id < kRaWarningAchievementMinId) {
                    unlock_map[id] = sess_resp.unlocks[i].when;
                }
            }
        }
        if (sess_resp.hardcore_unlocks) {
            for (uint32_t i = 0; i < sess_resp.num_hardcore_unlocks; ++i) {
                const uint32_t id = sess_resp.hardcore_unlocks[i].achievement_id;
                if (id > 0 && id < kRaWarningAchievementMinId) {
                    unlock_map[id] = sess_resp.hardcore_unlocks[i].when;
                }
            }
        }

        const std::filesystem::path awards_file = cache_dir / ("pending_awards_" + ukey + ".txt");
        const auto pending = load_pending_awards(awards_file);
        for (const auto& entry : pending) {
            if (entry.achievement_id > 0 && entry.achievement_id < kRaWarningAchievementMinId) {
                if (unlock_map.find(entry.achievement_id) == unlock_map.end()) {
                    unlock_map[entry.achievement_id] = entry.unlocked_epoch;
                }
            }
        }

        std::vector<UnlockedCacheEntry> entries;
        entries.reserve(unlock_map.size());
        for (const auto& [id, epoch] : unlock_map) {
            entries.push_back({id, epoch});
        }
        const std::filesystem::path unl_cache_file = cache_dir / ("unlocked_cache_" + ukey + ".txt");
        save_unlocked_cache(unl_cache_file, entries);
        rc_api_destroy_start_session_response(&sess_resp);
    }
}

struct HttpEngine::Impl {
    struct ServerCallItem {
        std::string url;
        std::string post_data;
        std::string content_type;
        rc_client_server_callback_t callback = nullptr;
        void* callback_data = nullptr;
    };

    struct SyncItem {
        std::string username;
        std::string server_username;
        std::string token;
        std::string password;
        SyncCallback callback;
    };

    enum class TaskType {
        ServerCall,
        Sync,
        PrefetchBadges
    };

    struct Task {
        TaskType type = TaskType::ServerCall;
        ServerCallItem call_item;
        SyncItem sync_item;
        std::vector<std::string> prefetch_badges;
    };

    HttpEngine& parent;
    CurlTransport transport;
    std::filesystem::path cache_dir;
    std::filesystem::path badges_dir;

    std::thread worker;
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::deque<Task> queue;
    bool stopping = false;

    std::mutex completion_mutex;
    std::vector<std::function<void()>> completions;

    Impl(HttpEngine& p, const std::filesystem::path& cdir)
        : parent(p), cache_dir(cdir / "ra_cache"), badges_dir(cdir / "ra_cache" / "badges") {
        std::error_code ec;
        std::filesystem::create_directories(badges_dir, ec);
        worker = std::thread(&Impl::worker_loop, this);
    }

    ~Impl() {
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            stopping = true;
        }
        queue_cv.notify_all();
        if (worker.joinable()) {
            worker.join();
        }

        std::deque<Task> remaining_tasks;
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            remaining_tasks.swap(queue);
        }
        for (const auto& task : remaining_tasks) {
            if (task.type == TaskType::ServerCall && task.call_item.callback) {
                rc_api_server_response_t resp{};
                resp.http_status_code = -1;
                task.call_item.callback(&resp, task.call_item.callback_data);
            }
        }

        std::vector<std::function<void()>> remaining_completions;
        {
            std::lock_guard<std::mutex> lock(completion_mutex);
            remaining_completions.swap(completions);
        }
        for (auto& fn : remaining_completions) {
            if (fn) {
                fn();
            }
        }
    }

    void enqueue_task(Task task) {
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            queue.push_back(std::move(task));
        }
        queue_cv.notify_one();
    }

    void queue_completion(std::function<void()> fn) {
        std::lock_guard<std::mutex> lock(completion_mutex);
        completions.push_back(std::move(fn));
    }

    void deliver_server_response(const ServerCallItem& item, int http_status, std::string body) {
        queue_completion([item, http_status, b = std::move(body)]() {
            if (item.callback) {
                rc_api_server_response_t srv_resp{};
                srv_resp.body = b.data();
                srv_resp.body_length = b.size();
                srv_resp.http_status_code = http_status;
                item.callback(&srv_resp, item.callback_data);
            }
        });
    }

    void worker_loop() {
        while (true) {
            Task task;
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                queue_cv.wait(lock, [this]() { return stopping || !queue.empty(); });
                if (stopping && queue.empty()) {
                    break;
                }
                task = std::move(queue.front());
                queue.pop_front();
            }

            if (task.type == TaskType::ServerCall) {
                process_server_call(task.call_item);
            } else if (task.type == TaskType::Sync) {
                process_sync(task.sync_item);
            } else if (task.type == TaskType::PrefetchBadges) {
                process_prefetch_badges(task.prefetch_badges);
            }
        }
    }

    void process_prefetch_badges(const std::vector<std::string>& badge_names) {
        for (const auto& name : badge_names) {
            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                if (stopping) {
                    break;
                }
            }
            std::string filename = name;
            if (filename.size() < 4 || filename.substr(filename.size() - 4) != ".png") {
                filename += ".png";
            }
            const std::filesystem::path badge_path = badges_dir / filename;
            std::error_code ec;
            if (std::filesystem::exists(badge_path, ec)) {
                continue;
            }
            const std::string url = "https://media.retroachievements.org/Badge/" + filename;
            CurlTransport::Result res = transport.perform(url, "", "");
            if (res.http_status >= kHttpOk && res.http_status < 300 && !res.body.empty()) {
                atomic_write_file(badge_path, res.body);
            }
        }
    }

    void process_server_call(const ServerCallItem& item) {
        const std::string badge_name = extract_badge_name(item.url);
        if (!badge_name.empty()) {
            handle_badge_request(badge_name, item);
            return;
        }

        const std::string r_method = find_request_param(item.post_data, item.url, "r");
        const std::string u_user = find_request_param(item.post_data, item.url, "u");
        const std::string g_game = find_request_param(item.post_data, item.url, "g");
        const std::string m_hash = find_request_param(item.post_data, item.url, "m");
        const std::string a_ach = find_request_param(item.post_data, item.url, "a");

        if (parent.offline_mode()) {
            if (handle_offline_method(r_method, u_user, g_game, m_hash, a_ach, item)) {
                return;
            }
        }

        CurlTransport::Result result = transport.perform(item.url, item.post_data, item.content_type);
        const bool live_succeeded = (result.http_status >= kHttpOk && result.http_status < 300);

        if (live_succeeded) {
            handle_online_cache(r_method, u_user, g_game, m_hash, result.body);
            if (r_method == "awardachievement" && !a_ach.empty() && !u_user.empty()) {
                const uint32_t parsed_id = static_cast<uint32_t>(std::strtoul(a_ach.c_str(), nullptr, 10));
                const std::string ukey = normalize_user_key(u_user);
                record_unlocked_cache_entry(cache_dir / ("unlocked_cache_" + ukey + ".txt"), parsed_id, std::time(nullptr));
            }
            deliver_server_response(item, result.http_status, std::move(result.body));
            return;
        }

        const bool is_network_error = (result.http_status <= 0 || result.http_status >= kHttpServerErrorThreshold);
        if (is_network_error) {
            if (handle_offline_method(r_method, u_user, g_game, m_hash, a_ach, item)) {
                return;
            }
        }

        deliver_server_response(item, result.http_status, std::move(result.body));
    }

    void handle_badge_request(const std::string& badge_name, const ServerCallItem& item) {
        const std::filesystem::path badge_path = badges_dir / badge_name;
        std::string file_content;
        if (read_file_string(badge_path, file_content)) {
            deliver_server_response(item, kHttpOk, std::move(file_content));
            return;
        }

        CurlTransport::Result result = transport.perform(item.url, item.post_data, item.content_type);
        if (result.http_status >= kHttpOk && result.http_status < 300 && !result.body.empty()) {
            atomic_write_file(badge_path, result.body);
        }

        deliver_server_response(item, result.http_status, std::move(result.body));
    }

    void handle_online_cache(const std::string& method, const std::string& user,
                             const std::string& game_id, const std::string& hash,
                             const std::string& response_body) {
        const std::string ukey = normalize_user_key(user);
        if (method == "login2") {
            rc_api_server_response_t srv_resp{};
            srv_resp.body = response_body.data();
            srv_resp.body_length = response_body.size();
            srv_resp.http_status_code = kHttpOk;
            rc_api_login_response_t login_resp{};
            if (rc_api_process_login_server_response(&login_resp, &srv_resp) == RC_OK &&
                login_resp.response.succeeded) {
                const std::string canonical_user = (login_resp.username && *login_resp.username) ? login_resp.username : user;
                std::string sanitized = "{\"Success\":true,\"User\":\"" + canonical_user +
                                        "\",\"Token\":\"" + (login_resp.api_token ? login_resp.api_token : "") +
                                        "\",\"Score\":" + std::to_string(login_resp.score) +
                                        ",\"SoftcoreScore\":" + std::to_string(login_resp.score_softcore);
                if (login_resp.display_name && *login_resp.display_name) {
                    sanitized += ",\"DisplayName\":\"";
                    sanitized += login_resp.display_name;
                    sanitized += "\"";
                }
                sanitized += "}";
                atomic_write_file(cache_dir / ("login_" + ukey + ".json"), sanitized);
                rc_api_destroy_login_response(&login_resp);
            }
        } else if (method == "gameid" && !hash.empty()) {
            atomic_write_file(cache_dir / ("gameid_" + hash + ".json"), response_body);
        } else if (method == "achievementsets") {
            if (!hash.empty()) {
                atomic_write_file(cache_dir / ("achievementsets_h_" + hash + ".json"), response_body);
            }
            uint32_t parsed_game_id = 0;
            if (!game_id.empty()) {
                parsed_game_id = static_cast<uint32_t>(std::strtoul(game_id.c_str(), nullptr, 10));
            } else {
                rc_api_server_response_t srv_resp{};
                srv_resp.body = response_body.data();
                srv_resp.body_length = response_body.size();
                srv_resp.http_status_code = kHttpOk;
                rc_api_fetch_game_sets_response_t sets_resp{};
                if (rc_api_process_fetch_game_sets_server_response(&sets_resp, &srv_resp) == RC_OK) {
                    parsed_game_id = sets_resp.id;
                    rc_api_destroy_fetch_game_sets_response(&sets_resp);
                }
            }
            if (parsed_game_id > 0) {
                atomic_write_file(cache_dir / ("achievementsets_g_" + std::to_string(parsed_game_id) + ".json"),
                                  response_body);
            }
        } else if (method == "startsession" && !ukey.empty() && !game_id.empty()) {
            atomic_write_file(cache_dir / ("startsession_" + ukey + "_" + game_id + ".json"), response_body);
            reconcile_unlocked_cache_from_session(cache_dir, ukey, response_body);
        }
    }

    bool handle_offline_method(const std::string& method, const std::string& user,
                               const std::string& game_id, const std::string& hash,
                               const std::string& ach_id, const ServerCallItem& item) {
        const std::string ukey = normalize_user_key(user);
        if (method == "login2") {
            const std::filesystem::path login_path = cache_dir / ("login_" + ukey + ".json");
            std::string content;
            if (read_file_string(login_path, content)) {
                parent.set_offline_mode(true);
                deliver_server_response(item, kHttpOk, std::move(content));
                return true;
            }
        } else if (method == "gameid" && !hash.empty()) {
            const std::filesystem::path gid_path = cache_dir / ("gameid_" + hash + ".json");
            std::string content;
            if (read_file_string(gid_path, content)) {
                parent.set_offline_mode(true);
                deliver_server_response(item, kHttpOk, std::move(content));
                return true;
            }
        } else if (method == "achievementsets") {
            std::string content;
            bool found = false;
            if (!hash.empty()) {
                found = read_file_string(cache_dir / ("achievementsets_h_" + hash + ".json"), content);
            }
            if (!found && !game_id.empty()) {
                found = read_file_string(cache_dir / ("achievementsets_g_" + game_id + ".json"), content);
            }
            if (found) {
                parent.set_offline_mode(true);
                deliver_server_response(item, kHttpOk, std::move(content));
                return true;
            }
        } else if (method == "startsession" && !ukey.empty() && !game_id.empty()) {
            const std::filesystem::path sess_path = cache_dir / ("startsession_" + ukey + "_" + game_id + ".json");
            std::string raw_content;
            if (read_file_string(sess_path, raw_content)) {
                parent.set_offline_mode(true);
                rc_api_server_response_t srv_resp{};
                srv_resp.body = raw_content.data();
                srv_resp.body_length = raw_content.size();
                srv_resp.http_status_code = kHttpOk;
                rc_api_start_session_response_t sess_resp{};
                const int parse_res = rc_api_process_start_session_server_response(&sess_resp, &srv_resp);

                std::map<uint32_t, std::time_t> unlock_map;
                if (parse_res == RC_OK && sess_resp.unlocks) {
                    for (uint32_t i = 0; i < sess_resp.num_unlocks; ++i) {
                        unlock_map[sess_resp.unlocks[i].achievement_id] = sess_resp.unlocks[i].when;
                    }
                }

                const std::filesystem::path awards_file = cache_dir / ("pending_awards_" + ukey + ".txt");
                const auto pending = load_pending_awards(awards_file);
                for (const auto& entry : pending) {
                    if (unlock_map.find(entry.achievement_id) == unlock_map.end()) {
                        unlock_map[entry.achievement_id] = entry.unlocked_epoch;
                    }
                }

                const std::filesystem::path unl_cache_file = cache_dir / ("unlocked_cache_" + ukey + ".txt");
                const auto cached_unlocks = load_unlocked_cache(unl_cache_file);
                for (const auto& entry : cached_unlocks) {
                    if (unlock_map.find(entry.achievement_id) == unlock_map.end()) {
                        unlock_map[entry.achievement_id] = entry.unlocked_epoch;
                    }
                }

                std::ostringstream oss;
                oss << "{\"Success\":true,\"Unlocks\":[";
                bool first = true;
                for (const auto& [id, when_val] : unlock_map) {
                    if (!first) {
                        oss << ",";
                    }
                    first = false;
                    oss << "{\"ID\":" << id << ",\"When\":" << static_cast<long long>(when_val) << "}";
                }
                oss << "],\"HardcoreUnlocks\":[";
                if (parse_res == RC_OK && sess_resp.hardcore_unlocks) {
                    for (uint32_t i = 0; i < sess_resp.num_hardcore_unlocks; ++i) {
                        if (i > 0) {
                            oss << ",";
                        }
                        oss << "{\"ID\":" << sess_resp.hardcore_unlocks[i].achievement_id
                            << ",\"When\":" << static_cast<long long>(sess_resp.hardcore_unlocks[i].when) << "}";
                    }
                }
                const std::time_t server_now = (parse_res == RC_OK && sess_resp.server_now > 0)
                                                   ? sess_resp.server_now
                                                   : std::time(nullptr);
                oss << "],\"ServerNow\":" << static_cast<long long>(server_now) << "}";

                if (parse_res == RC_OK) {
                    rc_api_destroy_start_session_response(&sess_resp);
                }

                deliver_server_response(item, kHttpOk, oss.str());
                return true;
            }
        } else if (method == "ping") {
            parent.set_offline_mode(true);
            deliver_server_response(item, kHttpOk, "{\"Success\":true}");
            return true;
        } else if (method == "awardachievement" && !ach_id.empty()) {
            parent.set_offline_mode(true);
            const uint32_t parsed_id = static_cast<uint32_t>(std::strtoul(ach_id.c_str(), nullptr, 10));
            const std::time_t now = std::time(nullptr);
            const std::filesystem::path awards_file = cache_dir / ("pending_awards_" + ukey + ".txt");
            record_pending_award_entry(awards_file, parsed_id, hash, now);
            const std::filesystem::path unl_cache_file = cache_dir / ("unlocked_cache_" + ukey + ".txt");
            record_unlocked_cache_entry(unl_cache_file, parsed_id, now);

            const std::string payload = "{\"Success\":true,\"AchievementID\":" + ach_id +
                                        ",\"Score\":0,\"SoftcoreScore\":0,\"AchievementsRemaining\":0}";
            deliver_server_response(item, kHttpOk, payload);
            return true;
        }

        return false;
    }

    std::vector<std::string> collect_candidate_user_keys(std::initializer_list<std::string_view> users) const {
        std::vector<std::string> keys;
        for (std::string_view u : users) {
            if (u.empty()) {
                continue;
            }
            const std::string k = normalize_user_key(u);
            if (!k.empty() && std::find(keys.begin(), keys.end(), k) == keys.end()) {
                keys.push_back(k);
            }
        }
        return keys;
    }

    std::vector<PendingAwardEntry> load_deduped_pending_awards(const std::vector<std::string>& keys) const {
        std::vector<PendingAwardEntry> result;
        std::set<uint32_t> seen_ids;
        for (const auto& k : keys) {
            const std::filesystem::path f = cache_dir / ("pending_awards_" + k + ".txt");
            const auto list = load_pending_awards(f);
            for (const auto& entry : list) {
                if (entry.achievement_id > 0 && entry.achievement_id < kRaWarningAchievementMinId) {
                    if (seen_ids.insert(entry.achievement_id).second) {
                        result.push_back(entry);
                    }
                }
            }
        }
        return result;
    }

    void process_sync(const SyncItem& item) {
        rc_api_login_request_t login_params{};
        login_params.username = item.username.c_str();
        if (!item.token.empty()) {
            login_params.api_token = item.token.c_str();
        } else if (!item.password.empty()) {
            login_params.password = item.password.c_str();
        }
        rc_api_request_t login_req{};
        rc_api_init_login_request(&login_req, &login_params);

        CurlTransport::Result login_res = transport.perform(
            login_req.url ? login_req.url : "",
            login_req.post_data ? login_req.post_data : "",
            login_req.content_type ? login_req.content_type : "");
        rc_api_destroy_request(&login_req);

        rc_api_server_response_t srv_resp{};
        srv_resp.body = login_res.body.data();
        srv_resp.body_length = login_res.body.size();
        srv_resp.http_status_code = login_res.http_status;
        rc_api_login_response_t login_resp{};
        const int parse_res = rc_api_process_login_server_response(&login_resp, &srv_resp);
        const bool is_online = (login_res.http_status >= kHttpOk && login_res.http_status < 300 &&
                                parse_res == RC_OK && login_resp.response.succeeded);

        const auto pre_keys = collect_candidate_user_keys({item.server_username, item.username});
        const auto pre_pending = load_deduped_pending_awards(pre_keys);

        if (!is_online) {
            std::string err = (login_resp.response.error_message && *login_resp.response.error_message)
                                  ? login_resp.response.error_message
                                  : "Offline: server unavailable";
            rc_api_destroy_login_response(&login_resp);
            SyncResult res{false, 0, static_cast<unsigned int>(pre_pending.size()), std::move(err), "", ""};
            queue_completion([cb = item.callback, res]() {
                if (cb) {
                    cb(res);
                }
            });
            return;
        }

        const std::string canonical_user = (login_resp.username && *login_resp.username)
                                               ? login_resp.username
                                               : (!item.server_username.empty() ? item.server_username : item.username);
        const std::string primary_ukey = normalize_user_key(canonical_user);
        const auto active_keys = collect_candidate_user_keys({canonical_user, item.server_username, item.username});
        const auto pending = load_deduped_pending_awards(active_keys);

        parent.set_offline_mode(false);
        handle_online_cache("login2", item.username, {}, {}, login_res.body);
        if (normalize_user_key(item.username) != primary_ukey) {
            handle_online_cache("login2", canonical_user, {}, {}, login_res.body);
        }
        const std::string resolved_token = (login_resp.api_token && *login_resp.api_token) ? login_resp.api_token : item.token;
        rc_api_destroy_login_response(&login_resp);

        unsigned int synced_count = 0;
        std::vector<PendingAwardEntry> remaining;
        const std::time_t now = std::time(nullptr);
        const std::filesystem::path unl_cache_file = cache_dir / ("unlocked_cache_" + primary_ukey + ".txt");

        for (const auto& entry : pending) {
            rc_api_award_achievement_request_t award_params{};
            award_params.username = canonical_user.c_str();
            award_params.api_token = resolved_token.c_str();
            award_params.achievement_id = entry.achievement_id;
            award_params.hardcore = 0;
            award_params.game_hash = entry.game_hash.c_str();
            const long long elapsed = std::max<long long>(0, now - entry.unlocked_epoch);
            award_params.seconds_since_unlock = static_cast<uint32_t>(elapsed);

            rc_api_request_t award_req{};
            rc_api_init_award_achievement_request(&award_req, &award_params);
            CurlTransport::Result award_res = transport.perform(
                award_req.url ? award_req.url : "",
                award_req.post_data ? award_req.post_data : "",
                award_req.content_type ? award_req.content_type : "");
            rc_api_destroy_request(&award_req);

            bool acknowledged = false;
            if (award_res.http_status >= kHttpOk && award_res.http_status < 300) {
                if (award_res.body.find("\"Success\":true") != std::string::npos ||
                    award_res.body.find("User already has") != std::string::npos) {
                    acknowledged = true;
                }
            }

            if (acknowledged) {
                record_unlocked_cache_entry(unl_cache_file, entry.achievement_id, entry.unlocked_epoch);
                ++synced_count;
            } else {
                remaining.push_back(entry);
            }
        }

        const std::filesystem::path primary_awards_file = cache_dir / ("pending_awards_" + primary_ukey + ".txt");
        save_pending_awards(primary_awards_file, remaining);

        for (const auto& k : active_keys) {
            if (k != primary_ukey) {
                const std::filesystem::path sec_file = cache_dir / ("pending_awards_" + k + ".txt");
                std::error_code ec;
                std::filesystem::remove(sec_file, ec);
            }
        }

        SyncResult res{true, synced_count, static_cast<unsigned int>(remaining.size()), "", resolved_token, canonical_user};
        queue_completion([cb = item.callback, res]() {
            if (cb) {
                cb(res);
            }
        });
    }
};

HttpEngine::HttpEngine(const std::filesystem::path& cdir)
    : cache_dir_(cdir / "ra_cache"), impl_(std::make_unique<Impl>(*this, cdir)) {}

HttpEngine::~HttpEngine() = default;

void HttpEngine::set_transport_hook(TransportHook hook) {
    if (impl_) {
        impl_->transport.set_hook(std::move(hook));
    }
}

void HttpEngine::post(const rc_api_request_t* request, rc_client_server_callback_t callback, void* callback_data) {
    if (!request) {
        if (callback) {
            rc_api_server_response_t resp{};
            resp.http_status_code = -1;
            callback(&resp, callback_data);
        }
        return;
    }
    Impl::Task task;
    task.type = Impl::TaskType::ServerCall;
    task.call_item.url = request->url ? request->url : "";
    task.call_item.post_data = request->post_data ? request->post_data : "";
    task.call_item.content_type = request->content_type ? request->content_type : "";
    task.call_item.callback = callback;
    task.call_item.callback_data = callback_data;
    impl_->enqueue_task(std::move(task));
}

void HttpEngine::server_call_bridge(const rc_api_request_t* request, rc_client_server_callback_t callback,
                                   void* callback_data, rc_client_t* client) {
    (void)client;
    HttpEngine* engine = active_http_engine();
    if (engine) {
        engine->post(request, callback, callback_data);
    } else if (callback) {
        rc_api_server_response_t resp{};
        resp.http_status_code = -1;
        callback(&resp, callback_data);
    }
}

void HttpEngine::sync_now(const std::string& username, const std::string& server_username,
                          const std::string& token, const std::string& password, SyncCallback callback) {
    Impl::Task task;
    task.type = Impl::TaskType::Sync;
    task.sync_item.username = username;
    task.sync_item.server_username = server_username;
    task.sync_item.token = token;
    task.sync_item.password = password;
    task.sync_item.callback = std::move(callback);
    impl_->enqueue_task(std::move(task));
}

void HttpEngine::dispatch_callbacks() {
    std::vector<std::function<void()>> ready;
    {
        std::lock_guard<std::mutex> lock(impl_->completion_mutex);
        ready.swap(impl_->completions);
    }
    for (auto& fn : ready) {
        if (fn) {
            fn();
        }
    }
}

bool HttpEngine::fetch_badge_image_sync(const std::string& badge_name, std::vector<uint8_t>& out_png) {
    const std::filesystem::path badge_path = impl_->badges_dir / badge_name;
    std::string content;
    if (read_file_string(badge_path, content)) {
        out_png.assign(content.begin(), content.end());
        return true;
    }
    const std::string url = "https://media.retroachievements.org/Badge/" + badge_name;
    CurlTransport::Result res = impl_->transport.perform(url, "", "");
    if (res.http_status >= kHttpOk && res.http_status < 300 && !res.body.empty()) {
        atomic_write_file(badge_path, res.body);
        out_png.assign(res.body.begin(), res.body.end());
        return true;
    }
    return false;
}

bool HttpEngine::read_cached_badge_png(const std::string& badge_name, std::vector<std::uint8_t>& out_png) const {
    std::string filename = badge_name;
    if (filename.size() < 4 || filename.substr(filename.size() - 4) != ".png") {
        filename += ".png";
    }
    const std::filesystem::path badge_path = impl_->badges_dir / filename;
    std::string content;
    if (read_file_string(badge_path, content)) {
        out_png.assign(content.begin(), content.end());
        return true;
    }
    return false;
}

void HttpEngine::prefetch_badges_async(std::vector<std::string> badge_names) {
    Impl::Task task;
    task.type = Impl::TaskType::PrefetchBadges;
    task.prefetch_badges = std::move(badge_names);
    impl_->enqueue_task(std::move(task));
}

void set_active_http_engine(HttpEngine* engine) {
    g_active_engine = engine;
}

HttpEngine* active_http_engine() {
    return g_active_engine;
}

}  // namespace dreamcomp::ra
