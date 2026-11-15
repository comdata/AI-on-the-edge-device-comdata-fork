"""Data update coordinator for an AI on the Edge device (REST polling)."""

from __future__ import annotations

import asyncio
from datetime import timedelta
import json as json_lib
import logging
from typing import Any

import aiohttp

from homeassistant.config_entries import ConfigEntry
from homeassistant.const import CONF_HOST, CONF_PASSWORD, CONF_PORT, CONF_USERNAME
from homeassistant.core import HomeAssistant
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


def base_url(host: str, port: int, use_ssl: bool) -> str:
    """Return the device's base URL."""
    return f"{'https' if use_ssl else 'http'}://{host}:{port}"


def _request_kwargs(verify_ssl: bool) -> dict[str, Any]:
    """aiohttp keyword arguments for the configured TLS setting."""
    return {} if verify_ssl else {"ssl": False}


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
    session = async_get_clientsession(hass)
    async with session.get(
        f"{base_url(host, port, use_ssl)}/info",
        auth=aiohttp.BasicAuth(username, password),
        timeout=aiohttp.ClientTimeout(total=20),
        **_request_kwargs(verify_ssl),
    ) as response:
        response.raise_for_status()
        return json_lib.loads(await response.text())


class AiotCoordinator(DataUpdateCoordinator[dict]):
    """Poll /process_data and /info every scan interval."""

    def __init__(self, hass: HomeAssistant, entry: ConfigEntry) -> None:
        conf = {**entry.data, **entry.options}
        self.host: str = conf[CONF_HOST]
        self.port: int = conf.get(CONF_PORT, DEFAULT_PORT)
        self.use_ssl: bool = conf.get(CONF_USE_SSL, False)
        self.verify_ssl: bool = conf.get(CONF_VERIFY_SSL, True)
        self.camera_image: str = conf.get(CONF_CAMERA_IMAGE, DEFAULT_CAMERA_IMAGE)
        self.auth = aiohttp.BasicAuth(conf[CONF_USERNAME], conf[CONF_PASSWORD])
        self._session = async_get_clientsession(hass)
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
        async with self._session.request(
            method,
            f"{self.base_url}{path}",
            auth=self.auth,
            timeout=aiohttp.ClientTimeout(total=30),
            **_request_kwargs(self.verify_ssl),
            **kwargs,
        ) as response:
            response.raise_for_status()
            text = await response.text()
            try:
                return json_lib.loads(text)
            except ValueError:
                return text

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
