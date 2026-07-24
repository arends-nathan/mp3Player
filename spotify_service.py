import logging
import os
from collections import Counter
from typing import Iterable, List, Optional
from urllib.parse import parse_qs, urlparse

import spotipy
from spotipy.oauth2 import SpotifyOAuth

logger = logging.getLogger(__name__)


def create_spotify_client(client_id, client_secret, redirect_uri):
    logger.info("[Spotify] Authenticating...")
    scope = "playlist-read-private user-top-read user-library-read"
    cache_path = os.getenv("SPOTIFY_TOKEN_CACHE_PATH", ".spotify_token_cache")

    auth_manager = SpotifyOAuth(
        client_id=client_id,
        client_secret=client_secret,
        redirect_uri=redirect_uri,
        scope=scope,
        cache_path=cache_path,
        open_browser=False,
    )

    cached_token = auth_manager.get_cached_token()
    if cached_token:
        logger.info("[Spotify] Found cached token; reusing it.")
        if auth_manager.is_token_expired(cached_token):
            logger.info("[Spotify] Cached token is expired; refreshing it.")
            auth_manager.refresh_access_token(cached_token["refresh_token"])
    else:
        logger.info("[Spotify] No cached token found.")
        auth_url = auth_manager.get_authorize_url()
        logger.info("[Spotify] Open this URL in your browser and approve access:")
        logger.info("[Spotify] %s", auth_url)
        auth_code = None
        while not auth_code:
            redirected_value = input(
                "[Spotify] Paste the full redirected URL after approval, or paste the code itself: "
            ).strip()
            parsed_url = urlparse(redirected_value)
            auth_code = parse_qs(parsed_url.query).get("code", [None])[0]
            if not auth_code and redirected_value and redirected_value != redirect_uri:
                auth_code = redirected_value
            if not auth_code:
                logger.error(
                    "[Spotify] No authorization code found. The value must look like %s?code=...",
                    redirect_uri,
                )
        logger.info("[Spotify] Exchanging authorization code for an access token...")
        auth_manager.get_access_token(code=auth_code, as_dict=True, check_cache=False)

    return spotipy.Spotify(auth_manager=auth_manager)


def get_spotify_playlist_tracks(client_id, client_secret, redirect_uri, playlist_id):
    """Authenticates with Spotify and extracts 'Artist - Track Name' strings from a playlist."""
    sp = create_spotify_client(client_id, client_secret, redirect_uri)
    logger.info("[Spotify] Auth client created.")

    logger.info("[Spotify] Fetching tracks from playlist ID: %s", playlist_id)
    queries = []

    results = sp.playlist_items(playlist_id)
    tracks = results['items']
    page_number = 1
    logger.info("[Spotify] Loaded page %d with %d items.", page_number, len(tracks))

    while results['next']:
        results = sp.next(results)
        tracks.extend(results['items'])
        page_number += 1
        logger.info("[Spotify] Loaded page %d with %d additional items.", page_number, len(results['items']))

    for item in tracks:
        track = item.get('track')
        if track is None:
            track = item.get('item')
        if track is None:
            logger.info("[Spotify] Skipping playlist item without a track payload.")
            continue
        if track.get('type') != 'track':
            logger.info("[Spotify] Skipping non-track playlist item of type %s.", track.get('type'))
            continue
        artist_name = track['artists'][0]['name']
        track_name = track['name']
        queries.append(f"{artist_name} - {track_name}")

    logger.info("[Spotify] Found %d tracks.", len(queries))
    return queries


def get_algorithmic_recommendations(client_id, client_secret, redirect_uri, genres=None, limit=15):
    """Builds a track list based on Spotify recommendation seeds and genre fallback search."""
    sp = create_spotify_client(client_id, client_secret, redirect_uri)
    normalized_genres = [g.strip().lower() for g in (genres or []) if isinstance(g, str) and g.strip()]

    logger.info("[Spotify] Building recommendations with genre seeds: %s", normalized_genres)
    queries = []

    try:
        if normalized_genres:
            logger.info("[Spotify] Using recommendation seed genres: %s", normalized_genres[:5])
            recommendation_limit = min(100, limit)
            recommendations = sp.recommendations(seed_genres=normalized_genres[:5], limit=recommendation_limit)
            for track in recommendations.get("tracks", []):
                artist_name = track['artists'][0]['name']
                track_name = track['name']
                queries.append(f"{artist_name} - {track_name}")

            if queries:
                logger.info("[Spotify] Found %d recommendation tracks.", len(queries))
                return queries[:limit]

            logger.warning("[Spotify] Recommendations endpoint returned no tracks; falling back to search.")
        else:
            logger.warning("[Spotify] No overlapping seed genres for recommendations; falling back.")
    except spotipy.exceptions.SpotifyException as exc:
        logger.warning("[Spotify] Recommendations endpoint unavailable (%s). Falling back to search methods.", exc)

    try:
        top_artists = sp.current_user_top_artists(time_range="medium_term", limit=10).get("items", [])
    except spotipy.exceptions.SpotifyException:
        top_artists = []

    for artist in top_artists:
        if len(queries) >= limit:
            break
        artist_name = artist.get("name")
        if not artist_name:
            continue

        results = sp.search(q=f"artist:{artist_name}", type="track", limit=10)
        for track in results.get("tracks", {}).get("items", []):
            candidate = f"{track['artists'][0]['name']} - {track['name']}"
            if candidate not in queries:
                queries.append(candidate)
            if len(queries) >= limit:
                break

    if queries:
        logger.info("[Spotify] Found %d tracks from top-artist fallback.", len(queries))
        return queries[:limit]

    primary_genre = normalized_genres[0] if normalized_genres else "pop"
    logger.info("[Spotify] Falling back to search for %d tracks in genre: %s", limit, primary_genre)

    offset = 0
    while len(queries) < limit:
        batch_size = min(50, limit - len(queries))
        results = sp.search(q=f"genre:{primary_genre}", type="track", limit=batch_size, offset=offset)
        tracks = results['tracks']['items']

        if not tracks:
            break

        for track in tracks:
            artist_name = track['artists'][0]['name']
            track_name = track['name']
            queries.append(f"{artist_name} - {track_name}")
            if len(queries) >= limit:
                break

        offset += len(tracks)

    logger.info("[Spotify] Found %d tracks from genre fallback search.", len(queries))
    return queries


def get_user_top_genres(client_id, client_secret, redirect_uri, time_range="medium_term", top_n=10):
    sp = create_spotify_client(client_id, client_secret, redirect_uri)
    logger.info("[Spotify] Fetching top artists for genre extraction (%s).", time_range)

    results = sp.current_user_top_artists(time_range=time_range, limit=50)
    artists = results.get('items', [])

    top_tracks = sp.current_user_top_tracks(time_range=time_range, limit=50).get('items', [])
    track_artist_ids = []
    seen_artist_ids = set()
    for track in top_tracks:
        for artist in track.get('artists', []):
            artist_id = artist.get('id')
            if artist_id and artist_id not in seen_artist_ids:
                seen_artist_ids.add(artist_id)
                track_artist_ids.append(artist_id)

    for artist_id in track_artist_ids:
        try:
            artist = sp.artist(artist_id)
        except spotipy.exceptions.SpotifyException as exc:
            logger.warning("[Spotify] Failed to load artist metadata for %s: %s", artist_id, exc)
            continue
        if artist:
            artists.append(artist)

    genre_counter = Counter()
    for artist in artists:
        for genre in artist.get('genres', []):
            genre_counter[genre] += 1

    top_genres = genre_counter.most_common(top_n)
    if not top_genres:
        logger.warning("[Spotify] No genres found from Spotify listening history. Using a local broad fallback.")
        fallback_genres = [
            'pop', 'rock', 'hip hop', 'indie', 'electronic',
            'r&b', 'country', 'folk', 'metal', 'dance'
        ]
        top_genres = [(genre, 1) for genre in fallback_genres[:top_n]]

    logger.info("[Spotify] Top genres: %s", [g for g, _ in top_genres])
    return top_genres
