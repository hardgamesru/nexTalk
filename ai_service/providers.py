from __future__ import annotations

import json
import os
import socket
import urllib.error
import urllib.request
from abc import ABC, abstractmethod
from typing import Iterable


class ProviderError(Exception):
    def __init__(self, message: str, *, status_code: int = 502):
        super().__init__(message)
        self.message = message
        self.status_code = status_code


class AIProvider(ABC):
    @abstractmethod
    def chat(self, messages: list[dict[str, str]]) -> str:
        raise NotImplementedError


class OllamaProvider(AIProvider):
    def __init__(self, base_url: str, model: str, timeout_seconds: float) -> None:
        self.base_url = base_url.rstrip("/")
        self.model = model
        self.timeout_seconds = timeout_seconds

    def chat(self, messages: list[dict[str, str]]) -> str:
        payload = {
            "model": self.model,
            "stream": False,
            "messages": messages,
        }
        request = urllib.request.Request(
            url=f"{self.base_url}/api/chat",
            data=json.dumps(payload).encode("utf-8"),
            headers={"Content-Type": "application/json"},
            method="POST",
        )

        try:
            with urllib.request.urlopen(request, timeout=self.timeout_seconds) as response:
                raw_body = response.read().decode("utf-8")
        except urllib.error.HTTPError as error:
            body = error.read().decode("utf-8", errors="replace")
            lowered = body.lower()
            if error.code == 404 and "model" in lowered:
                raise ProviderError(
                    f"Ollama model '{self.model}' is not available. Run 'ollama pull {self.model}'.",
                    status_code=502,
                ) from error
            raise ProviderError(_extract_remote_error(body) or "Ollama request failed.", status_code=502) from error
        except urllib.error.URLError as error:
            reason = getattr(error, "reason", None)
            if isinstance(reason, socket.timeout):
                raise ProviderError("Ollama request timed out.", status_code=504) from error
            raise ProviderError("Cannot reach Ollama. Is 'ollama serve' running?", status_code=502) from error
        except TimeoutError as error:
            raise ProviderError("Ollama request timed out.", status_code=504) from error

        try:
            data = json.loads(raw_body)
        except json.JSONDecodeError as error:
            raise ProviderError("Ollama returned invalid JSON.", status_code=502) from error

        content = (
            data.get("message", {}).get("content")
            if isinstance(data.get("message"), dict)
            else None
        )
        if not isinstance(content, str) or not content.strip():
            raise ProviderError("Ollama returned an empty answer.", status_code=502)
        return content.strip()


class OpenAIProvider(AIProvider):
    def __init__(self, _model: str, _timeout_seconds: float) -> None:
        self.model = _model
        self.timeout_seconds = _timeout_seconds

    def chat(self, messages: list[dict[str, str]]) -> str:
        raise ProviderError(
            "OpenAI provider is not configured yet. Switch AI_PROVIDER back to 'ollama'.",
            status_code=501,
        )


def build_provider_from_env() -> tuple[str, str, AIProvider]:
    provider_name = os.getenv("AI_PROVIDER", "ollama").strip().lower() or "ollama"
    model = os.getenv("OLLAMA_MODEL", "llama3.2").strip() or "llama3.2"
    base_url = os.getenv("OLLAMA_BASE_URL", "http://127.0.0.1:11434").strip() or "http://127.0.0.1:11434"
    timeout_seconds = _load_timeout(os.getenv("AI_REQUEST_TIMEOUT", "45"))

    if provider_name == "ollama":
        return provider_name, model, OllamaProvider(base_url, model, timeout_seconds)
    if provider_name == "openai":
        return provider_name, model, OpenAIProvider(model, timeout_seconds)
    raise ProviderError(f"Unknown AI provider '{provider_name}'.", status_code=400)


def normalize_messages(raw_messages: Iterable[object]) -> list[dict[str, str]]:
    normalized: list[dict[str, str]] = []
    for item in raw_messages:
        if not isinstance(item, dict):
            continue
        role = str(item.get("role", "")).strip().lower()
        content = str(item.get("content", "")).strip()
        if role not in {"system", "user", "assistant"} or not content:
            continue
        normalized.append({"role": role, "content": content})
    return normalized


def _extract_remote_error(body: str) -> str:
    if not body.strip():
        return ""
    try:
        data = json.loads(body)
    except json.JSONDecodeError:
        return body.strip()

    for key in ("error", "message"):
        value = data.get(key)
        if isinstance(value, str) and value.strip():
            return value.strip()
    return body.strip()


def _load_timeout(raw_value: str) -> float:
    try:
        timeout = float(raw_value)
    except ValueError:
        return 45.0
    if timeout <= 0:
        return 45.0
    return timeout
