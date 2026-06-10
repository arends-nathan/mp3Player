import glob
import logging
import os
import shutil
from pathlib import Path

from yt_dlp import YoutubeDL

logger = logging.getLogger(__name__)


def _find_winget_ffmpeg():
    """Search the current user's WinGet package store for a bundled FFmpeg build.

    Uses the LOCALAPPDATA environment variable (not a hardcoded username) so it
    resolves correctly for whoever is actually running the process.
    """
    local_app_data = os.getenv("LOCALAPPDATA")
    if not local_app_data:
        return None

    # WinGet installs FFmpeg under a versioned folder, so glob across versions.
    search_pattern = os.path.join(
        local_app_data,
        "Microsoft", "WinGet", "Packages",
        "*FFmpeg*", "*", "bin", "ffmpeg.exe",
    )
    for match in glob.glob(search_pattern):
        if os.path.isfile(match):
            return os.path.dirname(match)

    return None


def resolve_ffmpeg_location(configured_location=None):
    if configured_location:
        return configured_location

    ffmpeg_path = shutil.which("ffmpeg")
    ffprobe_path = shutil.which("ffprobe")
    if ffmpeg_path and ffprobe_path:
        return os.path.dirname(ffmpeg_path)

    return _find_winget_ffmpeg()


def download_mp3_from_query(query, output_folder, ffmpeg_location=None):
    """Use yt-dlp to search for a query and extract the audio stream as MP3."""
    out_template = f"{output_folder}/%(title)s.%(ext)s"

    ydl_opts = {
        'format': 'bestaudio/best',
        'default_search': 'ytsearch',
        'outtmpl': out_template,
        'noplaylist': True,
        'postprocessors': [{
            'key': 'FFmpegExtractAudio',
            'preferredcodec': 'mp3',
            'preferredquality': '192',
        }],
        'restrictfilenames': True,
        'quiet': False,
    }

    resolved_ffmpeg_location = resolve_ffmpeg_location(ffmpeg_location)
    if resolved_ffmpeg_location:
        ydl_opts['ffmpeg_location'] = resolved_ffmpeg_location
        logger.info("[Scraper] Using FFmpeg from: %s", resolved_ffmpeg_location)
    else:
        logger.warning("[Scraper] FFmpeg was not found automatically; audio conversion will fail.")

    logger.info("[Scraper] Searching for: %s", query)
    try:
        with YoutubeDL(ydl_opts) as ydl:
            search_results = ydl.extract_info(f"ytsearch10:{query}", download=False)
            entries = search_results.get("entries") or []
            if not entries:
                raise ValueError(f"No YouTube search results found for '{query}'")

            def score_entry(entry):
                title = (entry.get("title") or "").lower()
                score = 0

                if "official audio" in title or "audio" in title:
                    score += 30
                if "topic" in title:
                    score += 20
                if "lyrics" in title or "lyric" in title:
                    score += 10
                if "video" in title or "music video" in title or "official video" in title:
                    score -= 40
                if "live" in title:
                    score -= 20

                duration = entry.get("duration")
                if duration:
                    if duration > 1200:
                        score -= 10
                    elif duration < 120:
                        score -= 10

                return score

            best_entry = max(entries, key=score_entry)
            best_title = best_entry.get("title", "unknown title")
            best_url = best_entry.get("webpage_url") or best_entry.get("url")
            logger.info("[Scraper] Selected audio-first result: %s", best_title)
            if not best_url:
                raise ValueError(f"Selected YouTube result for '{query}' did not include a URL")

            ydl.download([best_url])
        logger.info("[Scraper] Finished: %s", query)
    except Exception:
        logger.exception("[Error] Failed to download '%s'", query)
