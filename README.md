# ZeroOS

An icon-driven shell for the **M5Stack CardputerZero** (Raspberry Pi CM0,
320×170 ST7789 LCD, 46-key keyboard, no touch). It runs on plain Raspberry Pi
OS Lite, draws straight to the framebuffer with [LVGL](https://lvgl.io) (no
X11 or Wayland), and is driven entirely from the keyboard.

| App | Status |
|---|---|
| **Books**: EPUB reader (also PDF, CBZ, FB2, MOBI) on MuPDF | working |
| **Meshtastic**: LoRa mesh chat via `meshtasticd` | working in the simulator; needs the Cap LoRa-1262 |
| **MeshCore**: LoRa mesh chat via a MeshCore companion | working in the simulator; needs the Cap LoRa-1262 |
| **Tailscale**: VPN status, QR sign-in, devices, exit node | working against a LocalAPI stand-in |
| **SSH**: terminal for OpenSSH (256 colours, vim/top, scrollback) | tested against a real sshd |
| **Emby**: browse, resume, play (mpv, server-side H.264 transcode) | working against an Emby stand-in |
| **Music**: library by artist/album/song, cover art, queue, shuffle, repeat, background playback | working (MP3, FLAC, Ogg, Opus, M4A) |
| **Video**: MP4/MKV/WebM…, resume, embedded and external subtitles, audio tracks, HDMI | working in the simulator |
| **Camera**: viewfinder, 8 MP photos, 720p video, gallery (rpicam-apps) | working against an rpicam stand-in |
| **Settings**: Wi-Fi, brightness, HDMI, sound, time zone, battery/storage, about, software update, power | working against stand-ins |
| **HDMI out**: the interface mirrored on a monitor; videos play there | working with a simulated monitor |
| **USB Drive**: shows up as a drive on a Mac or PC over USB-C (Music, Videos, Pictures, Books) | working against a helper stand-in |
| **Users**: login at boot (PIN, password or none), each user with their own books, music, videos, photos and app data; admins manage accounts | working in the simulator |
| **Software update**: new ZeroOS versions from GitHub releases, installed on the device, automatic rollback | working against a GitHub stand-in |

## Quick start (desktop simulator)

```sh
sudo ./tools/fetch-deps.sh --apt      # build deps + LVGL 9.3
cmake -S . -B build && cmake --build build -j
./build/zeroos --books assets/books   # opens a 3× zoomed 320×170 window
```

`assets/books/zeroos-guide.epub` is a small sample book (regenerate with
`tools/make_sample_epub.py`). Without `--books`, the shell uses `~/Books`.

To get screenshots without a window: `./tools/screenshots.sh` → `screenshots/`.

To try the mesh apps without a radio: `./tools/sim/mesh-demo.sh` (add
`--shots` for screenshots in `screenshots/mesh/`). It starts a meshtasticd
stand-in and three real pyMC MeshCore nodes on a simulated radio link.

To try Music, Video, Camera and Settings: `./tools/sim/media-demo.sh` (or
`--shots`). It generates tagged songs with cover art, an MKV with two audio
tracks and two subtitle tracks, and photos, then runs ZeroOS with stand-ins
for the camera (`tools/sim/fake-rpicam`), NetworkManager and ALSA
(`tools/sim/fake-system`) and a simulated 1280x720 HDMI monitor.

To try Tailscale and Emby: `./tools/sim/net-demo.sh` (or `--shots`). It
starts a tailscaled LocalAPI stand-in and an Emby stand-in with generated
media (sign in at `127.0.0.1` with `demo` / `demo`; needs ffmpeg). SSH needs
no stand-in: connect to any machine you can reach.

## Keys

On the CardputerZero, the keyboard driver does the layers. Tap **Fn** or
**Sym** once for the next key, hold it, or double-tap to lock.
- **Fn** gives the arrows (Fn+F ↑, Fn+Z ←, Fn+X ↓, Fn+C →), Home, End,
  Page Up/Down, Delete, F1–F12 and the printed media keys.
- **Sym** gives the symbols printed on the keys (`! @ # [ ] , . / ?` …).

The media keys work everywhere:

| Fn + | Does |
|---|---|
| A / S / D | mute / volume down / volume up: the video or music that's playing (its media volume), otherwise the system volume |
| Q | play / pause |
| W / E | previous / next: song, or video in the folder |
| U / I | brightness down / up |
| H | help (in the video player: the keys) |

| Where | Key | Action |
|---|---|---|
| Launcher | ← → | choose app |
| | Enter or 1–9 | open |
| Library | ↑ ↓ | choose book |
| | Enter | open |
| | R | rescan folder |
| | Esc | back to launcher |
| Reading | → ↓ Space Enter | next page |
| | ← ↑ Backspace | previous page |
| | + / − | text size |
| | T | theme: paper, sepia, night |
| | C | table of contents |
| | Esc / Q | back to library (position is saved) |
| Mesh: chats | ↑ ↓, Enter | choose and open a channel or DM |
| | N | nodes / contacts (Enter there starts a DM) |
| | A | send an advert (MeshCore) |
| Mesh: chat | type, Enter | send |
| | ↑ ↓ | scroll history |
| | Esc | back to chats |
| Tailscale | ↑ ↓, Enter | choose a device, open SSH to it |
| | C | connect / disconnect |
| | E | use the device as exit node (again to clear) |
| | Enter (signed out) | get a sign-in QR code |
| SSH: hosts | Enter, E, D | connect, edit, delete (D twice) |
| SSH: terminal | everything | goes to the remote side, Ctrl and Tab included |
| | Ctrl + ] | menu: ↑ ↓ scrollback, F text size (6×12, 6×10, 5×8), D disconnect |
| Emby | Enter, Esc | open, back |
| | Q | quality: 480p, 360p, 720p, original |
| | R (details) | play from the start instead of resuming |
| Emby: playing | Space, ← →, ↑ ↓, + − | pause, ±10 s, ±1 min, volume; Esc stops |
| Music: lists | Enter, Esc | open / play, back (music keeps playing) |
| | S | shuffle the album, artist or list |
| | Tab | now playing |
| | Space | pause / play from anywhere in the app |
| Music: now playing | Space, ← →, ↑ ↓ | pause, seek 10 s, previous / next track |
| | + −, S, R, Q, X | volume, shuffle, repeat (off / all / one), queue, stop |
| Video: browse | Enter, Esc | open folder or play (asks to resume), back |
| | O | output: TV (HDMI) or this screen |
| | Del | forget the saved position |
| Video: playing | Space (or Fn+Q), ← → | play/pause, ±10 s |
| | [ ], or , . | ±1 min |
| | ↑ ↓ (or + −), M | volume (saved as the media volume), mute |
| | N, P | next / previous video in the folder (the next one also starts by itself at the end) |
| | S, A | subtitles, audio track (a list on the remote screen; cycles when the video is on the panel) |
| | H | show these keys on the video |
| | Esc | stop (position is saved) |
| Camera | Enter | take a photo / start or stop recording |
| | Tab | photo ↔ video |
| | G | gallery: Enter views a photo or plays a video, ← → browse, D twice deletes |
| USB Drive | Enter | set up the drive / switch to device mode / share |
| | Esc | take the drive back (eject on the computer first) |
| | H | back to host mode (USB-A port and hub on again; restarts) |
| Login | ← →, a letter, Enter | choose a user (or jump to a name), log in or unlock |
| | type, Enter, Esc | PIN or password, log in, back |
| Settings | ↑ ↓, Enter, Esc | choose, open / toggle, back |
| | ← → | adjust brightness or volume |
| | R, F (Wi-Fi) | rescan, forget the network |

On the device, **Fn + ; . , /** are Up, Down, Left, Right, **Fn + `** is
Esc and **Fn + − / =** are Page Up / Down, as on the ESP32 Cardputer. Ctrl +
letter reaches the terminal as a control key. Change this in
`src/input_evdev.c`.

## How it fits together

```
src/main.c            backend selection (sdl | fbdev | headless), main loop, key scripts
src/app.c             app registry + launch/exit lifecycle (each app owns a screen + input group)
src/launcher.c        icon row, status bar (clock, Wi-Fi, battery from /sys)
src/theme.c           DejaVu Sans via Tiny TTF (accents work), Montserrat for symbols
src/input.c           one key queue for every backend (evdev, SDL, scripts); Ctrl/Tab/PgUp keys
src/input_evdev.c     device keyboard: evdev → keys, with an Fn layer
src/core/
  task.c              worker threads with results delivered on the UI thread
  http.c              libcurl wrapper (also over unix sockets)
  form.c              keyboard forms (logins, host entries)
  tailscale.c         tailscaled LocalAPI client + background status for the status bar
  player.c            mpv as a child process (JSON IPC): panel or HDMI output, playlists, tracks
  hdmi.c              HDMI hot-plug, mirroring the UI to the monitor (libdrm dumb buffer)
  ui.c                shared widgets: a virtual list that scales to thousands of rows
  imgdec.c            JPEG/PNG to small RGB565 images (MuPDF, decodes at reduced size)
  tags.c              ID3v2/ID3v1, FLAC/Ogg Vorbis comments, MP4 atoms, embedded covers
  settings.c          settings.conf (key=value)
  proc.c              run a command and capture its output (nmcli, amixer, ...)
src/apps/music/
  library.c           scan + tag cache, albums/artists, cover cache, play queue
  music_app.c         home, artists, albums, songs, now playing, queue
src/apps/video/       browser, resume positions, remote screen, track picker
src/apps/camera/      rpicam-vid viewfinder (raw YUV over a pipe), photos, recording, gallery
src/apps/settings/    Wi-Fi (nmcli), display and HDMI, sound (amixer), time, storage, about, power
src/apps/usb/         USB drive mode screens; the work is done by tools/image/files/zeroos-usb
src/apps/tailscale/   status, QR sign-in, devices, exit node
src/apps/ssh/
  ssh_app.c           hosts, form, PTY session running the system ssh, key generation
  term.c              libvterm terminal on an LVGL canvas, scrollback
src/apps/emby/
  emby_api.c          Emby REST: login, items, images, stream URLs, playback check-ins
  emby_app.c          home, folders, details, player
src/assets/termfonts.c  misc-fixed bitmap fonts (public domain), from tools/make_term_fonts.py
src/apps/mesh/
  mesh_app.c          Meshtastic + MeshCore UI: chats, chat + composer, nodes, radio switch
src/mesh/
  mesh.c              mesh service: model, history log, unread/notifications, radio ownership
  mt_backend.c        meshtasticd client API (TCP 4403, protobuf)
  mc_backend.c        MeshCore companion protocol (TCP 5000)
  pb.c                minimal protobuf reader/writer
  stream.c            non-blocking TCP client polled from the LVGL loop
src/apps/reader/
  book.c              MuPDF wrapper: reflow, render to RGB565, TOC, positions
  shelf.c             library scan + saved state (books.tsv, reader.conf)
  reader.c            library / page / contents views
src/assets/icons.c    generated by tools/make_icons.py (A8 masks, recoloured at draw time)
```

Design notes:

- **One process.** All apps share one LVGL process. That's simplest for a
  single SPI panel. The video player hands the panel to an mpv child process
  while it plays and takes it back on exit.
- **HDMI.** While a monitor is connected, every frame drawn for the panel is
  also scaled by a whole number (4× at 720p, 6× at 1080p, so pixels stay
  sharp) into a framebuffer on the HDMI output. Videos play on the monitor
  with HDMI sound; ZeroOS releases the output to mpv and takes it back after.
  Plugging or unplugging the cable mid-video moves it to the other screen at
  the same spot, with the same audio and subtitle tracks.
  The panel keeps working as the remote. Settings → Display & HDMI switches
  each part on or off.
- **Instant page turns.** The next page is rendered into a spare buffer
  while you read. Idle time then lays out the remaining chapters, so the
  footer switches from a % estimate to real page numbers.
- **Reading positions** are stored as *chapter + fraction through the
  chapter*, not as page numbers or MuPDF bookmarks (which are heap pointers
  for EPUB and can't be saved). Positions survive restarts and font-size
  changes; repeated size changes don't drift.
- **Themes** tint the greyscale page, so illustrations in night mode don't
  flash white.
- State lives in `~/.local/state/zeroos/` (`--state` / `ZOS_STATE`). Writes
  are atomic (temp file + rename), so a dead battery can't corrupt it.
- **Mesh runs in the background.** The connection to the radio daemon belongs
  to the shell, not the app, so messages keep arriving (with a banner, an
  unread count in the status bar and a badge on the tile) while you read.

## Mesh apps and the LoRa cap

```
 ZeroOS ── TCP 4403 ──▶ meshtasticd ───────────┐
        └─ TCP 5000 ──▶ openHop repeater ──────┤──▶ SX1262 on the Cap LoRa-1262
           (companion protocol)   (pyMC MeshCore)  (spidev0.1, one at a time)
```

Both apps are clients of a daemon that owns the radio, which is the
supported way to run each stack on Linux:

- **Meshtastic** uses the official `meshtasticd`. The app speaks its client
  API (the same one the phone apps use over TCP): nodes, channels, text,
  delivery acks.
- **MeshCore** uses the [openHop/pyMC repeater](https://github.com/pyMC-dev/pyMC_Repeater)
  in *monitor* mode (no repeating) with one companion identity on
  `127.0.0.1:5000`. The app speaks the standard MeshCore companion protocol:
  contacts, channels, DMs with delivery acks, adverts.

The cap has one radio, so only one stack is on air at a time. The two
services `Conflict=` with each other in systemd. Opening the other app offers
to switch; ZeroOS remembers which one was active and restarts it at boot.
Message history is kept per stack in the state dir.

**Setup on the device** (after ZeroOS itself):

```sh
sudo ./tools/install-mesh.sh --name "MyZero"   # enables SPI/I2C/UART; reboot when asked
sudo reboot
sudo ./tools/install-mesh.sh --name "MyZero"   # installs MeshCore, sets the Meshtastic region
```

Defaults are Europe: Meshtastic `EU_868` (LongFast) and MeshCore EU/UK Narrow
(869.618 MHz, SF8, BW62.5, CR8). Use `--region` and `--meshcore-preset` to
change them. The installer adds:

- `/etc/meshtasticd/config.d/lora-cardputerzero-cap1262.yaml`: the cap's pins
  and radio settings (header-pin mapping documented inside)
- `/usr/local/sbin/zos-lora-cap-init`: run before either daemon to turn on
  the cap's antenna switch (PI4IOE5V6408 at 0x43, P0 high)
- systemd drop-ins for `meshtasticd` and `openhop-repeater` (Conflicts=, init)
- a polkit rule so the ZeroOS user may start/stop exactly those two units

**Not verified on hardware yet:**

- **Cap power.** The CardputerZero powers its cap header through `PYG13_HAT_EN`
  on its M5IOE1 expander (0x4F). `zos-lora-cap-init` switches it on if the
  system exposes that line by name; otherwise M5Stack's image must do it. If
  `i2cdetect -y 1` doesn't show 0x43 with the cap fitted, start there.
- **TCXO.** The 1.8 V TCXO setting comes from Meshtastic's Cardputer-Adv
  variant (which also allows a crystal fallback). If the radio fails to
  init, try `DIO3_TCXO_VOLTAGE: false` in the meshtasticd config and
  `use_dio3_tcxo: false` in `/etc/openhop_repeater/config.yaml`.
- **GPS** is on `/dev/serial0` at 115200 baud and only used by meshtasticd.

## Tailscale, SSH and Emby

These three are thin front ends over well-tested programs:

- **Tailscale** talks to `tailscaled` through its LocalAPI (the unix socket
  the `tailscale` CLI uses), changing one setting at a time. That avoids
  `tailscale up`, which refuses to run when other flags were set earlier.
  Signing in shows the login link as a QR code to scan with your phone. A
  `VPN` mark in the status bar shows when you're connected, and the SSH app
  lists your online tailnet devices.
- **SSH** runs the real OpenSSH client in a pseudo-terminal, so keys,
  `~/.ssh/config`, agents, `known_hosts` prompts and passwords all work as
  usual. The terminal is libvterm drawn with crisp bitmap fonts: 53×14
  characters (6×12), 53×17 (6×10) or 64×21 (5×8). "My SSH key" makes an
  ed25519 key and shows the public half as a QR code, so you can get it onto
  a server from your phone.
- **Emby** signs in with your username and password once, then keeps only the
  access token (in `emby.json`, mode 600). Video is transcoded by the server
  to H.264 HLS at the chosen size (480p by default), which the CM0 decodes in
  hardware and mpv can seek in. On the device, mpv draws directly to the
  panel while ZeroOS pauses its own drawing; keys still control playback.
  Progress is reported every 10 s, so "Continue watching" stays in sync with
  your other Emby apps. Music plays through to the end of the album.

Setup on the device:

```sh
sudo ./tools/install-net.sh     # mpv, openssh-client, Tailscale (+ operator rights)
```

**Not verified on hardware yet:** mpv's `--vo=drm` output on the ST7789
panel and handing the panel back to ZeroOS afterwards. If video doesn't show,
try `ZOS_MPV_ARGS="--vo=drm --drm-connector=..."` or play to HDMI.

## USB drive mode

Open **USB Drive** and follow it. The first time, it switches USB to
device mode and restarts. Then:

1. **Slide the (tiny, easy to miss) switch on the left side from OTG to USB.**
2. Connect the **top-right USB-C port** to the computer, with a data cable.
3. Press Enter.

The computer sees a drive called ZEROOS with everyone's `Users/<Name>`
folders. Eject it on the computer, press Esc, and ZeroOS picks up the new
files.

The slide switch matters as much as the software: it connects the CM0's
USB either to the onboard hub (OTG: USB-A port, keyboard and mouse dongles)
or to the USB-C port (USB). On OTG, a computer never sees the drive.
Slide it back to OTG after switching back to host mode (**H**).

- **Where the files live.** On a freshly flashed card, the first boot gives
  the system 30% of the card (8-16 GB) and makes the rest an exFAT partition
  (works on macOS, Windows and Linux, and with files over 4 GB). It is
  mounted at `/data`, and `~/Music`, `~/Videos`, `~/Pictures` and `~/Books`
  point into it. Cards set up before this, or too small for a partition, get
  a drive image file instead (`/var/lib/zeroos/drive.img`, size chosen in the
  app). Either way the computer gets the drive to itself while it's shared:
  ZeroOS unmounts it, so the two never write at the same time.
- **Host or device.** The CM0 has a single USB controller. It normally runs
  as a USB *host* (the onboard hub and the USB-A port); to be a drive it has
  to run as a USB *device*. Switching is a `config.txt` change
  (`dtoverlay=dwc2,dr_mode=peripheral`, the same one M5Stack's ADB mode
  uses) and a restart, which the app does for you and then opens itself
  again. While in device mode the USB-A port and the hub are off; **H** in
  the app switches back (and the switch goes back to OTG).
- **Speed.** USB 2.0: expect ~30-35 MB/s, against ~0.5 MB/s over Wi-Fi.
- `sudo zeroos-usb status | start | stop | mode device|host` does the same
  from a shell.

## Users

ZeroOS starts at a login screen. Each user has:

- **Their own media**: `Users/<Name>/Books`, `Music`, `Videos` and
  `Pictures` on the USB drive area (`/data`). On a computer in USB drive
  mode that is `ZEROOS/Users/<Name>/…`.
- **Their own app data**: reading positions, music library and queue, video
  resume points, Emby sign-in, SSH hosts, key and known hosts, and settings
  like media volume and subtitle language.
- **A lock of their choice**: a 4-8 digit PIN, a password, or none. After
  5 wrong tries it waits 30 s, doubling each time after that.

Shared by everyone: Wi-Fi, brightness, HDMI, the radios
(Meshtastic/MeshCore messages), Tailscale and software updates.

**The first start** after this version (or on a new card) asks for a name
and an optional PIN or password. That first account is an admin and takes
over everything already on the device: media, app data and settings.

**Settings → Users** lists everyone:
- Admins can add users, make others admin, reset someone's lock and
  remove users (their app data goes, their files stay in `Users/<Name>`).
- Anyone can rename themselves (their folder follows) and set or change
  their own lock.
- **Log out** is here and on the Power page.
- Only admins can install software updates.

These are profiles within one Linux account, not separate Linux users.
Folders are kept apart but not locked: the drive in USB mode and a shell on
the device can reach everyone's files. ZeroOS restarts on log out, so
nothing of one user stays in memory for the next. Restarts for USB drive
mode and updates keep you logged in.

The list is in `~/.local/state/zeroos/users.json` (mode 600; PINs and
passwords as SHA-512 crypt hashes), each user's data in
`~/.local/state/zeroos/users/<id>/`.

## Software updates

Settings → **Software update** installs new ZeroOS versions without
reflashing. It checks this repository's GitHub releases (once a day when
online, or when you open the page), shows what's new, and installs on
Enter (press twice). ZeroOS restarts on the new version and says so.

- **What gets updated**: ZeroOS itself: the program, its helper scripts,
  systemd units and polkit/udev rules (under 1 MB).
  Every release publishes this as `zeroos-update-<version>.tar.xz` next to
  the image. Raspberry Pi OS keeps its own updates (apt), and the kernel and
  M5Stack drivers stay put; a release that needs a newer system says so,
  and then you flash the image.
- **Safety**: the download is checked against its `.sha256`, and the
  program's libraries are checked before anything changes. The previous
  version is saved first. If the new one fails to start three times in a
  row, it is put back by itself. **Go back to …** on the same page undoes
  the last update by hand.
- **From a shell**: `zeroos-update check`, `sudo zeroos-update install
  [v0.3.5]`, `sudo zeroos-update rollback`.
- **Older cards (v0.3.3 and earlier)** don't have the updater yet. Flash
  the new image, or over SSH:

  ```sh
  curl -fsSL -o /tmp/zeroos-update https://raw.githubusercontent.com/brechtryckaert/zeroos/main/tools/image/files/zeroos-update
  sudo bash /tmp/zeroos-update install
  ```

How it works: `tools/image/files/zeroos-update` (root, run with `sudo -n`
by ZeroOS) does the download and install. It runs as its own systemd unit,
so restarting ZeroOS doesn't stop it. It installs with
`tools/image/zeroos-install-files.sh` from the new version, the same script
`zeroos-setup.sh` uses, so an updated card and a freshly flashed one match.
`ZOS_UPDATE_REPO=owner/name` points it at a fork.

## Flashable image

`tools/image/build-image.sh` turns the official **Raspberry Pi OS Lite
(arm64, Trixie)** image into a ZeroOS SD-card image. (Raspberry Pis boot
from a raw disk image, `.img`, not an `.iso`.)

| Where you build | Command | Time |
|---|---|---|
| GitHub (arm64 runner, no emulation) | Actions → **ZeroOS image** → Run workflow; a `v*` tag also publishes a release | ~40 min |
| An arm64 Linux machine (Pi 4/5, arm64 VM) | `sudo ./tools/image/build-image.sh` | ~30-60 min |
| An x86-64 Linux machine | `sudo apt install qemu-user-static binfmt-support fdisk e2fsprogs xz-utils rsync curl git`, then the same command | 1-3 h (qemu) |
| Docker (Linux, or macOS with Docker Desktop) | `./tools/image/build-docker.sh` | as above |

The result is `out/zeroos-<version>-<date>.img.xz` and the update bundle
`out/zeroos-update-<version>.tar.xz`, each with a `.sha256`.
Options after `--` are passed to the setup script, for example:

```sh
sudo ./tools/image/build-image.sh -- --user zero --password s3cret \
    --hostname zero1 --region EU_868 --meshcore-preset eu-narrow --country BE
sudo ./tools/image/build-image.sh --base ~/Downloads/2026-10-01-raspios-trixie-arm64-lite.img.xz
```

What the build does (`tools/image/zeroos-setup.sh`, run inside the image):

- **CardputerZero hardware**: builds M5Stack's
  [m5stack-linux-dtoverlays](https://github.com/m5stack/m5stack-linux-dtoverlays)
  drivers from source against the image's kernel (ST7789V panel, TCA8418
  keyboard, ES8389 audio, M5IOE1 expander, BQ27220 battery gauge, IMU) and
  writes the same `config.txt` M5Stack's own image uses. M5Stack's own
  image builder does the same. The driver commit is pinned (it builds
  against Raspberry Pi's 6.18 kernel, not 6.12 or 7.0), and so are
  M5Stack's boot splash files, which are checksummed. Because a new kernel
  series can break these out-of-tree drivers, the kernel packages are
  **held** after the build. If you unhold them (`sudo apt-mark unhold
  linux-image-rpi-v8 linux-headers-rpi-v8`), a kernel hook rebuilds the
  drivers on every kernel update and warns if they no longer compile.
  `sudo zeroos-m5-drivers` rebuilds them by hand.
- **ZeroOS**, built from this tree and started at boot by `zeroos.service`
  (`zeroos-session` finds the panel, keyboard and DRM card). The source stays
  in `/opt/zeroos/src`, so you can rebuild on the device.
- **Mesh**: meshtasticd with the cap config; the openHop MeshCore repeater
  (pinned commit) in monitor mode with the ZeroOS companion; the cap init
  script, drop-ins and polkit rule. Neither daemon starts at boot: the apps
  start whichever you open. The Meshtastic region and owner are set the
  first time meshtasticd runs.
- **Network**: Tailscale (the ZeroOS user becomes operator on first boot),
  OpenSSH client, mpv.
- **System**: user `zero` (any UID-1000 `pi` account is renamed), hostname,
  time zone, Wi-Fi country, no serial console (the UART belongs to the cap's
  GPS), no login console on the panel (`fbcon=map:off`), persistent journal.
  Raspberry Pi OS's first-boot user wizard is turned off.

The image is shrunk to its contents. On first boot it splits the card into
the system and the USB drive area (see USB drive mode).

### Flashing and first boot

The CardputerZero boots from its microSD card. Flash the `.img.xz` with
[Raspberry Pi Imager](https://www.raspberrypi.com/software/) (Choose OS →
*Use custom*) or balenaEtcher. Imager's own settings (Wi-Fi, SSH) go
through Raspberry Pi OS's cloud-init and should work, but `zeroos.txt`
below is the route ZeroOS is built around. You can also use
rpiboot over USB with the card in the device; see M5Stack's
[image guide](https://docs.m5stack.com/en/guide/linux/cardputerzero/image).

Before the first boot, you can put a `zeroos.txt` file on the card's boot
partition (the one that holds `config.txt`). It is applied once, then
renamed to `zeroos.txt.applied` with the passwords blanked:

```ini
wifi_ssid=My network
wifi_password=secret
wifi_country=BE
hostname=zeroos
timezone=Europe/Brussels
password=choose-one
ssh_key=ssh-ed25519 AAAA... me@laptop
```

`ssh_key` also turns on the SSH server. Without `password` or `--password`,
the `zero` account has no password, so nobody can log in until you set one.

`tools/image/test/make-test-image.sh` builds a stand-in "Raspberry Pi OS"
image from any root file system, to test the builder without downloading
the real one.

## Installing by hand

On a CardputerZero that already runs Raspberry Pi OS Lite (or M5Stack's
image, whose launcher it switches off), `sudo ./tools/image/zeroos-setup.sh`
does the same as the image build; reboot afterwards. To install only the
shell:

```sh
sudo ./tools/fetch-deps.sh --apt
cmake -S . -B build -DZOS_WITH_SDL=OFF && cmake --build build -j4
sudo cmake --install build                      # → /usr/local/bin/zeroos
sudo usermod -aG video,input pi
sudo cp system/zeroos.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl enable --now zeroos
```

Build on the device itself or in an arm64 Raspberry Pi OS container. With 512
MB RAM, use `-j2` if the build runs out of memory.

### Display

ZeroOS draws to the LCD's framebuffer at 320×170 (RGB565). With M5Stack's
drivers (`cardputerzero-v5-overlay`, a `panel-mipi-dbi` DRM panel) that is
`/dev/fb_lcd`, a udev link to the panel's `/dev/fbN`; `zeroos-session`
picks it up. If a panel ever comes up as 170×320 portrait, run
`zeroos --rotate 90`.

Check with `fbset -fb /dev/fb_lcd` and `cat /dev/urandom > /dev/fb_lcd`
(noise means it works).

### Keyboard

M5Stack's TCA8418 driver handles the Fn and Sym layers itself and reports
plain key codes (arrows on Fn). ZeroOS uses
`/dev/input/by-path/platform-3f804000.i2c-event` when it exists, otherwise
the first evdev keyboard with letter keys. To pin one, use
`--kbd /dev/input/eventN`.

## Command line

```
zeroos [--backend sdl|fbdev|headless] [--fb /dev/fb0] [--kbd /dev/input/eventN]
       [--rotate 0|90|180|270] [--books DIR] [--state DIR] [--fonts DIR]
       [--zoom N] [--demo-status] [--script "key:right*3;wait:200;shot:x.ppm"]
       [--radio-control | --no-radio-control]
```

`--script` runs headless: `key:NAME[*N]` (right, left, up, down, enter, esc,
space, backspace, tab, pgup, pgdn, or a single character), `type:TEXT` (`\n`
= Enter), `ctrl:C`, `app:ID` (open an app), `wait:MS` (real time, so sockets
and child processes keep working), `shot:FILE.ppm`. Steps are separated by
`;`, so `type:` text can't contain one.

ZeroOS starts and stops the radio daemons only with the fbdev backend (on
the device) or `--radio-control`. `ZOS_MESHTASTIC_ADDR` and
`ZOS_MESHCORE_ADDR` (`host:port`) point the apps at other daemons, e.g. a
meshtasticd on another machine; `ZOS_MESHTASTIC_UNIT` / `ZOS_MESHCORE_UNIT`
override the systemd unit names.

## Next steps

1. Bring-up on hardware: boot the image, then check the cap (header power
   on GPIO5, antenna switch, TCXO; see above), mpv on the panel and on
   HDMI, HDMI sound, the camera (rpicam-vid's raw output stride), the first
   boot's card layout, and which USB-C port carries data in device mode.
2. Music: gapless across albums, playlists you can save, lyrics; the
   speaker amplifier (GPIO 24 on early boards) if it needs switching.
3. Camera: preview while recording (rpicam-vid's low-res stream), photo
   resolution and timer settings.
4. Video: thumbnails, and a "Cardputer" transcode preset (H.264, 480p, AAC)
   for files the CM0 can't decode in hardware (HEVC, 10-bit).
5. Reader extras: portrait mode (the IMU can flip it), bookmarks list,
   search, hyphenation.
6. Mesh extras: a region/preset picker in Settings (Meshtastic admin
   messages), positions from the cap's GPS, a map of nodes.
7. Settings: Bluetooth (headphones), screen sleep timer.
8. SSH: copy and paste, and Mosh for flaky connections.

## Licences

LVGL is MIT. **MuPDF is AGPL-3.0**, and linking it makes the whole program
AGPL. That's fine for an open-source image; distributing a closed build
needs a commercial MuPDF licence from Artifex.

meshtasticd (GPL-3.0) and the openHop repeater (MIT) run as separate
programs; ZeroOS only speaks their network protocols and links neither.
The test tools use the `meshtastic` and `pymc-core` Python packages.

libcurl (curl licence), cJSON (MIT) and libvterm (MIT) are linked;
OpenSSH, mpv and Tailscale run as separate programs. The terminal fonts are
X11 misc-fixed, which is in the public domain.

The image also contains M5Stack's CardputerZero kernel drivers (GPL-2.0,
built from source in the image), and M5Stack's boot splash build of the
Raspberry Pi firmware (Raspberry Pi's firmware licence; `--no-m5-splash`
keeps the stock firmware). Raspberry Pi OS itself is Debian plus Raspberry
Pi packages under their own licences.
