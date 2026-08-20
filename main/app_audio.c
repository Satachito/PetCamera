#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "bsp/esp-bsp.h"

#include "app_audio.h"

static const char *TAG = "audio";

#define BYTES_PER_SAMPLE (APP_AUDIO_BITS / 8)
#define BYTES_PER_SECOND (APP_AUDIO_SAMPLE_RATE * BYTES_PER_SAMPLE * APP_AUDIO_CHANNELS)

/* Two seconds of slack between the capture task and whoever is listening. Any
 * more just adds latency; any less and a slow fetch leaves a gap. */
#define RING_BYTES (BYTES_PER_SECOND * 2)

/* One I2S read. Small enough to keep latency down, large enough that the task
 * is not woken constantly. */
#define CAPTURE_CHUNK 1024

/* The codec is read as a stereo pair and one side is kept. Selecting a single
 * channel at the codec would be tidier, but the BSP builds the ES7210 with
 * inputs 1 and 2 enabled, and reading both then choosing in software works
 * against that configuration without reaching into the BSP. */
#define CAPTURE_CHANNELS 2

#define PLAYBACK_CHUNK 2048

/* Recordings live in a subdirectory, not the card root. A FAT16 root directory
 * is a fixed-size table and every long filename eats several of its entries, so
 * it fills up while gigabytes are still free — and then every new file fails
 * with EACCES, which is a bewildering way to hit the limit. */
#define SOUNDS_DIR CONFIG_BSP_SD_MOUNT_POINT "/sounds"

static struct {
    esp_codec_dev_handle_t mic;
    esp_codec_dev_handle_t speaker;
    uint8_t              *ring;
    size_t                write_pos;   /* total bytes ever written */
    size_t                read_pos;    /* total bytes ever handed out */
    SemaphoreHandle_t     lock;
    SemaphoreHandle_t     speaker_lock;
    QueueHandle_t         jobs;
    bool                  playing;
    bool                  recording;
    app_audio_phase_t     phase;
    int                   last_bytes;
    char                  last_error[48];
    int64_t               phase_ends_us;
    bool                  running;
    int                   volume;
    float                 gain;
} s_au;

static void capture_task(void *arg)
{
    uint8_t *chunk = heap_caps_malloc(CAPTURE_CHUNK * CAPTURE_CHANNELS, MALLOC_CAP_DEFAULT);
    int16_t *mono = heap_caps_malloc(CAPTURE_CHUNK, MALLOC_CAP_DEFAULT);

    if (!chunk || !mono) {
        ESP_LOGE(TAG, "out of memory for the capture chunk");
        vTaskDelete(NULL);
        return;
    }

    while (s_au.running) {
        /* esp_codec_dev_read returns a STATUS, not a byte count: ESP_CODEC_DEV_OK
         * is 0 and means the whole buffer was filled. Reading it as a length is
         * silent — every successful read looks like a failure and gets thrown
         * away, which is exactly what "no audio" turned out to be. */
        int rc = esp_codec_dev_read(s_au.mic, chunk, CAPTURE_CHUNK * CAPTURE_CHANNELS);
        int got = (rc == 0) ? CAPTURE_CHUNK : 0;

        if (got == 0) {
            static int complained;
            if (complained < 3) {
                complained++;
                ESP_LOGE(TAG, "esp_codec_dev_read failed: %d", rc);
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        {
            static int64_t last_report;
            int64_t now = esp_timer_get_time();
            if (now - last_report > 5000000) {
                last_report = now;
                int16_t *pcm = mono;
                int peak = 0;
                for (int i = 0; i < got / 2; i++) {
                    int v = pcm[i] < 0 ? -pcm[i] : pcm[i];
                    if (v > peak) {
                        peak = v;
                    }
                }
                ESP_LOGI(TAG, "capturing %d bytes/read, peak %d/32767, buffered %u",
                         got, peak, (unsigned)(s_au.write_pos - s_au.read_pos));
            }
        }

        {
            const int16_t *stereo = (const int16_t *)chunk;
            int pairs = got / 2;

            for (int i = 0; i < pairs; i++) {
                mono[i] = stereo[i * CAPTURE_CHANNELS + CONFIG_PETCAM_MIC_CHANNEL];
            }
        }

        xSemaphoreTake(s_au.lock, portMAX_DELAY);
        for (int i = 0; i < got; i++) {
            s_au.ring[(s_au.write_pos + i) % RING_BYTES] = ((uint8_t *)mono)[i];
        }
        s_au.write_pos += got;
        /* A listener that falls more than a buffer behind is dropped forward to
         * the newest audio: live sound matters more than complete sound. */
        if (s_au.write_pos - s_au.read_pos > RING_BYTES) {
            s_au.read_pos = s_au.write_pos - RING_BYTES;
        }
        xSemaphoreGive(s_au.lock);
    }

    free(chunk);
    free(mono);
    vTaskDelete(NULL);
}

/* Defined below; the worker needs them before its definition. */
static esp_err_t record_now(const char *name, int milliseconds);
static esp_err_t play_now(const char *name);

typedef struct {
    bool record;
    char name[96];
    int  milliseconds;
} audio_job_t;

/* One worker for both jobs, so a recording and a playback can never fight over
 * the codec, and neither ever runs on an HTTP handler's stack. */
static void worker_task(void *arg)
{
    audio_job_t job;

    (void)arg;
    for (;;) {
        if (xQueueReceive(s_au.jobs, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (job.record) {
            /* Count down on the device's own screen first: whoever is going to
             * speak is standing next to it, not looking at the phone. */
            s_au.phase = APP_AUDIO_COUNTDOWN;
            s_au.phase_ends_us = esp_timer_get_time() +
                                 CONFIG_PETCAM_RECORD_COUNTDOWN_S * 1000000LL;
            while (esp_timer_get_time() < s_au.phase_ends_us) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }

            s_au.recording = true;
            s_au.phase = APP_AUDIO_RECORDING;
            s_au.phase_ends_us = esp_timer_get_time() + job.milliseconds * 1000LL;
            record_now(job.name, job.milliseconds);
            s_au.recording = false;
        } else {
            s_au.phase = APP_AUDIO_PLAYING;
            s_au.phase_ends_us = 0;
            play_now(job.name);
        }
        s_au.phase = APP_AUDIO_IDLE;
    }
}

esp_err_t app_audio_start(void)
{
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = APP_AUDIO_BITS,
        .channel         = CAPTURE_CHANNELS,
        .channel_mask    = 0x03,
        .sample_rate     = APP_AUDIO_SAMPLE_RATE,
    };

    s_au.ring = heap_caps_malloc(RING_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_au.ring) {
        return ESP_ERR_NO_MEM;
    }
    s_au.lock = xSemaphoreCreateMutex();
    s_au.speaker_lock = xSemaphoreCreateMutex();
    if (!s_au.lock || !s_au.speaker_lock) {
        return ESP_ERR_NO_MEM;
    }

    s_au.mic = bsp_audio_codec_microphone_init();
    if (!s_au.mic) {
        ESP_LOGW(TAG, "no microphone; listening is unavailable");
        return ESP_FAIL;
    }
    s_au.speaker = bsp_audio_codec_speaker_init();
    if (!s_au.speaker) {
        ESP_LOGW(TAG, "no speaker; playback is unavailable");
    }

    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_au.mic, &fs) == 0, ESP_FAIL, TAG,
                        "cannot open the microphone");
    esp_codec_dev_set_in_gain(s_au.mic, 30.0f);

    if (mkdir(SOUNDS_DIR, 0777) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "cannot create %s: errno %d (%s)", SOUNDS_DIR, errno,
                 strerror(errno));
    }

    s_au.volume = CONFIG_PETCAM_SPEAKER_VOLUME;

    s_au.jobs = xQueueCreate(4, sizeof(audio_job_t));
    if (!s_au.jobs) {
        return ESP_ERR_NO_MEM;
    }

    s_au.running = true;
    if (xTaskCreate(capture_task, "petcam_audio", 4096, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    /* Generous stack: this one does FATFS writes. */
    if (xTaskCreate(worker_task, "petcam_sound", 8192, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "listening at %d Hz, %d-bit mono", APP_AUDIO_SAMPLE_RATE, APP_AUDIO_BITS);
    return ESP_OK;
}

/* Reads from an explicit position in the ring.
 *
 * A recorder and a listener must not share one cursor: whichever asked first
 * consumes the bytes and the other is left with a gap. Each keeps its own. */
static size_t read_at(size_t *cursor, uint8_t *out, size_t want_bytes, int timeout_ms)
{
    int waited = 0;

    if (!s_au.ring) {
        return 0;
    }

    for (;;) {
        size_t available;

        xSemaphoreTake(s_au.lock, portMAX_DELAY);
        /* Fallen further behind than the ring is long: skip to the oldest audio
         * still held rather than reading bytes that have been overwritten. */
        if (s_au.write_pos - *cursor > RING_BYTES) {
            *cursor = s_au.write_pos - RING_BYTES;
        }
        available = s_au.write_pos - *cursor;
        if (available >= want_bytes) {
            for (size_t i = 0; i < want_bytes; i++) {
                out[i] = s_au.ring[(*cursor + i) % RING_BYTES];
            }
            *cursor += want_bytes;
            xSemaphoreGive(s_au.lock);
            return want_bytes;
        }
        xSemaphoreGive(s_au.lock);

        if (waited >= timeout_ms) {
            return 0;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
        waited += 20;
    }
}

size_t app_audio_read(uint8_t *out, size_t want_bytes, int timeout_ms)
{
    return read_at(&s_au.read_pos, out, want_bytes, timeout_ms);
}

/* Minimal 44-byte canonical WAV header. Writing one by hand beats pulling in a
 * container library for a format this small. */
static void write_wav_header(FILE *f, uint32_t data_bytes)
{
    const uint32_t rate = APP_AUDIO_SAMPLE_RATE;
    const uint16_t channels = APP_AUDIO_CHANNELS;
    const uint16_t bits = APP_AUDIO_BITS;
    const uint32_t byte_rate = rate * channels * bits / 8;
    const uint16_t block_align = channels * bits / 8;
    uint8_t h[44];

    memcpy(h, "RIFF", 4);
    *(uint32_t *)(h + 4)  = 36 + data_bytes;
    memcpy(h + 8, "WAVEfmt ", 8);
    *(uint32_t *)(h + 16) = 16;
    *(uint16_t *)(h + 20) = 1;              /* PCM */
    *(uint16_t *)(h + 22) = channels;
    *(uint32_t *)(h + 24) = rate;
    *(uint32_t *)(h + 28) = byte_rate;
    *(uint16_t *)(h + 32) = block_align;
    *(uint16_t *)(h + 34) = bits;
    memcpy(h + 36, "data", 4);
    *(uint32_t *)(h + 40) = data_bytes;

    fwrite(h, 1, sizeof(h), f);
}

static void sound_path(char *out, size_t len, const char *name)
{
    snprintf(out, len, "%s/%s", SOUNDS_DIR, name);
}

static esp_err_t record_now(const char *name, int milliseconds)
{
    char path[300];
    FILE *f;
    uint8_t *chunk;
    uint32_t want = (uint32_t)((int64_t)BYTES_PER_SECOND * milliseconds / 1000);
    uint32_t written = 0;
    size_t cursor;

    if (!s_au.ring || !name || !name[0]) {
        return ESP_ERR_INVALID_STATE;
    }

    sound_path(path, sizeof(path), name);
    s_au.last_bytes = 0;
    strlcpy(s_au.last_error, "recording", sizeof(s_au.last_error));

    f = fopen(path, "wb");
    if (!f && errno == ENOENT) {
        /* The directory is created at startup, but that can fail if the card's
         * root was full at the time. Retry here so freeing space is enough to
         * recover, without needing a reboot. */
        if (mkdir(SOUNDS_DIR, 0777) == 0) {
            f = fopen(path, "wb");
        }
    }
    if (!f) {
        ESP_LOGE(TAG, "cannot create %s: errno %d (%s)", path, errno, strerror(errno));
        snprintf(s_au.last_error, sizeof(s_au.last_error), "open failed: %s", strerror(errno));
        return ESP_FAIL;
    }
    write_wav_header(f, want);

    chunk = malloc(CAPTURE_CHUNK);
    if (!chunk) {
        fclose(f);
        strlcpy(s_au.last_error, "out of memory", sizeof(s_au.last_error));
        return ESP_ERR_NO_MEM;
    }

    /* Start from this instant, not from whatever is already buffered.
     *
     * The ring always holds a couple of seconds of the recent past. Reading it
     * from wherever the listener happened to leave off meant a three-second
     * recording was filled mostly from before the countdown even finished — it
     * completed almost immediately and contained the silence from before the
     * speaker was told to begin. */
    xSemaphoreTake(s_au.lock, portMAX_DELAY);
    cursor = s_au.write_pos;
    xSemaphoreGive(s_au.lock);

    while (written < want) {
        size_t take = want - written;
        size_t got;

        if (take > CAPTURE_CHUNK) {
            take = CAPTURE_CHUNK;
        }
        got = read_at(&cursor, chunk, take, 2000);
        if (got == 0) {
            strlcpy(s_au.last_error, "microphone gave nothing", sizeof(s_au.last_error));
            break;
        }
        if (fwrite(chunk, 1, got, f) != got) {
            snprintf(s_au.last_error, sizeof(s_au.last_error), "write failed: %s",
                     strerror(errno));
            break;
        }
        written += got;
    }

    free(chunk);
    /* The header claims 'want' bytes; correct it if capture ended early. */
    if (written != want) {
        fseek(f, 0, SEEK_SET);
        write_wav_header(f, written);
    }
    fclose(f);

    s_au.last_bytes = (int)written;
    if (strcmp(s_au.last_error, "recording") == 0) {
        strlcpy(s_au.last_error, written > 0 ? "ok" : "nothing captured",
                sizeof(s_au.last_error));
    }
    ESP_LOGI(TAG, "recorded %s (%u ms, %u bytes) - %s", name,
             (unsigned)(written * 1000 / BYTES_PER_SECOND), (unsigned)written,
             s_au.last_error);
    return written > 0 ? ESP_OK : ESP_FAIL;
}

/* Scans the file for its loudest sample and works out how much the whole thing
 * can be scaled before anything clips.
 *
 * Recordings sit well below full scale, so playing them as stored wastes most
 * of the amplifier's range — a bigger effect on how loud the Tab5 sounds than
 * anything the volume control can do. */
static float playback_gain(FILE *f)
{
    const float ceiling = 30000.0f;            /* just under full scale */
    const float max_gain = powf(10.0f, CONFIG_PETCAM_PLAYBACK_MAX_BOOST / 20.0f);
    int16_t buf[512];
    int peak = 0;
    float gain;

    fseek(f, 44, SEEK_SET);
    for (;;) {
        size_t n = fread(buf, sizeof(int16_t), sizeof(buf) / sizeof(buf[0]), f);

        if (n == 0) {
            break;
        }
        for (size_t i = 0; i < n; i++) {
            int v = buf[i] < 0 ? -buf[i] : buf[i];
            if (v > peak) {
                peak = v;
            }
        }
    }
    fseek(f, 44, SEEK_SET);

    if (peak < 32) {
        return 1.0f;                            /* silence; leave it alone */
    }
    gain = ceiling / (float)peak;
    if (gain > max_gain) {
        gain = max_gain;
    }
    if (gain < 1.0f) {
        gain = 1.0f;
    }
    return gain;
}

static esp_err_t play_now(const char *name)
{
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = APP_AUDIO_BITS,
        .channel         = APP_AUDIO_CHANNELS,
        .channel_mask    = 1,
        .sample_rate     = APP_AUDIO_SAMPLE_RATE,
    };
    char path[300];
    uint8_t *chunk;
    FILE *f;

    if (!s_au.speaker) {
        return ESP_ERR_INVALID_STATE;
    }

    sound_path(path, sizeof(path), name);
    f = fopen(path, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }

    chunk = malloc(PLAYBACK_CHUNK);
    if (!chunk) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    /* One sound at a time: two overlapping writes to the codec would interleave
     * into noise. */
    xSemaphoreTake(s_au.speaker_lock, portMAX_DELAY);
    s_au.playing = true;

    /* Stand the microphone down for the duration.
     *
     * Both codecs sit on the same I2S peripheral, and the capture side holds it
     * open as a 2-channel receiver. Opening the speaker underneath that leaves
     * the peripheral configured for the wrong direction and width, so playback
     * runs to completion with nothing coming out of the speaker. */
    s_au.running = false;
    vTaskDelay(pdMS_TO_TICKS(80));
    esp_codec_dev_close(s_au.mic);

    {
        float gain = playback_gain(f);

        s_au.gain = gain;
        ESP_LOGI(TAG, "playing %s at %.1fx (+%.1f dB), volume %d%%",
                 name, gain, 20.0f * log10f(gain), s_au.volume);
    }

    fseek(f, 44, SEEK_SET);   /* skip the header we wrote */
    esp_codec_dev_open(s_au.speaker, &fs);
    /* After opening, not before. The codec is only open while a sound plays, so
     * a volume set at startup lands on a closed device and is lost — which is
     * why playback ran to completion and could not be heard. */
    esp_codec_dev_set_out_vol(s_au.speaker, s_au.volume);
    for (;;) {
        size_t got = fread(chunk, 1, PLAYBACK_CHUNK, f);

        if (got == 0) {
            break;
        }
        if (s_au.gain > 1.01f) {
            int16_t *pcm = (int16_t *)chunk;

            for (size_t i = 0; i < got / sizeof(int16_t); i++) {
                float v = pcm[i] * s_au.gain;

                /* Saturate rather than wrap: a wrapped sample turns a loud
                 * moment into a click. */
                pcm[i] = (int16_t)(v > 32767.0f ? 32767.0f
                                                : (v < -32768.0f ? -32768.0f : v));
            }
        }
        esp_codec_dev_write(s_au.speaker, chunk, got);
    }
    esp_codec_dev_close(s_au.speaker);

    /* Put the microphone back so listening continues. */
    {
        esp_codec_dev_sample_info_t mic_fs = {
            .bits_per_sample = APP_AUDIO_BITS,
            .channel         = CAPTURE_CHANNELS,
            .channel_mask    = 0x03,
            .sample_rate     = APP_AUDIO_SAMPLE_RATE,
        };

        esp_codec_dev_open(s_au.mic, &mic_fs);
        esp_codec_dev_set_in_gain(s_au.mic, 30.0f);
        s_au.running = true;
        xTaskCreate(capture_task, "petcam_audio", 4096, NULL, 4, NULL);
    }

    s_au.playing = false;
    xSemaphoreGive(s_au.speaker_lock);

    free(chunk);
    fclose(f);
    ESP_LOGI(TAG, "played %s", name);
    return ESP_OK;
}

void app_audio_normalise_name(char *name, size_t len)
{
    char *out = name;
    bool dropped = false;

    for (char *p = name; *p; p++) {
        unsigned char c = (unsigned char)*p;

        if (isalnum(c) || c == '-' || c == '_' || c == '.') {
            *out++ = (char)c;
        } else {
            dropped = true;
        }
    }
    *out = '\0';

    if (dropped && (out == name || strcmp(name, ".wav") == 0)) {
        snprintf(name, len, "sound_%08lu.wav",
                 (unsigned long)(esp_timer_get_time() / 1000000));
    }
}

esp_err_t app_audio_record(const char *name, int milliseconds)
{
    audio_job_t job = { .record = true, .milliseconds = milliseconds };

    if (!s_au.jobs) {
        return ESP_ERR_INVALID_STATE;
    }
    strlcpy(job.name, name, sizeof(job.name));
    return xQueueSend(s_au.jobs, &job, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t app_audio_play(const char *name)
{
    audio_job_t job = { .record = false };

    if (!s_au.jobs) {
        return ESP_ERR_INVALID_STATE;
    }
    strlcpy(job.name, name, sizeof(job.name));
    return xQueueSend(s_au.jobs, &job, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

bool app_audio_is_recording(void)
{
    return s_au.recording;
}

app_audio_phase_t app_audio_get_phase(int *ms_left)
{
    int64_t remain = s_au.phase_ends_us - esp_timer_get_time();

    *ms_left = (s_au.phase_ends_us && remain > 0) ? (int)(remain / 1000) : 0;
    return s_au.phase;
}

bool app_audio_is_playing(void)
{
    return s_au.playing;
}

esp_err_t app_audio_set_volume(int percent)
{
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    s_au.volume = percent;
    /* Applied on the next open; setting it now would only stick if a sound
     * happened to be playing. */
    if (s_au.playing && s_au.speaker) {
        esp_codec_dev_set_out_vol(s_au.speaker, percent);
    }
    ESP_LOGI(TAG, "speaker volume %d%%", percent);
    return ESP_OK;
}

const char *app_audio_last_record(int *bytes)
{
    *bytes = s_au.last_bytes;
    return s_au.last_error[0] ? s_au.last_error : "none";
}
