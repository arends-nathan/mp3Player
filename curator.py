import logging
import random
from typing import Iterable, List, Optional

import spotipy

logger = logging.getLogger(__name__)


class SpotifyCurator:
    """Curates personalized Spotify track selections from metadata only."""

    def __init__(self, client: spotipy.Spotify):
        self.client = client

    def _paginate(self, first_page: dict, item_key: str) -> List[dict]:
        items = list(first_page.get(item_key, []))
        page = first_page
        while page.get("next"):
            page = self.client.next(page)
            items.extend(page.get(item_key, []))
        return items

    @staticmethod
    def _track_display_name(track: dict) -> Optional[str]:
        artists = track.get("artists") or []
        if not artists or not track.get("name"):
            return None
        return f"{artists[0].get('name', 'Unknown Artist')} - {track.get('name')}"

    def _audio_features_by_id(self, track_ids: Iterable[str]) -> dict:
        features_by_id = {}
        chunk: List[str] = []
        for track_id in track_ids:
            if not track_id:
                continue
            chunk.append(track_id)
            if len(chunk) == 100:
                try:
                    audio_features = self.client.audio_features(chunk) or []
                except spotipy.exceptions.SpotifyException as exc:
                    logger.warning("[Curator] Audio features unavailable for current account (%s); using fallback recommendations.", exc)
                    return {}

                for feature in audio_features:
                    if feature and feature.get("id"):
                        features_by_id[feature["id"]] = feature
                chunk = []

        if chunk:
            try:
                audio_features = self.client.audio_features(chunk) or []
            except spotipy.exceptions.SpotifyException as exc:
                logger.warning("[Curator] Audio features unavailable for current account (%s); using fallback recommendations.", exc)
                return {}

            for feature in audio_features:
                if feature and feature.get("id"):
                    features_by_id[feature["id"]] = feature

        return features_by_id

    def _shuffle_limit(self, tracks: List[str], limit: int) -> List[str]:
        unique_tracks = list(dict.fromkeys(tracks))
        random.shuffle(unique_tracks)
        return unique_tracks[:limit]

    def _search_tracks_by_artist_names(self, artist_names: List[str], limit: int) -> List[str]:
        tracks = []
        for artist_name in artist_names:
            if len(tracks) >= limit:
                break
            if not artist_name:
                continue

            try:
                results = self.client.search(q=f"artist:{artist_name}", type="track", limit=10)
            except spotipy.exceptions.SpotifyException as exc:
                logger.warning("[Curator] Search fallback failed for artist %s: %s", artist_name, exc)
                continue

            for track in results.get("tracks", {}).get("items", []):
                display_name = self._track_display_name(track)
                if display_name and display_name not in tracks:
                    tracks.append(display_name)
                if len(tracks) >= limit:
                    break

        return tracks

    def _standard_recommendations(self, limit: int) -> List[str]:
        logger.info("[Curator] Falling back to standard Spotify recommendations.")
        top_artist_names = []
        try:
            top_artists = self.client.current_user_top_artists(time_range="medium_term", limit=5).get("items", [])
            top_artist_names = [artist.get("name") for artist in top_artists if artist.get("name")]
            seed_artists = [artist.get("id") for artist in top_artists if artist.get("id")]
        except spotipy.exceptions.SpotifyException as exc:
            logger.warning("[Curator] Unable to load fallback seed artists: %s", exc)
            seed_artists = []

        recommendation_args = {"limit": min(100, limit)}
        if seed_artists:
            recommendation_args["seed_artists"] = seed_artists[:5]

        try:
            if seed_artists:
                recommendations = self.client.recommendations(**recommendation_args)
                tracks = []
                for track in recommendations.get("tracks", []):
                    display_name = self._track_display_name(track)
                    if display_name:
                        tracks.append(display_name)

                if tracks:
                    return self._shuffle_limit(tracks, limit)
        except spotipy.exceptions.SpotifyException as exc:
            logger.warning("[Curator] Recommendations endpoint unavailable (%s); using search fallback.", exc)

        search_tracks = self._search_tracks_by_artist_names(top_artist_names, limit)
        if search_tracks:
            logger.info("[Curator] Search fallback produced %d tracks.", len(search_tracks))
            return self._shuffle_limit(search_tracks, limit)

        try:
            top_tracks = self.client.current_user_top_tracks(time_range="medium_term", limit=10).get("items", [])
        except spotipy.exceptions.SpotifyException as exc:
            logger.warning("[Curator] Unable to load top tracks for final fallback: %s", exc)
            top_tracks = []

        track_names = []
        for track in top_tracks:
            display_name = self._track_display_name(track)
            if display_name:
                track_names.append(display_name)

        if track_names:
            logger.info("[Curator] Top-track fallback produced %d tracks.", len(track_names))
            return self._shuffle_limit(track_names, limit)

        return []

    def _seed_artists_from_top(self, time_range: str, limit: int) -> List[str]:
        top_artists = self.client.current_user_top_artists(time_range=time_range, limit=limit).get("items", [])
        return [artist.get("id") for artist in top_artists if artist.get("id")]

    def get_mood_rotation(self, target_mood="energetic", limit=10):
        logger.info("[Curator] Building mood rotation for target mood: %s", target_mood)
        saved_tracks = self.client.current_user_saved_tracks(limit=50)
        saved_items = self._paginate(saved_tracks, "items")[:50]

        track_payloads = []
        track_ids = []
        for item in saved_items:
            track = item.get("track")
            if not track or track.get("type") != "track":
                continue
            track_id = track.get("id")
            if not track_id:
                continue
            track_payloads.append(track)
            track_ids.append(track_id)

        features_by_id = self._audio_features_by_id(track_ids)
        filtered_tracks = []
        for track in track_payloads:
            feature = features_by_id.get(track.get("id"))
            if not feature:
                continue

            energy = feature.get("energy")
            valence = feature.get("valence")
            acousticness = feature.get("acousticness")

            if target_mood == "energetic" and energy is not None and valence is not None:
                if energy > 0.7 and valence > 0.4:
                    filtered_tracks.append(self._track_display_name(track))
            elif target_mood == "chill" and energy is not None and acousticness is not None:
                if energy < 0.4 and acousticness > 0.5:
                    filtered_tracks.append(self._track_display_name(track))
            elif target_mood == "happy" and valence is not None:
                if valence > 0.7:
                    filtered_tracks.append(self._track_display_name(track))

        filtered_tracks = [track for track in filtered_tracks if track]
        if not filtered_tracks:
            logger.warning("[Curator] Mood filter returned no tracks; using standard recommendations.")
            return self._standard_recommendations(limit)

        logger.info("[Curator] Mood rotation produced %d tracks before shuffling.", len(filtered_tracks))
        return self._shuffle_limit(filtered_tracks, limit)

    def get_time_capsule_mix(self, limit=10):
        logger.info("[Curator] Building time capsule mix from long-term favorite artists.")
        seed_artists = self._seed_artists_from_top(time_range="long_term", limit=5)
        if not seed_artists:
            logger.warning("[Curator] No seed artists found; using standard recommendations.")
            return self._standard_recommendations(limit)

        try:
            recommendations = self.client.recommendations(seed_artists=seed_artists, limit=min(100, limit))
        except spotipy.exceptions.SpotifyException as exc:
            logger.warning("[Curator] Time capsule recommendations unavailable (%s); using standard recommendations.", exc)
            return self._standard_recommendations(limit)

        tracks = []
        for track in recommendations.get("tracks", []):
            display_name = self._track_display_name(track)
            if display_name:
                tracks.append(display_name)
        if not tracks:
            logger.warning("[Curator] Time capsule mix returned no tracks; using standard recommendations.")
            return self._standard_recommendations(limit)

        return self._shuffle_limit(tracks, limit)

    def get_collaborative_crossover(self, friend_artist_id, limit=10):
        logger.info("[Curator] Building collaborative crossover with artist ID: %s", friend_artist_id)
        try:
            user_top_artist = self.client.current_user_top_artists(time_range="short_term", limit=1).get("items", [])
        except spotipy.exceptions.SpotifyException as exc:
            logger.warning("[Curator] Failed to fetch short-term top artist: %s", exc)
            user_top_artist = []

        user_artist_id = user_top_artist[0].get("id") if user_top_artist else None
        seed_artists = [artist_id for artist_id in [user_artist_id, friend_artist_id] if artist_id]

        if len(seed_artists) < 2:
            logger.warning("[Curator] Missing crossover seeds; using standard recommendations.")
            return self._standard_recommendations(limit)

        try:
            recommendations = self.client.recommendations(seed_artists=seed_artists, limit=min(100, limit))
        except spotipy.exceptions.SpotifyException as exc:
            logger.warning("[Curator] Collaborative crossover recommendations unavailable (%s); using standard recommendations.", exc)
            return self._standard_recommendations(limit)

        tracks = []
        for track in recommendations.get("tracks", []):
            display_name = self._track_display_name(track)
            if display_name:
                tracks.append(display_name)
        if not tracks:
            logger.warning("[Curator] Collaborative crossover returned no tracks; using standard recommendations.")
            return self._standard_recommendations(limit)

        return self._shuffle_limit(tracks, limit)

    def get_clustered_vibe_of_the_day(self, limit=10):
        logger.info("[Curator] Building clustered vibe of the day from top tracks.")
        top_tracks = self.client.current_user_top_tracks(time_range="medium_term", limit=50)
        track_items = self._paginate(top_tracks, "items")[:50]

        track_payloads = []
        track_ids = []
        for track in track_items:
            track_id = track.get("id")
            if not track_id:
                continue
            track_payloads.append(track)
            track_ids.append(track_id)

        features_by_id = self._audio_features_by_id(track_ids)
        fast_vibe = []
        slow_vibe = []
        for track in track_payloads:
            feature = features_by_id.get(track.get("id"))
            if not feature or feature.get("tempo") is None:
                continue
            display_name = self._track_display_name(track)
            if not display_name:
                continue

            if feature["tempo"] >= 115:
                fast_vibe.append(display_name)
            else:
                slow_vibe.append(display_name)

        chosen_cluster = random.choice([cluster for cluster in [fast_vibe, slow_vibe] if cluster] or [None])
        if not chosen_cluster:
            logger.warning("[Curator] No tempo clusters found; using standard recommendations.")
            return self._standard_recommendations(limit)

        logger.info(
            "[Curator] Selected %s cluster with %d tracks.",
            "Fast Vibe" if chosen_cluster is fast_vibe else "Slow Vibe",
            len(chosen_cluster),
        )
        return self._shuffle_limit(chosen_cluster, limit)
