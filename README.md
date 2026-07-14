# mp3-player

TCP-controlled MP3 player: connect, send a base64-encoded MP3, close your end of the connection, and it plays immediately over ALSA. There's no filename, no upload/library concept, and no web UI — nothing is ever written to disk.

Only one track plays at a time. If a track is already playing, a new connection gets `{"error":"busy"}` instead of interrupting it.

MP3 decoding uses a vendored, public-domain single-header decoder ([minimp3](https://github.com/lieff/minimp3) — CC0), so there's no external decode library dependency. Audio output goes through ALSA.

## Protocol

Default port `8566`, plain TCP (not HTTP):

1. Open a TCP connection.
2. Optionally write a `volume=N\n` line first, where `N` is `0`-`100`. Omit it entirely to play at whatever the mixer is currently set to.
3. Write the base64-encoded MP3 bytes, then close (or shut down the write side of) the connection so the server sees EOF.
4. The server replies with one JSON line and closes the connection:

| Response | Meaning |
|----------|---------|
| `{"ok":true}` | Playback started |
| `{"error":"busy"}` | Already playing something else |
| `{"error":"invalid volume"}` | `volume=` line present but not an integer 0-100 |
| `{"error":"invalid base64"}` | Payload didn't decode as base64 |
| `{"error":"empty payload"}` | Nothing was sent |
| `{"error":"payload too large"}` | Exceeds `tcp.max_payload_mb` |

```bash
# Play a file at whatever volume the mixer is already at
base64 -w0 song.mp3 | nc -q1 127.0.0.1 8566

# Play a file at 40% volume
(printf "volume=40\n"; base64 -w0 song.mp3) | nc -q1 127.0.0.1 8566
```

`nc`'s `-q1` flag makes it close the connection one second after stdin (the base64 data) reaches EOF — without it, some `nc` builds keep the socket open waiting for more input and the server never sees the payload finish.

Volume is applied via the ALSA mixer (the first playback-capable simple element on the configured device, preferring one literally named `Master`) — it's a system-level mixer change, not a per-track gain, so it persists after the track finishes. If the device exposes no simple mixer control (common behind `dmix`/PulseAudio setups), setting the volume silently fails and playback proceeds at whatever level the device is already at.

## Discovery (mDNS/DNS-SD)

On startup the process announces itself on the LAN as `_mp3player._tcp` via Avahi, so you don't need to know its IP:

```bash
avahi-browse -rt _mp3player._tcp
```

This needs `avahi-daemon` running on the Pi (installed and enabled by default on Raspberry Pi OS). If it isn't running, `mp3_player` logs a warning at startup and continues normally — mDNS is discovery-only, not required for the TCP protocol to work. Disable it or change the advertised name via the `[mdns]` section in `config.ini`.

## Build

Requires a C++20 compiler, CMake 3.16+, `pkg-config`, ALSA development headers (`libasound2-dev`), and Avahi client development headers (`libavahi-client-dev`) — all on Debian/Raspberry Pi OS.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Binary: `build/mp3_player`

## Run

```bash
./build/mp3_player [--config config.ini]
```

The default config path is `config.ini` in the working directory. Copy and edit before running:

```bash
cp config.ini my-config.ini
# edit my-config.ini: set audio.device if not "default"
./build/mp3_player --config my-config.ini
```

## Configuration

`config.ini`:

```ini
[audio]
device           = default     ; ALSA PCM device name (e.g. default, hw:0,0)

[tcp]
port             = 8566        ; plain TCP port (send base64 MP3, get back JSON)
max_payload_mb   = 100         ; reject payloads larger than this (base64, not decoded size)

[mdns]
enabled          = true        ; announce _mp3player._tcp via avahi-daemon
name             = mp3-player  ; service instance name shown in discovery
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
- Registers and starts a systemd service

Uninstalling (`apt purge`) removes the config and user. There's no library directory to clean up — uploaded files only ever live in the running process's memory.

### From the APT repository

CI publishes to a signed APT repository (shared with other aipicam Raspberry Pi packages) hosted on Cloudflare R2, with two channels:

- **`main`** — pushing a `v*` tag publishes the clean release version here.
- **`nightly`** — every push (to any branch, and PRs) publishes a dev build here, versioned with a `+<UTC timestamp>-1` suffix.

```bash
curl -fsSL https://repo.aipicam.com/pubkey.asc | sudo gpg --dearmor -o /usr/share/keyrings/aipicam.gpg

# stable releases
echo "deb [signed-by=/usr/share/keyrings/aipicam.gpg] https://repo.aipicam.com main main" | sudo tee /etc/apt/sources.list.d/aipicam.list

# or nightly builds instead
echo "deb [signed-by=/usr/share/keyrings/aipicam.gpg] https://repo.aipicam.com nightly main" | sudo tee /etc/apt/sources.list.d/aipicam.list

sudo apt-get update
sudo apt-get install mp3-player
```

Builds run on GitHub's native `ubuntu-24.04-arm` hosted runner (no QEMU). Uses the same `R2_ACCOUNT_ID`, `R2_ACCESS_KEY_ID`, `R2_SECRET_ACCESS_KEY`, `GPG_PRIVATE_KEY`, and `GPG_KEY_ID` repo secrets described in [pi-block-cpu-cores](../pi-block-cpu-cores)'s README, since it publishes into the same shared repo.

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
