# Tab5 Pet Camera

A pet-watching camera on the M5Stack Tab5 (ESP32-P4). The built-in SC2356
camera is streamed as MJPEG over HTTP; any phone browser can watch it, no app
required. The Tab5's own screen doubles as a local console showing the URL, a QR
code to open it, and whether anyone is connected.

```
SC2356 (MIPI-CSI) ──> esp_video / V4L2 ──> hardware JPEG encoder ──> frame_bus
                                                                        │
                        ESP32-C6 (ESP-Hosted Wi-Fi) <── HTTP MJPEG <────┘
```

Frames are never copied. The sensor DMAs into the buffers the JPEG encoder reads
in place, and the HTTP handlers borrow the encoder's output buffer while sending
it.

## Measured on hardware

Verified on a Tab5 with chip revision v1.3, ESP-IDF v5.5.5:

| | |
| --- | --- |
| Capture + encode | 1280x720 RGB565, 14–15 fps, **0 frames dropped** |
| JPEG at quality 70 | ~110 KB/frame |
| JPEG at quality 80 | ~170 KB/frame |
| **Delivered over Wi-Fi** | **5.6–10.2 fps, 5–9 Mbps** at RSSI −61 dBm |
| Snapshot | valid baseline JPEG, 1280x720, 3 components |
| Firmware | 1.6 MB of an 8 MB app partition |

The capture side has headroom; the ESP-Hosted Wi-Fi link is what limits delivery,
and it is variable. Lower `PETCAM_MAX_FPS` to around 8 if you prefer a steady
rate over a higher peak.

**The on-board sensor only accepts 1280x720.** 1600x1200, 1024x768, 800x600 and
640x480 are all rejected by `VIDIOC_S_FMT`. The firmware probes and logs this at
startup, and falls back to the driver default if the configured size is refused.

## Build and flash

Requires ESP-IDF v5.4 or newer (developed and tested against v5.5.5).

```bash
. ~/esp/esp-idf/export.sh && idf.py set-target esp32p4 && idf.py menuconfig
```

Under **Tab5 Pet Camera**, set your Wi-Fi SSID and password. Resolution, JPEG
quality and the frame-rate cap live there too.

```bash
idf.py build flash monitor
```

## Using it

| URL | What it is |
| --- | --- |
| `http://<ip>/` or `http://tab5-petcam.local/` | Viewer page with live image and stats |
| `http://<ip>:81/stream` | Raw MJPEG — works in VLC or a bare `<img>` tag |
| `http://<ip>/snapshot` | One JPEG |
| `http://<ip>/status` | JSON: fps, resolution, frame size, uptime, heap, motion, rotation |
| `http://<ip>/screenshot` | JPEG of the Tab5's own panel (see below) |
| `http://<ip>/clips` | JSON list of recorded clips |
| `http://<ip>/clip?name=…` | Download one clip |
| `http://<ip>/audio` | Half a second of WAV from the microphone |
| `http://<ip>/sounds` | Recorded sounds on the card |
| `http://<ip>/recordsound?name=X.wav&ms=3000` | Record a sound on the device |
| `http://<ip>/playsound?name=X.wav` | Play one through the speaker |
| `POST http://<ip>/update` | Firmware update (see below) |

The Tab5's screen shows the URL and a QR code once it has an address. The serial
log prints fps and frame size every five seconds.

## Things that will bite you

**Never hand-write the ESP-Hosted SDIO pins.** esp_hosted 3.x ships a Tab5 board
preset — `CONFIG_ESP32P4_TAB5_C6_BOARD=y` — that fills in slot 1, 4-bit,
CMD=13 CLK=12 D0=11 D1=10 D2=9 D3=8 and the active-high reset on GPIO15. The 2.x
`CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_*` names no longer exist, and **unknown symbols
in `sdkconfig.defaults` are silently ignored** — a stale pin block reads fine and
leaves the EV-board defaults in place, so the C6 never enumerates. Run
`./check_sdkconfig.sh` after touching that file; it fails the build-time check
for any option that did not reach the generated `sdkconfig`.

**Chip revision.** `sdkconfig.defaults` targets pre-rev3.0 "ES" silicon, which is
what this unit is (v1.3) and what most shipped Tab5 units carry. On a production
rev3.x part, delete `CONFIG_ESP32P4_SELECTS_REV_LESS_V3` and
`CONFIG_ESP32P4_REV_MIN_0`; a mismatch is a boot loop right after the ROM log.

**ISP pipeline controller.** `CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER`
runs auto-exposure and auto-white-balance. Without it the image is dark and
green — it looks like a broken sensor but is raw demosaic output.

**USERPTR buffers belong to esp_video's rules, not the encoder's.**
`VIDIOC_QBUF` rejects any pointer not in PSRAM or not aligned to the driver's
`align_size`, so capture buffers use `heap_caps_aligned_alloc(64, …,
MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED)`. The JPEG encoder is the lenient
side: only its *output* buffer needs cache alignment, which is why the frame pool
uses `jpeg_alloc_encoder_mem()` and the capture buffers do not.

## Expected noise in the log

- `E csi_video: format width or height is invalid` — the startup size probe
  deliberately trying sizes the pipeline rejects. Not a fault.
- `E eh_init_evt: major version mismatch — OTA coprocessor from host` — the C6
  reports firmware version 0.0.0 against host esp_hosted 3.0.6. Wi-Fi works, but
  the mismatch is **not** entirely cosmetic: see below.

## Rotation

The Tab5 turns its console with the device, using the BMI270. Two rules make the
whole thing fall out:

1. **The camera and the panel are bolted to the same body**, so their alignment
   never changes. Only the reader moves. The live image therefore keeps a fixed
   relationship to the screen while the text, QR code and buttons rotate around
   it — exactly how a phone camera app behaves.
2. **LVGL rotates the preview and the UI by the same amount**, so relative to the
   upright UI the image is rotated by exactly the PPA angle.

From rule 1, the PPA angle must cancel the screen rotation:

```
ppa_angle = camera_mount_rotation + screen_rotation
```

**It is a plus, not a minus.** The PPA rotates counter-clockwise while LVGL's
display rotation runs the other way. Subtracting composes to `mount - 2R`, which
is self-consistent within the portrait poses and within the landscape poses but
**180 degrees apart between them** — the symptom is a preview that looks right
held one way and upside down held the other.

The geometry only closes for an odd quarter-turn. The camera is 16:9 and the
panel is 9:16 in its native scan order, so 90 or 270 is what lands the output
exactly on the logical resolution in all four poses — no letterboxing, no
overflow:

| Screen rotation | Logical resolution | PPA angle | PPA output |
| --- | --- | --- | --- |
| 0 | 720x1280 | 270 | 720x1280 |
| 90 | 1280x720 | 0 | 1280x720 |
| 180 | 720x1280 | 90 | 720x1280 |
| 270 | 1280x720 | 180 | 1280x720 |

From rule 2, a remote viewer matches the Tab5 by applying the same rotation the
Tab5 does relative to its own upright UI. `/status` publishes it as
`view_rotation`, already converted to CSS's clockwise convention
(`360 - ppa_angle`); the viewer page swaps the frame's aspect ratio along with
the image's dimensions so the rotated result still fills without gaps.

`/stream` itself is always the raw 1280x720 sensor image. A non-browser client
such as VLC sees it unrotated.

Calibration is empirical, not assumed: hold the device the intended way and read
`orientation.raw` from `/status`. On this unit, landscape with the USB port on
the left reports 90, which maps to LVGL rotation 90 and a 1280x720 logical
screen, so `PETCAM_ORIENTATION_OFFSET` stays 0.

Detection ignores the reading when the device is lying flat — gravity is then
almost all on Z and the X/Y values are noise — and requires about a second of
agreement before it turns the screen.

## The backlight is not part of display start

`bsp_display_start()` brings up the panel, the touch controller and LVGL, and
leaves the backlight off. The console renders perfectly into a black screen until
`bsp_display_brightness_set()` is called. The BSP header does say so; it is easy
to miss, and the failure looks like a dead panel rather than a missing call.

## A failing RPC will throttle the whole link

This cost a lot of debugging, so it is worth stating plainly.

`esp_wifi_sta_get_rssi()` cannot be encoded against the co-processor firmware
this Tab5 ships with — it fails inside ESP-Hosted with
`tx_worker: pack_req_payload failed (rc=-1 msg_id=341)`. The status UI called it
once per second. That one failing call per second dragged the **entire** data
path down:

| | fps | throughput |
| --- | --- | --- |
| Calling the broken RPC every second | 1.6 | 1.2 Mbps |
| Not calling it | **11.0** | **9.0 Mbps** |

A seven-fold difference from one unusable control call. `app_wifi_rssi()` now
gives up permanently after three consecutive failures, which is why `/status`
reports `"rssi":0` on this hardware. If you flash matching ESP-Hosted firmware to
the C6, RSSI starts working and the retry logic simply never trips.

The general lesson: on an ESP-Hosted link, control-plane RPCs share the transport
with your data. Never poll one that is failing.

## Known limits

- **One stream viewer at a time.** `esp_http_server` serves one request per
  instance and an MJPEG response never finishes. A second viewer waits. The page
  and `/snapshot` sit on a separate port so they always load.
- **The stream port is cross-origin.** Because the MJPEG stream has to live on
  its own port, the browser sees `:81` as a different origin from the page on
  `:80`, and some browsers and extensions refuse to load it — it shows up as
  `ERR_BLOCKED_BY_CLIENT` with no image. The page detects this and falls back to
  polling `/snapshot` on the same origin; the `transport` tile reads `mjpeg` or
  `poll` so you can tell which is in use. Polling costs one request per frame
  instead of one connection, but it works everywhere and lifts the
  single-viewer limit.
- **No night vision.** The SC2356 is an ordinary RGB sensor behind an IR-cut
  filter and the Tab5 has no IR illuminator; a dark room gives you nothing.
  Options: a night light, showing a dim white screen as an illuminator, or an IR
  USB camera on the USB-A port — `usb_host_uvc` is already pulled in by the BSP,
  and `bsp_usb_host_start(mode, limit_500mA)` gates that port's 5 V rail.
- **Wi-Fi is the bottleneck.** Measured 5–9 Mbps delivered here, against a
  theoretical ~30 Mbps for ESP-Hosted on the C6. Capture and JPEG encoding keep
  up easily; the link does not.
- **RSSI reads 0** with the co-processor firmware this unit shipped with.
- **Not safe to expose to the internet.** No authentication, no TLS. Keep it on
  the LAN and reach it from outside over a VPN.

## Layout

| File | Responsibility |
| --- | --- |
| `main/main.c` | Startup order and wiring |
| `main/app_camera.c` | V4L2 capture, size probing, hardware JPEG encoding |
| `main/frame_bus.c` | Reference-counted hand-off from producer to HTTP readers |
| `main/app_httpd.c` | Viewer page, snapshot, status JSON, MJPEG stream |
| `main/app_wifi.c` | ESP-Hosted station mode, NVS credentials, scanning, mDNS |
| `main/app_ui.c` | On-device console, record button, countdown banner |
| `main/app_audio.c` | Microphone ring buffer, recording, playback |
| `main/app_ota.c` | Firmware updates over Wi-Fi, with rollback |
| `main/app_preview.c` | PPA rotate/scale of the live image onto the panel |
| `main/app_orientation.c` | BMI270 to screen rotation, with debounce |
| `main/app_setup.c` | On-screen Wi-Fi setup: scan, keyboard, save to NVS |
| `main/app_motion.c` | Thumbnail differencing and .mjpeg clips to microSD |
| `check_sdkconfig.sh` | Fails if any `sdkconfig.defaults` option was ignored |

Wi-Fi credentials live in `sdkconfig.defaults.local` (gitignored, see
`.example`), or are set on the device itself and kept in NVS — NVS wins.

Neither Wi-Fi nor camera failure is fatal: both log the reason and leave the
console up, because a camera that reboots in a loop cannot tell you why.

## Seeing the device's own screen

`GET /screenshot` renders whatever the panel is showing — countdown, QR code,
settings panel — as a JPEG. Diagnosing a frozen or unexpected UI without it means
walking over to the device and describing what you see.

Two details it took a while to get right:

- **`lv_draw_buf_create()` cannot hold a full-screen snapshot.** It allocates
  through LVGL, whose pool is 64 KB against 1.8 MB needed. PSRAM is wrapped with
  `lv_draw_buf_init()` and `lv_snapshot_take_to_draw_buf()` draws into that.
- **The camera area comes out colour-shifted, and that is not fixable here.** The
  interface is rendered in LVGL's RGB565 byte order and the preview canvas holds
  the PPA's; they differ, so one `pixel_reverse` setting cannot honour both in a
  single JPEG. The interface wins, since that is what the endpoint is for.
  `/snapshot` gives the camera's real colour. The panel itself is correct either
  way — this is only an artefact of re-encoding the framebuffer.

## Sound

**Listening** works from the viewer page: press **Listen**. The page fetches half
a second of WAV at a time and schedules each piece to begin exactly where the
previous one ended, so they play as one continuous sound while staying within
about 1.5 s of live.

Short chunks rather than one endless response is deliberate. An endless response
occupies a whole `esp_http_server` instance — the MJPEG stream already does,
which is why it needs its own port — and that separate port then reads as
cross-origin and gets blocked in some browsers.

**Talking back is not browser microphone capture.** `getUserMedia` requires a
secure context and this camera is plain HTTP on the LAN, so the browser will
refuse regardless of what the page asks for. Instead a phrase is recorded on the
device and replayed on demand. For a pet camera that covers the same ground
without an HTTPS stack on a device whose link already tops out under 10 Mbps.

**Recording is a device-side action.** The microphone button on the Tab5 starts
a three-second countdown on its own screen, then shows a red REC banner with the
time remaining, then holds on zero for a second before the camera image returns.
The person who speaks is standing at the device, not looking at the phone, so
the phone cannot usefully be the thing that starts it — the remote trigger was
removed for that reason. Names are generated (`rec_HHMMSS.wav`) and changed
afterwards from the phone, which has a real keyboard.

Three things about the capture path are worth keeping in mind:

- **Recording starts from the present.** The ring buffer always holds a couple of
  seconds of the recent past, and reading it from wherever the listener left off
  filled most of a three-second recording with audio from before the countdown
  had even finished. It completed almost instantly and contained the silence
  before the speaker began.
- **A recorder and a listener need separate cursors** into that ring. Sharing one
  means whichever asks first consumes the bytes and the other gets a gap.
- **The microphone is on the codec's second input.** Speech through the first
  came out around -40 dBFS — a valid file that sounds like silence — against
  about -17 dBFS for the same words on input 1. Diagnose this by recording while
  talking and looking at the file: a four-channel probe of this codec reported
  levels that did not move when someone spoke and disagreed with what the
  capture path produced, and acting on it would have picked the wrong input.

**Playback needs the microphone out of the way.** Both codecs share one I2S
peripheral, and with the capture side holding it open as a 2-channel receiver,
playback ran to completion with nothing audible. The microphone is closed for
the duration and reopened afterwards.

**Status messages must not be written straight to LVGL.** `app_ui_set_status()`
stores the text for the refresh task to apply. Writing it inside the call meant
any message arriving while the LVGL lock was held — which is most of startup —
was silently dropped, leaving an earlier message stranded on screen.

**Volume is applied after the codec is opened**, not before. The speaker is only
open while a sound plays, so a level set at startup lands on a closed device and
is silently lost.

**Recordings are normalised on the way out.** Speech at a normal distance sits
around -17 dBFS, so playing it as stored wastes most of the amplifier. Each file
is scanned for its loudest sample and scaled to just under full scale, up to
`PETCAM_PLAYBACK_MAX_BOOST`. That is worth far more than any setting on a 1 W
speaker: 17 dB of unused headroom is more than the amplifier rating can recover.

Recording and playback run on their own worker task. Doing either inside an HTTP
handler blocks the single-threaded server for the duration and puts the FATFS
write on the handler's modest stack, which crashed the device outright.

One API detail worth remembering: **`esp_codec_dev_read` returns a status, not a
byte count.** `ESP_CODEC_DEV_OK` is 0, so treating the return as a length quietly
discards every successful read.

## Firmware updates over Wi-Fi

```bash
./tools/ota.sh <ip-or-hostname>
```

The image is written to the inactive app slot and the device reboots into it.
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` means a new image starts as *pending
verify* and is only made permanent when the application confirms itself — which
`main.c` does after the camera and the web server are running. A build that
cannot get that far is reverted on the next boot instead of stranding a device
that lives on a shelf. `/status` reports the running slot, and `tools/ota.sh`
warns if the slot did not change, since that means a rollback happened.

`/update` has no authentication. Anyone on the network can replace the firmware,
which raises the stakes of the "keep it off the internet" rule considerably.

## Do not change the FAT encoding on a card with data on it

`CONFIG_FATFS_API_ENCODING_UTF_8` with codepage 932 was tried so recordings could
be named in Japanese. The directory listing immediately became inconsistent —
files that had been deleted reappeared, others vanished — and although the
setting was reverted, **the card's allocation table stayed damaged**. Every
subsequent write failed with `ENOSPC` while `esp_vfs_fat_info` cheerfully
reported 113 MB free, so recordings and clips were created as zero-byte files.

The card had to be reformatted. There is deliberately no endpoint for that —
erasing storage over an unauthenticated HTTP interface is not a thing this
firmware should offer. Take the card out and format it as FAT on a computer, or
add `format_if_mount_failed` to the mount call if you want it handled
automatically.

Names are normalised to `a-z 0-9 - _` instead. It is a real limitation — a pet
called マレア cannot have a sound file named after her — but it is preferable to
a filesystem that reports space it cannot allocate.

The reformat also revealed the card is **64 GB, not the 512 MB the damaged
filesystem had been reporting** — about fifty hours of motion clips rather than
twenty minutes.

## The FAT root directory fills before the card does

Clips go to `/sdcard/clips` and recordings to `/sdcard/sounds`, not the card root.

A FAT16 root directory is a fixed-size table, and a long filename consumes
several of its entries — `clip_20260819_173701.mjpeg` costs three. On a 512 MB
card it filled after about forty clips, and then **every new file failed with
EACCES while 107 MB was still free**. Worse, a full root cannot hold a new
subdirectory either, so the fix could not apply itself: the old clips had to be
deleted first. Deletion therefore sweeps the root as well as the clips
directory, and both `mkdir` calls retry once space appears.

## Clips

Recorded clips appear on the viewer page under **Recorded clips**, newest first,
with **Play** and **Download** on each. Pressing Play scrolls the viewer back
into sight, since the list sits well below it.

Ordering comes from the modification time carried in `/clips`, not from the
order `readdir` happens to return, which on FAT is whatever sequence the
directory holds. Using the timestamp also sorts clips recorded before NTP set
the clock — those are named by uptime and would not sort against the dated ones
by name. Play works without any container or
transcoding step: the page streams the `.mjpeg` with `fetch`, cuts it at each
end-of-image marker, and shows the frames as blobs. That keeps playback
same-origin, avoids buffering a whole clip, and means the server is never held
open by a second endless response the way the live stream occupies port 81.

Off the device they are ordinary files:

```bash
curl -o clip.mjpeg "http://<ip>/clip?name=clip_20260819_163450.mjpeg"
```

A `.mjpeg` is JPEG frames end to end. It carries **no frame rate and no
duration**, so every player guesses — ffmpeg guesses 25 fps, and a clip recorded
at 5 fps then plays five times too fast and reports a fifth of its real length
(measured: a 22.8 s clip reported 4.56 s). QuickTime will not open the file at
all.

Convert with the recording rate given on the **input**:

```bash
./tools/mjpeg2mp4.sh clip.mjpeg
```

`-framerate` must come before `-i`. After `-i` it only relabels the output and
the timing stays wrong. The script defaults to 5 fps (`PETCAM_CLIP_FPS`) and
CRF 28 (`CRF`), which measured 11 MB against a 14 MB source; CRF 32 gave 5.9 MB.

Filenames are local time once SNTP has set the clock, and fall back to an uptime
counter if NTP could not be reached.

## Motion detection

Detection runs on the raw RGB565 frame the capture task already holds, so there
is no JPEG decode anywhere in the loop: the PPA shrinks each frame to a 40x24
thumbnail in hardware, the thumbnail is reduced to brightness, and successive
thumbnails a second apart are differenced.

Comparing brightness rather than colour matters. On RGB, every small
auto-white-balance correction reads as movement and the camera records itself
adjusting to a cloud.

Clips are concatenated JPEGs (`.mjpeg`) on the microSD card — ffmpeg and VLC play
them directly, and they can be appended to without rewriting a container. A
maximum clip length stops one long disturbance, a curtain in a draught, from
filling the card with a single enormous file; continued motion opens the next
clip.

No card simply means no recording: `app_motion_start()` failing is not fatal.

Two limits found by measuring rather than guessing:

- **The PPA will not shrink by more than 1/16 in one pass** — its scale fraction
  is four bits. A 40x24 thumbnail from 1280x720 is 1/32 and every call failed
  with `invalid scale`, so detection reported itself armed while never actually
  running. 96x54 sits comfortably above the floor. Watch `motion.score` in
  `/status`, not just `motion.armed`.
- **Writing every encoded frame cost 37 MB per minute** — 2.2 GB an hour of
  activity, at a sustained 1.3 MB/s to the card. `PETCAM_CLIP_FPS` caps it at 5
  by default, which more than halved the file size.

Long filenames must be enabled (`CONFIG_FATFS_LFN_HEAP`) or the timestamped
names do not fit 8.3 and the BSP warns at mount.

## Next steps

1. Night vision via an IR USB camera on the USB-A port.
2. Rotating `/stream` itself for non-browser clients, at the cost of one extra
   PPA pass per frame.
3. Matching ESP-Hosted firmware on the C6, which would restore RSSI and may lift
   the throughput ceiling.
4. Authentication on `/update`. It is unauthenticated, so anyone on the network
   can replace the firmware — acceptable on a trusted LAN, not beyond one.
