"""Firmware update entity for one AI on the Edge device."""

from __future__ import annotations

import asyncio
from datetime import datetime, timedelta
import logging

import aiohttp

from homeassistant.components.update import (
    UpdateDeviceClass,
    UpdateEntity,
    UpdateEntityFeature,
)
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.entity_platform import AddEntitiesCallback
from homeassistant.config_entries import ConfigEntry

from .const import DOMAIN
from .coordinator import AiotCoordinator
from .sensor import device_info_for

_LOGGER = logging.getLogger(__name__)

RELEASE_API_URL = (
    "https://api.github.com/repos/comdata/AI-on-the-edge-device-comdata-fork/releases/latest"
)
RELEASE_CACHE = timedelta(hours=6)
INSTALL_TIMEOUT = timedelta(minutes=5)


async def async_setup_entry(
    hass: HomeAssistant,
    entry: ConfigEntry,
    async_add_entities: AddEntitiesCallback,
) -> None:
    coordinator: AiotCoordinator = hass.data[DOMAIN][entry.entry_id]
    async_add_entities([AiotFirmwareUpdate(coordinator)])


class AiotFirmwareUpdate(UpdateEntity):
    """Latest comdata release, installed by the device itself via /ota/pull."""

    _attr_has_entity_name = True
    _attr_translation_key = "firmware"
    _attr_device_class = UpdateDeviceClass.FIRMWARE
    _attr_supported_features = UpdateEntityFeature.INSTALL | UpdateEntityFeature.PROGRESS
    _attr_should_poll = True

    def __init__(self, coordinator: AiotCoordinator) -> None:
        self.coordinator = coordinator
        self._attr_unique_id = f"{coordinator.host}-firmware"
        self._latest_version: str | None = None
        self._release_checked: datetime | None = None

    @property
    def device_info(self):
        return device_info_for(self.coordinator)

    @property
    def installed_version(self) -> str | None:
        info = (self.coordinator.data or {}).get("info") or {}
        return info.get("firmware_version")

    @property
    def latest_version(self) -> str | None:
        return self._latest_version

    @property
    def available(self) -> bool:
        return self.coordinator.last_update_success

    async def async_update(self) -> None:
        now = datetime.now()
        if self._release_checked and now - self._release_checked < RELEASE_CACHE:
            return
        session = async_get_clientsession(self.hass)
        try:
            async with session.get(
                RELEASE_API_URL,
                headers={"Accept": "application/vnd.github+json"},
                timeout=aiohttp.ClientTimeout(total=15),
            ) as response:
                response.raise_for_status()
                release = await response.json()
        except (aiohttp.ClientError, asyncio.TimeoutError, ValueError) as err:
            _LOGGER.debug("Cannot read the latest release: %s", err)
            return
        tag = release.get("tag_name") if isinstance(release, dict) else None
        if isinstance(tag, str) and tag:
            self._latest_version = tag
            self._release_checked = now

    async def async_install(self, version: str | None, backup: bool, **kwargs) -> None:
        """Let the device download and flash the latest release, then reboot."""
        self._attr_in_progress = True
        self._attr_update_percentage = 0
        self.async_write_ha_state()
        deadline = datetime.now() + INSTALL_TIMEOUT

        try:
            await self.coordinator.async_request("POST", "/ota/pull")
            while datetime.now() < deadline:
                await asyncio.sleep(5)
                try:
                    status = await self.coordinator.async_request("GET", "/ota/pull")
                except (aiohttp.ClientError, asyncio.TimeoutError):
                    # The expected case after "done": the device reboots to install.
                    break
                if isinstance(status, str) and status.startswith("failed"):
                    raise HomeAssistantError(f"Device rejected the update: {status}")
                if status == "done":
                    self._attr_update_percentage = 100
                    break
                self._attr_update_percentage = min(90, (self._attr_update_percentage or 0) + 10)
                self.async_write_ha_state()
            else:
                raise HomeAssistantError("Update did not finish in time, see the device log")
        finally:
            self._attr_in_progress = False
            self.async_write_ha_state()
