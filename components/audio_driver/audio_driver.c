#include "audio_driver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"

#include "esp_log.h"
#include "driver/i2s_std.h"

#include "mp3dec.h"   // Helix fixed-point MP3 decoder (chmorgan/esp-libhelix-mp3)

static const char *TAG = "AUDIO_DRIVER";

// ---------------------------------------------------------------------------
// Hardware configuration. These map to a MAX98357A (or similar) I2S class-D
// amplifier. Defaults are chosen so they do NOT collide with the OLED's I2C
// bus on GPIO 21/22. Override via menuconfig (Kconfig) if your wiring differs.
// ---------------------------------------------------------------------------
#ifndef CONFIG_AUDIO_I2S_BCLK_PIN
#define CONFIG_AUDIO_I2S_BCLK_PIN 27
#endif
#ifndef CONFIG_AUDIO_I2S_LRCLK_PIN
#define CONFIG_AUDIO_I2S_LRCLK_PIN 26
#endif
#ifndef CONFIG_AUDIO_I2S_DOUT_PIN
#define CONFIG_AUDIO_I2S_DOUT_PIN 25
#endif

// Decode/streaming buffer sizing.
#define MP3_INPUT_BUFFER_SIZE   2048               // raw MP3 bytes held at once
#define MP3_MAX_SAMPLES_PER_CH  1152               // Helix max for layer III
#define PCM_OUTPUT_BUFFER_SIZE  (MP3_MAX_SAMPLES_PER_CH * 2) // stereo interleaved shorts

// Bluetooth A2DP hand-off buffer. Holds decoded PCM (16-bit stereo) waiting to
// be pulled by the A2DP source callback. ~16 KB is roughly 90 ms of 44.1 kHz
// stereo audio, enough to absorb decode/Bluetooth scheduling jitter.
#define BT_PCM_STREAM_BUFFER_SIZE  (16 * 1024)

// Player command messages delivered to the audio task.
typedef enum {
    CMD_PLAY,
    CMD_STOP,
} audio_cmd_type_t;

typedef struct {
    audio_cmd_type_t type;
    char filepath[128];
} audio_cmd_t;

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
static i2s_chan_handle_t s_tx_chan = NULL;
static QueueHandle_t     s_cmd_queue = NULL;
static SemaphoreHandle_t s_status_mutex = NULL;
static TaskHandle_t      s_audio_task = NULL;

static volatile bool s_pause_requested = false;
static volatile bool s_stop_requested  = false;
static volatile uint8_t s_volume = 70;    // 0-100, default to a safe listening level
static uint32_t s_current_sample_rate = 44100;

// PCM routing. Default to the local I2S amplifier; switched to BT by the
// Bluetooth A2DP source while a sink is connected.
static volatile audio_output_t s_output = AUDIO_OUTPUT_I2S;
static StreamBufferHandle_t s_bt_stream = NULL;

static audio_status_t s_status = {
    .state = AUDIO_STATE_IDLE,
    .elapsed_seconds = 0,
    .total_seconds = 0,
    .volume = 70,
    .title = "",
};

// ---------------------------------------------------------------------------
// Status helpers (mutex guarded)
// ---------------------------------------------------------------------------
static void status_set_state(audio_state_t state) {
    if (xSemaphoreTake(s_status_mutex, portMAX_DELAY) == pdTRUE) {
        s_status.state = state;
        xSemaphoreGive(s_status_mutex);
    }
}

static void status_set_track(const char *title, uint32_t total_seconds) {
    if (xSemaphoreTake(s_status_mutex, portMAX_DELAY) == pdTRUE) {
        strncpy(s_status.title, title, AUDIO_TITLE_MAX_LEN - 1);
        s_status.title[AUDIO_TITLE_MAX_LEN - 1] = '\0';
        s_status.total_seconds = total_seconds;
        s_status.elapsed_seconds = 0;
        xSemaphoreGive(s_status_mutex);
    }
}

static void status_set_elapsed(uint32_t elapsed_seconds) {
    if (xSemaphoreTake(s_status_mutex, portMAX_DELAY) == pdTRUE) {
        s_status.elapsed_seconds = elapsed_seconds;
        xSemaphoreGive(s_status_mutex);
    }
}

// ---------------------------------------------------------------------------
// Derive a friendly title from a file path: "/sdcard/Some_Song.mp3" -> "Some Song"
// ---------------------------------------------------------------------------
static void derive_title(const char *filepath, char *out, size_t out_len) {
    const char *base = strrchr(filepath, '/');
    base = base ? base + 1 : filepath;

    size_t i = 0;
    for (; base[i] != '\0' && i < out_len - 1; i++) {
        char c = base[i];
        if (c == '.' && (strcasecmp(base + i, ".mp3") == 0)) {
            break; // strip extension
        }
        out[i] = (c == '_') ? ' ' : c;
    }
    out[i] = '\0';
}

// ---------------------------------------------------------------------------
// Reconfigure the I2S clock when a frame reports a different sample rate.
// ---------------------------------------------------------------------------
static esp_err_t apply_sample_rate(uint32_t sample_rate) {
    if (sample_rate == s_current_sample_rate) {
        return ESP_OK;
    }

    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);

    ESP_ERROR_CHECK(i2s_channel_disable(s_tx_chan));
    esp_err_t err = i2s_channel_reconfig_std_clock(s_tx_chan, &clk_cfg);
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_chan));

    if (err == ESP_OK) {
        s_current_sample_rate = sample_rate;
        ESP_LOGI(TAG, "I2S clock set to %" PRIu32 " Hz", sample_rate);
    }
    return err;
}

// ---------------------------------------------------------------------------
// Apply software volume to an interleaved 16-bit PCM frame in place.
// ---------------------------------------------------------------------------
static void apply_volume(int16_t *pcm, int sample_count, uint8_t volume_pct) {
    if (volume_pct >= 100) {
        return; // unity gain, skip the work
    }
    if (volume_pct == 0) {
        memset(pcm, 0, sample_count * sizeof(int16_t));
        return;
    }
    int32_t scale = (int32_t)volume_pct; // 0..99
    for (int i = 0; i < sample_count; i++) {
        pcm[i] = (int16_t)((pcm[i] * scale) / 100);
    }
}

// ---------------------------------------------------------------------------
// Estimate total track length from file size and the first frame's bitrate.
// ---------------------------------------------------------------------------
static uint32_t estimate_total_seconds(long file_size_bytes, int bitrate_bps) {
    if (bitrate_bps <= 0 || file_size_bytes <= 0) {
        return 0;
    }
    return (uint32_t)((file_size_bytes * 8) / bitrate_bps);
}

// ---------------------------------------------------------------------------
// Core decode loop for a single file. Returns when the file ends, errors, or a
// stop is requested.
// ---------------------------------------------------------------------------
static void play_file(const char *filepath) {
    char title[AUDIO_TITLE_MAX_LEN];
    derive_title(filepath, title, sizeof(title));

    FILE *fp = fopen(filepath, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "Could not open file: %s", filepath);
        status_set_state(AUDIO_STATE_ERROR);
        return;
    }

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    HMP3Decoder decoder = MP3InitDecoder();
    uint8_t *input_buf = malloc(MP3_INPUT_BUFFER_SIZE);
    int16_t *pcm_buf = malloc(PCM_OUTPUT_BUFFER_SIZE * sizeof(int16_t));

    if (!decoder || !input_buf || !pcm_buf) {
        ESP_LOGE(TAG, "Out of memory bringing up MP3 decoder");
        status_set_state(AUDIO_STATE_ERROR);
        goto cleanup;
    }

    status_set_track(title, 0);
    status_set_state(AUDIO_STATE_PLAYING);
    ESP_LOGI(TAG, "Playing: %s", title);

    int bytes_in_buffer = 0;
    uint8_t *read_ptr = input_buf;
    uint64_t total_samples_played = 0;
    bool total_known = false;

    while (!s_stop_requested) {
        // Honor pause requests without burning CPU.
        if (s_pause_requested) {
            status_set_state(AUDIO_STATE_PAUSED);
            while (s_pause_requested && !s_stop_requested) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            if (s_stop_requested) {
                break;
            }
            status_set_state(AUDIO_STATE_PLAYING);
        }

        // Refill the input buffer when we are running low on bytes.
        if (bytes_in_buffer < MP3_INPUT_BUFFER_SIZE / 2) {
            memmove(input_buf, read_ptr, bytes_in_buffer);
            read_ptr = input_buf;
            int space = MP3_INPUT_BUFFER_SIZE - bytes_in_buffer;
            int read = fread(input_buf + bytes_in_buffer, 1, space, fp);
            bytes_in_buffer += read;
            if (read == 0 && bytes_in_buffer == 0) {
                break; // clean end of stream
            }
        }

        int offset = MP3FindSyncWord(read_ptr, bytes_in_buffer);
        if (offset < 0) {
            // No frame sync in the buffer; drop it and refill.
            bytes_in_buffer = 0;
            continue;
        }
        read_ptr += offset;
        bytes_in_buffer -= offset;

        int err = MP3Decode(decoder, &read_ptr, &bytes_in_buffer, pcm_buf, 0);
        if (err == ERR_MP3_INDATA_UNDERFLOW || err == ERR_MP3_MAINDATA_UNDERFLOW) {
            // Need more bytes; loop will refill.
            continue;
        }
        if (err != ERR_MP3_NONE) {
            ESP_LOGW(TAG, "MP3 decode error %d; skipping frame", err);
            // Skip a byte to resynchronize.
            if (bytes_in_buffer > 0) {
                read_ptr++;
                bytes_in_buffer--;
            }
            continue;
        }

        MP3FrameInfo frame;
        MP3GetLastFrameInfo(decoder, &frame);

        if (!total_known && frame.bitrate > 0) {
            uint32_t total = estimate_total_seconds(file_size, frame.bitrate);
            status_set_track(title, total);
            total_known = true;
        }

        apply_sample_rate(frame.samprate);

        int sample_count = frame.outputSamps; // total interleaved samples
        if (sample_count <= 0) {
            continue;
        }

        // Helix outputs mono as a single channel; duplicate to stereo so the
        // amplifier always receives an interleaved stereo stream.
        if (frame.nChans == 1) {
            for (int i = sample_count - 1; i >= 0; i--) {
                pcm_buf[2 * i] = pcm_buf[i];
                pcm_buf[2 * i + 1] = pcm_buf[i];
            }
            sample_count *= 2;
        }

        apply_volume(pcm_buf, sample_count, s_volume);

        const size_t pcm_bytes = sample_count * sizeof(int16_t);
        if (s_output == AUDIO_OUTPUT_BT) {
            // Feed the Bluetooth A2DP source. xStreamBufferSend blocks until
            // space is available, which naturally paces decoding to the sink's
            // consumption rate (the A2DP timeline replaces the I2S clock).
            const uint8_t *p = (const uint8_t *)pcm_buf;
            size_t remaining = pcm_bytes;
            while (remaining > 0 && !s_stop_requested) {
                size_t sent = xStreamBufferSend(s_bt_stream, p, remaining,
                                                pdMS_TO_TICKS(100));
                p += sent;
                remaining -= sent;
            }
        } else {
            size_t bytes_written = 0;
            i2s_channel_write(s_tx_chan, pcm_buf, pcm_bytes,
                              &bytes_written, portMAX_DELAY);
        }

        // Track elapsed time from the number of per-channel samples emitted.
        total_samples_played += (uint64_t)(sample_count / 2);
        if (frame.samprate > 0) {
            status_set_elapsed((uint32_t)(total_samples_played / frame.samprate));
        }
    }

    ESP_LOGI(TAG, "Playback finished: %s", title);
    status_set_state(s_stop_requested ? AUDIO_STATE_STOPPED : AUDIO_STATE_IDLE);

cleanup:
    if (decoder) {
        MP3FreeDecoder(decoder);
    }
    free(input_buf);
    free(pcm_buf);
    if (fp) {
        fclose(fp);
    }
}

// ---------------------------------------------------------------------------
// Audio task: waits for commands and drives playback.
// ---------------------------------------------------------------------------
static void audio_task(void *arg) {
    ESP_LOGI(TAG, "Audio task started on core %d", xPortGetCoreID());
    audio_cmd_t cmd;

    while (1) {
        if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) == pdTRUE) {
            if (cmd.type == CMD_PLAY) {
                s_stop_requested = false;
                s_pause_requested = false;
                play_file(cmd.filepath);
            }
            // CMD_STOP only needs to flip the flag, which is handled in the API
            // function; nothing extra to do here.
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
esp_err_t audio_driver_init(void) {
    if (s_audio_task != NULL) {
        return ESP_OK; // already initialized
    }

    s_status_mutex = xSemaphoreCreateMutex();
    s_cmd_queue = xQueueCreate(4, sizeof(audio_cmd_t));
    if (!s_status_mutex || !s_cmd_queue) {
        return ESP_ERR_NO_MEM;
    }

    s_bt_stream = xStreamBufferCreate(BT_PCM_STREAM_BUFFER_SIZE, 1);
    if (!s_bt_stream) {
        return ESP_ERR_NO_MEM;
    }

    // Create the I2S TX channel in standard (Philips) mode.
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx_chan, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(s_current_sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CONFIG_AUDIO_I2S_BCLK_PIN,
            .ws   = CONFIG_AUDIO_I2S_LRCLK_PIN,
            .dout = CONFIG_AUDIO_I2S_DOUT_PIN,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_chan));

    s_status.volume = s_volume;

    // Pin the audio task to core 0 to keep it isolated from the UI on core 1.
    BaseType_t ok = xTaskCreatePinnedToCore(audio_task, "audio_task", 8192, NULL,
                                            6, &s_audio_task, 0);
    if (ok != pdPASS) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Audio driver initialized (BCLK=%d, LRCLK=%d, DOUT=%d)",
             CONFIG_AUDIO_I2S_BCLK_PIN, CONFIG_AUDIO_I2S_LRCLK_PIN,
             CONFIG_AUDIO_I2S_DOUT_PIN);
    return ESP_OK;
}

esp_err_t audio_player_play(const char *filepath) {
    if (!filepath || !s_cmd_queue) {
        return ESP_ERR_INVALID_ARG;
    }

    // Cancel any current playback first so the task is free to pick up the new
    // track promptly.
    s_stop_requested = true;
    s_pause_requested = false;

    audio_cmd_t cmd = { .type = CMD_PLAY };
    strncpy(cmd.filepath, filepath, sizeof(cmd.filepath) - 1);
    cmd.filepath[sizeof(cmd.filepath) - 1] = '\0';

    if (xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

void audio_player_pause(void) {
    s_pause_requested = true;
}

void audio_player_resume(void) {
    s_pause_requested = false;
}

void audio_player_toggle_pause(void) {
    s_pause_requested = !s_pause_requested;
}

void audio_player_stop(void) {
    s_stop_requested = true;
    s_pause_requested = false;
}

void audio_player_set_volume(uint8_t volume_pct) {
    if (volume_pct > 100) {
        volume_pct = 100;
    }
    s_volume = volume_pct;
    if (xSemaphoreTake(s_status_mutex, portMAX_DELAY) == pdTRUE) {
        s_status.volume = volume_pct;
        xSemaphoreGive(s_status_mutex);
    }
}

uint8_t audio_player_get_volume(void) {
    return s_volume;
}

void audio_player_get_status(audio_status_t *out) {
    if (!out) {
        return;
    }
    if (xSemaphoreTake(s_status_mutex, portMAX_DELAY) == pdTRUE) {
        *out = s_status;
        xSemaphoreGive(s_status_mutex);
    }
}

void audio_driver_set_output(audio_output_t output) {
    if (output == s_output) {
        return;
    }
    s_output = output;
    if (s_bt_stream) {
        // Drop stale samples so the new sink starts clean.
        xStreamBufferReset(s_bt_stream);
    }
    ESP_LOGI(TAG, "Audio output -> %s", output == AUDIO_OUTPUT_BT ? "Bluetooth" : "I2S");
}

audio_output_t audio_driver_get_output(void) {
    return s_output;
}

int audio_driver_read_pcm(uint8_t *buf, int len) {
    if (!buf || len <= 0) {
        return 0;
    }
    int received = 0;
    if (s_bt_stream) {
        received = (int)xStreamBufferReceive(s_bt_stream, buf, (size_t)len,
                                             pdMS_TO_TICKS(20));
    }
    // The A2DP source contract expects a full buffer every time; zero-pad any
    // underrun so the stream stays silent rather than glitching.
    if (received < len) {
        memset(buf + received, 0, (size_t)(len - received));
    }
    return len;
}
