"""Buttons of one AI on the Edge device."""

from __future__ import annotations

from dataclasses import dataclass

from homeassistant.components.button import ButtonEntity, ButtonEntityDescription
from homeassistant.const import EntityCategory
from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.entity_platform import AddEntitiesCallback
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from .const import DOMAIN
from .coordinator import AiotCoordinator
from .sensor import device_info_for


@dataclass(frozen=True, kw_only=True)
class AiotButtonDescription(ButtonEntityDescription):
    """One device action reachable over the REST API."""

    path: str
    enabled_by_default: bool = True


BUTTONS: tuple[AiotButtonDescription, ...] = (
    AiotButtonDescription(
        key="start_cycle",
        translation_key="start_cycle",
        icon="mdi:camera-iris",
        path="/cycle_start",
    ),
    AiotButtonDescription(
        key="reboot",
        translation_key="reboot",
        icon="mdi:restart",
        entity_category=EntityCategory.CONFIG,
        path="/reboot",
        # A restart interrupts the measurement for a minute; keep it opt-in.
        enabled_by_default=False,
    ),
)


async def async_setup_entry(hass, entry, async_add_entities: AddEntitiesCallback) -> None:
    coordinator: AiotCoordinator = hass.data[DOMAIN][entry.entry_id]
    async_add_entities(
        AiotButton(coordinator, entry, description) for description in BUTTONS
    )


class AiotButton(CoordinatorEntity[AiotCoordinator], ButtonEntity):
    """A REST action of the device as a button."""

    entity_description: AiotButtonDescription
    _attr_has_entity_name = True

    def __init__(self, coordinator: AiotCoordinator, entry, description: AiotButtonDescription) -> None:
        super().__init__(coordinator)
        self.entity_description = description
        self._attr_unique_id = f"{entry.unique_id}_{description.key}"
        self._attr_entity_registry_enabled_default = description.enabled_by_default

    @property
    def device_info(self) -> DeviceInfo:
        return device_info_for(self.coordinator)

    async def async_press(self) -> None:
        await self.coordinator.async_request("GET", self.entity_description.path)
