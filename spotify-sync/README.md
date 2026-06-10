# MP3 Spotify Sync

A Python backend that curates daily Spotify-based track selections and downloads them as MP3s for an embedded player.

## Layout

- `main.py` orchestrates the daily sync pipeline.
- `config.py` loads local configuration from `config.json` and environment variables.
- `spotify_service.py` handles Spotify auth, genre extraction, playlist reads, and legacy recommendations.
- `curator.py` provides the `SpotifyCurator` class for the newer metadata-driven selection modes.
- `downloader.py` wraps yt-dlp and FFmpeg for MP3 conversion.
- `api.py` serves `sync_folder/` over HTTP with a manifest endpoint for microcontrollers.

## Setup

1. Install Python dependencies:
   ```bash
   pip install -r requirements.txt
   ```
2. Copy `config.example.json` to `config.json` for local runs, or copy `.env.example` to `.env` for Docker.
3. Run:
   ```bash
   python main.py
   ```

## Docker

Build the image:

```bash
docker build -t mp3-sync .
```

Create a `.env` file from the example, then run it with Docker Compose:

```bash
copy .env.example .env
```

Then run:

```bash
docker compose up -d
```

The container uses these mounts:

- `sync_folder/` for downloaded MP3s.
- A named volume for `/app/.spotify_token_cache`, with the actual cache stored at `/app/.spotify_token_cache/token_cache.json` so Spotify auth persists.

The API container exposes:

- `GET /manifest` for file metadata and checksums.
- `GET /files/<name>` for downloading a specific MP3.
- `GET /health` for a simple readiness check.

To test a single run inside Docker:

```bash
docker compose run --rm mp3-sync python main.py --run-once
```

To start the API by itself:

```bash
docker compose up -d mp3-api
```

By default it listens on `http://localhost:8000`.

## API

Run the file API locally:

```bash
python api.py --port 8000
```

Or use the console script after installing the project:

```bash
pip install -e .
mp3-api
```

Endpoints:

- `GET /manifest` returns file names, sizes, timestamps, and SHA-256 hashes.
- `GET /files/<name>` downloads a single file.
- `GET /health` returns a simple readiness check.

## Run Continuously

By default, `main.py` waits until the next 1:00 AM local time, runs once, then repeats every day:

```bash
python main.py
```

For a quick immediate test run, use:

```bash
python main.py --run-once
```

You can also install the project and use the console script:

```bash
pip install -e .
mp3-sync
```

For a true background process on Windows, the most reliable option is Task Scheduler:

1. Create a task that starts at logon or system startup.
2. Point it at your virtualenv Python executable or `mp3-sync`.
3. Set the trigger to daily at 1:00 AM.
4. Set the task to restart on failure.

## Notes

- `config.json`, `.spotify_token_cache`, `.venv`, `__pycache__/`, and generated audio in `sync_folder/` are ignored by Git.
- The default curator mode is controlled by `SPOTIFY_CURATOR_MODE`.
