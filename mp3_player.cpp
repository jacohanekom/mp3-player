/**
 * mp3_player.cpp
 * ==============
 * HTTP-controlled MP3 jukebox: upload files, list the library, play a
 * specific file, or play a random one. Decoding uses the vendored
 * public-domain minimp3 (no external decode library); output goes through
 * ALSA (libasound2).
 *
 * No web UI — this is a JSON API, driven by curl or scripts.
 *
 * Endpoints (default port 8566):
 *   GET  /files                    List library files: [{"name":..,"size":..}]
 *   POST /upload?name=song.mp3     Body = raw MP3 bytes (not multipart)
 *   POST /play?name=song.mp3       Play a specific library file
 *   POST /play/random              Play a random library file
 *   POST /stop                     Stop current playback
 *   GET  /status                   {"playing":bool,"file":..,"elapsed_s":..,"duration_s":..}
 *
 * Build (Linux / Raspberry Pi):
 *   cmake -B build && cmake --build build -j$(nproc)
 * Run:
 *   ./build/mp3_player [--config config.ini]
 * Example:
 *   curl --data-binary @song.mp3 "http://127.0.0.1:8566/upload?name=song.mp3"
 *   curl "http://127.0.0.1:8566/files"
 *   curl -X POST "http://127.0.0.1:8566/play?name=song.mp3"
 *   curl -X POST "http://127.0.0.1:8566/play/random"
 *   curl "http://127.0.0.1:8566/status"
 */

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <random>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <alsa/asoundlib.h>

#define MINIMP3_IMPLEMENTATION
#include "minimp3_ex.h"

#include "config.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Global stop flag
// ─────────────────────────────────────────────────────────────────────────────
static std::atomic<bool> g_stop{false};
static void signal_handler(int) { g_stop = true; }

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────
static std::string json_escape(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// Only a bare filename ending in .mp3 — blocks path traversal and hidden files.
static bool is_safe_mp3_name(const std::string &name) {
    if (name.empty() || name.size() > 255) return false;
    if (name.find('/') != std::string::npos) return false;
    if (name.find('\\') != std::string::npos) return false;
    if (name[0] == '.') return false;
    if (name.size() < 5) return false;
    std::string ext = name.substr(name.size() - 4);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".mp3";
}

static std::string url_decode(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() &&
            isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else if (s[i] == '+') {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

static std::string query_param(const std::string &query, const std::string &key) {
    size_t pos = 0;
    while (pos < query.size()) {
        size_t amp = query.find('&', pos);
        std::string pair = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        size_t eq = pair.find('=');
        if (eq != std::string::npos && pair.substr(0, eq) == key)
            return url_decode(pair.substr(eq + 1));
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return "";
}

static std::vector<std::string> list_mp3_files(const std::string &dir) {
    std::vector<std::string> files;
    DIR *d = opendir(dir.c_str());
    if (!d) return files;
    struct dirent *ent;
    while ((ent = readdir(d)) != nullptr) {
        std::string name = ent->d_name;
        if (is_safe_mp3_name(name)) files.push_back(name);
    }
    closedir(d);
    std::sort(files.begin(), files.end());
    return files;
}

// ─────────────────────────────────────────────────────────────────────────────
// Player — decodes a whole file via minimp3, streams PCM to ALSA.
// Runs on a single background thread so a new play request interrupts
// whatever is currently playing.
// ─────────────────────────────────────────────────────────────────────────────
class Player {
public:
    explicit Player(std::string alsa_device) : alsa_device_(std::move(alsa_device)) {}

    void start() { thread_ = std::thread(&Player::run, this); }
    void join()  { if (thread_.joinable()) thread_.join(); }

    void requestPlay(const std::string &path) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            pending_ = path;
        }
        interrupt_ = true;
        cv_.notify_all();
    }

    void requestStop() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            pending_.clear();
        }
        interrupt_ = true;
        cv_.notify_all();
    }

    struct Status {
        bool        playing;
        std::string file;
        double      elapsed_s;
        double      duration_s;
    };

    Status status() const {
        std::lock_guard<std::mutex> lk(mu_);
        return { playing_, current_file_, elapsed_s_, duration_s_ };
    }

private:
    void run() {
        while (!g_stop) {
            std::string file;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [&] { return g_stop.load() || !pending_.empty(); });
                if (g_stop) break;
                file = pending_;
                pending_.clear();
            }
            interrupt_ = false;
            playFile(file);
        }
    }

    void playFile(const std::string &path) {
        mp3dec_t dec;
        mp3dec_file_info_t info{};
        mp3dec_init(&dec);

        if (mp3dec_load(&dec, path.c_str(), &info, nullptr, nullptr) != 0 || !info.buffer) {
            std::cerr << "[Player] Failed to decode: " << path << "\n";
            if (info.buffer) free(info.buffer);
            return;
        }
        if (info.channels <= 0 || info.hz <= 0 || info.samples == 0) {
            std::cerr << "[Player] Empty/invalid decode: " << path << "\n";
            free(info.buffer);
            return;
        }

        snd_pcm_t *pcm = nullptr;
        int err = snd_pcm_open(&pcm, alsa_device_.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
        if (err < 0) {
            std::cerr << "[ALSA] open(" << alsa_device_ << "): " << snd_strerror(err) << "\n";
            free(info.buffer);
            return;
        }
        err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                                  static_cast<unsigned>(info.channels),
                                  static_cast<unsigned>(info.hz),
                                  1 /*allow resample*/, 200000 /*200ms latency*/);
        if (err < 0) {
            std::cerr << "[ALSA] set_params: " << snd_strerror(err) << "\n";
            snd_pcm_close(pcm);
            free(info.buffer);
            return;
        }

        const size_t total_frames = info.samples / static_cast<size_t>(info.channels);
        const size_t chunk_frames = 4096;

        {
            std::lock_guard<std::mutex> lk(mu_);
            current_file_ = path.substr(path.find_last_of('/') + 1);
            duration_s_   = static_cast<double>(total_frames) / info.hz;
            elapsed_s_    = 0;
            playing_      = true;
        }
        std::cerr << "[Player] Playing " << current_file_
                  << " (" << info.channels << "ch " << info.hz << "Hz, "
                  << duration_s_ << "s)\n";

        size_t pos = 0;
        while (pos < total_frames && !interrupt_ && !g_stop) {
            size_t want = std::min(chunk_frames, total_frames - pos);
            snd_pcm_sframes_t written = snd_pcm_writei(
                pcm, info.buffer + pos * static_cast<size_t>(info.channels), want);
            if (written < 0) {
                written = snd_pcm_recover(pcm, static_cast<int>(written), 1);
                if (written < 0) {
                    std::cerr << "[ALSA] write error: " << snd_strerror(static_cast<int>(written)) << "\n";
                    break;
                }
                continue;
            }
            pos += static_cast<size_t>(written);
            {
                std::lock_guard<std::mutex> lk(mu_);
                elapsed_s_ = static_cast<double>(pos) / info.hz;
            }
        }

        if (interrupt_ || g_stop) snd_pcm_drop(pcm);
        else                      snd_pcm_drain(pcm);
        snd_pcm_close(pcm);
        free(info.buffer);

        {
            std::lock_guard<std::mutex> lk(mu_);
            playing_ = false;
            current_file_.clear();
            elapsed_s_  = 0;
            duration_s_ = 0;
        }
        std::cerr << "[Player] Stopped\n";
    }

    std::string             alsa_device_;
    mutable std::mutex      mu_;
    std::condition_variable cv_;
    std::string             pending_;
    std::atomic<bool>       interrupt_{false};

    bool        playing_    = false;
    std::string current_file_;
    double      elapsed_s_  = 0;
    double      duration_s_ = 0;

    std::thread thread_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Minimal HTTP/1.1 server — one thread per connection, "Connection: close".
// ─────────────────────────────────────────────────────────────────────────────
class HttpServer {
public:
    HttpServer(int port, std::string library_dir, size_t max_upload_bytes, Player &player)
        : port_(port), library_dir_(std::move(library_dir)),
          max_upload_bytes_(max_upload_bytes), player_(player) {}

    ~HttpServer() { if (listenFd_ >= 0) ::close(listenFd_); }

    void start() {
        listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int opt = 1;
        ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port        = htons(static_cast<uint16_t>(port_));
        ::bind(listenFd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
        ::listen(listenFd_, 16);
        thread_ = std::thread(&HttpServer::acceptLoop, this);
        thread_.detach();
        std::cerr << "[HTTP] Listening on 0.0.0.0:" << port_ << "\n";
    }

private:
    struct Conn {
        int         fd;
        std::string buf;

        bool fill() {
            char tmp[8192];
            ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
            if (n <= 0) return false;
            buf.append(tmp, static_cast<size_t>(n));
            return true;
        }
    };

    void acceptLoop() {
        while (!g_stop) {
            int cfd = ::accept(listenFd_, nullptr, nullptr);
            if (cfd < 0) { if (errno == EINTR) continue; break; }
            std::thread([this, cfd] { handle(cfd); }).detach();
        }
    }

    static void sendResponse(int fd, int code, const std::string &reason,
                              const std::string &body,
                              const std::string &content_type = "application/json") {
        std::string head = "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\n"
                          + "Content-Type: " + content_type + "\r\n"
                          + "Content-Length: " + std::to_string(body.size()) + "\r\n"
                          + "Connection: close\r\n\r\n";
        ::send(fd, head.data(), head.size(), MSG_NOSIGNAL);
        if (!body.empty()) ::send(fd, body.data(), body.size(), MSG_NOSIGNAL);
    }

    static void sendJson(int fd, int code, const std::string &reason, const std::string &json) {
        sendResponse(fd, code, reason, json, "application/json");
    }

    void handle(int fd) {
        struct timeval tv{10, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        Conn conn{fd, ""};
        size_t header_end;
        while ((header_end = conn.buf.find("\r\n\r\n")) == std::string::npos) {
            if (conn.buf.size() > 16384 || !conn.fill()) {
                sendJson(fd, 400, "Bad Request", R"({"error":"malformed request"})");
                ::close(fd);
                return;
            }
        }

        std::string header_block = conn.buf.substr(0, header_end);
        conn.buf.erase(0, header_end + 4);

        std::vector<std::string> lines;
        size_t start = 0;
        while (true) {
            size_t nl = header_block.find("\r\n", start);
            lines.push_back(header_block.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
            if (nl == std::string::npos) break;
            start = nl + 2;
        }
        if (lines.empty()) { ::close(fd); return; }

        std::string method, target;
        {
            std::string &req = lines[0];
            size_t sp1 = req.find(' ');
            size_t sp2 = req.find(' ', sp1 == std::string::npos ? std::string::npos : sp1 + 1);
            if (sp1 == std::string::npos || sp2 == std::string::npos) {
                sendJson(fd, 400, "Bad Request", R"({"error":"malformed request line"})");
                ::close(fd);
                return;
            }
            method = req.substr(0, sp1);
            target = req.substr(sp1 + 1, sp2 - sp1 - 1);
        }

        size_t content_length = 0;
        for (size_t i = 1; i < lines.size(); ++i) {
            std::string line = lines[i];
            size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string key = line.substr(0, colon);
            std::transform(key.begin(), key.end(), key.begin(), ::tolower);
            if (key == "content-length") {
                std::string val = line.substr(colon + 1);
                size_t a = val.find_first_not_of(" \t");
                if (a != std::string::npos) {
                    try { content_length = std::stoul(val.substr(a)); } catch (...) {}
                }
            }
        }

        std::string path, query;
        size_t qpos = target.find('?');
        path  = qpos == std::string::npos ? target : target.substr(0, qpos);
        query = qpos == std::string::npos ? ""      : target.substr(qpos + 1);

        if (content_length > max_upload_bytes_) {
            sendJson(fd, 413, "Payload Too Large", R"({"error":"file too large"})");
            ::close(fd);
            return;
        }

        std::string body;
        if (content_length > 0) {
            body = std::move(conn.buf);
            conn.buf.clear();  // moved-from state is unspecified; force empty before reuse
            while (body.size() < content_length) {
                if (!conn.fill()) break;
                body += conn.buf;
                conn.buf.clear();
            }
            if (body.size() > content_length) body.resize(content_length);
        }

        dispatch(fd, method, path, query, body);
        ::close(fd);
    }

    void dispatch(int fd, const std::string &method, const std::string &path,
                  const std::string &query, const std::string &body) {
        if (method == "GET" && path == "/files") {
            handleFiles(fd);
        } else if (method == "POST" && path == "/upload") {
            handleUpload(fd, query, body);
        } else if (method == "POST" && path == "/play") {
            handlePlay(fd, query);
        } else if (method == "POST" && path == "/play/random") {
            handlePlayRandom(fd);
        } else if (method == "POST" && path == "/stop") {
            player_.requestStop();
            sendJson(fd, 200, "OK", R"({"ok":true})");
        } else if (method == "GET" && path == "/status") {
            handleStatus(fd);
        } else {
            sendJson(fd, 404, "Not Found", R"({"error":"not found"})");
        }
    }

    void handleFiles(int fd) {
        auto files = list_mp3_files(library_dir_);
        std::string json = "[";
        for (size_t i = 0; i < files.size(); ++i) {
            std::string full = library_dir_ + "/" + files[i];
            struct stat st{};
            long size = (::stat(full.c_str(), &st) == 0) ? static_cast<long>(st.st_size) : 0;
            if (i) json += ",";
            json += "{\"name\":\"" + json_escape(files[i]) + "\",\"size\":" + std::to_string(size) + "}";
        }
        json += "]";
        sendJson(fd, 200, "OK", json);
    }

    void handleUpload(int fd, const std::string &query, const std::string &body) {
        std::string name = query_param(query, "name");
        if (!is_safe_mp3_name(name)) {
            sendJson(fd, 400, "Bad Request", R"({"error":"invalid or missing 'name': must be a bare *.mp3 filename"})");
            return;
        }
        std::string full = library_dir_ + "/" + name;
        int f = ::open(full.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0640);
        if (f < 0) {
            std::cerr << "[HTTP] upload open(" << full << "): " << strerror(errno) << "\n";
            sendJson(fd, 500, "Internal Server Error", R"({"error":"could not write file"})");
            return;
        }
        size_t written = 0;
        while (written < body.size()) {
            ssize_t n = ::write(f, body.data() + written, body.size() - written);
            if (n <= 0) break;
            written += static_cast<size_t>(n);
        }
        ::close(f);
        if (written != body.size()) {
            sendJson(fd, 500, "Internal Server Error", R"({"error":"short write"})");
            return;
        }
        sendJson(fd, 200, "OK",
                 "{\"ok\":true,\"name\":\"" + json_escape(name) + "\",\"size\":" + std::to_string(written) + "}");
    }

    void handlePlay(int fd, const std::string &query) {
        std::string name = query_param(query, "name");
        if (!is_safe_mp3_name(name)) {
            sendJson(fd, 400, "Bad Request", R"({"error":"invalid or missing 'name'"})");
            return;
        }
        std::string full = library_dir_ + "/" + name;
        struct stat st{};
        if (::stat(full.c_str(), &st) != 0) {
            sendJson(fd, 404, "Not Found", R"({"error":"file not in library"})");
            return;
        }
        player_.requestPlay(full);
        sendJson(fd, 200, "OK", "{\"ok\":true,\"playing\":\"" + json_escape(name) + "\"}");
    }

    void handlePlayRandom(int fd) {
        auto files = list_mp3_files(library_dir_);
        if (files.empty()) {
            sendJson(fd, 404, "Not Found", R"({"error":"library is empty"})");
            return;
        }
        static std::mt19937 rng{std::random_device{}()};
        std::uniform_int_distribution<size_t> dist(0, files.size() - 1);
        const std::string &name = files[dist(rng)];
        player_.requestPlay(library_dir_ + "/" + name);
        sendJson(fd, 200, "OK", "{\"ok\":true,\"playing\":\"" + json_escape(name) + "\"}");
    }

    void handleStatus(int fd) {
        Player::Status s = player_.status();
        char buf[512];
        snprintf(buf, sizeof(buf),
            "{\"playing\":%s,\"file\":\"%s\",\"elapsed_s\":%.1f,\"duration_s\":%.1f}",
            s.playing ? "true" : "false",
            json_escape(s.file).c_str(),
            s.elapsed_s, s.duration_s);
        sendJson(fd, 200, "OK", buf);
    }

    int         port_;
    std::string library_dir_;
    size_t      max_upload_bytes_;
    Player     &player_;
    int         listenFd_ = -1;
    std::thread thread_;
};

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char **argv) {
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGHUP,  signal_handler);
    std::signal(SIGPIPE, SIG_IGN);

    std::string cfg_path = "config.ini";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if ((a == "--config" || a == "-c") && i + 1 < argc) cfg_path = argv[++i];
        else if (a == "--help" || a == "-h") {
            std::cout << "Usage: " << argv[0] << " [--config config.ini]\n"
                      << "  List    : curl http://127.0.0.1:8566/files\n"
                      << "  Upload  : curl --data-binary @song.mp3 \"http://127.0.0.1:8566/upload?name=song.mp3\"\n"
                      << "  Play    : curl -X POST \"http://127.0.0.1:8566/play?name=song.mp3\"\n"
                      << "  Random  : curl -X POST http://127.0.0.1:8566/play/random\n"
                      << "  Stop    : curl -X POST http://127.0.0.1:8566/stop\n"
                      << "  Status  : curl http://127.0.0.1:8566/status\n";
            return 0;
        }
    }

    Config cfg(cfg_path);

    const std::string library_dir  = cfg.get_str("library.dir", "/var/lib/mp3-player/library");
    const std::string alsa_device  = cfg.get_str("audio.device", "default");
    const int         http_port    = cfg.get_int("http.port", 8566);
    const int         max_upload_mb = cfg.get_int("http.max_upload_mb", 100);

    struct stat st{};
    if (::stat(library_dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        std::cerr << "[Config] Library directory missing or not a directory: " << library_dir << "\n";
        return 1;
    }

    std::cerr << "[Config] library  : " << library_dir << "\n"
              << "[Config] audio    : " << alsa_device << "\n"
              << "[Config] http     : 0.0.0.0:" << http_port << "\n"
              << "[Config] max_upload: " << max_upload_mb << " MB\n";

    Player player(alsa_device);
    player.start();

    HttpServer server(http_port, library_dir,
                       static_cast<size_t>(max_upload_mb) * 1024 * 1024, player);
    server.start();

    std::cerr << "[Main] Running.\n";
    while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(200));

    player.requestStop();
    player.join();

    std::cerr << "[Main] Done.\n";
    return 0;
}
