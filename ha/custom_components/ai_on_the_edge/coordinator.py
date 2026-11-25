"""Data update coordinator for an AI on the Edge device (REST polling)."""

from __future__ import annotations

import asyncio
from datetime import timedelta
import json as json_lib
import logging
from typing import Any

import aiohttp
import yarl

from homeassistant.config_entries import ConfigEntry
from homeassistant.const import CONF_HOST, CONF_PASSWORD, CONF_PORT, CONF_USERNAME
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import ConfigEntryAuthFailed
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.update_coordinator import DataUpdateCoordinator, UpdateFailed

from .const import (
    CONF_CAMERA_IMAGE,
    CONF_SCAN_INTERVAL,
    CONF_USE_SSL,
    CONF_VERIFY_SSL,
    DEFAULT_CAMERA_IMAGE,
    DEFAULT_PORT,
    DEFAULT_SCAN_INTERVAL,
    DOMAIN,
)

_LOGGER = logging.getLogger(__name__)

SESSION_COOKIE = "AIOTE_SESSION"


def base_url(host: str, port: int, use_ssl: bool) -> str:
    """Return the device's base URL."""
    return f"{'https' if use_ssl else 'http'}://{host}:{port}"


def _request_kwargs(verify_ssl: bool) -> dict[str, Any]:
    """aiohttp keyword arguments for the configured TLS setting."""
    return {} if verify_ssl else {"ssl": False}


class DeviceAuth:
    """Credential state of one device: form login cookie or Basic auth fallback."""

    def __init__(
        self,
        session: aiohttp.ClientSession,
        base_url: str,
        username: str,
        password: str,
        verify_ssl: bool,
    ) -> None:
        self._session = session
        self._base_url = base_url
        self._username = username
        self._password = password
        self._basic = aiohttp.BasicAuth(username, password)
        self._verify_ssl = verify_ssl
        self._token: str | None = None
        self._form_login: bool | None = None  # None = not probed yet
        self._lock = asyncio.Lock()

    async def _async_login(self) -> None:
        """Log in through the WebUI form; falls back to Basic on old firmware."""
        async with self._session.post(
            f"{self._base_url}/login",
            data={"username": self._username, "password": self._password, "next": "/"},
            allow_redirects=False,
            timeout=aiohttp.ClientTimeout(total=20),
            **_request_kwargs(self._verify_ssl),
        ) as response:
            if response.status == 404:
                # Firmware before the form login: Basic auth only.
                self._form_login = False
                self._token = None
                return
            location = response.headers.get("Location", "")
            if response.status >= 400 or "err=1" in location:
                raise ConfigEntryAuthFailed("Device rejected the credentials")
            self._form_login = True
            cookie = response.cookies.get(SESSION_COOKIE)
            self._token = cookie.value if cookie else None
            if self._token:
                # The shared session (camera image) carries the cookie too.
                self._session.cookie_jar.update_cookies(
                    {SESSION_COOKIE: self._token}, yarl.URL(self._base_url)
                )

    async def async_request(self, method: str, path: str, **kwargs: Any) -> Any:
        """Send one request to the device, logging in again once on a 401."""
        base_headers = dict(kwargs.pop("headers", None) or {})
        retried = False
        while True:
            async with self._lock:
                if self._form_login is None or retried:
                    await self._async_login()
            headers = dict(base_headers)
            if self._token:
                headers["Cookie"] = f"{SESSION_COOKIE}={self._token}"
            async with self._session.request(
                method,
                f"{self._base_url}{path}",
                auth=None if self._token else self._basic,
                headers=headers or None,
                timeout=aiohttp.ClientTimeout(total=30),
                **_request_kwargs(self._verify_ssl),
                **kwargs,
            ) as response:
                if response.status == 401 and self._form_login:
                    if retried:
                        raise ConfigEntryAuthFailed("Device rejected the session")
                    retried = True
                    continue
                response.raise_for_status()
                text = await response.text()
                try:
                    return json_lib.loads(text)
                except ValueError:
                    return text


    async def async_get_bytes(self, path: str) -> bytes | None:
        """Fetch a binary body (e.g. a camera image); None for a non-200 answer."""
        retried = False
        while True:
            async with self._lock:
                if self._form_login is None or retried:
                    await self._async_login()
            headers = {}
            if self._token:
                headers["Cookie"] = f"{SESSION_COOKIE}={self._token}"
            async with self._session.get(
                f"{self._base_url}{path}",
                auth=None if self._token else self._basic,
                headers=headers or None,
                timeout=aiohttp.ClientTimeout(total=30),
                **_request_kwargs(self._verify_ssl),
            ) as response:
                if response.status == 401 and self._form_login and not retried:
                    retried = True
                    continue
                if response.status != 200:
                    return None
                return await response.read()


async def async_probe_device(
    hass: HomeAssistant,
    host: str,
    port: int,
    username: str,
    password: str,
    use_ssl: bool = False,
    verify_ssl: bool = True,
) -> dict[str, Any]:
    """Fetch /info to check host and credentials; raises on failure."""
    auth = DeviceAuth(
        async_get_clientsession(hass),
        base_url(host, port, use_ssl),
        username,
        password,
        verify_ssl,
    )
    return await auth.async_request("GET", "/info")


class AiotCoordinator(DataUpdateCoordinator[dict]):
    """Poll /process_data and /info every scan interval."""

    def __init__(self, hass: HomeAssistant, entry: ConfigEntry) -> None:
        conf = {**entry.data, **entry.options}
        self.host: str = conf[CONF_HOST]
        self.port: int = conf.get(CONF_PORT, DEFAULT_PORT)
        self.use_ssl: bool = conf.get(CONF_USE_SSL, False)
        self.verify_ssl: bool = conf.get(CONF_VERIFY_SSL, True)
        self.camera_image: str = conf.get(CONF_CAMERA_IMAGE, DEFAULT_CAMERA_IMAGE)
        self.device_auth = DeviceAuth(
            async_get_clientsession(hass),
            base_url(self.host, self.port, self.use_ssl),
            conf[CONF_USERNAME],
            conf[CONF_PASSWORD],
            self.verify_ssl,
        )
        interval = conf.get(CONF_SCAN_INTERVAL, DEFAULT_SCAN_INTERVAL)
        super().__init__(
            hass,
            _LOGGER,
            name=f"{DOMAIN} {self.host}",
            update_interval=timedelta(seconds=interval),
        )

    @property
    def base_url(self) -> str:
        return base_url(self.host, self.port, self.use_ssl)

    async def async_request(self, method: str, path: str, **kwargs: Any) -> Any:
        """Send one request to the device; returns the parsed body."""
        return await self.device_auth.async_request(method, path, **kwargs)

    async def async_get_bytes(self, path: str) -> bytes | None:
        """Fetch a binary body (e.g. a camera image)."""
        return await self.device_auth.async_get_bytes(path)

    async def _async_get(self, path: str) -> dict:
        return await self.async_request("GET", path)

    async def _async_update_data(self) -> dict:
        try:
            process = await self._async_get("/process_data")
            info = await self._async_get("/info")
        except (aiohttp.ClientError, asyncio.TimeoutError, ValueError) as err:
            raise UpdateFailed(f"Cannot reach {self.base_url}: {err}") from err

        # The device reports one entry per configured number sequence.
        sequences = process.get("actual_value") or {}
        if not sequences:
            raise UpdateFailed("Device returned no number sequence")
        return {"sequences": list(sequences), "process": process, "info": info}

    @property
    def sequences(self) -> list[str]:
        """Number sequences the device currently reports."""
        return list((self.data or {}).get("sequences") or [])

    def sequence_value(self, key: str, sequence: str | None = None):
        """Return one per-sequence value of the polled data."""
        data = self.data or {}
        values = data.get("process", {}).get(key) or {}
        if sequence is None:
            sequences = self.sequences
            sequence = sequences[0] if sequences else None
        return values.get(sequence)
