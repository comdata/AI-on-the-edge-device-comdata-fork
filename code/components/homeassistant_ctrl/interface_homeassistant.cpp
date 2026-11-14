#include "interface_homeassistant.h"
#include "../../include/defines.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include <cJSON.h>
#include <esp_http_client.h>
#include <esp_log.h>

#include "ClassLogFile.h"
#include "helper.h"


static const char *TAG = "HA_API";

static HomeAssistantConfig haConfig;
static std::string haApiUrl = ""; // Base URL without trailing slash

bool homeassistantLoadConfig(HomeAssistantConfig &config)
{
    std::ifstream file(HOMEASSISTANT_CONFIG_FILE);
    if (!file.is_open()) {
        LogFile.writeToFile(ESP_LOG_DEBUG, TAG, "No config file found: " HOMEASSISTANT_CONFIG_FILE);
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    file.close();

    cJSON *json = cJSON_Parse(buffer.str().c_str());
    if (json == NULL) {
        LogFile.writeToFile(ESP_LOG_ERROR, TAG, "Config file is not valid JSON: " HOMEASSISTANT_CONFIG_FILE);
        return false;
    }

    cJSON *item = cJSON_GetObjectItemCaseSensitive(json, "enabled");
    if (cJSON_IsBool(item)) {
        config.enabled = cJSON_IsTrue(item);
    }

    item = cJSON_GetObjectItemCaseSensitive(json, "url");
    if (cJSON_IsString(item)) {
        config.url = item->valuestring;
    }

    item = cJSON_GetObjectItemCaseSensitive(json, "token");
    if (cJSON_IsString(item)) {
        config.token = item->valuestring;
    }

    item = cJSON_GetObjectItemCaseSensitive(json, "entity_prefix");
    if (cJSON_IsString(item) && strlen(item->valuestring) > 0) {
        config.entityPrefix = item->valuestring;
    }

    item = cJSON_GetObjectItemCaseSensitive(json, "publish_rate");
    if (cJSON_IsBool(item)) {
        config.publishRate = cJSON_IsTrue(item);
    }

    cJSON_Delete(json);
    return true;
}


std::string homeassistantSanitizeEntityPart(const std::string &value)
{
    std::string result;
    result.reserve(value.size());

    for (char c : toLower(value)) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') {
            result += c;
        }
        else if (c == ' ' || c == '-' || c == '.') {
            result += '_';
        }
    }

    return result.empty() ? std::string("main") : result;
}


bool homeassistantInit(const HomeAssistantConfig &config)
{
    haConfig = config;

    if (!haConfig.enabled) {
        LogFile.writeToFile(ESP_LOG_DEBUG, TAG, "Publishing to Home Assistant is disabled");
        return false;
    }

    if (haConfig.url.empty() || haConfig.token.empty()) {
        LogFile.writeToFile(ESP_LOG_ERROR, TAG, "Init failed, missing parameter 'url' or 'token'");
        return false;
    }

    haApiUrl = haConfig.url;
    // Remove trailing slashes to build stable endpoint URLs
    while (!haApiUrl.empty() && haApiUrl.back() == '/') {
        haApiUrl.pop_back();
    }
    haConfig.entityPrefix = homeassistantSanitizeEntityPart(haConfig.entityPrefix);

    LogFile.writeToFile(ESP_LOG_INFO, TAG,
                        "Init: URL: " + haApiUrl + ", Entity prefix: " + haConfig.entityPrefix +
                            ", Rate: " + std::to_string(haConfig.publishRate));

    return true;
}


esp_err_t homeassistantPublishState(const std::string &entityId, const std::string &state, cJSON *attributes)
{
    if (haApiUrl.empty() || haConfig.token.empty()) {
        cJSON_Delete(attributes);
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *payload = cJSON_CreateObject();
    if (payload == NULL) {
        cJSON_Delete(attributes);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(payload, "state", state.c_str());
    if (attributes != NULL) {
        cJSON_AddItemToObject(payload, "attributes", attributes);
    }

    char *jsonChar = cJSON_PrintUnformatted(payload);
    cJSON_Delete(payload);
    if (jsonChar == NULL) {
        return ESP_ERR_NO_MEM;
    }

    std::string endpoint = haApiUrl + "/api/states/" + entityId;
    std::string authorization = "Bearer " + haConfig.token;

    esp_http_client_config_t httpConfig = {};
    httpConfig.url = endpoint.c_str();
    httpConfig.method = HTTP_METHOD_POST;
    httpConfig.timeout_ms = 10000;
    httpConfig.buffer_size = 4096;

    esp_http_client_handle_t client = esp_http_client_init(&httpConfig);
    if (client == NULL) {
        cJSON_free(jsonChar);
        LogFile.writeToFile(ESP_LOG_ERROR, TAG, "Failed to initialize HTTP client");
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Authorization", authorization.c_str());
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, jsonChar, strlen(jsonChar));

    esp_err_t result = esp_http_client_perform(client);
    int statusCode = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    cJSON_free(jsonChar);

    if (result != ESP_OK) {
        LogFile.writeToFile(ESP_LOG_ERROR, TAG, "Publish " + entityId + " failed: " + esp_err_to_name(result));
        return result;
    }

    if (statusCode < 200 || statusCode >= 300) {
        LogFile.writeToFile(ESP_LOG_ERROR, TAG,
                            "Publish " + entityId + " failed: HTTP " + std::to_string(statusCode) +
                                " (check URL and token)");
        return ESP_ERR_INVALID_RESPONSE;
    }

    LogFile.writeToFile(ESP_LOG_DEBUG, TAG, "Published " + entityId + " = " + state);
    return ESP_OK;
}
