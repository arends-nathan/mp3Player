import json
import logging
import os
from pathlib import Path

logger = logging.getLogger(__name__)

BASE_DIR = Path(__file__).resolve().parent
CONFIG_PATH = BASE_DIR / "config.json"

DEFAULT_CONFIG = {
    "spotify_client_id": "",
    "spotify_client_secret": "",
    "spotify_redirect_uri": "http://127.0.0.1:8888/callback",
    "playlist_id": "",
    "output_dir": "./sync_folder",
    "ffmpeg_location": None,
}


def load_config():
    config = DEFAULT_CONFIG.copy()
    if CONFIG_PATH.exists():
        with CONFIG_PATH.open("r", encoding="utf-8") as config_file:
            loaded_config = json.load(config_file)
        if isinstance(loaded_config, dict):
            config.update({key: value for key, value in loaded_config.items() if value is not None})
        else:
            logger.warning("[Config] config.json did not contain an object; using defaults.")
    else:
        logger.warning("[Config] No config.json found; using defaults and environment variables.")

    config["spotify_client_id"] = os.getenv("SPOTIFY_CLIENT_ID", config["spotify_client_id"])
    config["spotify_client_secret"] = os.getenv("SPOTIFY_CLIENT_SECRET", config["spotify_client_secret"])
    config["spotify_redirect_uri"] = os.getenv("SPOTIFY_REDIRECT_URI", config["spotify_redirect_uri"])
    config["playlist_id"] = os.getenv("PLAYLIST_ID", config["playlist_id"])
    config["output_dir"] = os.getenv("OUTPUT_DIR", config["output_dir"])
    config["ffmpeg_location"] = os.getenv("FFMPEG_LOCATION", config["ffmpeg_location"])
    return config


CONFIG = load_config()
SPOTIFY_CLIENT_ID = CONFIG["spotify_client_id"]
SPOTIFY_CLIENT_SECRET = CONFIG["spotify_client_secret"]
SPOTIFY_REDIRECT_URI = CONFIG["spotify_redirect_uri"]
PLAYLIST_ID = CONFIG["playlist_id"]
OUTPUT_DIR = CONFIG["output_dir"]
FFMPEG_LOCATION = CONFIG["ffmpeg_location"]
