#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/jpeg_encode.h"
#include "esp_heap_caps.h"
#include "linux/videodev2.h"
/* Defines ESP_VIDEO_MIPI_CSI_DEVICE_NAME, which the BSP header refers to via
 * BSP_CAMERA_DEVICE without including it itself. */
#include "esp_video_device.h"
#include "bsp/esp-bsp.h"

#include "app_camera.h"
#include "app_motion.h"
#include "app_preview.h"
#include "frame_bus.h"

static const char *TAG = "camera";

/* Two capture buffers keep the sensor streaming while we encode the other one.
 * More would only add latency — we always want the newest frame. */
#define CAPTURE_BUFFERS 2

/* The JPEG pool needs one frame being encoded, one published, and headroom for
 * readers still sending an older one. */
#define FRAME_POOL_SIZE 4

/* Worst case for a detailed 1600x1200 scene at quality 100. Sized generously
 * because a truncated JPEG shows up as a corrupt image, not an error. */
#define JPEG_BUFFER_BYTES (512 * 1024)

/* esp_video checks the USERPTR against its own align_size; 64 bytes covers the
 * PSRAM cache line on the ESP32-P4. */
#define CAPTURE_BUFFER_ALIGN 64

#define MIN_FRAME_INTERVAL_US (1000000 / CONFIG_PETCAM_MAX_FPS)

static struct {
    int                   fd;
    jpeg_encoder_handle_t encoder;
    uint8_t              *capture_buf[CAPTURE_BUFFERS];
    uint32_t              capture_len[CAPTURE_BUFFERS];
    uint32_t              width;
    uint32_t              height;
    app_camera_stats_t    stats;
} s_cam;

/* Sizes worth offering for a room-watching stream, largest first. */
static const struct { uint32_t w, h; } k_candidate_sizes[] = {
    { 1600, 1200 }, { 1280, 720 }, { 1024, 768 }, { 800, 600 }, { 640, 480 },
};

static esp_err_t try_format(int fd, uint32_t width, uint32_t height)
{
    struct v4l2_format format = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .fmt.pix.width  = width,
        .fmt.pix.height = height,
        /* RGB565 is what the P4 ISP emits by default from the SC202CS RAW8
         * stream, and the JPEG encoder takes it directly — no conversion. */
        .fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565,
    };

    return ioctl(fd, VIDIOC_S_FMT, &format) == 0 ? ESP_OK : ESP_FAIL;
}

/* VIDIOC_ENUM_FRAMESIZES is not implemented for the CSI device, so the only
 * honest way to find out what the pipeline accepts is to ask it. Probing with
 * VIDIOC_S_FMT before any buffers exist is harmless, and the caller sets the
 * format it actually wants afterwards. */
static void log_accepted_frame_sizes(int fd)
{
    ESP_LOGI(TAG, "probing which RGB565 frame sizes this pipeline accepts.");
    ESP_LOGI(TAG, "the driver logs 'format width or height is invalid' at error "
             "level for each size it rejects — that is this probe working, not a fault:");
    for (size_t i = 0; i < sizeof(k_candidate_sizes) / sizeof(k_candidate_sizes[0]); i++) {
        bool ok = try_format(fd, k_candidate_sizes[i].w, k_candidate_sizes[i].h) == ESP_OK;
        ESP_LOGI(TAG, "  %4ux%-4u %s",
                 (unsigned)k_candidate_sizes[i].w, (unsigned)k_candidate_sizes[i].h,
                 ok ? "yes" : "no");
    }
}

static esp_err_t open_and_configure_device(void)
{
    struct v4l2_format format = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };

    s_cam.fd = open(BSP_CAMERA_DEVICE, O_RDWR);
    if (s_cam.fd < 0) {
        ESP_LOGE(TAG, "cannot open %s (errno %d) — did bsp_camera_start() succeed?",
                 BSP_CAMERA_DEVICE, errno);
        return ESP_FAIL;
    }

    log_accepted_frame_sizes(s_cam.fd);

    /* The SC202CS pipeline only offers a fixed set of sizes; a request it does
     * not recognise is rejected outright rather than rounded. Fall back to
     * whatever the driver has already selected so an awkward Kconfig value
     * degrades into a working stream instead of no camera at all. */
    if (try_format(s_cam.fd, CONFIG_PETCAM_WIDTH, CONFIG_PETCAM_HEIGHT) != ESP_OK) {
        ESP_LOGW(TAG, "%ux%u was rejected — using the driver's own default. Pick one "
                 "of the sizes listed above in menuconfig to choose deliberately.",
                 (unsigned)CONFIG_PETCAM_WIDTH, (unsigned)CONFIG_PETCAM_HEIGHT);
    }

    if (ioctl(s_cam.fd, VIDIOC_G_FMT, &format) != 0) {
        ESP_LOGE(TAG, "cannot read back the capture format");
        return ESP_FAIL;
    }

    if (format.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565) {
        ESP_LOGE(TAG, "driver settled on a non-RGB565 format; this build only "
                 "feeds RGB565 to the JPEG encoder");
        return ESP_ERR_NOT_SUPPORTED;
    }

    s_cam.width  = format.fmt.pix.width;
    s_cam.height = format.fmt.pix.height;
    s_cam.stats.width  = s_cam.width;
    s_cam.stats.height = s_cam.height;
    ESP_LOGI(TAG, "capturing %ux%u RGB565", (unsigned)s_cam.width, (unsigned)s_cam.height);
    return ESP_OK;
}

static esp_err_t setup_capture_buffers(void)
{
    /* USERPTR rather than MMAP so the sensor DMAs straight into memory the JPEG
     * encoder can read, and no frame is ever copied.
     *
     * These are allocated to esp_video's rules, not the JPEG encoder's:
     * VIDIOC_QBUF rejects any pointer that is not in PSRAM or not aligned to the
     * driver's align_size. The encoder is the lenient side here — per the
     * esp_driver_jpeg source, only its *output* buffer needs cache alignment,
     * while the input has no alignment restriction at all. */
    struct v4l2_requestbuffers req = {
        .count  = CAPTURE_BUFFERS,
        .type   = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_USERPTR,
    };

    if (ioctl(s_cam.fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(TAG, "VIDIOC_REQBUFS failed (errno %d)", errno);
        return ESP_FAIL;
    }

    for (int i = 0; i < CAPTURE_BUFFERS; i++) {
        struct v4l2_buffer buf = {
            .type   = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_USERPTR,
            .index  = i,
        };

        if (ioctl(s_cam.fd, VIDIOC_QUERYBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QUERYBUF %d failed", i);
            return ESP_FAIL;
        }

        s_cam.capture_buf[i] = heap_caps_aligned_alloc(CAPTURE_BUFFER_ALIGN, buf.length,
                                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
        if (!s_cam.capture_buf[i]) {
            ESP_LOGE(TAG, "out of memory for capture buffer %d (%u bytes)",
                     i, (unsigned)buf.length);
            return ESP_ERR_NO_MEM;
        }
        s_cam.capture_len[i] = buf.length;

        buf.m.userptr = (unsigned long)s_cam.capture_buf[i];
        buf.length    = s_cam.capture_len[i];
        if (ioctl(s_cam.fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QBUF %d failed", i);
            return ESP_FAIL;
        }
    }

    return ESP_OK;
}

static void capture_task(void *arg)
{
    const jpeg_encode_cfg_t encode_cfg = {
        .width         = s_cam.width,
        .height        = s_cam.height,
        .src_type      = JPEG_ENCODE_IN_FORMAT_RGB565,
        /* 4:2:0 halves the chroma resolution. On a room-watching stream this is
         * invisible and cuts the JPEG roughly in half. */
        .sub_sample    = JPEG_DOWN_SAMPLING_YUV420,
        .image_quality = CONFIG_PETCAM_JPEG_QUALITY,
#if CONFIG_PETCAM_PIXEL_REVERSE
        .pixel_reverse = true,
#else
        .pixel_reverse = false,
#endif
    };
    int64_t last_publish_us = 0;
    int64_t window_start_us = esp_timer_get_time();
    uint32_t window_frames = 0;
    uint32_t seconds_elapsed = 0;

    for (;;) {
        struct v4l2_buffer buf = {
            .type   = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_USERPTR,
        };

        if (ioctl(s_cam.fd, VIDIOC_DQBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_DQBUF failed (errno %d)", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        int64_t now_us = esp_timer_get_time();
        bool usable = (buf.flags & V4L2_BUF_FLAG_DONE) != 0;
        bool due    = (now_us - last_publish_us) >= MIN_FRAME_INTERVAL_US;

        if (usable && due) {
            frame_t *frame = frame_bus_acquire_writable();
            if (frame) {
                uint32_t jpeg_len = 0;
                esp_err_t err = jpeg_encoder_process(s_cam.encoder, &encode_cfg,
                                                     s_cam.capture_buf[buf.index],
                                                     buf.bytesused,
                                                     frame->data, frame->capacity,
                                                     &jpeg_len);
                if (err == ESP_OK) {
#if CONFIG_PETCAM_ENABLE_MOTION
                    app_motion_submit_jpeg(frame->data, jpeg_len);
#endif
                    frame_bus_publish(frame, jpeg_len);
                    s_cam.stats.last_jpeg_len = jpeg_len;
                    s_cam.stats.frames++;
                    window_frames++;
                    last_publish_us = now_us;
                } else {
                    ESP_LOGW(TAG, "JPEG encode failed: %s", esp_err_to_name(err));
                    frame_bus_release(frame);
                    s_cam.stats.dropped++;
                }
            } else {
                /* Every pooled frame is still being sent. Skipping is the right
                 * call: a live view wants the newest frame, not a backlog. */
                s_cam.stats.dropped++;
            }
        } else if (!usable) {
            s_cam.stats.dropped++;
        }

#if CONFIG_PETCAM_ENABLE_MOTION
        if (usable) {
            app_motion_submit_frame(s_cam.capture_buf[buf.index], s_cam.width, s_cam.height);
        }
#endif
#if CONFIG_PETCAM_ENABLE_PREVIEW
        if (usable) {
            /* Must happen before the buffer goes back to the sensor. The PPA
             * copy is blocking but hardware, so it costs the capture task very
             * little, and the preview throttles itself internally. */
            app_preview_submit(s_cam.capture_buf[buf.index], s_cam.width, s_cam.height);
        }
#endif

        /* Requeue unconditionally — a buffer we fail to return is a buffer the
         * sensor never gets back. */
        buf.m.userptr = (unsigned long)s_cam.capture_buf[buf.index];
        buf.length    = s_cam.capture_len[buf.index];
        if (ioctl(s_cam.fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QBUF failed (errno %d)", errno);
        }

        if (now_us - window_start_us >= 1000000) {
            s_cam.stats.fps = window_frames * 1000000.0f / (now_us - window_start_us);
            window_start_us = now_us;
            window_frames = 0;

            /* An unattended camera is usually watched through its serial log, so
             * report health periodically rather than only on request. */
            if (++seconds_elapsed % 5 == 0) {
                ESP_LOGI(TAG, "%.1f fps, %u KB/frame, %u frames, %u dropped",
                         s_cam.stats.fps, (unsigned)(s_cam.stats.last_jpeg_len / 1024),
                         (unsigned)s_cam.stats.frames, (unsigned)s_cam.stats.dropped);
            }
        }
    }
}

esp_err_t app_camera_start(void)
{
    const jpeg_encode_engine_cfg_t engine_cfg = {
        .intr_priority = 0,
        .timeout_ms    = 100,
    };
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    esp_err_t err;

    err = open_and_configure_device();
    if (err != ESP_OK) {
        return err;
    }

    err = setup_capture_buffers();
    if (err != ESP_OK) {
        return err;
    }

    err = frame_bus_init(FRAME_POOL_SIZE, JPEG_BUFFER_BYTES);
    if (err != ESP_OK) {
        return err;
    }

    err = jpeg_new_encoder_engine(&engine_cfg, &s_cam.encoder);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cannot create the JPEG encoder: %s", esp_err_to_name(err));
        return err;
    }

    if (ioctl(s_cam.fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "VIDIOC_STREAMON failed (errno %d)", errno);
        return ESP_FAIL;
    }

    /* Priority 5 keeps capture ahead of the HTTP tasks; the encode is hardware
     * so this task is mostly blocked in ioctl(). */
    if (xTaskCreatePinnedToCore(capture_task, "petcam_capture", 6144, NULL, 5, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "cannot create the capture task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "streaming at up to %d fps, JPEG quality %d",
             CONFIG_PETCAM_MAX_FPS, CONFIG_PETCAM_JPEG_QUALITY);
    return ESP_OK;
}

void app_camera_get_stats(app_camera_stats_t *out)
{
    *out = s_cam.stats;
    out->dropped += frame_bus_dropped();
}
