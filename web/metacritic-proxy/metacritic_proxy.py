"""Minimal, cached Metacritic score bridge for SwitchU.

This service only reads public game pages that are not disallowed by the
published robots.txt. It never calls Metacritic search, login, review, or user
areas; it does not use proxies, browser automation, CAPTCHAs, or credentials.
The public API exposes only the two aggregate scores, their counts, and the
source URL needed for attribution.
"""

from __future__ import annotations

import hmac
import html as html_module
import ipaddress
import json
import os
import re
import sqlite3
import threading
import time
import unicodedata
from collections import defaultdict, deque
from datetime import UTC, datetime
from pathlib import Path
from typing import Any, Callable
from urllib.error import HTTPError, URLError
from urllib.parse import quote, urlencode, urlparse
from urllib.request import Request, urlopen

from fastapi import FastAPI, HTTPException, Request as FastApiRequest
from fastapi.responses import JSONResponse


APP_NAME = "switchu-metacritic"
SOURCE_HOST = "www.metacritic.com"
SOURCE_GAME_URL = f"https://{SOURCE_HOST}/game"
IGDB_API = "https://api.igdb.com/v4/games"
IGDB_TIME_TO_BEAT_API = "https://api.igdb.com/v4/game_time_to_beats"
IGDB_PLATFORM_IDS = {
    "nintendo-switch": 130, "pc": 6, "playstation-5": 167, "playstation-4": 48,
    "xbox-series-x": 169, "xbox-one": 49, "xbox-360": 12, "playstation-3": 9,
    "playstation-2": 8, "playstation": 7, "gamecube": 21, "wii": 5, "wii-u": 41,
    "nintendo-64": 4, "super-nintendo": 19, "nes": 18, "game-boy-advance": 24,
    "nintendo-ds": 20, "nintendo-3ds": 37, "psp": 38, "playstation-vita": 46,
    "dreamcast": 23, "sega-genesis": 29,
}
TWITCH_TOKEN_API = "https://id.twitch.tv/oauth2/token"
GEMINI_API = "https://generativelanguage.googleapis.com/v1beta/models"
USER_AGENT = "SwitchU-Metadata/0.1 (+https://switchu-api.nclabs.dev)"
CACHE_TTL_SECONDS = 30 * 24 * 60 * 60
STALE_TTL_SECONDS = 180 * 24 * 60 * 60
NEGATIVE_CACHE_SECONDS = 24 * 60 * 60
# Gemini's free tier caps requests per DAY, counted per project and per model
# (quotaId GenerateRequestsPerDayPerProjectPerModel-FreeTier). Exhausting it
# makes the dossier fall back to the original English text. That fallback must
# not be stored for the normal 30 days, or the game stays English long after the
# quota resets. See _gemini_model_chain for how the daily budget is stretched.
UNTRANSLATED_CACHE_SECONDS = 30 * 60
UPSTREAM_MIN_INTERVAL_SECONDS = 0.5
RATE_LIMIT_REQUESTS = 120
RATE_LIMIT_AVAILABILITY_REQUESTS = 120
RATE_LIMIT_WINDOW_SECONDS = 60
SUPPORTED_PLATFORMS = {
    "nintendo-switch": "Nintendo Switch",
    "pc": "PC",
    "playstation-5": "PlayStation 5",
    "playstation-4": "PlayStation 4",
    "xbox-series-x": "Xbox Series X",
    "xbox-one": "Xbox One",
    "xbox-360": "Xbox 360",
    "playstation-3": "PlayStation 3",
    "playstation-2": "PlayStation 2",
    "playstation": "PlayStation",
    "gamecube": "GameCube",
    "wii": "Wii",
    "wii-u": "Wii U",
    "nintendo-64": "Nintendo 64",
    "super-nintendo": "Super Nintendo Entertainment System",
    "nes": "NES",
    "game-boy-advance": "Game Boy Advance",
    "nintendo-ds": "Nintendo DS",
    "nintendo-3ds": "Nintendo 3DS",
    "psp": "PSP",
    "playstation-vita": "PlayStation Vita",
    "dreamcast": "Dreamcast",
    "sega-genesis": "Genesis",
}
DB_PATH = Path(os.environ.get("SWITCHU_METADATA_DB", "/var/lib/switchu-metacritic/cache.sqlite3"))


def _trusted_networks() -> tuple[ipaddress.IPv4Network | ipaddress.IPv6Network, ...]:
    raw = os.environ.get(
        "SWITCHU_TRUSTED_NETWORKS",
        "172.26.128.0/26,10.0.0.0/26,127.0.0.0/8,::1/128",
    )
    try:
        return tuple(ipaddress.ip_network(value.strip()) for value in raw.split(",") if value.strip())
    except ValueError as exc:
        raise RuntimeError("SWITCHU_TRUSTED_NETWORKS contains an invalid network") from exc


TRUSTED_NETWORKS = _trusted_networks()

# Official-client gate. Builds made by the project carry a secret key that is
# injected at compile time and never committed, so a build compiled from the
# public source (for example a fork) cannot present one. "monitor" only logs,
# "enforce" rejects. Keys are read from a file so they can be rotated live.
CLIENT_AUTH_MODE = os.environ.get("SWITCHU_CLIENT_AUTH", "monitor").strip().lower()
if CLIENT_AUTH_MODE not in {"off", "monitor", "enforce"}:
    raise RuntimeError("SWITCHU_CLIENT_AUTH must be off, monitor or enforce")
CLIENT_KEYS_FILE = os.environ.get("SWITCHU_CLIENT_KEYS_FILE", "/etc/switchu/client-keys")
CLIENT_KEY_HEADER = "x-switchu-key"
_client_keys_state: tuple[float, tuple[bytes, ...]] = (-1.0, ())
_client_keys_lock = threading.Lock()
_client_auth_logged: dict[str, float] = {}


def _client_keys() -> tuple[bytes, ...]:
    global _client_keys_state
    try:
        mtime = os.stat(CLIENT_KEYS_FILE).st_mtime
    except OSError:
        return ()
    with _client_keys_lock:
        if _client_keys_state[0] != mtime:
            try:
                with open(CLIENT_KEYS_FILE, encoding="utf-8") as handle:
                    lines = (line.strip() for line in handle)
                    keys = tuple(line.encode() for line in lines if line and not line.startswith("#"))
            except OSError:
                return _client_keys_state[1]
            _client_keys_state = (mtime, keys)
        return _client_keys_state[1]


def _client_authorized(request: FastApiRequest) -> bool:
    presented = (request.headers.get("x-switchu-key") or request.headers.get("x-omnilaunch-key") or "").strip().encode()
    if not presented or len(presented) > 256:
        return False
    matched = False
    for key in _client_keys():
        matched |= hmac.compare_digest(presented, key)
    return matched


def _log_unauthorized_client(request: FastApiRequest, identity: str) -> None:
    # One line per client per ten minutes keeps a hostile client from flooding
    # the journal while still showing who is calling without a key.
    now = time.monotonic()
    with _client_keys_lock:
        if len(_client_auth_logged) > 2000:
            for stale in [k for k, t in _client_auth_logged.items() if now - t >= 600]:
                _client_auth_logged.pop(stale, None)
        if now - _client_auth_logged.get(identity, -600.0) < 600:
            return
        _client_auth_logged[identity] = now
    agent = request.headers.get("user-agent", "")[:80]
    print(f"client-auth: no valid key mode={CLIENT_AUTH_MODE} ip={identity} path={request.url.path} ua={agent!r}", flush=True)


_database_lock = threading.Lock()
_upstream_lock = threading.Lock()
_last_upstream_request = 0.0
_igdb_token_lock = threading.Lock()
_igdb_access_token = ""
_igdb_access_token_expires_at = 0.0
_rate_windows: dict[str, deque[float]] = defaultdict(deque)
_rate_windows_availability: dict[str, deque[float]] = defaultdict(deque)
_rate_windows_availability: dict[str, deque[float]] = defaultdict(deque)
# Outcome of the most recent real translation attempt. The admin panel reports
# this instead of probing Gemini, which would consume the scarce free-tier quota.
_last_translation_ok = True
_rate_lock = threading.Lock()

app = FastAPI(docs_url=None, redoc_url=None, openapi_url=None, title=APP_NAME)


def _utc_now() -> str:
    return datetime.now(UTC).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def _is_trusted_proxy(host: str | None) -> bool:
    if not host:
        return False
    try:
        address = ipaddress.ip_address(host)
    except ValueError:
        return False
    return any(address in network for network in TRUSTED_NETWORKS)


def _client_identity(request: FastApiRequest) -> str:
    # Cloudflare's connector sets this header. Read it only after checking that
    # the actual TCP peer is one of our tunnel/private networks.
    forwarded = request.headers.get("cf-connecting-ip", "").strip()
    return forwarded or (request.client.host if request.client else "unknown")


def _allow_request(identity: str, is_availability: bool = False) -> bool:
    now = time.monotonic()
    limit = RATE_LIMIT_AVAILABILITY_REQUESTS if is_availability else RATE_LIMIT_REQUESTS
    with _rate_lock:
        rate_dict = _rate_windows_availability if is_availability else _rate_windows
        if len(rate_dict) > 2000:
            stale_keys = [k for k, w in rate_dict.items() if not w or now - w[-1] >= RATE_LIMIT_WINDOW_SECONDS]
            for k in stale_keys:
                rate_dict.pop(k, None)
        window = rate_dict[identity]
        while window and now - window[0] >= RATE_LIMIT_WINDOW_SECONDS:
            window.popleft()
        if len(window) >= limit:
            return False
        window.append(now)
        return True


def _connect() -> sqlite3.Connection:
    DB_PATH.parent.mkdir(parents=True, exist_ok=True)
    database = sqlite3.connect(DB_PATH)
    database.row_factory = sqlite3.Row
    return database


def _initialize_database() -> None:
    with _database_lock, _connect() as database:
        database.execute(
            """
            CREATE TABLE IF NOT EXISTS score_cache (
                cache_key TEXT PRIMARY KEY,
                response_json TEXT NOT NULL,
                fetched_at INTEGER NOT NULL,
                fresh_until INTEGER NOT NULL,
                stale_until INTEGER NOT NULL,
                found INTEGER NOT NULL
            )
            """
        )


def _load_cached(cache_key: str) -> tuple[dict[str, Any], bool] | None:
    now = int(time.time())
    with _database_lock, _connect() as database:
        row = database.execute(
            "SELECT response_json, fresh_until, stale_until FROM score_cache WHERE cache_key = ?", (cache_key,)
        ).fetchone()
    if row is None or row["stale_until"] <= now:
        return None
    try:
        response = json.loads(row["response_json"])
    except json.JSONDecodeError:
        return None
    return response, row["fresh_until"] > now


def _save_cached(cache_key: str, response: dict[str, Any], found: bool,
                 ttl_override: int | None = None) -> None:
    now = int(time.time())
    ttl = ttl_override if ttl_override is not None else (
        CACHE_TTL_SECONDS if found else NEGATIVE_CACHE_SECONDS)
    with _database_lock, _connect() as database:
        database.execute(
            """
            INSERT INTO score_cache(cache_key, response_json, fetched_at, fresh_until, stale_until, found)
            VALUES (?, ?, ?, ?, ?, ?)
            ON CONFLICT(cache_key) DO UPDATE SET
                response_json=excluded.response_json,
                fetched_at=excluded.fetched_at,
                fresh_until=excluded.fresh_until,
                stale_until=excluded.stale_until,
                found=excluded.found
            """,
            (cache_key, json.dumps(response, ensure_ascii=False, separators=(",", ":")), now,
             now + ttl, now + STALE_TTL_SECONDS, 1 if found else 0),
        )


def _normalise_title(value: str) -> str:
    value = unicodedata.normalize("NFKD", value).encode("ascii", "ignore").decode("ascii")
    value = value.casefold().replace("&", " and ")
    return " ".join(re.findall(r"[a-z0-9]+", value))


def _catalogue_title(value: str) -> str:
    """Remove launcher packaging annotations before IGDB matching and caching.

    Community ports commonly append a suffix such as ``(Android port)`` to the
    NACP title. That text is not part of the game name, so including it in the
    IGDB search and cache key turns otherwise exact matches (for example GTA:
    San Andreas) into permanent negative-cache entries.
    """
    return re.sub(
        r"\s*\((?:android\s+)?port\)\s*$", "", value,
        flags=re.IGNORECASE,
    ).strip()


def _urlopen_with_retry(request: Request, *, timeout: int):
    """Retry a single upstream call once after a brief pause on a network-level
    failure (DNS blip, reset connection). IGDB, Twitch and Gemini occasionally
    drop one request; without this a lone hiccup turned into a permanent 502
    for the console even though the very next attempt would have worked.
    HTTP status errors are not retried here; they are handled by callers.
    """
    try:
        return urlopen(request, timeout=timeout)
    except (URLError, TimeoutError):
        time.sleep(1.5)
        return urlopen(request, timeout=timeout)


def _igdb_credentials() -> tuple[str, str]:
    client_id = os.environ.get("IGDB_CLIENT_ID", "").strip()
    client_secret = os.environ.get("IGDB_CLIENT_SECRET", "").strip()
    if not client_id or not client_secret:
        raise HTTPException(status_code=503, detail="IGDB metadata is not configured")
    return client_id, client_secret


def _igdb_token() -> str:
    global _igdb_access_token, _igdb_access_token_expires_at

    with _igdb_token_lock:
        if _igdb_access_token and time.time() < _igdb_access_token_expires_at - 60:
            return _igdb_access_token
        client_id, client_secret = _igdb_credentials()
        payload = urlencode(
            {
                "client_id": client_id,
                "client_secret": client_secret,
                "grant_type": "client_credentials",
            }
        ).encode("ascii")
        request = Request(
            TWITCH_TOKEN_API,
            data=payload,
            headers={"Accept": "application/json", "User-Agent": USER_AGENT},
            method="POST",
        )
        try:
            with _urlopen_with_retry(request, timeout=18) as response:
                token_response = json.loads(response.read().decode("utf-8"))
        except (HTTPError, URLError, TimeoutError, json.JSONDecodeError) as exc:
            raise RuntimeError("Twitch token service is temporarily unavailable") from exc
        token = token_response.get("access_token")
        expires_in = token_response.get("expires_in")
        if not isinstance(token, str) or not token or not isinstance(expires_in, int):
            raise RuntimeError("Twitch token response was invalid")
        _igdb_access_token = token
        _igdb_access_token_expires_at = time.time() + expires_in
        return token


def _igdb_query_literal(value: str) -> str:
    return value.replace("\\", "\\\\").replace('"', '\\"')


def _igdb_query(query: str, endpoint: str = IGDB_API, retry: bool = True) -> list[dict[str, Any]]:
    global _igdb_access_token, _igdb_access_token_expires_at

    client_id, _ = _igdb_credentials()
    request = Request(
        endpoint,
        data=query.encode("utf-8"),
        headers={
            "Accept": "application/json",
            "Client-ID": client_id,
            "Authorization": f"Bearer {_igdb_token()}",
            "User-Agent": USER_AGENT,
        },
        method="POST",
    )
    try:
        with _urlopen_with_retry(request, timeout=18) as response:
            payload = json.loads(response.read().decode("utf-8"))
    except HTTPError as exc:
        if exc.code == 401 and retry:
            with _igdb_token_lock:
                _igdb_access_token = ""
                _igdb_access_token_expires_at = 0.0
            return _igdb_query(query, endpoint, retry=False)
        raise RuntimeError(f"IGDB returned HTTP {exc.code}") from exc
    except (URLError, TimeoutError, json.JSONDecodeError) as exc:
        raise RuntimeError("IGDB is temporarily unavailable") from exc
    return [value for value in payload if isinstance(value, dict)] if isinstance(payload, list) else []


def _igdb_image(image_id: Any, size: str) -> str | None:
    if not isinstance(image_id, str) or not re.fullmatch(r"[a-zA-Z0-9_]+", image_id):
        return None
    return f"https://images.igdb.com/igdb/image/upload/t_{size}/{image_id}.jpg"


def _igdb_game_matches_platform(game: dict[str, Any], platform: str) -> bool:
    platforms = game.get("platforms")
    return isinstance(platforms, list) and any(
        isinstance(value, dict) and value.get("id") == IGDB_PLATFORM_IDS[platform]
        for value in platforms
    )


def _igdb_best_game(games: list[dict[str, Any]], requested_title: str,
                    platform: str) -> dict[str, Any] | None:
    requested = _normalise_title(requested_title)
    candidates = [game for game in games if _igdb_game_matches_platform(game, platform)]
    if not candidates:
        return None

    def score(game: dict[str, Any]) -> tuple[int, int]:
        name = _normalise_title(str(game.get("name", "")))
        exact = int(name == requested)
        contained = int(bool(name and (name in requested or requested in name)))
        return exact, contained

    best = max(candidates, key=score)
    return best if score(best)[1] else None


def _igdb_date(value: Any) -> str | None:
    if not isinstance(value, int | float) or isinstance(value, bool):
        return None
    try:
        return datetime.fromtimestamp(value, UTC).date().isoformat()
    except (OverflowError, OSError, ValueError):
        return None


def _igdb_hours(value: Any) -> float | None:
    if not isinstance(value, int | float) or isinstance(value, bool) or value <= 0:
        return None
    # IGDB's game_time_to_beat values are seconds.
    return round(float(value) / 3600, 1)


def _language_code(value: str) -> str:
    return value.strip().replace("_", "-").split("-", 1)[0].casefold()


def _translation_chunks(summary: str, maximum_characters: int = 360) -> list[str]:
    """Split a synopsis only at sentence boundaries before translation.

    Gemini occasionally returned a valid but unfinished long response. Keeping
    each request small and sentence-complete avoids a translated synopsis
    ending midway through a game description.
    """
    chunks: list[str] = []
    for paragraph in re.split(r"\n\s*\n", summary.strip()):
        paragraph = paragraph.strip()
        if not paragraph:
            continue
        sentences = re.split(r"(?<=[.!?])\s+", paragraph)
        current = ""
        for sentence in sentences:
            sentence = sentence.strip()
            if not sentence:
                continue
            if current and len(current) + 1 + len(sentence) > maximum_characters:
                chunks.append(current)
                current = ""
            if len(sentence) > maximum_characters:
                if current:
                    chunks.append(current)
                    current = ""
                chunks.extend(sentence[index:index + maximum_characters]
                              for index in range(0, len(sentence), maximum_characters))
            else:
                current = f"{current} {sentence}".strip()
        if current:
            chunks.append(current)
    return chunks or [summary]


_TRANSLATION_TARGETS = {
    "pt-br": "Brazilian Portuguese",
    "es-es": "Spanish (Spain)",
    "fr-fr": "French",
    "de-de": "German",
    "it-it": "Italian",
    "nl-nl": "Dutch",
    "ru-ru": "Russian",
}

# IGDB reuses this compact vocabulary across a large part of its catalogue.
# Keeping these visible classification labels local avoids making the dossier
# depend on an AI response for basic navigation metadata.
_LABEL_TRANSLATIONS = {
    "pt-br": {"Platform": "Plataforma", "Adventure": "Aventura", "Action": "Ação", "Fantasy": "Fantasia", "Sandbox": "Sandbox", "Open world": "Mundo aberto", "Single player": "Um jogador", "Co-operative": "Cooperativo", "Multiplayer": "Multijogador", "Puzzle": "Quebra-cabeça", "Racing": "Corrida", "Fighting": "Luta", "Shooter": "Tiro", "Simulator": "Simulador", "Sport": "Esporte", "Strategy": "Estratégia", "Horror": "Terror", "Science fiction": "Ficção científica", "Stealth": "Furtividade", "Survival": "Sobrevivência"},
    "es-es": {"Platform": "Plataformas", "Adventure": "Aventura", "Action": "Acción", "Fantasy": "Fantasía", "Sandbox": "Sandbox", "Open world": "Mundo abierto", "Single player": "Un jugador", "Co-operative": "Cooperativo", "Multiplayer": "Multijugador", "Puzzle": "Rompecabezas", "Racing": "Carreras", "Fighting": "Lucha", "Shooter": "Disparos", "Simulator": "Simulador", "Sport": "Deportes", "Strategy": "Estrategia", "Horror": "Terror", "Science fiction": "Ciencia ficción", "Stealth": "Sigilo", "Survival": "Supervivencia"},
    "fr-fr": {"Platform": "Plateforme", "Adventure": "Aventure", "Action": "Action", "Fantasy": "Fantaisie", "Sandbox": "Bac à sable", "Open world": "Monde ouvert", "Single player": "Solo", "Co-operative": "Coopératif", "Multiplayer": "Multijoueur", "Puzzle": "Réflexion", "Racing": "Course", "Fighting": "Combat", "Shooter": "Tir", "Simulator": "Simulation", "Sport": "Sport", "Strategy": "Stratégie", "Horror": "Horreur", "Science fiction": "Science-fiction", "Stealth": "Infiltration", "Survival": "Survie"},
    "de-de": {"Platform": "Plattformer", "Adventure": "Abenteuer", "Action": "Action", "Fantasy": "Fantasy", "Sandbox": "Sandbox", "Open world": "Offene Welt", "Single player": "Einzelspieler", "Co-operative": "Kooperativ", "Multiplayer": "Mehrspieler", "Puzzle": "Rätsel", "Racing": "Rennen", "Fighting": "Kampf", "Shooter": "Shooter", "Simulator": "Simulation", "Sport": "Sport", "Strategy": "Strategie", "Horror": "Horror", "Science fiction": "Science-Fiction", "Stealth": "Schleichen", "Survival": "Überleben"},
    "it-it": {"Platform": "Piattaforme", "Adventure": "Avventura", "Action": "Azione", "Fantasy": "Fantasy", "Sandbox": "Sandbox", "Open world": "Mondo aperto", "Single player": "Giocatore singolo", "Co-operative": "Cooperativa", "Multiplayer": "Multigiocatore", "Puzzle": "Rompicapo", "Racing": "Corsa", "Fighting": "Combattimento", "Shooter": "Sparatutto", "Simulator": "Simulatore", "Sport": "Sport", "Strategy": "Strategia", "Horror": "Horror", "Science fiction": "Fantascienza", "Stealth": "Furtivo", "Survival": "Sopravvivenza"},
    "nl-nl": {"Platform": "Platform", "Adventure": "Avontuur", "Action": "Actie", "Fantasy": "Fantasy", "Sandbox": "Zandbak", "Open world": "Open wereld", "Single player": "Eén speler", "Co-operative": "Coöperatief", "Multiplayer": "Meerspeler", "Puzzle": "Puzzel", "Racing": "Racen", "Fighting": "Vechten", "Shooter": "Schieten", "Simulator": "Simulator", "Sport": "Sport", "Strategy": "Strategie", "Horror": "Horror", "Science fiction": "Sciencefiction", "Stealth": "Sluipen", "Survival": "Overleven"},
    "ru-ru": {"Platform": "Платформер", "Adventure": "Приключение", "Action": "Экшен", "Fantasy": "Фэнтези", "Sandbox": "Песочница", "Open world": "Открытый мир", "Single player": "Одиночная игра", "Co-operative": "Кооператив", "Multiplayer": "Многопользовательская игра", "Puzzle": "Головоломка", "Racing": "Гонки", "Fighting": "Файтинг", "Shooter": "Шутер", "Simulator": "Симулятор", "Sport": "Спорт", "Strategy": "Стратегия", "Horror": "Хоррор", "Science fiction": "Научная фантастика", "Stealth": "Скрытность", "Survival": "Выживание"},
}


def _normalized_locale(language: str) -> str:
    return language.strip().replace("_", "-").casefold()


def _translation_target(language: str) -> str | None:
    """Resolve a locale to the translator that serves its cache slot.

    Entries are cached per base language ("es"), not per region, so a variant
    the table does not list explicitly - es-MX, pt-PT, fr-CA - must resolve to
    the same translator as its base. Without this it would be treated as having
    no translator at all, and its untranslated English would be stored in the
    slot that the listed variant reads from.
    """
    locale = _normalized_locale(language)
    listed = _TRANSLATION_TARGETS.get(locale)
    if listed:
        return listed
    base = _language_code(locale)
    for key, value in _TRANSLATION_TARGETS.items():
        if _language_code(key) == base:
            return value
    return None


def _localize_labels(labels: list[str], language: str) -> list[str]:
    labels_by_locale = _LABEL_TRANSLATIONS.get(_normalized_locale(language), {})
    return [labels_by_locale.get(label, label) for label in labels]


def _translation_incomplete(result: dict[str, Any], language: str) -> bool:
    """True when a translation was expected for this language but did not happen.

    Distinguishes a real failure from a game that simply has no prose: if there
    is no summary and no storyline, there was nothing to translate and the entry
    is complete as it stands.
    """
    if not _translation_target(language):
        return False
    if not (result.get("summary") or result.get("storyline")):
        return False
    # Compare at base-language granularity, which is the granularity the cache
    # key uses. Text translated for es-ES fills the "es" slot and is complete
    # for an es-MX reader; demanding an exact locale match would mark a good
    # translation as failed and expire it in 30 minutes.
    return _language_code(result.get("summaryLanguage") or "en") != _language_code(language)


def _localize_cached_metadata(result: dict[str, Any], language: str) -> dict[str, Any]:
    localized = dict(result)
    for key in ("genres", "themes", "gameModes"):
        values = localized.get(key)
        if isinstance(values, list):
            localized[key] = _localize_labels([value for value in values if isinstance(value, str)], language)
    return localized


def _clean_gemini_json(text: str) -> str:
    cleaned = text.strip()
    if cleaned.startswith("```"):
        lines = cleaned.splitlines()
        if lines and lines[0].startswith("```"):
            lines = lines[1:]
        if lines and lines[-1].startswith("```"):
            lines = lines[:-1]
        cleaned = "\n".join(lines).strip()
    else:
        match = re.search(r"```(?:json)?\s*(.*?)\s*```", cleaned, re.DOTALL)
        if match:
            cleaned = match.group(1).strip()
    return cleaned


DEFAULT_GEMINI_MODEL = "gemini-3.1-flash-lite"
DEFAULT_GEMINI_FALLBACKS = (
    "gemini-3-flash-preview,"
    "gemini-2.5-flash,"
    "gemini-2.5-flash-lite"
)

# Google AI Studio's free tier only ever grants quota to these four models
# (verified against aistudio.google.com, 2026-09-04). Every Gemini 3.x model
# above flash-lite, and every 3.0 model, is paid-tier only there and would
# never accept a free-tier key -- keeping them in the fallback chain just
# spent time on requests guaranteed to 403/429. Trimmed the chain down to
# what the free tier actually serves.
ALLOWED_GEMINI_MODELS = frozenset({
    "gemini-3.1-flash-lite",
    "gemini-3-flash-preview",
    "gemini-2.5-flash",
    "gemini-2.5-flash-lite",
})


def _gemini_model_chain() -> list[str]:
    """Primary model first, then the fallbacks, de-duplicated.

    Each entry carries its own daily allowance. The order here is the order
    the budget is spent in. Only models in ALLOWED_GEMINI_MODELS (the ones
    Google AI Studio actually grants free-tier quota for) are permitted;
    anything else -- including newer/larger Gemini releases -- would need a
    paid key and is silently dropped from the chain.
    """
    primary_env = os.environ.get("GEMINI_MODEL", DEFAULT_GEMINI_MODEL).strip()
    fallback_env = os.environ.get("GEMINI_FALLBACK_MODELS", DEFAULT_GEMINI_FALLBACKS)

    configured = [primary_env] + fallback_env.split(",")
    chain: list[str] = []
    for name in (value.strip() for value in configured):
        if not name:
            continue
        if not re.fullmatch(r"[A-Za-z0-9._-]{3,100}", name):
            continue
        if name not in ALLOWED_GEMINI_MODELS:
            continue
        if name not in chain:
            chain.append(name)

    if not chain:
        for name in (DEFAULT_GEMINI_MODEL + "," + DEFAULT_GEMINI_FALLBACKS).split(","):
            if name not in chain and name in ALLOWED_GEMINI_MODELS:
                chain.append(name)

    return chain


_gemini_key_index = 0
_gemini_key_lock = threading.Lock()


def _gemini_api_keys() -> list[str]:
    raw_keys = os.environ.get("GEMINI_API_KEYS", "") or os.environ.get("GEMINI_API_KEY", "")
    keys = [k.strip() for k in re.split(r"[,\s]+", raw_keys) if k.strip()]
    return keys


def _gemini_text(prompt: str, api_keys: str | list[str], models: list[str],
                 validator: Callable[[str], bool] | None = None) -> str:
    """Ask models and API keys sequentially until one yields a valid translation.

    Rotates through configured keys on 429/quota or transient errors to maximize
    free-tier throughput.
    """
    global _last_translation_ok, _gemini_key_index
    keys = [api_keys] if isinstance(api_keys, str) else [k for k in api_keys if k]
    if not keys:
        raise RuntimeError("No Gemini API key configured")

    with _gemini_key_lock:
        start_idx = _gemini_key_index % len(keys)
    ordered_keys = keys[start_idx:] + keys[:start_idx]

    payload = json.dumps({
        "contents": [{"parts": [{"text": prompt}]}],
        "generationConfig": {"temperature": 0.1, "maxOutputTokens": 4096},
    }).encode("utf-8")
    last_error: Exception | None = None

    for model in models:
        if model not in ALLOWED_GEMINI_MODELS:
            continue
        for key_offset, key in enumerate(ordered_keys):
            request = Request(
                f"{GEMINI_API}/{model}:generateContent",
                data=payload,
                headers={"Content-Type": "application/json", "x-goog-api-key": key,
                         "User-Agent": USER_AGENT},
                method="POST",
            )
            try:
                with _urlopen_with_retry(request, timeout=20) as response:
                    response_json = json.loads(response.read().decode("utf-8"))
            except HTTPError as exc:
                last_error = RuntimeError(f"Gemini HTTP {exc.code} for {model}")
                continue
            except (URLError, TimeoutError, json.JSONDecodeError) as exc:
                last_error = RuntimeError(f"Gemini translation temporarily unavailable for {model}: {exc}")
                continue

            candidates = response_json.get("candidates") if isinstance(response_json, dict) else None
            content = candidates[0].get("content") if isinstance(candidates, list) and candidates and isinstance(candidates[0], dict) else None
            parts = content.get("parts") if isinstance(content, dict) else None
            raw_text = "".join(item.get("text", "") for item in parts if isinstance(item, dict)).strip() if isinstance(parts, list) else ""
            translated = _clean_gemini_json(raw_text)
            if not translated:
                last_error = RuntimeError(f"Gemini model {model} returned empty or blocked content")
                continue

            if validator is not None:
                try:
                    if not validator(translated):
                        last_error = RuntimeError(f"Gemini model {model} payload failed validation")
                        continue
                except Exception as val_exc:
                    last_error = RuntimeError(f"Gemini model {model} validation error: {val_exc}")
                    continue

            with _gemini_key_lock:
                _gemini_key_index = (start_idx + key_offset) % len(keys)
            _last_translation_ok = True
            return translated

    _last_translation_ok = False
    raise last_error or RuntimeError("No Gemini model accepted the request")


def _translation_credentials(language: str) -> tuple[str, list[str], list[str]] | None:
    target = _translation_target(language)
    keys = _gemini_api_keys()
    if not target or not keys:
        return None
    return target, keys, _gemini_model_chain()


def _translate_catalogue_texts(summary: str | None,
                               storyline: str | None,
                               language: str) -> tuple[str | None, str | None, str]:
    """Translate the two long text fields in one bounded Gemini request.

    Keeping summary and story together avoids a chain of API calls on a cache
    miss; the launcher can still receive one timely metadata response.
    """
    credentials = _translation_credentials(language)
    if not credentials:
        return summary, storyline, "en"
    target, api_key, models = credentials
    fields = [summary or "", storyline or ""]
    prompt = (
        f"Translate every non-empty value in this JSON array of video-game catalogue text into {target}. "
        "Do not summarize, omit, or add information. Preserve proper names and game titles. "
        "Keep exactly two strings in the original order. Return only valid JSON, without Markdown.\n\n"
        + json.dumps(fields, ensure_ascii=False)
    )

    def validate_payload(raw: str) -> bool:
        try:
            values = json.loads(raw)
        except json.JSONDecodeError:
            return False
        return (isinstance(values, list) and len(values) == 2 and
                all(isinstance(item, str) and len(item) <= len(fields[idx]) * 3 + 200
                    for idx, item in enumerate(values)))

    try:
        translated = _gemini_text(prompt, api_key, models, validator=validate_payload)
        values = json.loads(translated)
        return values[0] or None, values[1] or None, language.strip().replace("_", "-")
    except (RuntimeError, json.JSONDecodeError):
        return summary, storyline, "en"


def _translate_labels(labels: list[str], language: str) -> list[str]:
    """Translate genres, themes, and game modes in one deterministic request."""
    clean = [label.strip() for label in labels if isinstance(label, str) and label.strip()]
    credentials = _translation_credentials(language)
    if not clean or not credentials:
        return clean
    target, api_key, models = credentials
    prompt = (
        f"Translate every video-game classification in this JSON array into {target}. "
        "Keep the same number and order of items. Return only a valid JSON array of strings, "
        "without Markdown or commentary.\n\n"
        + json.dumps(clean, ensure_ascii=False)
    )

    def validate_payload(raw: str) -> bool:
        try:
            result = json.loads(raw)
        except json.JSONDecodeError:
            return False
        return (isinstance(result, list) and len(result) == len(clean) and
                all(isinstance(item, str) and item.strip() and len(item) <= 180 for item in result))

    try:
        translated = _gemini_text(prompt, api_key, models, validator=validate_payload)
        result = json.loads(translated)
        return [item.strip() for item in result]
    except (RuntimeError, json.JSONDecodeError):
        return clean


def _translate_metadata_fields(summary: str | None, storyline: str | None,
                               genres: list[str], themes: list[str], game_modes: list[str],
                               language: str) -> tuple[str | None, str | None, list[str], list[str], list[str], str]:
    """Localize all visible metadata in one short request, or keep a safe fallback."""
    credentials = _translation_credentials(language)
    if not credentials:
        return summary, storyline, genres, themes, game_modes, "en"
    target, api_key, models = credentials
    original = {
        "summary": summary or "",
        "storyline": storyline or "",
        "genres": genres,
        "themes": themes,
        "gameModes": game_modes,
    }
    prompt = (
        f"Translate every string value in this video-game metadata JSON into {target}. "
        "Do not summarize, omit, reorder, or add information. Preserve proper names and game titles. "
        "Keep the exact JSON object shape and each array length. Return only valid JSON, without Markdown.\n\n"
        + json.dumps(original, ensure_ascii=False)
    )

    def validate_payload(raw: str) -> bool:
        try:
            data = json.loads(raw)
        except json.JSONDecodeError:
            return False
        return isinstance(data, dict)

    try:
        translated_json = _gemini_text(prompt, api_key, models, validator=validate_payload)
        translated = json.loads(translated_json)
    except (RuntimeError, json.JSONDecodeError):
        return summary, storyline, genres, themes, game_modes, "en"
    if not isinstance(translated, dict):
        return summary, storyline, genres, themes, game_modes, "en"
    def text_value(key: str, fallback: str) -> str:
        value = translated.get(key)
        return value.strip() if isinstance(value, str) and value.strip() and len(value) <= len(fallback) * 3 + 200 else fallback
    def label_values(key: str, fallback: list[str]) -> list[str]:
        value = translated.get(key)
        if (not isinstance(value, list) or len(value) != len(fallback) or
                any(not isinstance(item, str) or not item.strip() or len(item) > 180 for item in value)):
            return fallback
        return [item.strip() for item in value]
    return (text_value("summary", original["summary"]) or None,
            text_value("storyline", original["storyline"]) or None,
            label_values("genres", genres), label_values("themes", themes),
            label_values("gameModes", game_modes), language.strip().replace("_", "-"))


def _igdb_availability(title: str, platform: str) -> dict[str, Any]:
    """Cheap existence check: does this title exist on IGDB for this platform.

    The platform picker calls this once per platform card (up to 12 requests)
    every time its dossier opens, just to light a green/red dot -- it never
    needed the summary, storyline, screenshots, translated genres/themes or
    time-to-beat that /v1/metadata fetches and (before translation) sends
    through Gemini. Reusing that endpoint made every picker open fire up to
    12 full dossier fetches, which is what made it slow and prone to timing
    out on a re-open. This asks IGDB for id/name/platforms only.
    """
    query = (
        "fields id,name,platforms.id; "
        f"search \"{_igdb_query_literal(title)}\"; "
        f"where platforms = ({IGDB_PLATFORM_IDS[platform]}); limit 10;"
    )
    game = _igdb_best_game(_igdb_query(query), title, platform)
    return {
        "found": game is not None,
        "title": title,
        "platform": SUPPORTED_PLATFORMS[platform],
        "source": "IGDB",
        "fetchedAt": _utc_now(),
    }


def _igdb_metadata(title: str, platform: str, language: str) -> dict[str, Any]:
    query = (
        "fields id,name,summary,storyline,first_release_date,platforms.id,platforms.name,genres.name,themes.name,game_modes.name,"
        "involved_companies.company.name,involved_companies.developer,involved_companies.publisher,"
        f"screenshots.image_id,cover.image_id; search \"{_igdb_query_literal(title)}\"; "
        f"where platforms = ({IGDB_PLATFORM_IDS[platform]}); limit 10;"
    )
    game = _igdb_best_game(_igdb_query(query), title, platform)
    if game is None:
        return {
            "found": False,
            "title": title,
            "platform": SUPPORTED_PLATFORMS[platform],
            "source": "IGDB",
            "fetchedAt": _utc_now(),
        }

    companies = game.get("involved_companies") if isinstance(game.get("involved_companies"), list) else []

    def company_names(kind: str) -> list[str]:
        return list(
            dict.fromkeys(
                item["company"]["name"]
                for item in companies
                if isinstance(item, dict)
                and item.get(kind) is True
                and isinstance(item.get("company"), dict)
                and isinstance(item["company"].get("name"), str)
            )
        )

    genres = game.get("genres") if isinstance(game.get("genres"), list) else []
    themes = game.get("themes") if isinstance(game.get("themes"), list) else []
    game_modes = game.get("game_modes") if isinstance(game.get("game_modes"), list) else []
    screenshots = game.get("screenshots") if isinstance(game.get("screenshots"), list) else []
    game_id = game.get("id")
    time_to_beat = {}
    if isinstance(game_id, int):
        times = _igdb_query(
            "fields hastily,normally,completely; where game_id = " + str(game_id) + "; limit 1;",
            IGDB_TIME_TO_BEAT_API,
        )
        time_to_beat = times[0] if times else {}
    raw_genres = [item["name"] for item in genres if isinstance(item, dict) and isinstance(item.get("name"), str)]
    raw_themes = [item["name"] for item in themes if isinstance(item, dict) and isinstance(item.get("name"), str)]
    raw_game_modes = [item["name"] for item in game_modes if isinstance(item, dict) and isinstance(item.get("name"), str)]
    raw_genres = _localize_labels(raw_genres, language)
    raw_themes = _localize_labels(raw_themes, language)
    raw_game_modes = _localize_labels(raw_game_modes, language)
    summary, storyline, localized_genres, localized_themes, localized_game_modes, text_language = _translate_metadata_fields(
        game.get("summary") or None, game.get("storyline") or None,
        raw_genres, raw_themes, raw_game_modes, language)
    return {
        "found": True,
        "title": game.get("name") if isinstance(game.get("name"), str) else title,
        "platform": SUPPORTED_PLATFORMS[platform],
        "source": "IGDB",
        "summary": summary,
        "summaryLanguage": text_language,
        "storyline": storyline,
        "storylineLanguage": text_language,
        "releaseDate": _igdb_date(game.get("first_release_date")),
        "developers": company_names("developer"),
        "publishers": company_names("publisher"),
        "genres": localized_genres,
        "themes": localized_themes,
        "gameModes": localized_game_modes,
        "coverUrl": _igdb_image(game.get("cover", {}).get("image_id") if isinstance(game.get("cover"), dict) else None, "cover_big"),
        "screenshots": [
            image
            for image in (_igdb_image(item.get("image_id"), "screenshot_big") for item in screenshots if isinstance(item, dict))
            if image is not None
        ][:8],
        "timeToBeat": {
            "hastily": _igdb_hours(time_to_beat.get("hastily")),
            "main": _igdb_hours(time_to_beat.get("normally")),
            "completionist": _igdb_hours(time_to_beat.get("completely")),
        },
        "fetchedAt": _utc_now(),
    }


def _slug_candidates(title: str) -> list[str]:
    # An apostrophe closes up rather than separating: Metacritic writes Link's
    # Awakening as "links-awakening", never "link-s-awakening". _normalise_title
    # keeps only [a-z0-9] runs, so the apostrophe split the word in two and every
    # title carrying one missed -- Link's Awakening, Luigi's Mansion, Assassin's
    # Creed, No Man's Sky. Reported for Link's Awakening, which returned no
    # scores at all.
    #
    # The split form stays as a second candidate. It costs one 404 on a title
    # that does not have it, and this has no way to know which convention a
    # given page was written under.
    closed = _normalise_title(re.sub(r"['‘’ʼ]", "", title))
    normal = _normalise_title(title)

    candidates: list[str] = []
    for words in ([closed.split(), normal.split()] if closed != normal else [normal.split()]):
        if not words:
            continue
        candidates.append("-".join(words))
        # Some local titles include a leading article while the Metacritic URL
        # does not. Never introduce arbitrary URLs beyond these.
        if words[:1] in (["the"], ["a"], ["an"]) and len(words) > 2:
            candidates.append("-".join(words[1:]))
        # The reverse also happens: Metacritic's canonical slug carries a
        # leading "the" that the catalogue title omits (e.g. "Simpsons Hit &
        # Run" -> "the-simpsons-hit-and-run"). Reported for The Simpsons: Hit
        # & Run, which returned no scores at all despite the page existing.
        elif words[:1] != ["the"]:
            candidates.append("the-" + "-".join(words))
    return list(dict.fromkeys(candidate for candidate in candidates if 2 <= len(candidate) <= 180))


def _throttled_page(url: str) -> tuple[str, str]:
    global _last_upstream_request
    with _upstream_lock:
        remaining = UPSTREAM_MIN_INTERVAL_SECONDS - (time.monotonic() - _last_upstream_request)
        if remaining > 0:
            time.sleep(remaining)
        _last_upstream_request = time.monotonic()
        request = Request(url, headers={"Accept": "text/html", "User-Agent": USER_AGENT}, method="GET")
        try:
            with _urlopen_with_retry(request, timeout=18) as response:
                final_url = response.geturl()
                if urlparse(final_url).hostname != SOURCE_HOST:
                    raise RuntimeError("Metacritic redirected to an unexpected host")
                payload = response.read().decode("utf-8", "replace")
        except HTTPError:
            raise
        except (URLError, TimeoutError) as exc:
            raise RuntimeError("Metacritic is temporarily unavailable") from exc
    return payload, final_url


def _json_ld_game(page: str) -> dict[str, Any] | None:
    for raw in re.findall(r'<script type="application/ld\+json">(.*?)</script>', page, re.IGNORECASE | re.DOTALL):
        try:
            candidate = json.loads(html_module.unescape(raw))
        except json.JSONDecodeError:
            continue
        values = candidate if isinstance(candidate, list) else [candidate]
        for value in values:
            if isinstance(value, dict) and value.get("@type") == "VideoGame":
                return value
    return None


def _number(value: Any) -> int | float | None:
    if isinstance(value, bool):
        return None
    if isinstance(value, int | float):
        return value
    if isinstance(value, str):
        try:
            return float(value) if "." in value else int(value)
        except ValueError:
            return None
    return None


def _extract_user_score(page: str) -> tuple[float | None, int | None]:
    section = re.search(
        r'data-testid="global-score-header">\s*User score\s*</div>(.*?)(?=data-testid="global-score-header"|</main>)',
        page,
        re.IGNORECASE | re.DOTALL,
    )
    if section is None:
        return None, None
    content = section.group(1)
    score_match = re.search(r'title="User score\s+([0-9]+(?:\.[0-9]+)?)\s+out of 10"', content, re.IGNORECASE)
    count_match = re.search(r'See All\s+([0-9][0-9,]*)\s+User Reviews', content, re.IGNORECASE)
    if count_match is None:
        # Current game pages expose the three distribution counts rather than
        # one total.  Summing only these aggregate bars avoids touching the
        # user-review area and gives the count that produced the shown score.
        distribution = re.findall(
            r'aria-label="([0-9][0-9,]*)\s+(?:positive|neutral|negative) reviews',
            content,
            re.IGNORECASE,
        )
        review_count = sum(int(value.replace(",", "")) for value in distribution) if distribution else None
    else:
        review_count = int(count_match.group(1).replace(",", ""))
    return (
        float(score_match.group(1)) if score_match else None,
        review_count,
    )


def _parse_page(page: str, final_url: str, requested_title: str, platform: str) -> dict[str, Any] | None:
    game = _json_ld_game(page)
    if game is None:
        return None
    title = str(game.get("name", "")).strip()
    requested_normal = _normalise_title(requested_title)
    found_normal = _normalise_title(title)
    if not title or (requested_normal != found_normal and requested_normal not in found_normal and found_normal not in requested_normal):
        return None
    if SUPPORTED_PLATFORMS[platform].casefold() not in page.casefold():
        return None
    aggregate = game.get("aggregateRating") if isinstance(game.get("aggregateRating"), dict) else {}
    metascore = _number(aggregate.get("ratingValue"))
    if not isinstance(metascore, (int, float)) or not 0 <= metascore <= 100:
        return None
    review_count = _number(aggregate.get("reviewCount"))
    userscore, user_count = _extract_user_score(page)
    return {
        "found": True,
        "title": title,
        "platform": SUPPORTED_PLATFORMS[platform],
        "source": "Metacritic",
        "sourceUrl": final_url,
        "metascore": {"value": int(metascore), "reviewCount": int(review_count) if review_count is not None else None},
        "userScore": {"value": userscore, "reviewCount": user_count},
        "fetchedAt": _utc_now(),
    }


def _not_found(title: str, platform: str) -> dict[str, Any]:
    return {
        "found": False,
        "title": title,
        "platform": SUPPORTED_PLATFORMS[platform],
        "source": "Metacritic",
        "sourceUrl": None,
        "metascore": None,
        "userScore": None,
        "fetchedAt": _utc_now(),
    }


def _resolve(title: str, platform: str) -> dict[str, Any]:
    for slug in _slug_candidates(title):
        url = f"{SOURCE_GAME_URL}/{quote(slug, safe='-')}?{urlencode({'platform': platform})}"
        try:
            page, final_url = _throttled_page(url)
        except HTTPError as exc:
            if exc.code == 404:
                continue
            raise RuntimeError(f"Metacritic returned HTTP {exc.code}") from exc
        parsed = _parse_page(page, final_url, title, platform)
        if parsed is not None:
            return parsed
    return _not_found(title, platform)


@app.on_event("startup")
def startup() -> None:
    _initialize_database()


def _admin_list_entries() -> list[dict[str, Any]]:
    """Flatten the cache into rows the panel can render."""
    now = int(time.time())
    rows: list[dict[str, Any]] = []
    with _database_lock, _connect() as database:
        cursor = database.execute(
            "SELECT cache_key, response_json, fetched_at, fresh_until FROM score_cache")
        records = cursor.fetchall()
    for record in records:
        key = record["cache_key"]
        try:
            payload = json.loads(record["response_json"])
        except json.JSONDecodeError:
            continue
        dossier = key.startswith("igdb:")
        parts = key.split(":")
        language = parts[3] if dossier and len(parts) > 4 else ""
        # The key carries the base language ("pt") while the payload records the
        # full locale ("pt-BR"), so compare base codes or every translated entry
        # would be listed as a failure.
        translated = bool(
            dossier
            and (payload.get("summary") or payload.get("storyline"))
            and _language_code(payload.get("summaryLanguage") or "en") == _language_code(language or "en")
        )
        if dossier and not (payload.get("summary") or payload.get("storyline")):
            # Nothing to translate; do not flag it as a failure.
            translated = True
        rows.append({
            "cacheKey": key,
            "kind": "dossie" if dossier else "notas",
            "title": payload.get("title") or key.rsplit(":", 1)[-1],
            "language": language or "-",
            "translated": translated,
            "expiresIn": max(int(record["fresh_until"]) - now, 0),
            "ttlSeconds": max(int(record["fresh_until"]) - int(record["fetched_at"]), 1),
        })
    rows.sort(key=lambda row: (row["translated"], row["expiresIn"]))
    return rows


def _admin_service_status() -> dict[str, Any]:
    entries = _admin_list_entries()
    return {
        "total": len(entries),
        "untranslated": sum(1 for row in entries if row["kind"] == "dossie" and not row["translated"]),
        "igdbReady": bool(os.environ.get("IGDB_CLIENT_ID") and os.environ.get("IGDB_CLIENT_SECRET")),
        "geminiReady": _gemini_reachable(),
    }


def _gemini_reachable() -> bool:
    """Report whether translation is currently possible.

    A cheap probe would burn one of the few free-tier calls per minute, so this
    reports configuration plus the outcome of the most recent real attempt.
    """
    if not _gemini_api_keys():
        return False
    return _last_translation_ok


def _admin_refresh_metadata(title: str, language: str) -> dict[str, Any]:
    """Re-query the origin for one title, ignoring whatever is cached."""
    platform = "nintendo-switch"
    result = _igdb_metadata(title, platform, language)
    cache_key = f"igdb:v4:{platform}:{_language_code(language)}:{_normalise_title(title)}"
    incomplete = _translation_incomplete(result, language)
    _save_cached(cache_key, result, bool(result["found"]),
                 ttl_override=UNTRANSLATED_CACHE_SECONDS if incomplete else None)
    return {
        "found": bool(result.get("found")),
        "title": result.get("title") or title,
        "translated": not incomplete,
        "cacheKey": cache_key,
    }


def _admin_refresh_scores(title: str) -> dict[str, Any]:
    platform = "nintendo-switch"
    result = _resolve(title, platform)
    cache_key = f"{platform}:{_normalise_title(title)}"
    _save_cached(cache_key, result, bool(result["found"]))
    return {"found": bool(result.get("found")), "title": result.get("title") or title,
            "translated": True, "cacheKey": cache_key}


def _admin_delete_entry(cache_key: str) -> int:
    with _database_lock, _connect() as database:
        cursor = database.execute("DELETE FROM score_cache WHERE cache_key=?", (cache_key,))
        return cursor.rowcount


def _admin_purge_untranslated() -> int:
    removed = 0
    for row in _admin_list_entries():
        if row["kind"] == "dossie" and not row["translated"]:
            removed += _admin_delete_entry(row["cacheKey"])
    return removed


@app.middleware("http")
async def restrict_to_tunnel(request: FastApiRequest, call_next: Any) -> Any:
    # BaseHTTPMiddleware does not convert a raised HTTPException into a
    # response the way route handlers do; it crashes the request as an
    # unhandled 500 instead. Every check here must return a Response.
    client_host = request.client.host if request.client else None
    if not _is_trusted_proxy(client_host):
        return JSONResponse(status_code=403, content={"detail": "Tunnel access required"})
    # The panel is behind a session and its own login throttle, and one screen
    # legitimately issues several calls. Counting it against the console's
    # 20-per-minute console budget would lock the operator out of the tool used
    # to fix things.
    exempt = request.url.path == "/health" or request.url.path.startswith("/admin")
    if not exempt:
        identity = _client_identity(request)
        if CLIENT_AUTH_MODE != "off" and not _client_authorized(request):
            _log_unauthorized_client(request, identity)
            if CLIENT_AUTH_MODE == "enforce":
                return JSONResponse(status_code=401, content={"detail": "Official client required"})
        is_avail = request.url.path == "/v1/availability"
        if not _allow_request(identity, is_availability=is_avail):
            return JSONResponse(status_code=429, content={"detail": "Too many requests"})
    return await call_next(request)


# Mounted last so every helper it receives is already defined.
try:
    import admin_panel

    app.include_router(admin_panel.build_router({
        "list_entries": _admin_list_entries,
        "service_status": _admin_service_status,
        "refresh_metadata": _admin_refresh_metadata,
        "refresh_scores": _admin_refresh_scores,
        "delete_entry": _admin_delete_entry,
        "purge_untranslated": _admin_purge_untranslated,
    }))
except Exception as _admin_error:  # keep the public API alive if the panel fails
    print(f"admin panel disabled: {_admin_error}", flush=True)


@app.get("/health")
def health() -> dict[str, object]:
    return {
        "service": APP_NAME,
        "status": "ok",
        "source": "metacritic.com",
        "clientAuth": CLIENT_AUTH_MODE,
        "clientKeysLoaded": len(_client_keys()),
        "cacheTtlDays": CACHE_TTL_SECONDS // (24 * 60 * 60),
        "igdbConfigured": bool(os.environ.get("IGDB_CLIENT_ID") and os.environ.get("IGDB_CLIENT_SECRET")),
        "geminiConfigured": bool(_gemini_api_keys()),
        "translationCacheTtlDays": CACHE_TTL_SECONDS // (24 * 60 * 60),
    }


@app.get("/v1/scores")
def scores(title: str, platform: str = "nintendo-switch") -> dict[str, Any]:
    title = " ".join(title.split())
    if not 2 <= len(title) <= 180:
        raise HTTPException(status_code=422, detail="title must contain 2 to 180 characters")
    if platform not in SUPPORTED_PLATFORMS:
        raise HTTPException(status_code=422, detail="unsupported platform")
    title = _catalogue_title(title)

    cache_key = f"{platform}:{_normalise_title(title)}"
    cached = _load_cached(cache_key)
    if cached is not None and cached[1]:
        result, _ = cached
        return {**result, "cached": True, "stale": False}

    try:
        result = _resolve(title, platform)
        _save_cached(cache_key, result, bool(result["found"]))
        return {**result, "cached": False, "stale": False}
    except RuntimeError as exc:
        if cached is not None:
            result, _ = cached
            return {**result, "cached": True, "stale": True}
        raise HTTPException(status_code=502, detail="Metacritic is temporarily unavailable") from exc


@app.get("/v1/availability")
def availability(title: str, platform: str = "nintendo-switch") -> dict[str, Any]:
    """Fast, cached existence check used by the platform picker.

    Same 30-day cache store as /v1/metadata (score_cache), under its own
    key prefix so it never collides with or invalidates the full dossier
    cache, and shared across every console that asks about the same
    title/platform pair -- the first user to open the picker for a given
    homebrew/port pays the IGDB round trip, everyone else for the next
    30 days reads the cached row.
    """
    title = " ".join(title.split())
    if not 2 <= len(title) <= 180:
        raise HTTPException(status_code=422, detail="title must contain 2 to 180 characters")
    if platform not in SUPPORTED_PLATFORMS:
        raise HTTPException(status_code=422, detail="unsupported platform")
    title = _catalogue_title(title)

    cache_key = f"avail:v1:{platform}:{_normalise_title(title)}"
    cached = _load_cached(cache_key)
    if cached is not None and cached[1]:
        result, _ = cached
        return {**result, "cached": True, "stale": False}

    try:
        result = _igdb_availability(title, platform)
        _save_cached(cache_key, result, bool(result["found"]))
        return {**result, "cached": False, "stale": False}
    except HTTPException:
        raise
    except RuntimeError as exc:
        if cached is not None:
            result, _ = cached
            return {**result, "cached": True, "stale": True}
        raise HTTPException(status_code=502, detail="IGDB is temporarily unavailable") from exc


@app.get("/v1/metadata")
def metadata(title: str, platform: str = "nintendo-switch", language: str = "en-US") -> dict[str, Any]:
    title = " ".join(title.split())
    if not 2 <= len(title) <= 180:
        raise HTTPException(status_code=422, detail="title must contain 2 to 180 characters")
    if platform not in SUPPORTED_PLATFORMS:
        raise HTTPException(status_code=422, detail="unsupported platform")
    if not re.fullmatch(r"[A-Za-z]{2,3}(?:-[A-Za-z]{2,4})?", language):
        raise HTTPException(status_code=422, detail="invalid language")
    title = _catalogue_title(title)

    # Version the normalized contract so cached entries created before
    # storyline, themes and game modes existed are never served as complete
    # dossiers for another 30 days.
    cache_key = f"igdb:v4:{platform}:{_language_code(language)}:{_normalise_title(title)}"
    cached = _load_cached(cache_key)
    if cached is not None and cached[1]:
        result, _ = cached
        return {**_localize_cached_metadata(result, language), "cached": True, "stale": False}

    try:
        result = _igdb_metadata(title, platform, language)
        _save_cached(cache_key, result, bool(result["found"]),
                     ttl_override=UNTRANSLATED_CACHE_SECONDS
                     if _translation_incomplete(result, language) else None)
        return {**result, "cached": False, "stale": False}
    except HTTPException:
        raise
    except RuntimeError as exc:
        if cached is not None:
            result, _ = cached
            return {**_localize_cached_metadata(result, language), "cached": True, "stale": True}
        raise HTTPException(status_code=502, detail="IGDB is temporarily unavailable") from exc
