/*
 * app_audio — listening to the room, and talking back through recorded clips.
 *
 * Listening streams as short same-origin chunks rather than one endless
 * response. An endless response would occupy a whole esp_http_server instance
 * (the MJPEG stream already does, which is why it needs its own port) and that
 * port then reads as cross-origin to the browser. Repeatedly fetching half a
 * second of WAV avoids both problems at the cost of a little latency, which for
 * hearing a pet is not a cost at all.
 *
 * Talking back is deliberately not browser microphone capture: getUserMedia
 * requires a secure context, and this camera is served over plain HTTP on the
 * LAN. Instead a sound is recorded once on the device and replayed on demand.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define APP_AUDIO_SAMPLE_RATE 16000
#define APP_AUDIO_BITS        16
#define APP_AUDIO_CHANNELS    1

/** @brief Bring up the codec and start capturing into the ring buffer. */
esp_err_t app_audio_start(void);

/**
 * @brief Take the next block of captured audio.
 *
 * Blocks until @p want_bytes are available or the timeout expires.
 *
 * @return Bytes written to @p out.
 */
size_t app_audio_read(uint8_t *out, size_t want_bytes, int timeout_ms);

/**
 * @brief Queue a recording from the microphone into a WAV file on the card.
 *
 * Returns as soon as the job is queued. Recording for several seconds inside an
 * HTTP handler blocks the whole single-threaded server for that long, and does
 * the FATFS write on the handler's modest stack — which crashed the device.
 */
esp_err_t app_audio_record(const char *name, int milliseconds);

/**
 * @brief Strip a typed name down to what FAT can actually store.
 *
 * Anything outside a-z, 0-9, dash and underscore is dropped. Making non-ASCII
 * names work needs the volume's encoding changed, which corrupted the directory
 * listing when tried — names that had been deleted came back and others
 * vanished — so names are normalised instead.
 */
void app_audio_normalise_name(char *name, size_t len);

/** @brief Queue playback of a WAV file through the speaker. Returns immediately. */
esp_err_t app_audio_play(const char *name);

/** @brief True while a recorded sound is playing. */
bool app_audio_is_playing(void);

/** @brief True while a recording is being captured. */
bool app_audio_is_recording(void);

typedef enum {
    APP_AUDIO_IDLE,
    APP_AUDIO_COUNTDOWN,   /* about to record — this is the cue to start talking */
    APP_AUDIO_RECORDING,
    APP_AUDIO_PLAYING,
} app_audio_phase_t;

/**
 * @brief What the audio worker is doing, and for how much longer.
 *
 * Pressing record on a phone gives no clue when the microphone actually opens,
 * so the person standing next to the Tab5 talks into nothing. The device shows
 * this on its own screen instead.
 *
 * @param ms_left  Receives the milliseconds remaining in the current phase.
 */
app_audio_phase_t app_audio_get_phase(int *ms_left);

/**
 * @brief How the last recording finished.
 *
 * Serial is not always attached to a device that lives on a shelf, so the
 * outcome has to be readable over the network.
 *
 * @param bytes  Receives the audio bytes written.
 * @return Error string, or "ok".
 */
const char *app_audio_last_record(int *bytes);

/** @brief Speaker volume, 0-100. */
esp_err_t app_audio_set_volume(int percent);
