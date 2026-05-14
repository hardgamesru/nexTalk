from __future__ import annotations

import json
import os
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from providers import ProviderError, build_provider_from_env, normalize_messages


HOST = os.getenv("AI_SERVICE_HOST", "127.0.0.1")
PORT = int(os.getenv("AI_SERVICE_PORT", "5000"))
MAX_BODY_BYTES = int(os.getenv("AI_SERVICE_MAX_BODY_BYTES", "65536"))
MAX_MESSAGES = int(os.getenv("AI_SERVICE_MAX_MESSAGES", "24"))
ALLOWED_ORIGINS = {
    origin.strip()
    for origin in os.getenv("AI_SERVICE_ALLOWED_ORIGINS", "").split(",")
    if origin.strip()
}


class Handler(BaseHTTPRequestHandler):
    server_version = "NexTalkAI/1.0"

    def do_OPTIONS(self) -> None:
        if not self._is_origin_allowed():
            self._send_json(403, {"status": "error", "error": "origin_not_allowed"})
            return
        self.send_response(204)
        self._send_cors_headers()
        self.end_headers()

    def do_GET(self) -> None:
        if not self._is_origin_allowed():
            self._send_json(403, {"status": "error", "error": "origin_not_allowed"})
            return
        if self.path != "/health":
            self._send_json(404, {"status": "error", "error": "not_found"})
            return

        try:
            provider_name, model, _provider = build_provider_from_env()
        except ProviderError as error:
            self._send_json(error.status_code, {"status": "error", "error": error.message})
            return

        self._send_json(
            200,
            {
                "status": "ok",
                "provider": provider_name,
                "model": model,
            },
        )

    def do_POST(self) -> None:
        if not self._is_origin_allowed():
            self._send_json(403, {"status": "error", "error": "origin_not_allowed"})
            return
        if self.path != "/chat":
            self._send_json(404, {"status": "error", "error": "not_found"})
            return

        try:
            data = self._read_json_body()
        except ValueError as error:
            self._send_json(400, {"status": "error", "error": str(error)})
            return

        messages = normalize_messages(data.get("messages", []))
        if not messages:
            self._send_json(400, {"status": "error", "error": "Request must include non-empty messages."})
            return
        if len(messages) > MAX_MESSAGES:
            self._send_json(413, {"status": "error", "error": "Too many messages in one request."})
            return

        try:
            _provider_name, _model, provider = build_provider_from_env()
            answer = provider.chat(messages)
        except ProviderError as error:
            self._send_json(error.status_code, {"status": "error", "error": error.message})
            return
        except Exception:
            self._send_json(500, {"status": "error", "error": "Unexpected AI service error."})
            return

        self._send_json(200, {"status": "ok", "answer": answer})

    def log_message(self, format: str, *args) -> None:
        print(f"[ai_service] {self.address_string()} - {format % args}")

    def _read_json_body(self) -> dict:
        content_length = int(self.headers.get("Content-Length", "0") or "0")
        if content_length <= 0:
            raise ValueError("Request body is required.")
        if content_length > MAX_BODY_BYTES:
            raise ValueError("Request body is too large.")

        raw_body = self.rfile.read(content_length).decode("utf-8", errors="replace")
        if not raw_body.strip():
            raise ValueError("Request body is empty.")

        try:
            data = json.loads(raw_body)
        except json.JSONDecodeError as error:
            raise ValueError("Request body must be valid JSON.") from error

        if not isinstance(data, dict):
            raise ValueError("Request body must be a JSON object.")
        return data

    def _is_origin_allowed(self) -> bool:
        origin = self.headers.get("Origin", "")
        if not origin:
            return True
        if origin in ALLOWED_ORIGINS:
            return True
        return origin.startswith("http://localhost:") or origin.startswith("http://127.0.0.1:")

    def _send_cors_headers(self) -> None:
        origin = self.headers.get("Origin", "")
        if origin and self._is_origin_allowed():
            self.send_header("Access-Control-Allow-Origin", origin)
            self.send_header("Vary", "Origin")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")

    def _send_json(self, status_code: int, payload: dict) -> None:
        encoded = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status_code)
        self._send_cors_headers()
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)


if __name__ == "__main__":
    server = ThreadingHTTPServer((HOST, PORT), Handler)
    print(f"AI service started on http://{HOST}:{PORT}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nAI service stopped")
    finally:
        server.server_close()
