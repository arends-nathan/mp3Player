import logging
import os
import argparse
from datetime import datetime, timedelta
import time

from config import OUTPUT_DIR, SPOTIFY_CLIENT_ID, SPOTIFY_CLIENT_SECRET, SPOTIFY_REDIRECT_URI, FFMPEG_LOCATION
from curator import SpotifyCurator
from downloader import download_mp3_from_query
from spotify_service import create_spotify_client, get_algorithmic_recommendations, get_user_top_genres


logging.basicConfig(level=logging.INFO, format="[%(levelname)s] %(message)s")
logger = logging.getLogger(__name__)


def run_once():
    logger.info("[Start] Audio sync pipeline starting.")
    logger.info("[Config] Using redirect URI: %s", SPOTIFY_REDIRECT_URI)
    logger.info("[Config] Spotify Dashboard redirect URI must exactly match that value.")

    if not SPOTIFY_CLIENT_ID or not SPOTIFY_CLIENT_SECRET or not SPOTIFY_REDIRECT_URI:
        logger.error("[Error] Please set Spotify client credentials in config.json or environment variables.")
        raise SystemExit(1)

    os.makedirs(OUTPUT_DIR, exist_ok=True)

    try:
        logger.info("[Step 1/3] Authenticating Spotify and building curator.")
        spotify_client = create_spotify_client(
            SPOTIFY_CLIENT_ID,
            SPOTIFY_CLIENT_SECRET,
            SPOTIFY_REDIRECT_URI,
        )
        curator = SpotifyCurator(spotify_client)

        curator_mode = os.getenv("SPOTIFY_CURATOR_MODE", "clustered_vibe_of_the_day").strip().lower()
        logger.info("[Pipeline] Curator mode: %s", curator_mode)

        logger.info("[Step 2/3] Generating curated track selection from Spotify.")
        if curator_mode == "mood_rotation":
            target_mood = os.getenv("SPOTIFY_TARGET_MOOD", "energetic").strip().lower()
            song_queries = curator.get_mood_rotation(target_mood=target_mood, limit=10)
        elif curator_mode == "time_capsule_mix":
            song_queries = curator.get_time_capsule_mix(limit=10)
        elif curator_mode == "collaborative_crossover":
            friend_artist_id = os.getenv("SPOTIFY_FRIEND_ARTIST_ID", "").strip()
            if not friend_artist_id:
                logger.warning("[Pipeline] Missing SPOTIFY_FRIEND_ARTIST_ID; falling back to time capsule mix.")
                song_queries = curator.get_time_capsule_mix(limit=10)
            else:
                song_queries = curator.get_collaborative_crossover(friend_artist_id=friend_artist_id, limit=10)
        else:
            song_queries = curator.get_clustered_vibe_of_the_day(limit=10)

        if not song_queries:
            logger.warning("[Pipeline] Curator returned no tracks; falling back to legacy recommendation pipeline.")
            my_top_genres = get_user_top_genres(
                SPOTIFY_CLIENT_ID,
                SPOTIFY_CLIENT_SECRET,
                SPOTIFY_REDIRECT_URI,
                time_range="medium_term",
                top_n=10,
            )
            seed_genres = [genre for genre, _count in my_top_genres[:5]]
            song_queries = get_algorithmic_recommendations(
                SPOTIFY_CLIENT_ID,
                SPOTIFY_CLIENT_SECRET,
                SPOTIFY_REDIRECT_URI,
                genres=seed_genres,
                limit=10,
            )

        logger.info("[Step 3/3] Starting audio download loop for %d tracks.", len(song_queries))
        for index, song in enumerate(song_queries, start=1):
            logger.info("[Progress] Downloading track %d/%d.", index, len(song_queries))
            download_mp3_from_query(song, OUTPUT_DIR, ffmpeg_location=FFMPEG_LOCATION)

        logger.info("[Success] Daily synchronization complete! Check the '%s' directory.", OUTPUT_DIR)
    except Exception:
        logger.exception("[Fatal Error] Pipeline broke down")
        raise


def parse_args():
    parser = argparse.ArgumentParser(description="Spotify-powered MP3 sync pipeline")
    parser.add_argument(
        "--run-once",
        action="store_true",
        help="Run immediately once and exit instead of waiting for the next 1:00 AM.",
    )
    return parser.parse_args()


def seconds_until_next_1am(now=None):
    now = now or datetime.now()
    next_run = now.replace(hour=1, minute=0, second=0, microsecond=0)
    if now >= next_run:
        next_run += timedelta(days=1)
    return (next_run - now).total_seconds(), next_run


def main():
    args = parse_args()

    if args.run_once:
        run_once()
        return

    logger.info("[Loop] Running daily at 1:00 AM local time.")
    try:
        while True:
            sleep_seconds, next_run = seconds_until_next_1am()
            logger.info("[Loop] Next run scheduled for %s.", next_run.strftime("%Y-%m-%d %H:%M:%S"))
            logger.info("[Loop] Sleeping for %.2f seconds.", sleep_seconds)
            time.sleep(sleep_seconds)
            run_once()
    except KeyboardInterrupt:
        logger.info("[Loop] Interrupted by user; exiting.")


if __name__ == "__main__":
    main()
