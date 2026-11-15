#ifndef INTERFACE_HOMEASSISTANT_H
#define INTERFACE_HOMEASSISTANT_H

#include <string>

#include <cJSON.h>
#include <esp_err.h>

#define HOMEASSISTANT_CONFIG_FILE "/sdcard/config/homeassistant.json"

// Configuration, read from /config/homeassistant.json:
// {
//   "enabled": true,
//   "url": "http://homeassistant.local:8123",
//   "token": "<long-lived access token>",
//   "entity_prefix": "ai_on_the_edge",
//   "publish_rate": true
// }
struct HomeAssistantConfig {
    bool enabled = false;
    std::string url = "";
    std::string token = "";
    std::string entityPrefix = "ai_on_the_edge";
    bool publishRate = true;
};

// Initialize the Home Assistant API client. Returns false if the configuration
// is incomplete (the module then stays disabled and the flow continues).
bool homeassistantInit(const HomeAssistantConfig &config);

// POST /api/states/<entityId> with the given state and attributes.
// Takes ownership of "attributes" (may be NULL).
esp_err_t homeassistantPublishState(const std::string &entityId, const std::string &state, cJSON *attributes);

// Load /config/homeassistant.json into the given struct. Returns false when the
// file is missing or not valid JSON.
bool homeassistantLoadConfig(HomeAssistantConfig &config);

// Normalize a string to a valid Home Assistant entity id part (lowercase, a-z 0-9 _).
std::string homeassistantSanitizeEntityPart(const std::string &value);

#endif // INTERFACE_HOMEASSISTANT_H
