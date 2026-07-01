# mp3-player

HTTP-controlled MP3 jukebox: upload files, list the library, play a specific file, or play a random one. There is no web UI — it's a small JSON API driven by `curl` or scripts.

MP3 decoding uses a vendored, public-domain single-header decoder ([minimp3](https://github.com/lieff/minimp3) — CC0), so there's no external decode library dependency. Audio output goes through ALSA.

## HTTP API

| Method | Path           | Description |
|--------|----------------|-------------|
| GET    | `/files`       | List library files: `[{"name":"song.mp3","size":1234}]` |
| POST   | `/upload?name=song.mp3` | Body is the raw MP3 bytes (not multipart) |
| POST   | `/play?name=song.mp3`   | Play a specific library file |
| POST   | `/play/random` | Play a random library file |
| POST   | `/stop`        | Stop current playback |
| GET    | `/status`      | `{"playing":true,"file":"song.mp3","elapsed_s":12.3,"duration_s":180.0}` |

```bash
# Upload a file
curl --data-binary @song.mp3 "http://127.0.0.1:8566/upload?name=song.mp3"

# List the library
curl http://127.0.0.1:8566/files

# Play a specific file
curl -X POST "http://127.0.0.1:8566/play?name=song.mp3"

# Play something random
curl -X POST http://127.0.0.1:8566/play/random

# Stop playback
curl -X POST http://127.0.0.1:8566/stop

# Check status
curl http://127.0.0.1:8566/status
```

Uploaded/played filenames must be a bare `*.mp3` name (no `/`, no leading `.`) — this blocks path traversal outside the library directory.

## Build

Requires a C++20 compiler, CMake 3.16+, `pkg-config`, and ALSA development headers (`libasound2-dev` on Debian/Raspberry Pi OS).

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Binary: `build/mp3_player`

## Run

```bash
./build/mp3_player [--config config.ini]
```

The default config path is `config.ini` in the working directory, and the library directory must already exist. Copy and edit before running:

```bash
mkdir -p ./library
cp config.ini my-config.ini
# edit my-config.ini: set library.dir to ./library, audio.device if not "default"
./build/mp3_player --config my-config.ini
```

## Configuration

`config.ini`:

```ini
[library]
dir              = /var/lib/mp3-player/library  ; directory of *.mp3 files

[audio]
device           = default     ; ALSA PCM device name (e.g. default, hw:0,0)

[http]
port             = 8566        ; JSON API port (no web UI)
max_upload_mb    = 100         ; reject uploads larger than this
```

Run `aplay -L` to list available ALSA devices if `default` doesn't route to the output you want.

## Debian package

Build the `.deb` on the target machine (e.g. Raspberry Pi):

```bash
cd mp3-player
dpkg-buildpackage -us -uc -b
```

The package lands one level up: `../mp3-player_1.0.0+..._armhf.deb`

Install:

```bash
sudo dpkg -i ../mp3-player_*.deb
```

The package:
- Creates a dedicated `mp3-player` system user with `audio` group membership
- Installs config to `/etc/mp3-player/config.ini`
- Creates the library directory at `/var/lib/mp3-player/library`
- Registers and starts a systemd service

Uninstalling (`apt purge`) removes the config and user but **leaves `/var/lib/mp3-player` in place** — it holds uploaded music, not package state.

## systemd service

```bash
# Enable and start
systemctl enable --now mp3-player

# Check status
systemctl status mp3-player

# Follow logs
journalctl -fu mp3-player
```

Edit `/etc/mp3-player/config.ini` and `systemctl restart mp3-player` to apply changes.
