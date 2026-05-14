from __future__ import annotations

import json
import os
import socket
import ssl
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from abc import ABC, abstractmethod
from typing import Iterable


class ProviderError(Exception):
    # status_code пробрасывается наружу в HTTP-ответ ai_service. Так UI видит
    # понятную ошибку, а не всегда одинаковый 500.
    def __init__(self, message: str, *, status_code: int = 502):
        super().__init__(message)
        self.message = message
        self.status_code = status_code


class AIProvider(ABC):
    @abstractmethod
    def chat(self, messages: list[dict[str, str]]) -> str:
        raise NotImplementedError

    def health_check(self) -> None:
        return None


class _TokenCache:
    def __init__(self) -> None:
        self.access_token = ""
        self.expires_at = 0.0
        self.lock = threading.Lock()


_gigachat_token_cache = _TokenCache()


class OllamaProvider(AIProvider):
    def __init__(self, base_url: str, model: str, timeout_seconds: float) -> None:
        self.base_url = base_url.rstrip("/")
        self.model = model
        self.timeout_seconds = timeout_seconds

    def chat(self, messages: list[dict[str, str]]) -> str:
        # Ollama поддерживает OpenAI-like список role/content, поэтому фронтенд
        # может использовать тот же формат для всех провайдеров.
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


class GigaChatProvider(AIProvider):
    def __init__(
        self,
        auth_key: str,
        scope: str,
        auth_url: str,
        base_url: str,
        model: str,
        timeout_seconds: float,
        ssl_context: ssl.SSLContext | None,
    ) -> None:
        self.auth_key = auth_key
        self.scope = scope
        self.auth_url = auth_url
        self.base_url = base_url.rstrip("/")
        self.model = model
        self.timeout_seconds = timeout_seconds
        self.ssl_context = ssl_context

    def health_check(self) -> None:
        # Для GigaChat health-check проверяет именно OAuth, потому что без токена
        # последующий /chat все равно не сможет работать.
        self._access_token()

    def chat(self, messages: list[dict[str, str]]) -> str:
        return self._chat_with_retry(messages, retry_on_auth_error=True)

    def _chat_with_retry(self, messages: list[dict[str, str]], *, retry_on_auth_error: bool) -> str:
        token = self._access_token()
        payload = {
            "model": self.model,
            "stream": False,
            "messages": messages,
        }
        request = urllib.request.Request(
            url=f"{self.base_url}/chat/completions",
            data=json.dumps(payload, ensure_ascii=False).encode("utf-8"),
            headers={
                "Accept": "application/json",
                "Authorization": f"Bearer {token}",
                "Content-Type": "application/json",
            },
            method="POST",
        )

        try:
            raw_body = self._open_json_request(request)
        except urllib.error.HTTPError as error:
            body = error.read().decode("utf-8", errors="replace")
            # Токен может протухнуть раньше expires_at на стороне провайдера.
            # Один раз очищаем кэш и повторяем запрос, чтобы пользователь не
            # видел ошибку авторизации из-за устаревшего access_token.
            if error.code == 401 and retry_on_auth_error:
                self._clear_cached_token()
                return self._chat_with_retry(messages, retry_on_auth_error=False)
            if error.code == 404:
                raise ProviderError(f"GigaChat model '{self.model}' is not available.", status_code=502) from error
            if error.code == 429:
                raise ProviderError("GigaChat rate limit exceeded.", status_code=429) from error
            raise ProviderError(_extract_remote_error(body) or "GigaChat request failed.", status_code=502) from error
        except urllib.error.URLError as error:
            reason = getattr(error, "reason", None)
            if isinstance(reason, socket.timeout):
                raise ProviderError("GigaChat request timed out.", status_code=504) from error
            raise ProviderError(f"Cannot reach GigaChat API: {_format_url_error(error)}", status_code=502) from error
        except TimeoutError as error:
            raise ProviderError("GigaChat request timed out.", status_code=504) from error

        try:
            data = json.loads(raw_body)
        except json.JSONDecodeError as error:
            raise ProviderError("GigaChat returned invalid JSON.", status_code=502) from error

        choices = data.get("choices")
        message = choices[0].get("message") if isinstance(choices, list) and choices else None
        content = message.get("content") if isinstance(message, dict) else None
        if not isinstance(content, str) or not content.strip():
            raise ProviderError("GigaChat returned an empty answer.", status_code=502)
        return content.strip()

    def _access_token(self) -> str:
        now = time.time()
        with _gigachat_token_cache.lock:
            # Держим один токен на процесс ai_service: так все пользователи web
            # не получают OAuth-токен заново на каждое сообщение.
            if _gigachat_token_cache.access_token and _gigachat_token_cache.expires_at - 60 > now:
                return _gigachat_token_cache.access_token

            token, expires_at = self._request_access_token()
            _gigachat_token_cache.access_token = token
            _gigachat_token_cache.expires_at = expires_at
            return token

    def _request_access_token(self) -> tuple[str, float]:
        encoded_body = urllib.parse.urlencode({"scope": self.scope}).encode("utf-8")
        request = urllib.request.Request(
            url=self.auth_url,
            data=encoded_body,
            headers={
                "Accept": "application/json",
                "Authorization": _basic_auth_header(self.auth_key),
                "Content-Type": "application/x-www-form-urlencoded",
                # RqUID обязателен для GigaChat OAuth: это уникальный id запроса.
                "RqUID": str(uuid.uuid4()),
            },
            method="POST",
        )

        try:
            raw_body = self._open_json_request(request)
        except urllib.error.HTTPError as error:
            body = error.read().decode("utf-8", errors="replace")
            raise ProviderError(_extract_remote_error(body) or "GigaChat authorization failed.", status_code=502) from error
        except urllib.error.URLError as error:
            reason = getattr(error, "reason", None)
            if isinstance(reason, socket.timeout):
                raise ProviderError("GigaChat authorization timed out.", status_code=504) from error
            raise ProviderError(
                f"Cannot reach GigaChat authorization endpoint: {_format_url_error(error)}",
                status_code=502,
            ) from error

        try:
            data = json.loads(raw_body)
        except json.JSONDecodeError as error:
            raise ProviderError("GigaChat authorization returned invalid JSON.", status_code=502) from error

        token = data.get("access_token")
        raw_expires_at = data.get("expires_at", 0)
        if not isinstance(token, str) or not token:
            raise ProviderError("GigaChat authorization returned no access token.", status_code=502)

        expires_at = _normalize_expires_at(raw_expires_at)
        return token, expires_at

    def _open_json_request(self, request: urllib.request.Request) -> str:
        with urllib.request.urlopen(
            request,
            timeout=self.timeout_seconds,
            context=self.ssl_context,
        ) as response:
            return response.read().decode("utf-8")

    def _clear_cached_token(self) -> None:
        with _gigachat_token_cache.lock:
            _gigachat_token_cache.access_token = ""
            _gigachat_token_cache.expires_at = 0.0


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
    # Единая фабрика провайдеров держит настройки в env, а не в UI. Благодаря
    # этому пользователям web-клиента не нужно знать API-ключи.
    provider_name = os.getenv("AI_PROVIDER", "ollama").strip().lower() or "ollama"
    timeout_seconds = _load_timeout(os.getenv("AI_REQUEST_TIMEOUT", "45"))

    if provider_name == "ollama":
        model = os.getenv("OLLAMA_MODEL", "llama3.2").strip() or "llama3.2"
        base_url = os.getenv("OLLAMA_BASE_URL", "http://127.0.0.1:11434").strip() or "http://127.0.0.1:11434"
        return provider_name, model, OllamaProvider(base_url, model, timeout_seconds)
    if provider_name == "gigachat":
        auth_key = os.getenv("GIGACHAT_AUTH_KEY", "").strip()
        if not auth_key:
            raise ProviderError("GIGACHAT_AUTH_KEY is required for GigaChat provider.", status_code=500)
        model = os.getenv("GIGACHAT_MODEL", "GigaChat").strip() or "GigaChat"
        scope = os.getenv("GIGACHAT_SCOPE", "GIGACHAT_API_PERS").strip() or "GIGACHAT_API_PERS"
        auth_url = (
            os.getenv("GIGACHAT_AUTH_URL", "https://ngw.devices.sberbank.ru:9443/api/v2/oauth").strip()
            or "https://ngw.devices.sberbank.ru:9443/api/v2/oauth"
        )
        base_url = (
            os.getenv("GIGACHAT_BASE_URL", "https://gigachat.devices.sberbank.ru/api/v1").strip()
            or "https://gigachat.devices.sberbank.ru/api/v1"
        )
        ssl_context = _build_ssl_context()
        return provider_name, model, GigaChatProvider(
            auth_key,
            scope,
            auth_url,
            base_url,
            model,
            timeout_seconds,
            ssl_context,
        )
    if provider_name == "openai":
        model = os.getenv("OPENAI_MODEL", "gpt-4o-mini").strip() or "gpt-4o-mini"
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


def _normalize_expires_at(raw_value: object) -> float:
    try:
        expires_at = float(raw_value)
    except (TypeError, ValueError):
        return time.time() + 25 * 60
    # GigaChat может вернуть expires_at в миллисекундах Unix time.
    # Внутри сервиса храним секунды, как ожидает time.time().
    if expires_at > 10_000_000_000:
        expires_at /= 1000
    return expires_at


def _build_ssl_context() -> ssl.SSLContext | None:
    verify = os.getenv("GIGACHAT_VERIFY_SSL", "1").strip().lower()
    if verify in {"0", "false", "no"}:
        # Это аварийный локальный режим для стендов с самоподписанной цепочкой.
        # В обычной эксплуатации лучше указать GIGACHAT_CA_FILE или оставить verify=1.
        return ssl._create_unverified_context()

    ca_file = os.getenv("GIGACHAT_CA_FILE", "").strip()
    if ca_file:
        return ssl.create_default_context(cafile=ca_file)
    return None


def _basic_auth_header(auth_key: str) -> str:
    # В .env удобно хранить либо чистый Authorization key, либо уже готовое
    # значение вида "Basic ...". Поддерживаем оба варианта.
    return auth_key if auth_key.lower().startswith("basic ") else f"Basic {auth_key}"


def _format_url_error(error: urllib.error.URLError) -> str:
    reason = getattr(error, "reason", None)
    if reason:
        return str(reason)
    return str(error)


def _load_timeout(raw_value: str) -> float:
    try:
        timeout = float(raw_value)
    except ValueError:
        return 45.0
    if timeout <= 0:
        return 45.0
    return timeout
