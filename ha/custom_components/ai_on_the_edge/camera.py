"""Camera of the AI on the Edge device."""

from __future__ import annotations

import aiohttp

from homeassistant.components.camera import Camera
from homeassistant.core import HomeAssistant
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.entity_platform import AddEntitiesCallback
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from .const import CAMERA_IMAGES, DOMAIN
from .coordinator import AiotCoordinator
from .sensor import device_info_for


async def async_setup_entry(hass, entry, async_add_entities: AddEntitiesCallback) -> None:
    coordinator: AiotCoordinator = hass.data[DOMAIN][entry.entry_id]
    async_add_entities([AiotCamera(coordinator, entry)])


class AiotCamera(CoordinatorEntity[AiotCoordinator], Camera):
    """The device's camera image, chosen in the integration options."""

    _attr_has_entity_name = True
    _attr_translation_key = "camera"
    _attr_icon = "mdi:camera"

    def __init__(self, coordinator: AiotCoordinator, entry) -> None:
        CoordinatorEntity.__init__(self, coordinator)
        Camera.__init__(self)
        self._attr_unique_id = f"{entry.unique_id}_camera"

    @property
    def device_info(self):
        return device_info_for(self.coordinator)

    async def async_camera_image(self, width: int | None = None, height: int | None = None) -> bytes | None:
        image = CAMERA_IMAGES.get(self.coordinator.camera_image, CAMERA_IMAGES["alg"])
        session = async_get_clientsession(self.hass)
        async with session.get(
            f"{self.coordinator.base_url}/img_tmp/{image}",
            auth=self.coordinator.auth,
            timeout=aiohttp.ClientTimeout(total=30),
            ssl=False if not self.coordinator.verify_ssl else None,
        ) as response:
            if response.status != 200:
                return None
            return await response.read()
