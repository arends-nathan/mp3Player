import argparse
import hashlib
import json
import logging
import mimetypes
import os
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote, urlparse


logger = logging.getLogger(__name__)

DEFAULT_ROOT = Path(os.getenv("OUTPUT_DIR", "./sync_folder")).resolve()
DEFAULT_HOST = os.getenv("SYNC_API_HOST", "0.0.0.0")
DEFAULT_PORT = int(os.getenv("SYNC_API_PORT", "8000"))
DEFAULT_API_KEY = os.getenv("SYNC_API_KEY", "").strip()


def _utc_iso(timestamp: float) -> str:
    return datetime.fromtimestamp(timestamp, tz=timezone.utc).isoformat().replace("+00:00", "Z")


def _safe_resolve(root: Path, relative_path: str) -> Path | None:
    candidate = (root / relative_path).resolve()
    try:
        candidate.relative_to(root)
    except ValueError:
        return None
    return candidate


def _sha256(file_path: Path) -> str:
    digest = hashlib.sha256()
    with file_path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def build_manifest(root: Path) -> dict:
    root = root.resolve()
    files = []

    for file_path in sorted(root.rglob("*")):
        if not file_path.is_file():
            continue
        relative_name = file_path.relative_to(root).as_posix()
        stat = file_path.stat()
        files.append(
            {
                "name": relative_name,
                "size": stat.st_size,
                "modified": _utc_iso(stat.st_mtime),
                "sha256": _sha256(file_path),
                "url": f"/files/{relative_name}",
            }
        )

    latest_modified = max((item["modified"] for item in files), default=None)
    manifest_seed = "|".join(f'{item["name"]}:{item["sha256"]}' for item in files)
    manifest_version = hashlib.sha256(manifest_seed.encode("utf-8")).hexdigest() if files else "empty"

    return {
        "root": root.name,
        "version": manifest_version,
        "latest_modified": latest_modified,
        "count": len(files),
        "files": files,
    }


class SyncRequestHandler(BaseHTTPRequestHandler):
    server_version = "MP3SyncAPI/1.0"

    def _send_json(self, status_code: int, payload: dict) -> None:
        encoded = json.dumps(payload, indent=2).encode("utf-8")
        self.send_response(status_code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(encoded)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(encoded)

    def _send_file(self, file_path: Path) -> None:
        content_type = mimetypes.guess_type(file_path.name)[0] or "application/octet-stream"
        size = file_path.stat().st_size
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(size))
        self.send_header("Content-Disposition", f'attachment; filename="{file_path.name}"')
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        with file_path.open("rb") as stream:
            while True:
                chunk = stream.read(1024 * 1024)
                if not chunk:
                    break
                self.wfile.write(chunk)

    @property
    def root(self) -> Path:
        return self.server.root  # type: ignore[attr-defined]

    @property
    def api_key(self) -> str:
        return self.server.api_key  # type: ignore[attr-defined]

    def _is_authorized(self) -> bool:
        if not self.api_key:
            return True
        request_key = self.headers.get("X-API-Key", "").strip()
        return request_key == self.api_key

    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path

        if path in {"/", "/health"}:
            return self._send_json(
                200,
                {
                    "ok": True,
                    "manifest": "/manifest",
                    "files": "/files/<name>",
                    "status": "/status",
                    "auth_required": bool(self.api_key),
                },
            )

        if not self._is_authorized():
            return self._send_json(401, {"error": "unauthorized"})

        if path == "/manifest":
            return self._send_json(200, build_manifest(self.root))

        if path == "/status":
            manifest = build_manifest(self.root)
            return self._send_json(
                200,
                {
                    "ok": True,
                    "version": manifest.get("version"),
                    "file_count": manifest.get("count"),
                    "latest_modified": manifest.get("latest_modified"),
                    "root": manifest.get("root"),
                },
            )

        if path.startswith("/files/"):
            relative_name = unquote(path.removeprefix("/files/"))
            file_path = _safe_resolve(self.root, relative_name)
            if file_path is None or not file_path.exists() or not file_path.is_file():
                return self._send_json(404, {"error": "file not found"})
            return self._send_file(file_path)

        self._send_json(404, {"error": "not found"})

    def log_message(self, format, *args):
        logger.info("%s - %s", self.address_string(), format % args)


def run_server(host: str, port: int, root: Path) -> None:
    root = root.resolve()
    root.mkdir(parents=True, exist_ok=True)
    server = ThreadingHTTPServer((host, port), SyncRequestHandler)
    server.root = root  # type: ignore[attr-defined]
    server.api_key = DEFAULT_API_KEY  # type: ignore[attr-defined]

    logger.info("[API] Serving %s on http://%s:%d", root, host, port)
    if DEFAULT_API_KEY:
        logger.info("[API] API key authentication enabled.")
    else:
        logger.warning("[API] API key authentication disabled. Set SYNC_API_KEY to enable protection.")
    logger.info("[API] Manifest endpoint: /manifest")
    logger.info("[API] File endpoint: /files/<name>")
    logger.info("[API] Status endpoint: /status")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        logger.info("[API] Shutting down.")
    finally:
        server.server_close()


def parse_args():
    parser = argparse.ArgumentParser(description="Lightweight MP3 file API")
    parser.add_argument("--host", default=DEFAULT_HOST, help="Host to bind to.")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help="Port to bind to.")
    parser.add_argument("--root", default=str(DEFAULT_ROOT), help="Directory to serve.")
    return parser.parse_args()


def main():
    logging.basicConfig(level=logging.INFO, format="[%(levelname)s] %(message)s")
    args = parse_args()
    run_server(args.host, args.port, Path(args.root))


if __name__ == "__main__":
    main()