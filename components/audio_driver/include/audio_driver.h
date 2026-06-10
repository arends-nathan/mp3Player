#ifndef AUDIO_DRIVER_H
#define AUDIO_DRIVER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_TITLE_MAX_LEN 64

/**
 * @brief High-level playback state for the audio engine.
 */
typedef enum {
    AUDIO_STATE_IDLE,     /*!< Engine initialized, nothing loaded */
    AUDIO_STATE_PLAYING,  /*!< Actively decoding and streaming a track */
    AUDIO_STATE_PAUSED,   /*!< Track loaded, output halted */
    AUDIO_STATE_STOPPED,  /*!< Playback ended or was cancelled */
    AUDIO_STATE_ERROR,    /*!< Last operation failed (e.g. bad file) */
} audio_state_t;

/**
 * @brief Snapshot of the player used by the UI layer. Always fetched through
 * ::audio_player_get_status so access stays thread-safe.
 */
typedef struct {
    audio_state_t state;
    uint32_t elapsed_seconds;             /*!< Position within the current track */
    uint32_t total_seconds;               /*!< Estimated track length (0 if unknown) */
    uint8_t volume;                       /*!< 0-100 */
    char title[AUDIO_TITLE_MAX_LEN];      /*!< Display name (derived from filename) */
} audio_status_t;

/**
 * @brief Destination for decoded PCM audio.
 */
typedef enum {
    AUDIO_OUTPUT_I2S, /*!< Local I2S amplifier (default) */
    AUDIO_OUTPUT_BT,  /*!< Bluetooth A2DP sink (headphones/speaker) */
} audio_output_t;

/**
 * @brief Bring up the I2S peripheral and the internal playback task.
 *
 * Must be called once during boot before any other audio_* function.
 *
 * @return ESP_OK on success, or an error from the underlying I2S driver.
 */
esp_err_t audio_driver_init(void);

/**
 * @brief Begin decoding and playing an MP3 file from a mounted filesystem.
 *
 * If a track is already playing it is stopped first. The call is asynchronous;
 * decoding happens on the dedicated audio task.
 *
 * @param filepath Absolute path to an .mp3 file (e.g. "/sdcard/song.mp3").
 * @return ESP_OK if the request was accepted, ESP_ERR_* otherwise.
 */
esp_err_t audio_player_play(const char *filepath);

/** @brief Halt output but keep the current position. */
void audio_player_pause(void);

/** @brief Resume output from a paused state. */
void audio_player_resume(void);

/** @brief Toggle between playing and paused. */
void audio_player_toggle_pause(void);

/** @brief Stop playback and unload the current track. */
void audio_player_stop(void);

/**
 * @brief Set output volume.
 * @param volume_pct Linear volume from 0 (mute) to 100 (full scale).
 */
void audio_player_set_volume(uint8_t volume_pct);

/** @brief Current volume (0-100). */
uint8_t audio_player_get_volume(void);

/**
 * @brief Copy the current player status into @p out (thread-safe).
 * @param out Destination snapshot, must not be NULL.
 */
void audio_player_get_status(audio_status_t *out);

/**
 * @brief Select where decoded PCM is routed.
 *
 * Switching to ::AUDIO_OUTPUT_BT silences the local I2S amp and feeds samples
 * into an internal buffer that ::audio_driver_read_pcm drains (used by the
 * Bluetooth A2DP source). Switching back to ::AUDIO_OUTPUT_I2S resumes local
 * output and discards any buffered samples.
 *
 * @param output Desired destination.
 */
void audio_driver_set_output(audio_output_t output);

/** @brief Current PCM output destination. */
audio_output_t audio_driver_get_output(void);

/**
 * @brief Drain decoded PCM for the Bluetooth A2DP source callback.
 *
 * Always fills exactly @p len bytes (16-bit little-endian stereo, 44.1 kHz),
 * zero-padding on underrun so the A2DP timeline never stalls. Only meaningful
 * while the output is ::AUDIO_OUTPUT_BT.
 *
 * @param buf Destination buffer.
 * @param len Number of bytes requested.
 * @return Number of bytes written (always @p len).
 */
int audio_driver_read_pcm(uint8_t *buf, int len);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_DRIVER_H
