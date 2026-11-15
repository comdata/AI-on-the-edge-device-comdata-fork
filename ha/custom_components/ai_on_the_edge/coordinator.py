"""Data update coordinator for an AI on the Edge device (REST polling)."""

from __future__ import annotations

import asyncio
from datetime import timedelta
import logging

import aiohttp

from homeassistant.config_entries import ConfigEntry
from homeassistant.const import CONF_HOST, CONF_PASSWORD, CONF_PORT, CONF_USERNAME
from homeassistant.core import HomeAssistant
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.update_coordinator import DataUpdateCoordinator, UpdateFailed

from .const import CONF_SCAN_INTERVAL, DEFAULT_SCAN_INTERVAL, DOMAIN

_LOGGER = logging.getLogger(__name__)


class AiotCoordinator(DataUpdateCoordinator[dict]):
    """Poll /process_data and /info every scan interval."""

    def __init__(self, hass: HomeAssistant, entry: ConfigEntry) -> None:
        self.host: str = entry.data[CONF_HOST]
        self.port: int = entry.data[CONF_PORT]
        self.auth = aiohttp.BasicAuth(entry.data[CONF_USERNAME], entry.data[CONF_PASSWORD])
        self._session = async_get_clientsession(hass)
        interval = entry.data.get(CONF_SCAN_INTERVAL, DEFAULT_SCAN_INTERVAL)
        super().__init__(
            hass,
            _LOGGER,
            name=f"{DOMAIN} {self.host}",
            update_interval=timedelta(seconds=interval),
        )

    @property
    def base_url(self) -> str:
        return f"http://{self.host}:{self.port}"

    async def _async_get(self, path: str) -> dict:
        async with self._session.get(
            f"{self.base_url}{path}",
            auth=self.auth,
            timeout=aiohttp.ClientTimeout(total=30),
        ) as response:
            response.raise_for_status()
            return await response.json(content_type=None)

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
        sequence = next(iter(sequences))
        return {"sequence": sequence, "process": process, "info": info}

    def sequence_value(self, key: str) -> str | None:
        """Return one per-sequence value of the polled data."""
        data = self.data or {}
        return (data.get("process", {}).get(key) or {}).get(data.get("sequence"))
