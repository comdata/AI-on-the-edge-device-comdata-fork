#ifndef GITHUB_OTA_H
#define GITHUB_OTA_H

#include <string>

#include <esp_err.h>

// Updates are taken from the assets of the latest GitHub release of this
// repository. Override per request with ?repo=owner/name.
#define GITHUB_OTA_DEFAULT_REPO "comdata/AI-on-the-edge-device-comdata-fork"

// Board filter used to pick the right asset out of a release
#if defined(BOARD_AITHINKER_ESP32CAM)
    #define GITHUB_OTA_DEFAULT_ASSET_FILTER "esp32cam"
#elif defined(BOARD_XIAO_ESP32S3)
    #define GITHUB_OTA_DEFAULT_ASSET_FILTER "xiao-esp32s3-sense"
#elif defined(BOARD_FREENOVE_ESP32S3_N16R8)
    #define GITHUB_OTA_DEFAULT_ASSET_FILTER "freenove-esp32s3-n16r8"
#elif defined(BOARD_FREENOVE_ESP32S3_N8R8)
    #define GITHUB_OTA_DEFAULT_ASSET_FILTER "freenove-esp32s3-n8r8"
#else
    #define GITHUB_OTA_DEFAULT_ASSET_FILTER "esp32cam"
#endif

#define GITHUB_OTA_ZIP_PATH "/sdcard/firmware/github_ota.zip"
#define GITHUB_OTA_INSTALLED_TAG_FILE "/sdcard/config/ota_installed.txt"

struct GitHubOtaInfo {
    std::string repo;          // Repository that was queried
    std::string currentVersion;// Running firmware (git tag/branch + revision)
    std::string latestTag;     // tag_name of the latest release
    std::string releaseName;   // name of the latest release
    std::string notes;         // short excerpt of the release body
    std::string assetName;     // asset that matches the board
    std::string assetUrl;      // download URL of that asset
    std::string installedTag;  // release tag installed through this feature
    bool updateAvailable = false;
    std::string error;         // empty when the check succeeded
};

// Query the GitHub API for the latest release.
bool githubOtaCheck(GitHubOtaInfo &info, const std::string &repo = "");

// Download the asset of a checked release to GITHUB_OTA_ZIP_PATH (streamed to SD card).
esp_err_t githubOtaDownload(const GitHubOtaInfo &info);

// Remember the tag that is about to be installed (read back after the update).
void githubOtaRememberPendingTag(const std::string &tag);

// Resolve "<repo>@<tag>" that was pending for the running firmware, empty if none.
std::string githubOtaConsumePendingTag();

#endif // GITHUB_OTA_H
