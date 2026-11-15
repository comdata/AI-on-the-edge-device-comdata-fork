"""Config flow for AI on the Edge."""

from __future__ import annotations

import asyncio
import logging
from typing import Any

import aiohttp
import voluptuous as vol

from homeassistant.config_entries import ConfigFlow, ConfigFlowResult
from homeassistant.const import CONF_PASSWORD, CONF_PORT, CONF_USERNAME
from homeassistant.helpers.aiohttp_client import async_get_clientsession

from .const import CONF_SCAN_INTERVAL, DEFAULT_PORT, DEFAULT_SCAN_INTERVAL, DOMAIN

_LOGGER = logging.getLogger(__name__)

STEP_USER_DATA_SCHEMA = vol.Schema(
    {
        vol.Required("host"): str,
        vol.Optional(CONF_PORT, default=DEFAULT_PORT): int,
        vol.Required(CONF_USERNAME, default="aiote"): str,
        vol.Required(CONF_PASSWORD): str,
        vol.Optional(CONF_SCAN_INTERVAL, default=DEFAULT_SCAN_INTERVAL): int,
    }
)


class AiotConfigFlow(ConfigFlow, domain=DOMAIN):
    """Handle the UI setup of one AI on the Edge device."""

    VERSION = 1

    async def async_step_user(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        errors: dict[str, str] = {}

        if user_input is not None:
            host = user_input["host"]
            port = user_input[CONF_PORT]
            auth = aiohttp.BasicAuth(user_input[CONF_USERNAME], user_input[CONF_PASSWORD])
            session = async_get_clientsession(self.hass)

            try:
                async with session.get(
                    f"http://{host}:{port}/info",
                    auth=auth,
                    timeout=aiohttp.ClientTimeout(total=20),
                ) as response:
                    response.raise_for_status()
                    info = await response.json(content_type=None)
            except aiohttp.ClientResponseError as err:
                errors["base"] = "auth" if err.status == 401 else "cannot_connect"
            except (aiohttp.ClientError, asyncio.TimeoutError, ValueError):
                errors["base"] = "cannot_connect"
            else:
                await self.async_set_unique_id(info.get("mac_address", host))
                self._abort_if_unique_id_configured()
                return self.async_create_entry(
                    title=f"AI on the Edge ({info.get('hostname', host)})",
                    data=user_input,
                )

        return self.async_show_form(step_id="user", data_schema=STEP_USER_DATA_SCHEMA, errors=errors)
