"""Entities for one AI on the Edge device."""

from __future__ import annotations

from dataclasses import dataclass

from homeassistant.components.sensor import (
    SensorDeviceClass,
    SensorEntity,
    SensorEntityDescription,
    SensorStateClass,
)
from homeassistant.const import EntityCategory, UnitOfTemperature, UnitOfTime
from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.entity_platform import AddEntitiesCallback
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from .const import DOMAIN
from .coordinator import AiotCoordinator

def _as_float(value):
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


@dataclass(frozen=True, kw_only=True)
class AiotSensorDescription(SensorEntityDescription):
    """One sensor of the device, fed either by /process_data or /info."""

    flow_key: str | None = None
    info_key: str | None = None
    as_float: bool = False


SENSORS: tuple[AiotSensorDescription, ...] = (
    AiotSensorDescription(
        key="value",
        translation_key="value",
        device_class=SensorDeviceClass.WATER,
        state_class=SensorStateClass.TOTAL_INCREASING,
        native_unit_of_measurement="m³",
        icon="mdi:water",
        suggested_display_precision=3,
        flow_key="actual_value",
        as_float=True,
    ),
    AiotSensorDescription(
        key="raw_value",
        translation_key="raw_value",
        device_class=SensorDeviceClass.WATER,
        state_class=SensorStateClass.TOTAL_INCREASING,
        native_unit_of_measurement="m³",
        icon="mdi:water-outline",
        suggested_display_precision=3,
        flow_key="raw_value",
        as_float=True,
    ),
    AiotSensorDescription(
        key="rate",
        translation_key="rate",
        device_class=SensorDeviceClass.VOLUME_FLOW_RATE,
        state_class=SensorStateClass.MEASUREMENT,
        native_unit_of_measurement="m³/h",
        icon="mdi:water-pump",
        suggested_display_precision=3,
        flow_key="rate_per_minute",
        as_float=True,
    ),
    AiotSensorDescription(
        key="value_status",
        translation_key="value_status",
        icon="mdi:check-decagram-outline",
        entity_category=EntityCategory.DIAGNOSTIC,
        flow_key="value_status",
    ),
    AiotSensorDescription(
        key="firmware",
        translation_key="firmware",
        icon="mdi:chip",
        entity_category=EntityCategory.DIAGNOSTIC,
        info_key="firmware_version",
    ),
    AiotSensorDescription(
        key="uptime",
        translation_key="uptime",
        device_class=SensorDeviceClass.DURATION,
        native_unit_of_measurement=UnitOfTime.SECONDS,
        icon="mdi:clock-outline",
        entity_category=EntityCategory.DIAGNOSTIC,
        info_key="device_uptime",
        as_float=True,
    ),
    AiotSensorDescription(
        key="wifi_signal",
        translation_key="wifi_signal",
        device_class=SensorDeviceClass.SIGNAL_STRENGTH,
        native_unit_of_measurement="dBm",
        state_class=SensorStateClass.MEASUREMENT,
        icon="mdi:wifi",
        entity_category=EntityCategory.DIAGNOSTIC,
        info_key="wlan_rssi",
        as_float=True,
    ),
    AiotSensorDescription(
        key="cpu_temperature",
        translation_key="cpu_temperature",
        device_class=SensorDeviceClass.TEMPERATURE,
        native_unit_of_measurement=UnitOfTemperature.CELSIUS,
        state_class=SensorStateClass.MEASUREMENT,
        icon="mdi:thermometer",
        entity_category=EntityCategory.DIAGNOSTIC,
        info_key="chip_temp",
        as_float=True,
    ),
    AiotSensorDescription(
        key="process_status",
        translation_key="process_status",
        icon="mdi:cog-sync-outline",
        entity_category=EntityCategory.DIAGNOSTIC,
        info_key="process_status",
    ),
    AiotSensorDescription(
        key="cycle_counter",
        translation_key="cycle_counter",
        state_class=SensorStateClass.TOTAL_INCREASING,
        icon="mdi:counter",
        entity_category=EntityCategory.DIAGNOSTIC,
        info_key="cycle_counter",
        as_float=True,
    ),
)


def device_info_for(coordinator: AiotCoordinator) -> DeviceInfo:
    """One HA device for the meter, shared by all its entities."""
    info = coordinator.data.get("info", {}) or {}
    return DeviceInfo(
        identifiers={(DOMAIN, coordinator.host)},
        name="Wasserzähler",
        manufacturer="AI-on-the-edge [comdataFork]",
        model=info.get("board_type"),
        hw_version=info.get("chip_model"),
        sw_version=info.get("firmware_version"),
        serial_number=info.get("mac_address"),
        configuration_url=coordinator.base_url,
    )


async def async_setup_entry(hass, entry, async_add_entities: AddEntitiesCallback) -> None:
    coordinator: AiotCoordinator = hass.data[DOMAIN][entry.entry_id]
    async_add_entities(AiotSensor(coordinator, entry, description) for description in SENSORS)


class AiotSensor(CoordinatorEntity[AiotCoordinator], SensorEntity):
    """A sensor of the AI on the Edge device."""

    entity_description: AiotSensorDescription
    _attr_has_entity_name = True

    def __init__(self, coordinator: AiotCoordinator, entry, description: AiotSensorDescription) -> None:
        super().__init__(coordinator)
        self.entity_description = description
        self._attr_unique_id = f"{entry.unique_id}_{description.key}"

    @property
    def device_info(self) -> DeviceInfo:
        return device_info_for(self.coordinator)

    @property
    def native_value(self):
        description = self.entity_description
        if description.flow_key:
            value = self.coordinator.sequence_value(description.flow_key)
        else:
            value = (self.coordinator.data.get("info", {}) or {}).get(description.info_key)
        return _as_float(value) if description.as_float else value
