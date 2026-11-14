#include "github_ota.h"
#include "../../include/defines.h"

#include <cstdio>
#include <cstring>
#include <string>

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>

#include "ClassLogFile.h"
#include "helper.h"
#include "time_sntp.h"

// The firmware version is provided by version.h, which must only be included
// once in the whole firmware (it defines functions). Declare the symbols here.
extern "C" {
extern const char *GIT_TAG;
extern const char *GIT_REV;
extern const char *GIT_BRANCH;
}


static const char *TAG = "GITHUB_OTA";

static const size_t MAX_API_RESPONSE = 64 * 1024;
static const size_t DOWNLOAD_BUFFER = 4096;


struct ApiResponse {
    std::string data;
    bool overflow = false;
};


static esp_err_t apiEventHandler(esp_http_client_event_t *event)
{
    if (event->event_id == HTTP_EVENT_ON_DATA) {
        ApiResponse *response = (ApiResponse *)event->user_data;
        if (response == NULL || event->data == NULL || event->data_len <= 0) {
            return ESP_OK;
        }
        if (response->data.size() + event->data_len > MAX_API_RESPONSE) {
            response->overflow = true;
            return ESP_FAIL;
        }
        response->data.append((const char *)event->data, event->data_len);
    }
    return ESP_OK;
}


struct DownloadContext {
    FILE *file = NULL;
    size_t written = 0;
    bool writeFailed = false;
    size_t expected = 0; // Content-Length of the answer
};


static esp_err_t downloadEventHandler(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || event->user_data == NULL) {
        return ESP_OK;
    }
    DownloadContext *context = (DownloadContext *)event->user_data;
    if (event->data == NULL || event->data_len <= 0 || context->file == NULL) {
        return ESP_OK;
    }
    if (fwrite(event->data, 1, event->data_len, context->file) != (size_t)event->data_len) {
        context->writeFailed = true;
        return ESP_FAIL;
    }
    context->written += event->data_len;
    return ESP_OK;
}


// A release asset must be complete before the device replaces its own files:
// check the size against Content-Length and look for the end-of-central-directory
// record of a ZIP archive.
static bool downloadIsComplete(const DownloadContext &context)
{
    if (context.written < 512 * 1024) {
        return false;
    }
    if (context.expected > 0 && context.written != context.expected) {
        return false;
    }

    FILE *file = fopen(GITHUB_OTA_ZIP_PATH, "rb");
    if (file == NULL) {
        return false;
    }
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    long tail = (size > 66000) ? 66000 : size;
    std::string buffer;
    buffer.resize(tail);
    fseek(file, size - tail, SEEK_SET);
    size_t read = fread(&buffer[0], 1, tail, file);
    fclose(file);

    const std::string eocd("PK\x05\x06", 4);
    return buffer.find(eocd) != std::string::npos;
}


esp_err_t githubOtaDownload(const GitHubOtaInfo &info)
{
    if (info.assetUrl.empty()) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!getTimeIsSet()) {
        LogFile.writeToFile(ESP_LOG_ERROR, TAG, "Cannot download: system time not set");
        return ESP_ERR_INVALID_STATE;
    }

    deleteFile(GITHUB_OTA_ZIP_PATH);

    FILE *file = fopen(GITHUB_OTA_ZIP_PATH, "wb");
    if (file == NULL) {
        LogFile.writeToFile(ESP_LOG_ERROR, TAG, "Cannot write " GITHUB_OTA_ZIP_PATH);
        return ESP_FAIL;
    }

    DownloadContext context = {};
    context.file = file;

    esp_http_client_config_t config = {};
    config.url = info.assetUrl.c_str();
    config.user_agent = "AI-on-the-Edge Device";
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = 30000;
    config.buffer_size = DOWNLOAD_BUFFER;
    // The signed asset URL of a GitHub release is longer than the default TX
    // buffer (1024), which makes the request fail before it is sent.
    config.buffer_size_tx = 4096;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.max_redirection_count = 5;
    // The body is written by the event handler; esp_http_client_perform also
    // follows the redirect to the asset host.
    config.event_handler = downloadEventHandler;
    config.user_data = &context;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(client, "Accept", "application/octet-stream");

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    int64_t contentLength = esp_http_client_get_content_length(client);
    esp_http_client_cleanup(client);
    fclose(file);

    if (err != ESP_OK) {
        LogFile.writeToFile(ESP_LOG_ERROR, TAG, "Download failed: " + std::string(esp_err_to_name(err)) + " (" +
                                                     std::to_string((int)err) + ")");
        deleteFile(GITHUB_OTA_ZIP_PATH);
        return err;
    }
    if (status != 200) {
        LogFile.writeToFile(ESP_LOG_ERROR, TAG, "Download failed with HTTP " + std::to_string(status));
        deleteFile(GITHUB_OTA_ZIP_PATH);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (context.writeFailed) {
        LogFile.writeToFile(ESP_LOG_ERROR, TAG, "Write to SD card failed");
        deleteFile(GITHUB_OTA_ZIP_PATH);
        return ESP_FAIL;
    }

    context.expected = (contentLength > 0) ? (size_t)contentLength : 0;
    if (!downloadIsComplete(context)) {
        LogFile.writeToFile(ESP_LOG_ERROR, TAG,
                            "Download incomplete: " + std::to_string(context.written) + " of " +
                                std::to_string(context.expected) + " bytes, no ZIP end record");
        deleteFile(GITHUB_OTA_ZIP_PATH);
        return ESP_ERR_INVALID_SIZE;
    }

    LogFile.writeToFile(ESP_LOG_INFO, TAG,
                        "Downloaded " + info.assetName + " (" + std::to_string(context.written) + " bytes) to " GITHUB_OTA_ZIP_PATH);
    return ESP_OK;
}
