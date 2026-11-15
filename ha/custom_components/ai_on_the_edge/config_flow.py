"""Config flow for AI on the Edge."""

from __future__ import annotations

import asyncio
import logging
from typing import Any

import aiohttp
import voluptuous as vol

from homeassistant.config_entries import ConfigEntry, ConfigFlow, ConfigFlowResult, OptionsFlow
from homeassistant.const import CONF_HOST, CONF_PASSWORD, CONF_PORT, CONF_USERNAME
from homeassistant.helpers.selector import (
    TextSelector,
    TextSelectorConfig,
    TextSelectorType,
)

from .const import (
    CAMERA_IMAGES,
    CONF_CAMERA_IMAGE,
    CONF_SCAN_INTERVAL,
    CONF_USE_SSL,
    CONF_VERIFY_SSL,
    DEFAULT_CAMERA_IMAGE,
    DEFAULT_PORT,
    DEFAULT_SCAN_INTERVAL,
    DOMAIN,
)
from .coordinator import async_probe_device

_LOGGER = logging.getLogger(__name__)

STEP_USER_DATA_SCHEMA = vol.Schema(
    {
        vol.Required(CONF_HOST): str,
        vol.Optional(CONF_PORT, default=DEFAULT_PORT): int,
        vol.Required(CONF_USERNAME, default="aiote"): str,
        vol.Required(CONF_PASSWORD): str,
        vol.Optional(CONF_SCAN_INTERVAL, default=DEFAULT_SCAN_INTERVAL): vol.All(
            int, vol.Range(min=10, max=3600)
        ),
    }
)


class AiotConfigFlow(ConfigFlow, domain=DOMAIN):
    """Handle the UI setup of one AI on the Edge device."""

    VERSION = 1

    async def _async_validate(
        self, data: dict[str, Any], use_ssl: bool, verify_ssl: bool
    ) -> tuple[dict[str, Any] | None, dict[str, str]]:
        """Probe the device; returns (info, errors)."""
        try:
            info = await async_probe_device(
                self.hass,
                data[CONF_HOST],
                data[CONF_PORT],
                data[CONF_USERNAME],
                data[CONF_PASSWORD],
                use_ssl,
                verify_ssl,
            )
        except aiohttp.ClientResponseError as err:
            return None, {"base": "auth" if err.status == 401 else "cannot_connect"}
        except (aiohttp.ClientError, asyncio.TimeoutError, ValueError):
            return None, {"base": "cannot_connect"}
        return info, {}

    async def async_step_user(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        errors: dict[str, str] = {}

        if user_input is not None:
            info, errors = await self._async_validate(user_input, False, True)
            if info is not None:
                await self.async_set_unique_id(info.get("mac_address", user_input[CONF_HOST]))
                self._abort_if_unique_id_configured(updates={CONF_HOST: user_input[CONF_HOST]})
                return self.async_create_entry(
                    title=f"AI on the Edge ({info.get('hostname', user_input[CONF_HOST])})",
                    data=user_input,
                )

        return self.async_show_form(
            step_id="user", data_schema=STEP_USER_DATA_SCHEMA, errors=errors
        )

    async def async_step_reauth(self, entry_data: dict[str, Any]) -> ConfigFlowResult:
        self._reauth_entry = self.hass.config_entries.async_get_entry(self.context["entry_id"])
        return await self.async_step_reauth_confirm()

    async def async_step_reauth_confirm(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        entry = self._reauth_entry
        conf = {**entry.data, **entry.options}
        errors: dict[str, str] = {}

        if user_input is not None:
            data = {**conf, **user_input}
            info, errors = await self._async_validate(
                data, conf.get(CONF_USE_SSL, False), conf.get(CONF_VERIFY_SSL, True)
            )
            if info is not None:
                self.hass.config_entries.async_update_entry(
                    entry,
                    data={**entry.data, **user_input},
                    options={**entry.options, **user_input},
                )
                await self.hass.config_entries.async_reload(entry.entry_id)
                return self.async_abort(reason="reauth_successful")

        return self.async_show_form(
            step_id="reauth_confirm",
            data_schema=vol.Schema(
                {
                    vol.Required(CONF_USERNAME, default=conf[CONF_USERNAME]): str,
                    vol.Required(CONF_PASSWORD): str,
                }
            ),
            description_placeholders={"host": conf[CONF_HOST]},
            errors=errors,
        )

    async def async_step_reconfigure(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        entry = self.hass.config_entries.async_get_entry(self.context["entry_id"])
        conf = {**entry.data, **entry.options}
        errors: dict[str, str] = {}

        if user_input is not None:
            if not user_input.get(CONF_PASSWORD):
                user_input[CONF_PASSWORD] = conf[CONF_PASSWORD]
            info, errors = await self._async_validate(
                user_input, conf.get(CONF_USE_SSL, False), conf.get(CONF_VERIFY_SSL, True)
            )
            if info is not None:
                self.hass.config_entries.async_update_entry(
                    entry,
                    data={**entry.data, **user_input},
                    options={**entry.options, **user_input},
                )
                await self.hass.config_entries.async_reload(entry.entry_id)
                return self.async_abort(reason="reconfigure_successful")

        schema = self.add_suggested_values_to_schema(
            STEP_USER_DATA_SCHEMA,
            {**conf, CONF_PASSWORD: ""},
        )
        return self.async_show_form(step_id="reconfigure", data_schema=schema, errors=errors)

    @staticmethod
    def async_get_options_flow(config_entry: ConfigEntry) -> OptionsFlow:
        return AiotOptionsFlow()


class AiotOptionsFlow(OptionsFlow):
    """Change connection, polling and camera settings of a configured device."""

    async def async_step_init(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        if user_input is not None:
            if not user_input.get(CONF_PASSWORD):
                user_input.pop(CONF_PASSWORD, None)
            return self.async_create_entry(
                title="", data={**self.config_entry.options, **user_input}
            )

        conf = {**self.config_entry.data, **self.config_entry.options}
        schema = vol.Schema(
            {
                vol.Optional(CONF_PORT, default=conf.get(CONF_PORT, DEFAULT_PORT)): int,
                vol.Optional(CONF_USERNAME, default=conf[CONF_USERNAME]): str,
                vol.Optional(CONF_PASSWORD, default=""): TextSelector(
                    TextSelectorConfig(type=TextSelectorType.PASSWORD)
                ),
                vol.Optional(
                    CONF_SCAN_INTERVAL, default=conf.get(CONF_SCAN_INTERVAL, DEFAULT_SCAN_INTERVAL)
                ): vol.All(int, vol.Range(min=10, max=3600)),
                vol.Optional(
                    CONF_CAMERA_IMAGE, default=conf.get(CONF_CAMERA_IMAGE, DEFAULT_CAMERA_IMAGE)
                ): vol.In(CAMERA_IMAGES),
                vol.Optional(CONF_USE_SSL, default=conf.get(CONF_USE_SSL, False)): bool,
                vol.Optional(CONF_VERIFY_SSL, default=conf.get(CONF_VERIFY_SSL, True)): bool,
            }
        )
        return self.async_show_form(step_id="init", data_schema=schema)
