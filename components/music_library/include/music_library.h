#ifndef MUSIC_LIBRARY_H
#define MUSIC_LIBRARY_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MUSIC_LIBRARY_MAX_TRACKS 64

/**
 * @brief Mount local storage and scan it for MP3 tracks.
 *
 * Registers a SPIFFS filesystem at @p dir (formatting it if necessary) and
 * performs an initial library scan.
 *
 * @param dir Mount point / music directory (e.g. "/spiffs" or "/sdcard").
 * @return true if storage mounted successfully.
 */
bool music_library_init(const char *dir);

/** @brief Re-scan the music directory for .mp3 files. */
void music_library_refresh(void);

/** @brief Number of tracks currently known. */
int music_library_count(void);

/**
 * @brief Filename of the track at @p index (without directory).
 * @return The name, or an empty string if @p index is out of range.
 */
const char *music_library_name(int index);

/** @brief Index of the track currently selected for playback, or -1. */
int music_library_current(void);

/**
 * @brief Begin playback of the track at @p index via the audio driver.
 */
void music_library_play(int index);

/**
 * @brief Play the track @p delta positions from the current one (wrapping).
 *
 * No-op when nothing is playing or the library is empty.
 */
void music_library_play_relative(int delta);

/** @brief Returns the current mount point used by the music library (eg "/sdcard" or "/spiffs"). */
const char *music_library_get_mount_point(void);

#ifdef __cplusplus
}
#endif

#endif // MUSIC_LIBRARY_H
