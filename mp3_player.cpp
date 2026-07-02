/**
 * mp3_player.cpp
 * ==============
 * TCP-controlled MP3 player: connect, send a base64-encoded MP3 (the whole
 * file), close your end of the connection, and it plays immediately over
 * ALSA. There is no library, no filenames, and nothing is ever written to
 * disk — the decoded audio lives only in memory for the duration of
 * playback.
 *
 * Protocol (default port 8566):
 *   1. Client opens a TCP connection.
 *   2. Client optionally writes a "volume=N\n" line (N is 0-100; omit it
 *      entirely to play at the current/full mixer level), then writes the
 *      base64-encoded MP3 bytes, then closes (or shuts down the write side
 *      of) the connection so the server sees EOF.
 *   3. Server replies with one JSON line and closes the connection:
 *        {"ok":true}                  - playback started
 *        {"error":"busy"}             - already playing something else
 *        {"error":"invalid volume"}
 *        {"error":"invalid base64"}
 *        {"error":"empty payload"}
 *        {"error":"payload too large"}
 *
 * Only one track plays at a time. While busy, new connections are rejected
 * with "busy" rather than interrupting what's currently playing.
 *
 * Volume is applied via the ALSA mixer (the first playback-capable simple
 * element on the configured device, preferring one literally named
 * "Master") — it's a system-level mixer change, not a per-track software
 * gain, so it persists after the track finishes.
 *
 * MP3 decoding uses the vendored public-domain minimp3 (no external decode
 * library); output goes through ALSA (libasound2).
 *
 * Build (Linux / Raspberry Pi):
 *   cmake -B build && cmake --build build -j$(nproc)
 * Run:
 *   ./build/mp3_player [--config config.ini]
 * Example:
 *   base64 -w0 song.mp3 | nc -q1 127.0.0.1 8566
 *   (printf "volume=40\n"; base64 -w0 song.mp3) | nc -q1 127.0.0.1 8566
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
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
// Base64 decode — permissive: ignores whitespace/newlines, stops at '=' or
// the first invalid character (the latter is treated as a decode failure).
// ─────────────────────────────────────────────────────────────────────────────
static bool base64_decode(const std::string &in, std::vector<uint8_t> &out) {
    static const std::array<int8_t, 256> table = [] {
        std::array<int8_t, 256> t{};
        t.fill(-1);
        const char *alphabet =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i)
            t[static_cast<unsigned char>(alphabet[i])] = static_cast<int8_t>(i);
        return t;
    }();

    out.clear();
    int val = 0, bits = -8;
    for (unsigned char c : in) {
        if (c == '=') break;
        if (std::isspace(c)) continue;
        int8_t d = table[c];
        if (d < 0) return false;
        val = (val << 6) + d;
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<uint8_t>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Volume header — an optional "volume=N\n" line at the start of a request.
// Absent means "leave the mixer alone, play at whatever it's currently at".
// ─────────────────────────────────────────────────────────────────────────────
struct VolumeHeader {
    bool present = false;  // a "volume=..." first line was found
    bool ok      = true;   // false only if present and malformed/out of range
    int  volume  = -1;     // 0-100, valid only when present && ok
};

// If `raw`'s first line looks like "volume=<0-100>", parses it and strips
// that line (including the newline) from `raw`. Otherwise leaves `raw`
// untouched and returns a header with `present == false`.
static VolumeHeader extract_volume_header(std::string &raw) {
    VolumeHeader vh;
    size_t nl = raw.find('\n');
    if (nl == std::string::npos) return vh;

    std::string first = raw.substr(0, nl);
    if (!first.empty() && first.back() == '\r') first.pop_back();

    std::string lower = first;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    const std::string prefix = "volume=";
    if (lower.compare(0, prefix.size(), prefix) != 0) return vh;

    vh.present = true;
    std::string num = first.substr(prefix.size());
    size_t b = num.find_first_not_of(" \t");
    size_t e = num.find_last_not_of(" \t");
    if (b == std::string::npos) { vh.ok = false; return vh; }
    num = num.substr(b, e - b + 1);

    try {
        size_t idx = 0;
        int v = std::stoi(num, &idx);
        if (idx != num.size() || v < 0 || v > 100) { vh.ok = false; return vh; }
        vh.volume = v;
    } catch (...) {
        vh.ok = false;
        return vh;
    }

    raw.erase(0, nl + 1);
    return vh;
}

// Sets playback volume (0-100) on the first playback-capable simple mixer
// element on `device` (preferring one named "Master"). Best-effort: some
// devices (e.g. behind dmix/PulseAudio) expose no simple mixer control, in
// which case this just fails and playback proceeds at whatever level the
// device is already at.
static bool set_alsa_volume(const std::string &device, int volume_percent) {
    std::string mixer_device = device;
    size_t comma = mixer_device.find(',');
    if (comma != std::string::npos) mixer_device = mixer_device.substr(0, comma);

    snd_mixer_t *mixer = nullptr;
    if (snd_mixer_open(&mixer, 0) < 0) return false;
    if (snd_mixer_attach(mixer, mixer_device.c_str()) < 0 ||
        snd_mixer_selem_register(mixer, nullptr, nullptr) < 0 ||
        snd_mixer_load(mixer) < 0) {
        snd_mixer_close(mixer);
        return false;
    }

    snd_mixer_elem_t *elem = nullptr;
    for (snd_mixer_elem_t *e = snd_mixer_first_elem(mixer); e; e = snd_mixer_elem_next(e)) {
        if (!snd_mixer_selem_is_active(e) || !snd_mixer_selem_has_playback_volume(e)) continue;
        if (!elem) elem = e;
        snd_mixer_selem_id_t *sid;
        snd_mixer_selem_id_alloca(&sid);
        snd_mixer_selem_get_id(e, sid);
        if (std::string(snd_mixer_selem_id_get_name(sid)) == "Master") { elem = e; break; }
    }
    if (!elem) {
        snd_mixer_close(mixer);
        return false;
    }

    long min = 0, max = 0;
    snd_mixer_selem_get_playback_volume_range(elem, &min, &max);
    long value = min + (max - min) * volume_percent / 100;
    snd_mixer_selem_set_playback_volume_all(elem, value);

    snd_mixer_close(mixer);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Player — decodes an in-memory MP3 buffer via minimp3, streams PCM to
// ALSA. Runs on a single background thread; tryPlay() rejects a new track
// while one is already playing instead of interrupting it.
// ─────────────────────────────────────────────────────────────────────────────
class Player {
public:
    explicit Player(std::string alsa_device) : alsa_device_(std::move(alsa_device)) {}

    void start() { thread_ = std::thread(&Player::run, this); }
    void join()  { if (thread_.joinable()) thread_.join(); }

    // Wakes a Player thread that's idling in run()'s cv_.wait() so it can
    // notice g_stop during shutdown.
    void wake() {
        std::lock_guard<std::mutex> lk(mu_);
        cv_.notify_all();
    }

    // Starts playback of `data` unless something is already playing, in
    // which case it returns false and leaves the current playback alone.
    // `volume_percent` is 0-100, or -1 to leave the mixer untouched.
    bool tryPlay(std::vector<uint8_t> data, int volume_percent) {
        std::lock_guard<std::mutex> lk(mu_);
        if (busy_) return false;
        busy_            = true;
        pending_volume_  = volume_percent;
        pending_         = std::make_shared<const std::vector<uint8_t>>(std::move(data));
        cv_.notify_all();
        return true;
    }

private:
    void run() {
        while (!g_stop) {
            std::shared_ptr<const std::vector<uint8_t>> data;
            int volume_percent;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [&] { return g_stop.load() || pending_ != nullptr; });
                if (g_stop) break;
                data           = pending_;
                volume_percent = pending_volume_;
                pending_.reset();
            }
            playBuffer(*data, volume_percent);
            {
                std::lock_guard<std::mutex> lk(mu_);
                busy_ = false;
            }
        }
    }

    void playBuffer(const std::vector<uint8_t> &data, int volume_percent) {
        mp3dec_t dec;
        mp3dec_file_info_t info{};
        mp3dec_init(&dec);

        if (mp3dec_load_buf(&dec, data.data(), data.size(), &info, nullptr, nullptr) != 0 || !info.buffer) {
            std::cerr << "[Player] Failed to decode buffer\n";
            if (info.buffer) free(info.buffer);
            return;
        }
        if (info.channels <= 0 || info.hz <= 0 || info.samples == 0) {
            std::cerr << "[Player] Empty/invalid decode\n";
            free(info.buffer);
            return;
        }

        if (volume_percent >= 0 && !set_alsa_volume(alsa_device_, volume_percent)) {
            std::cerr << "[ALSA] Could not set mixer volume on " << alsa_device_
                       << " (no simple mixer control?); playing at current level\n";
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
        const double duration_s   = static_cast<double>(total_frames) / info.hz;

        std::cerr << "[Player] Playing buffer (" << info.channels << "ch " << info.hz
                  << "Hz, " << duration_s << "s)\n";

        size_t pos = 0;
        while (pos < total_frames && !g_stop) {
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
        }

        if (g_stop) snd_pcm_drop(pcm);
        else        snd_pcm_drain(pcm);
        snd_pcm_close(pcm);
        free(info.buffer);

        std::cerr << "[Player] Stopped\n";
    }

    std::string             alsa_device_;
    std::mutex              mu_;
    std::condition_variable cv_;
    std::shared_ptr<const std::vector<uint8_t>> pending_;
    int                     pending_volume_ = -1;
    bool                    busy_ = false;

    std::thread thread_;
};

// ─────────────────────────────────────────────────────────────────────────────
// TcpServer — one thread per connection. Reads the connection to EOF,
// base64-decodes it, and hands the result to the Player.
// ─────────────────────────────────────────────────────────────────────────────
class TcpServer {
public:
    TcpServer(int port, size_t max_payload_bytes, Player &player)
        : port_(port), max_payload_bytes_(max_payload_bytes), player_(player) {}

    ~TcpServer() { if (listenFd_ >= 0) ::close(listenFd_); }

    void start() {
        listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int opt = 1;
        ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port        = htons(static_cast<uint16_t>(port_));
        if (::bind(listenFd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
            std::cerr << "[TCP] bind: " << strerror(errno) << "\n";
        }
        if (::listen(listenFd_, 16) < 0) {
            std::cerr << "[TCP] listen: " << strerror(errno) << "\n";
        }
        thread_ = std::thread(&TcpServer::acceptLoop, this);
        thread_.detach();
        std::cerr << "[TCP] Listening on 0.0.0.0:" << port_ << "\n";
    }

private:
    void acceptLoop() {
        while (!g_stop) {
            int cfd = ::accept(listenFd_, nullptr, nullptr);
            if (cfd < 0) { if (errno == EINTR) continue; break; }
            std::thread([this, cfd] { handle(cfd); }).detach();
        }
    }

    static void reply(int fd, const char *json) {
        std::string line = std::string(json) + "\n";
        ::send(fd, line.data(), line.size(), MSG_NOSIGNAL);
    }

    void handle(int fd) {
        struct timeval tv{10, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        std::string raw;
        char buf[8192];
        bool too_large = false;
        while (true) {
            ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;  // EOF, timeout, or error: client is done sending
            raw.append(buf, static_cast<size_t>(n));
            if (raw.size() > max_payload_bytes_) {
                too_large = true;
                break;
            }
        }

        if (too_large) {
            reply(fd, R"({"error":"payload too large"})");
            ::close(fd);
            return;
        }
        if (raw.empty()) {
            reply(fd, R"({"error":"empty payload"})");
            ::close(fd);
            return;
        }

        VolumeHeader vh = extract_volume_header(raw);
        if (vh.present && !vh.ok) {
            reply(fd, R"({"error":"invalid volume"})");
            ::close(fd);
            return;
        }
        if (raw.empty()) {
            reply(fd, R"({"error":"empty payload"})");
            ::close(fd);
            return;
        }

        std::vector<uint8_t> mp3;
        if (!base64_decode(raw, mp3) || mp3.empty()) {
            reply(fd, R"({"error":"invalid base64"})");
            ::close(fd);
            return;
        }

        if (!player_.tryPlay(std::move(mp3), vh.volume)) {
            reply(fd, R"({"error":"busy"})");
            ::close(fd);
            return;
        }

        reply(fd, R"({"ok":true})");
        ::close(fd);
    }

    int         port_;
    size_t      max_payload_bytes_;
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
                      << "  Play        : base64 -w0 song.mp3 | nc -q1 127.0.0.1 8566\n"
                      << "  Play at vol : (printf \"volume=40\\n\"; base64 -w0 song.mp3) | nc -q1 127.0.0.1 8566\n"
                      << "                (server replies {\"ok\":true} or {\"error\":\"busy\"})\n";
            return 0;
        }
    }

    Config cfg(cfg_path);

    const std::string alsa_device    = cfg.get_str("audio.device", "default");
    const int         tcp_port       = cfg.get_int("tcp.port", 8566);
    const int         max_payload_mb = cfg.get_int("tcp.max_payload_mb", 100);

    std::cerr << "[Config] audio      : " << alsa_device << "\n"
              << "[Config] tcp        : 0.0.0.0:" << tcp_port << "\n"
              << "[Config] max_payload: " << max_payload_mb << " MB\n";

    Player player(alsa_device);
    player.start();

    TcpServer server(tcp_port, static_cast<size_t>(max_payload_mb) * 1024 * 1024, player);
    server.start();

    std::cerr << "[Main] Running.\n";
    while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(200));

    player.wake();
    player.join();

    std::cerr << "[Main] Done.\n";
    return 0;
}
