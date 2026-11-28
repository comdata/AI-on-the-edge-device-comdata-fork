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

# The releases page redirects to the newest tag and is not rate limited like the API
RELEASE_LATEST_URL = (
    "https://github.com/comdata/AI-on-the-edge-device-comdata-fork/releases/latest"
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
        installed = self.installed_version
        # Like the device's auto updater: a branch build cannot be compared with a
        # release tag, so no update is offered (a release may even be older).
        if installed and not (installed.startswith("v") and installed[1:2].isdigit()):
            return installed
        return self._latest_version

    @property
    def available(self) -> bool:
        return self.coordinator.last_update_success

    async def async_added_to_hass(self) -> None:
        """Fetch the latest release once on setup instead of waiting for the first poll."""
        await super().async_added_to_hass()
        await self.async_update()
        self.async_write_ha_state()

    async def async_update(self) -> None:
        now = datetime.now()
        if self._release_checked and now - self._release_checked < RELEASE_CACHE:
            return
        session = async_get_clientsession(self.hass)
        try:
            async with session.get(
                RELEASE_LATEST_URL,
                allow_redirects=False,
                timeout=aiohttp.ClientTimeout(total=15),
            ) as response:
                location = response.headers.get("Location", "")
        except (aiohttp.ClientError, asyncio.TimeoutError) as err:
            _LOGGER.debug("Cannot read the latest release: %s", err)
            return
        tag = location.rstrip("/").rsplit("/", 1)[-1] if "/tag/" in location else None
        if tag:
            self._latest_version = tag
            self._release_checked = now
            _LOGGER.debug("Latest release: %s", tag)

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
