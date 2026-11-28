"""Set up the AI on the Edge integration."""

from __future__ import annotations

import json as json_lib
from typing import Any

import aiohttp
import voluptuous as vol

from homeassistant.config_entries import ConfigEntry
from homeassistant.const import Platform
from homeassistant.core import HomeAssistant, ServiceCall
from homeassistant.exceptions import HomeAssistantError, ServiceValidationError

from .const import (
    ATTR_CONFIG,
    ATTR_ENTRY_ID,
    ATTR_SEQUENCE,
    ATTR_VALUE,
    DOMAIN,
    SERVICE_SET_CONFIG,
    SERVICE_SET_FALLBACK_VALUE,
)
from .coordinator import AiotCoordinator

PLATFORMS = [
    Platform.BINARY_SENSOR,
    Platform.BUTTON,
    Platform.CAMERA,
    Platform.SENSOR,
    Platform.UPDATE,
]

SET_CONFIG_SCHEMA = vol.Schema(
    {
        vol.Optional(ATTR_ENTRY_ID): str,
        vol.Required(ATTR_CONFIG): vol.Any(dict, str),
    }
)

SET_FALLBACK_VALUE_SCHEMA = vol.Schema(
    {
        vol.Optional(ATTR_ENTRY_ID): str,
        vol.Optional(ATTR_SEQUENCE, default="main"): str,
        vol.Required(ATTR_VALUE): vol.Coerce(float),
    }
)


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    coordinator = AiotCoordinator(hass, entry)
    await coordinator.async_config_entry_first_refresh()
    hass.data.setdefault(DOMAIN, {})[entry.entry_id] = coordinator
    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)
    entry.async_on_unload(entry.add_update_listener(_async_reload_entry))
    _async_register_services(hass)
    return True


async def async_unload_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    unload_ok = await hass.config_entries.async_unload_platforms(entry, PLATFORMS)
    if unload_ok:
        hass.data[DOMAIN].pop(entry.entry_id)
    return unload_ok


async def _async_reload_entry(hass: HomeAssistant, entry: ConfigEntry) -> None:
    """Apply changed options by reloading the entry."""
    await hass.config_entries.async_reload(entry.entry_id)


def _deep_merge(base: dict, patch: dict) -> dict:
    """Merge a partial config into the device's full config."""
    for key, value in patch.items():
        if isinstance(value, dict) and isinstance(base.get(key), dict):
            base[key] = _deep_merge(base[key], value)
        else:
            base[key] = value
    return base


def _coordinator_for_call(hass: HomeAssistant, call: ServiceCall) -> AiotCoordinator:
    """Pick the device a service call targets."""
    coordinators: dict[str, AiotCoordinator] = hass.data.get(DOMAIN) or {}
    entry_id = call.data.get(ATTR_ENTRY_ID)
    if entry_id:
        if entry_id not in coordinators:
            raise ServiceValidationError(f"Unknown config entry {entry_id}")
        return coordinators[entry_id]
    if len(coordinators) == 1:
        return next(iter(coordinators.values()))
    raise ServiceValidationError(
        "Several AI on the Edge devices are set up, pass entry_id to choose one"
    )


def _async_register_services(hass: HomeAssistant) -> None:
    if hass.services.has_service(DOMAIN, SERVICE_SET_CONFIG):
        return

    async def _async_set_config(call: ServiceCall) -> None:
        coordinator = _coordinator_for_call(hass, call)
        patch: Any = call.data[ATTR_CONFIG]
        if isinstance(patch, str):
            try:
                patch = json_lib.loads(patch)
            except ValueError as err:
                raise ServiceValidationError(f"config is not valid JSON: {err}") from err
        try:
            current = await coordinator.async_request("GET", "/config")
            merged = _deep_merge(current, patch)
            # reload=true reinitializes the running process in place, no reboot
            await coordinator.async_request("POST", "/config?reload=true", json=merged)
        except (aiohttp.ClientError, TimeoutError) as err:
            raise HomeAssistantError(f"Cannot write config: {err}") from err

    async def _async_set_fallback_value(call: ServiceCall) -> None:
        coordinator = _coordinator_for_call(hass, call)
        sequence = call.data[ATTR_SEQUENCE]
        value = call.data[ATTR_VALUE]
        try:
            await coordinator.async_request(
                "GET", f"/set_fallbackvalue?sequence={sequence}&value={value}"
            )
        except (aiohttp.ClientError, TimeoutError) as err:
            raise HomeAssistantError(f"Cannot set fallback value: {err}") from err

    hass.services.async_register(
        DOMAIN, SERVICE_SET_CONFIG, _async_set_config, schema=SET_CONFIG_SCHEMA
    )
    hass.services.async_register(
        DOMAIN,
        SERVICE_SET_FALLBACK_VALUE,
        _async_set_fallback_value,
        schema=SET_FALLBACK_VALUE_SCHEMA,
    )
