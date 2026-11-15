"""Problem binary sensor of one AI on the Edge device."""

from __future__ import annotations

from homeassistant.components.binary_sensor import (
    BinarySensorDeviceClass,
    BinarySensorEntity,
)
from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.entity_platform import AddEntitiesCallback
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from .const import DOMAIN, VALUE_STATUS_VALID
from .coordinator import AiotCoordinator
from .sensor import device_info_for


async def async_setup_entry(hass, entry, async_add_entities: AddEntitiesCallback) -> None:
    coordinator: AiotCoordinator = hass.data[DOMAIN][entry.entry_id]
    async_add_entities([AiotProblemSensor(coordinator, entry)])


class AiotProblemSensor(CoordinatorEntity[AiotCoordinator], BinarySensorEntity):
    """On when the device reports a process or value status error."""

    _attr_has_entity_name = True
    _attr_translation_key = "problem"
    _attr_device_class = BinarySensorDeviceClass.PROBLEM

    def __init__(self, coordinator: AiotCoordinator, entry) -> None:
        super().__init__(coordinator)
        self._attr_unique_id = f"{entry.unique_id}_problem"

    @property
    def device_info(self) -> DeviceInfo:
        return device_info_for(self.coordinator)

    @property
    def is_on(self) -> bool:
        process = self.coordinator.data.get("process") or {}
        statuses = {
            sequence: status
            for sequence, status in (process.get("value_status") or {}).items()
            if status and status != VALUE_STATUS_VALID
        }
        try:
            process_error = int(process.get("process_error") or 0)
        except (TypeError, ValueError):
            process_error = 0
        self._attr_extra_state_attributes = {
            "value_status": statuses,
            "process_error": process_error,
            "process_state": process.get("process_state"),
        }
        return bool(statuses or process_error)
