"""Diagnostics for one AI on the Edge device."""

from __future__ import annotations

from homeassistant.config_entries import ConfigEntry
from homeassistant.const import CONF_PASSWORD
from homeassistant.core import HomeAssistant
from homeassistant.helpers.redact import async_redact_data

from .const import DOMAIN
from .coordinator import AiotCoordinator

TO_REDACT = {CONF_PASSWORD}


async def async_get_config_entry_diagnostics(
    hass: HomeAssistant, entry: ConfigEntry
) -> dict:
    """Return the polled device data without the WebUI password."""
    coordinator: AiotCoordinator = hass.data[DOMAIN][entry.entry_id]
    return {
        "entry": async_redact_data({**entry.data, **entry.options}, TO_REDACT),
        "data": coordinator.data,
    }
