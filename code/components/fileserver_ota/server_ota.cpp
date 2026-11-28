#include "server_ota.h"
#include "../../include/defines.h"

#include <string>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <algorithm>

#include <freertos/task.h>

#include <sys/stat.h>
#include <esp_task_wdt.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <esp_app_format.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <cJSON.h>
#include "miniz.h"

#include "../main/version.h"

#ifdef ENABLE_MQTT
#include "interface_mqtt.h"
#endif // ENABLE_MQTT

#include "server_help.h"
#include "webserver.h"
#include "MainFlowControl.h"
#include "gpioControl.h"
#include "ClassControlCamera.h"
#include "network_main.h"
#include "ClassLogFile.h"
#include "helper.h"
#include "system.h"
#include "statusled.h"


static const char *TAG = "SERVER_OTA";

constexpr const char *DIR_OTA_STAGED_CONFIG = DIR_OTA_STAGED "/config";
constexpr const char *DIR_OTA_STAGED_HTML = DIR_OTA_STAGED "/html";
constexpr const char *FILE_OTA_STAGED_FW = DIR_OTA_STAGED "/firmware.bin";
constexpr const char *FILE_OTA_STAGED_PACKAGE = DIR_OTA_STAGED "/ota_package";
constexpr const char *FILE_OTA_STAGED_CONFIG_BACKUP_MANIFEST = DIR_OTA_STAGED "/meta.json";


static bool unzipFile(const std::string &inputZipFile, const std::string &destFolder)
{
    mz_zip_archive zipArchive = {};

    if (!mz_zip_reader_init_file(&zipArchive, inputZipFile.c_str(), 0)) {
        LogFile.writeToFile(ESP_LOG_ERROR, TAG, "unzipFile: Failed to init");
        return false;
    }

    const mz_uint numberOfFiles = mz_zip_reader_get_num_files(&zipArchive);

    std::string destFolderValid = destFolder;
    if (!destFolderValid.empty() && destFolderValid.back() != '/') {
        destFolderValid += '/';
    }

    for (mz_uint i = 0; i < numberOfFiles; ++i) {
        mz_zip_archive_file_stat fileStat;

        if (!mz_zip_reader_file_stat(&zipArchive, i, &fileStat)) {
            LogFile.writeToFile(ESP_LOG_ERROR, TAG, "unzipFile: Failed to get file stat | Index: " + std::to_string(i));
            mz_zip_reader_end(&zipArchive);
            return false;
        }

        if (fileStat.m_is_directory) {
            continue;
        }

        const std::string archiveFilename(fileStat.m_filename);

        if (!isSafePath(archiveFilename)) {
            LogFile.writeToFile(ESP_LOG_ERROR, TAG, "unzipFile: Unsafe path: " + archiveFilename);
            mz_zip_reader_end(&zipArchive);
            return false;
        }

        const std::string destinationPath = destFolderValid + archiveFilename;

        // Ignore an archive entry that would overwrite the uploaded ZIP itself
        if (destinationPath == inputZipFile) {
            continue;
        }

        ESP_LOGI(TAG, "Unzip file: %s", destinationPath.c_str());

        makeDir(getDirectory(destinationPath));
        deleteFile(destinationPath);

        if (!mz_zip_reader_extract_to_file(&zipArchive, i, destinationPath.c_str(), 0)) {
            const mz_zip_error zipError = mz_zip_get_last_error(&zipArchive);
            LogFile.writeToFile(ESP_LOG_ERROR, TAG,
                                "unzipFile: Failed to extract: " + archiveFilename + " | Error: " + std::to_string(zipError));
            deleteFile(destinationPath);
            mz_zip_reader_end(&zipArchive);
            return false;
        }
    }

    mz_zip_reader_end(&zipArchive);

    LogFile.writeToFile(ESP_LOG_INFO, TAG, "Unzip successful");

    return true;
}


// Reads the first few bytes to classify the staged OTA package
// Returns "ZIP", "BIN", or "" if unrecognized.
static std::string detectPackageType(const std::string &path)
{
    uint8_t header[4] = {0};
    FILE *file = fopen(path.c_str(), "rb");
    if (!file) {
        return "";
    }
    size_t n = fread(header, 1, sizeof(header), file);
    fclose(file);

    // Inspect header for valid zip file
    if (n >= 4 && header[0] == 'P' && header[1] == 'K' && header[2] == 0x03 && header[3] == 0x04) {
        return "ZIP";
    }
    // Inspect header for valid ESP bin file
    else if (n >= 1 && header[0] == ESP_IMAGE_HEADER_MAGIC) {
        return "BIN";
    }

    return "";
}


// Reads the firmware version string from the staged binary's application descriptor
static std::string readStagedFirmwareVersion(void)
{
    FILE *file = fopen(FILE_OTA_STAGED_FW, "rb");
    if (!file) {
        return "";
    }

    constexpr size_t headerSize = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
    uint8_t header[headerSize] = {0};
    const size_t bytesRead = fread(header, 1, sizeof(header), file);
    fclose(file);

    if (bytesRead < headerSize) {
        return "";
    }

    esp_app_desc_t appInfo;
    memcpy(&appInfo, &header[sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t)], sizeof(appInfo));
    if (appInfo.magic_word != ESP_APP_DESC_MAGIC_WORD) {
        return "";
    }

    return std::string(appInfo.version, strnlen(appInfo.version, sizeof(appInfo.version)));
}


// Ensures the unzipped package is complete and that firmware and WebUI belong to the same build.
// A ZIP that was already flashed but whose WebUI assets were not installed must not be marked valid.
static bool validateStagedPackage(void)
{
    const std::string htmlIndex = std::string(DIR_OTA_STAGED_HTML) + "/index.html";
    const std::string htmlVersionFile = std::string(DIR_OTA_STAGED_HTML) + "/version.txt";
    if (!fileExists(FILE_OTA_STAGED_FW) || !fileExists(htmlIndex) || !fileExists(htmlVersionFile)) {
        LogFile.writeToFile(ESP_LOG_ERROR, TAG, "Staged package incomplete: firmware.bin or WebUI files missing");
        return false;
    }

    const std::string firmwareVersion = readStagedFirmwareVersion();
    FILE *file = fopen(htmlVersionFile.c_str(), "rb");
    if (firmwareVersion.empty() || !file) {
        LogFile.writeToFile(ESP_LOG_ERROR, TAG, "Staged package invalid: firmware or WebUI version unreadable");
        if (file) {
            fclose(file);
        }
        return false;
    }

    char htmlVersion[64] = {0};
    const size_t bytesRead = fread(htmlVersion, 1, sizeof(htmlVersion) - 1, file);
    fclose(file);
    std::string htmlVersionStr(htmlVersion, bytesRead);
    while (!htmlVersionStr.empty() && (htmlVersionStr.back() == '\n' || htmlVersionStr.back() == '\r' || htmlVersionStr.back() == ' ')) {
        htmlVersionStr.pop_back();
    }

    // esp_app_desc_t.version is truncated to 31 characters, the version file carries the full tag
    const size_t compareLength = std::min(firmwareVersion.size(), htmlVersionStr.size());
    if (compareLength == 0 || firmwareVersion.compare(0, compareLength, htmlVersionStr, 0, compareLength) != 0) {
        LogFile.writeToFile(ESP_LOG_ERROR, TAG,
                            "Staged package mismatch: firmware " + firmwareVersion + " vs WebUI " + htmlVersionStr);
        return false;
    }

    return true;
}


static bool updateOtaAssets(void)
{
    bool success = true;

    if (dirExists(DIR_OTA_STAGED_CONFIG)) {
        if (mergeFolder(DIR_OTA_STAGED_CONFIG, DIR_CONFIG_ROOT)) {
            LogFile.writeToFile(ESP_LOG_INFO, TAG, "Staged asset: Config update successful");
        }
        else {
            LogFile.writeToFile(ESP_LOG_ERROR, TAG, "Staged asset: Failed to process config folder");
            success = false;
        }
    }

    if (dirExists(DIR_OTA_STAGED_HTML)) {
        if (replaceFolder(DIR_OTA_STAGED_HTML, DIR_HTML_ROOT)) {
            LogFile.writeToFile(ESP_LOG_INFO, TAG, "Staged asset: WebUI update successful");
        }
        else {
            LogFile.writeToFile(ESP_LOG_WARN, TAG, "Staged asset: Failed to update webUI (HTML folder)");
            success = false;
        }
    }

    return success;
}


#ifdef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
/**
 * @brief System self-diagnostic check after an OTA update
 * @return true if healthy, false if failure detected
 */
static bool firmwareVerification(void)
{
    const esp_reset_reason_t resetReason = esp_reset_reason();
    if (resetReason == ESP_RST_PANIC || resetReason == ESP_RST_INT_WDT || resetReason == ESP_RST_TASK_WDT || resetReason == ESP_RST_WDT) {
        LogFile.writeToFile(ESP_LOG_ERROR, TAG, "Firmware verification failed | Reset reason: " + std::to_string(resetReason));
        return false;
    }

    return true;
}


// 5. Evaluates partition state on boot up and updates assets if valid
static void finalizeOtaUpdate(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) {
        LogFile.writeToFile(ESP_LOG_ERROR, TAG, "Failed to get running partition");
        deleteAllFilesInDirectory(DIR_OTA_STAGED, true);
        return;
    }

    esp_ota_img_states_t otaState;
    const esp_err_t otaStateError = esp_ota_get_state_partition(running, &otaState);
    if (otaStateError != ESP_OK) {
        LogFile.writeToFile(ESP_LOG_ERROR, TAG, "Failed to get OTA state: " + intToHexString(otaStateError));
        deleteAllFilesInDirectory(DIR_OTA_STAGED, true);
        return;
    }

    const char *otaStateStr = "UNKNOWN";
    switch (otaState) {
        case ESP_OTA_IMG_NEW:
            otaStateStr = "NEW";
            break;
        case ESP_OTA_IMG_PENDING_VERIFY:
            otaStateStr = "PENDING_VERIFY";
            break;
        case ESP_OTA_IMG_VALID:
            otaStateStr = "VALID";
            break;
        case ESP_OTA_IMG_INVALID:
            otaStateStr = "INVALID";
            break;
        case ESP_OTA_IMG_ABORTED:
            otaStateStr = "ABORTED";
            break;
        case ESP_OTA_IMG_UNDEFINED:
            otaStateStr = "UNDEFINED";
            break;
        default:
            break;
    }

    LogFile.writeToFile(ESP_LOG_INFO, TAG, "Active Partition: " + std::string(running->label) + " | OTA State: " + otaStateStr);

    // Legacy bootloader: Rollback verification is not supported
    if (otaState == ESP_OTA_IMG_NEW || otaState == ESP_OTA_IMG_UNDEFINED) {
        LogFile.writeToFile(ESP_LOG_INFO, TAG, "Update bootloader manually to support firmware rollback feature");

        // Process webUI update, after unzip done (during previous boot)
        if (!fileExists(FILE_OTA_STAGED_PACKAGE)) {
            updateOtaAssets();
            deleteAllFilesInDirectory(DIR_OTA_STAGED, true);
        }
    }
    // Bootloader with rollback support (firmware version >= v18.x.x-SLFORK)
    else if (otaState == ESP_OTA_IMG_PENDING_VERIFY) {
        LogFile.writeToFile(ESP_LOG_INFO, TAG, "Firmware verification...");

        if (!firmwareVerification()) {
            deleteAllFilesInDirectory(DIR_OTA_STAGED, true);

            LogFile.writeToFile(ESP_LOG_ERROR, TAG, "Firmware verification failed. Trying to rollback...");
            const esp_err_t rollbackError = esp_ota_mark_app_invalid_rollback_and_reboot();
            if (rollbackError != ESP_OK) {
                LogFile.writeToFile(ESP_LOG_ERROR, TAG, "Rollback failed: " + intToHexString(rollbackError) + ". Rebooting...");
                doRebootOTA();
            }
            return;
        }

        const esp_err_t otaMarkError = esp_ota_mark_app_valid_cancel_rollback();
        if (otaMarkError != ESP_OK) {
            LogFile.writeToFile(ESP_LOG_ERROR, TAG, "Failed to mark firmware valid. Trying to rollback: " + intToHexString(otaMarkError));
            const esp_err_t rollbackError = esp_ota_mark_app_invalid_rollback_and_reboot();
            if (rollbackError != ESP_OK) {
                LogFile.writeToFile(ESP_LOG_ERROR, TAG, "Rollback failed: " + intToHexString(rollbackError) + ". Rebooting...");
                doRebootOTA();
            }
            return;
        }

        LogFile.writeToFile(ESP_LOG_INFO, TAG, "Firmware verification successful | OTA State: VALID");

        // Update assets when firmware is validated.
        // Note: Firmware remains valid even assetUpdate is failing. Could be resolved with another OTA update.
        updateOtaAssets();
        deleteAllFilesInDirectory(DIR_OTA_STAGED, true);
    }
}
#endif // CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE


// 4. Flash firmware
static bool otaUpdateFirmware(const std::string &filename)
{
    esp_ota_handle_t otaHandle = 0;
    esp_err_t retVal = ESP_OK;
    bool success = false;

    const esp_partition_t *configured = esp_ota_get_boot_partition();
    const esp_partition_t *running = esp_ota_get_running_partition();

    if (!configured || !running) {
        LogFile.writeToFile(ESP_LOG_ERROR, TAG, "Failed to get OTA partition information");
        return false;
    }

    LogFile.writeToFile(ESP_LOG_INFO, TAG, "Flashing firmware...");

    if (configured != running) {
        LogFile.writeToFile(ESP_LOG_ERROR, TAG,
                            "Configured boot partition (0x" + intToHexString(configured->address) + ") differs from running partition (0x" +
                                intToHexString(running->address) + ")");
    }

    ESP_LOGI(TAG, "Running partition type %d subtype %d (offset 0x%08x)", running->type, running->subtype, (unsigned int)running->address);

    const esp_partition_t *updatePartition = esp_ota_get_next_update_partition(NULL);
    if (!updatePartition) {
        LogFile.writeToFile(ESP_LOG_ERROR, TAG, "otaUpdateFirmware: Target OTA partition not found");
        return false;
    }

    ESP_LOGI(TAG, "Writing to partition subtype %d at offset 0x%x", updatePartition->subtype, (unsigned int)updatePartition->address);

    FILE *file = fopen(filename.c_str(), "rb");
    if (!file) {
        LogFile.writeToFile(ESP_LOG_ERROR, TAG, "otaUpdateFirmware: Failed to open file: " + filename);
        return false;
    }

    // Use a 512-byte buffer for SD card access (newlib default: 128 bytes)
    setvbuf(file, NULL, _IOFBF, 512);

    struct stat st;
    if (fstat(fileno(file), &st) != 0 || st.st_size < 0) {
        LogFile.writeToFile(ESP_LOG_ERROR, TAG, "otaUpdateFirmware: Binary file size eval failed or invalid size on: " + filename);
        fclose(file);
        return false;
    }
    const size_t totalFileSize = (size_t)st.st_size;

    if (totalFileSize == 0) {
        LogFile.writeToFile(ESP_LOG_ERROR, TAG, "otaUpdateFirmware: Binary file is empty (0 bytes)");
        fclose(file);
        return false;
    }

    if (totalFileSize > updatePartition->size) {
        LogFile.writeToFile(ESP_LOG_ERROR, TAG, "otaUpdateFirmware: Firmware too large for OTA partition");
        fclose(file);
        return false;
    }

    char *otaDataBuffer = (char *)malloc(SERVER_OTA_SCRATCH_BUFSIZE);
    if (!otaDataBuffer) {
        LogFile.writeToFile(ESP_LOG_ERROR, TAG, "otaUpdateFirmware: Heap allocation failed for buffer");
        fclose(file);
        return false;
    }

    constexpr size_t headerRequiredSize = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
    static_assert(SERVER_OTA_SCRATCH_BUFSIZE >= headerRequiredSize,
                  "SERVER_OTA_SCRATCH_BUFSIZE must be large enough to hold the full image header "
                  "in a single read (see otaUpdateFirmware header-validity check)");


    bool imageHeaderValid = false;
    size_t binaryFileLength = 0;
    size_t bytesRead = fread(otaDataBuffer, 1, SERVER_OTA_SCRATCH_BUFSIZE, file);

    while (bytesRead > 0) {
        if (!imageHeaderValid) {
            if (bytesRead >= headerRequiredSize) {
                esp_app_desc_t newAppInfo;
                memcpy(&newAppInfo, &otaDataBuffer[sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t)],
                       sizeof(esp_app_desc_t));

                ESP_LOGI(TAG, "New firmware version: %.*s", sizeof(newAppInfo.version), newAppInfo.version);

                esp_app_desc_t runningAppInfo;
                if (esp_ota_get_partition_description(running, &runningAppInfo) == ESP_OK) {
                    ESP_LOGI(TAG, "Running firmware version: %.*s", sizeof(runningAppInfo.version), runningAppInfo.version);
                }

                imageHeaderValid = true;

                retVal = esp_ota_begin(updatePartition, totalFileSize, &otaHandle);
                if (retVal != ESP_OK) {
                    LogFile.writeToFile(ESP_LOG_ERROR, TAG, "otaUpdateFirmware: esp_ota_begin failed | Error: " + intToHexString(retVal));
                    goto cleanup;
                }
                ESP_LOGI(TAG, "esp_ota_begin succeeded");
            }
            else {
                LogFile.writeToFile(ESP_LOG_ERROR, TAG, "otaUpdateFirmware: File header too small: " + std::to_string(bytesRead));
                goto cleanup;
            }
        }

        retVal = esp_ota_write(otaHandle, (const void *)otaDataBuffer, bytesRead);
        if (retVal != ESP_OK) {
            LogFile.writeToFile(ESP_LOG_ERROR, TAG, "otaUpdateFirmware: esp_ota_write failed | Error: " + intToHexString(retVal));
            goto cleanup;
        }

        binaryFileLength += bytesRead;
        bytesRead = fread(otaDataBuffer, 1, SERVER_OTA_SCRATCH_BUFSIZE, file);

        if (bytesRead == 0 && ferror(file)) {
            LogFile.writeToFile(ESP_LOG_ERROR, TAG,
                                "otaUpdateFirmware: fread() failed after " + std::to_string(binaryFileLength) +
                                    " bytes (I/O error, not EOF)");
            goto cleanup;
        }
    }

    if (binaryFileLength != totalFileSize) {
        LogFile.writeToFile(ESP_LOG_ERROR, TAG,
                            "otaUpdateFirmware: Incomplete firmware read | Expected: " + std::to_string(totalFileSize) +
                                " | Written: " + std::to_string(binaryFileLength));
        goto cleanup;
    }

    ESP_LOGI(TAG, "Total written image length: %u", (unsigned int)binaryFileLength);

    retVal = esp_ota_end(otaHandle);
    if (retVal != ESP_OK) {
        if (retVal == ESP_ERR_OTA_VALIDATE_FAILED) {
            LogFile.writeToFile(ESP_LOG_ERROR, TAG, "otaUpdateFirmware: Firmware validation failed (corrupt or mismatched chip ID)");
        }
        else {
            LogFile.writeToFile(ESP_LOG_ERROR, TAG, "otaUpdateFirmware: esp_ota_end failed | Error: " + intToHexString(retVal));
        }
        otaHandle = 0; // esp_ota_end already cleaned handle
        goto cleanup;
    }
    otaHandle = 0; // Successfully ended

    retVal = esp_ota_set_boot_partition(updatePartition);
    if (retVal != ESP_OK) {
        LogFile.writeToFile(ESP_LOG_ERROR, TAG, "otaUpdateFirmware: esp_ota_set_boot_partition failed | Error: " + intToHexString(retVal));
        goto cleanup;
    }

    // Required to use goto cleanup
    {
        // Clear Core Dump partition on success
        const esp_partition_t *coredumpPartition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP,
                                                                            "coredump");
        if (coredumpPartition) {
            const esp_err_t eraseError = esp_partition_erase_range(coredumpPartition, 0, coredumpPartition->size);

            if (eraseError != ESP_OK) {
                LogFile.writeToFile(ESP_LOG_WARN, TAG, "Failed to erase coredump partition | Error: " + intToHexString(eraseError));
            }
        }
    }

    success = true;

cleanup:
    if (file) {
        fclose(file);
    }
    if (otaDataBuffer) {
        free(otaDataBuffer);
    }
    if (!success && otaHandle != 0) {
        esp_ota_abort(otaHandle);
    }

    return success;
}


// 3. Process OTA Update
static bool processOtaUpdate()
{
    const std::string fileType = detectPackageType(FILE_OTA_STAGED_PACKAGE);
    if (fileType == "ZIP") {
        LogFile.writeToFile(ESP_LOG_INFO, TAG, "Processing ZIP file...");

        // Remove leftovers of an earlier attempt so a failed asset install cannot leak into this package
        deleteFile(FILE_OTA_STAGED_FW);
        deleteFile(FILE_OTA_STAGED_CONFIG_BACKUP_MANIFEST);
        deleteAllFilesInDirectory(DIR_OTA_STAGED_HTML, true);
        deleteAllFilesInDirectory(DIR_OTA_STAGED_CONFIG, true);

        if (!unzipFile(FILE_OTA_STAGED_PACKAGE, DIR_OTA_STAGED)) {
            LogFile.writeToFile(ESP_LOG_ERROR, TAG, "Failed to unzip files");
            deleteAllFilesInDirectory(DIR_OTA_STAGED, true);
            return false;
        }

        // Configuration Restore Flow
        if (fileExists(FILE_OTA_STAGED_CONFIG_BACKUP_MANIFEST)) {
            LogFile.writeToFile(ESP_LOG_INFO, TAG, "Restoring configuration...");

            bool success = mergeFolder(DIR_OTA_STAGED_CONFIG, DIR_CONFIG_ROOT);
            LogFile.writeToFile(success ? ESP_LOG_INFO : ESP_LOG_ERROR, TAG,
                                success ? "Restore configuration successful" : "Failed to restore configuration");

            deleteAllFilesInDirectory(DIR_OTA_STAGED, true);
            return success;
        }

        // Firmware + WebUI Flow
        if (!fileExists(FILE_OTA_STAGED_FW) || !dirExists(DIR_OTA_STAGED_HTML)) {
            LogFile.writeToFile(ESP_LOG_ERROR, TAG, "Firmware.bin or HTML folder missing");
            deleteAllFilesInDirectory(DIR_OTA_STAGED, true);
            return false;
        }

        if (!validateStagedPackage()) {
            LogFile.writeToFile(ESP_LOG_ERROR, TAG, "Staged package rejected, firmware not flashed");
            deleteAllFilesInDirectory(DIR_OTA_STAGED, true);
            return false;
        }

        if (!otaUpdateFirmware(FILE_OTA_STAGED_FW)) {
            LogFile.writeToFile(ESP_LOG_ERROR, TAG, "Failed to flash firmware");
            deleteAllFilesInDirectory(DIR_OTA_STAGED, true);
            return false;
        }

#ifdef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
        // Keep extracted assets until the new firmware has been verified
        deleteFile(FILE_OTA_STAGED_PACKAGE);
#else
        // Legacy bootloader cannot rollback, so promote assets immediately
        updateOtaAssets();
        deleteAllFilesInDirectory(DIR_OTA_STAGED, true);
#endif
        return true;
    }

    if (fileType == "BIN") {
        LogFile.writeToFile(ESP_LOG_INFO, TAG, "Processing BIN file...");

        if (!otaUpdateFirmware(FILE_OTA_STAGED_PACKAGE)) {
            LogFile.writeToFile(ESP_LOG_ERROR, TAG, "Failed to flash firmware");
            deleteAllFilesInDirectory(DIR_OTA_STAGED, true);
            return false;
        }

        deleteAllFilesInDirectory(DIR_OTA_STAGED, true);
        return true;
    }

    LogFile.writeToFile(ESP_LOG_ERROR, TAG, "processOtaUpdate: File type not supported");
    deleteAllFilesInDirectory(DIR_OTA_STAGED, true);
    return false;
}


// 2. OTA update task
static void taskOtaUpdate(void *pvParameter)
{
    setStatusLed(AP_OR_OTA, 1, true);

    if (!processOtaUpdate()) {
        LogFile.writeToFile(ESP_LOG_ERROR, TAG, "OTA update aborted / rejected");
    }

    LogFile.writeToFile(ESP_LOG_INFO, TAG, "Rebooting to finalize process...");

    doRebootOTA();
    vTaskDelete(NULL);
}


// 1. OTA Update Checker
void checkOtaUpdate()
{
    // Finalize pending OTA update tasks and partition ievaluation before checking for new updates
#ifdef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    finalizeOtaUpdate();
#endif // CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE

    // Checking for staged update package
    if (!fileExists(FILE_OTA_STAGED_PACKAGE)) {
        LogFile.writeToFile(ESP_LOG_INFO, TAG, "No pending update");
        return;
    }

    const std::string fileType = detectPackageType(FILE_OTA_STAGED_PACKAGE);
    if (fileType.empty()) {
        LogFile.writeToFile(ESP_LOG_ERROR, TAG, "Staged OTA package invalid (supported: ZIP, BIN). Aborting...");
        deleteAllFilesInDirectory(DIR_OTA_STAGED, true);
        return;
    }
    LogFile.writeToFile(ESP_LOG_INFO, TAG, "Preparing OTA process | Type: " + fileType);

    BaseType_t taskCreated = xTaskCreate(&taskOtaUpdate, "taskOTAUpdate", 16384, NULL, tskIDLE_PRIORITY + 5, NULL);
    if (taskCreated != pdPASS) {
        LogFile.writeToFile(ESP_LOG_ERROR, TAG, "Failed to create taskOTAUpdate task. Aborting...");
        deleteAllFilesInDirectory(DIR_OTA_STAGED, true);
        doRebootOTA();
        return;
    }

    // 5-minute watchdog block (300,000 ms)
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(300000));

    LogFile.writeToFile(ESP_LOG_ERROR, TAG, "OTA task timed out or failed to reboot after 5 mins. Aborting...");
    deleteAllFilesInDirectory(DIR_OTA_STAGED, true);
    doRebootOTA();

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}


// --------------------------------------------------------------------------------
// OTA update from GitHub releases (comdata fork)
// --------------------------------------------------------------------------------
static constexpr const char *OTA_RELEASE_API_URL = "https://api.github.com/repos/comdata/AI-on-the-edge-device-comdata-fork/releases/latest";
static constexpr const char *OTA_RELEASE_DOWNLOAD_PREFIX =
    "https://github.com/comdata/AI-on-the-edge-device-comdata-fork/releases/download/";
static constexpr const char *OTA_RELEASE_ASSET_PREFIX = "AI-on-the-edge-device__";
static constexpr int OTA_AUTO_UPDATE_DELAY_MS = 60000;               // initial delay after boot
static constexpr int OTA_AUTO_UPDATE_INTERVAL_MS = 6 * 60 * 60 * 1000; // re-check every 6h
static constexpr size_t OTA_RELEASE_METADATA_MAX_SIZE = 128 * 1024;

static char otaPullStatus[160] = "idle";
static bool otaPullActive = false;
static std::string otaPullError; // last download failure reason, shown in the WebUI


static void setOtaPullStatus(const std::string &status)
{
    snprintf(otaPullStatus, sizeof(otaPullStatus), "%s", status.c_str());
}


// Reads the whole HTTP(S) response body into a string (used for the small GitHub release JSON)
static bool otaHttpGetToString(const char *url, const char *accept, std::string &response)
{
    esp_http_client_config_t httpConfig = {};
    httpConfig.url = url;
    httpConfig.crt_bundle_attach = esp_crt_bundle_attach;
    httpConfig.user_agent = "AI-on-the-Edge Device";
    httpConfig.timeout_ms = 15000;

    esp_http_client_handle_t httpClient = esp_http_client_init(&httpConfig);
    if (httpClient == NULL) {
        return false;
    }
    esp_http_client_set_header(httpClient, "Accept", accept);

    bool success = false;
    if (esp_http_client_open(httpClient, 0) == ESP_OK) {
        esp_http_client_fetch_headers(httpClient);

        response.clear();
        char buffer[512];
        int read = 0;
        while ((read = esp_http_client_read(httpClient, buffer, sizeof(buffer))) > 0) {
            response.append(buffer, read);
            if (response.size() > OTA_RELEASE_METADATA_MAX_SIZE) {
                read = -1; // response too large, abort
                break;
            }
        }
        success = esp_http_client_get_status_code(httpClient) == 200 && read == 0 && !response.empty();
    }

    esp_http_client_close(httpClient);
    esp_http_client_cleanup(httpClient);

    return success;
}


// Resolves tag and download URL of the latest release asset matching the board type
static bool otaGetLatestRelease(const std::string &boardType, std::string &tag, std::string &assetUrl, size_t &assetSize)
{
    std::string response;
    if (!otaHttpGetToString(OTA_RELEASE_API_URL, "application/vnd.github+json", response)) {
        LogFile.writeToFile(ESP_LOG_WARN, TAG, "Failed to read release information from GitHub");
        return false;
    }

    cJSON *jsonRoot = cJSON_Parse(response.c_str());
    if (jsonRoot == NULL) {
        LogFile.writeToFile(ESP_LOG_WARN, TAG, "Failed to parse release information from GitHub");
        return false;
    }

    bool found = false;
    const cJSON *jsonTag = cJSON_GetObjectItemCaseSensitive(jsonRoot, "tag_name");
    const cJSON *jsonAssets = cJSON_GetObjectItemCaseSensitive(jsonRoot, "assets");
    if (cJSON_IsString(jsonTag) && cJSON_IsArray(jsonAssets)) {
        const std::string expectedPrefix = std::string(OTA_RELEASE_ASSET_PREFIX) + boardType + "__comdataFork_";
        const cJSON *jsonAsset = NULL;

        cJSON_ArrayForEach(jsonAsset, jsonAssets) {
            const cJSON *jsonName = cJSON_GetObjectItemCaseSensitive(jsonAsset, "name");
            const cJSON *jsonUrl = cJSON_GetObjectItemCaseSensitive(jsonAsset, "browser_download_url");
            const cJSON *jsonSize = cJSON_GetObjectItemCaseSensitive(jsonAsset, "size");

            if (!cJSON_IsString(jsonName) || !cJSON_IsString(jsonUrl)) {
                continue;
            }
            const std::string assetName = jsonName->valuestring;
            if (assetName.rfind(expectedPrefix, 0) == 0 && assetName.size() > 4 &&
                assetName.compare(assetName.size() - 4, 4, ".zip") == 0) {
                tag = jsonTag->valuestring;
                assetUrl = jsonUrl->valuestring;
                assetSize = cJSON_IsNumber(jsonSize) ? (size_t)jsonSize->valuedouble : 0;
                found = true;
                break;
            }
        }
    }

    cJSON_Delete(jsonRoot);
    return found;
}


// Version check for release tags like "v18.2.0-comdataFork"
static bool otaIsReleaseVersion(const std::string &version)
{
    return version.size() > 1 && version[0] == 'v' && isdigit((unsigned char)version[1]) != 0;
}


static void otaParseVersion(const std::string &version, int part[3])
{
    part[0] = part[1] = part[2] = 0;
    sscanf(version.c_str(), "v%d.%d.%d", &part[0], &part[1], &part[2]);
}


static bool otaIsNewerVersion(const std::string &candidate, const std::string &installed)
{
    int candidatePart[3];
    int installedPart[3];
    otaParseVersion(candidate, candidatePart);
    otaParseVersion(installed, installedPart);

    for (int i = 0; i < 3; ++i) {
        if (candidatePart[i] != installedPart[i]) {
            return candidatePart[i] > installedPart[i];
        }
    }
    return false;
}


// Opens the package URL and follows redirects.
// Note: esp_http_client only follows redirects in perform(); with open()/read() the caller has to do it.
static bool otaHttpOpenPackage(esp_http_client_handle_t httpClient, int &httpStatusCode, int64_t &contentLength)
{
    char currentUrl[512];

    for (int redirect = 0; redirect < 5; ++redirect) {
        const esp_err_t openError = esp_http_client_open(httpClient, 0);
        esp_http_client_get_url(httpClient, currentUrl, sizeof(currentUrl));

        if (openError != ESP_OK) {
            LogFile.writeToFile(ESP_LOG_ERROR, TAG,
                                "OTA download: connect failed (hop " + std::to_string(redirect + 1) + ", error " +
                                    intToHexString(openError) + ") | URL: " + currentUrl);
            return false;
        }

        contentLength = esp_http_client_fetch_headers(httpClient);
        httpStatusCode = esp_http_client_get_status_code(httpClient);
        LogFile.writeToFile(ESP_LOG_INFO, TAG,
                            "OTA download: hop " + std::to_string(redirect + 1) + " | HTTP " + std::to_string(httpStatusCode) +
                                " | Size: " + std::to_string(contentLength) + " | URL: " + currentUrl);

        if (httpStatusCode != 301 && httpStatusCode != 302 && httpStatusCode != 303 && httpStatusCode != 307 && httpStatusCode != 308) {
            return true;
        }
        if (esp_http_client_set_redirection(httpClient) != ESP_OK) {
            LogFile.writeToFile(ESP_LOG_ERROR, TAG, "OTA download: redirect without usable location");
            return false;
        }
    }
    return false;
}


// Streams the release package into the OTA staging directory
static bool otaDownloadPackage(const std::string &url)
{
    makeDir(DIR_OTA_STAGED);
    deleteAllFilesInDirectory(DIR_OTA_STAGED, true);

    esp_http_client_config_t httpConfig = {};
    httpConfig.url = url.c_str();
    httpConfig.crt_bundle_attach = esp_crt_bundle_attach;
    httpConfig.user_agent = "AI-on-the-Edge Device";
    httpConfig.timeout_ms = 20000;
    httpConfig.buffer_size = SERVER_OTA_SCRATCH_BUFSIZE;
    // The signed URL behind GitHub's redirect is ~1.3 kB long and has to fit the request line
    httpConfig.buffer_size_tx = 2048;

    esp_http_client_handle_t httpClient = esp_http_client_init(&httpConfig);
    if (httpClient == NULL) {
        LogFile.writeToFile(ESP_LOG_ERROR, TAG, "OTA download: Failed to init HTTP client");
        return false;
    }
    esp_http_client_set_header(httpClient, "Accept", "application/octet-stream");

    bool success = false;
    FILE *file = NULL;
    char *buffer = (char *)malloc(SERVER_OTA_SCRATCH_BUFSIZE);
    int httpStatusCode = 0;
    int64_t contentLength = 0;

    if (buffer == NULL) {
        LogFile.writeToFile(ESP_LOG_ERROR, TAG, "OTA download: Failed to allocate buffer");
    }
    else if (!otaHttpOpenPackage(httpClient, httpStatusCode, contentLength)) {
        LogFile.writeToFile(ESP_LOG_ERROR, TAG, "OTA download: Failed to connect | URL: " + url);
        otaPullError = "connection failed";
    }
    else {
        LogFile.writeToFile(ESP_LOG_INFO, TAG,
                            "OTA download: HTTP " + std::to_string(httpStatusCode) + " | Size: " + std::to_string(contentLength));

        if (httpStatusCode != 200) {
            LogFile.writeToFile(ESP_LOG_ERROR, TAG, "OTA download: HTTP request failed | Status: " + std::to_string(httpStatusCode));
            otaPullError = "HTTP " + std::to_string(httpStatusCode);
        }
        else if (contentLength <= 0 || contentLength > MAX_FILE_SIZE) {
            LogFile.writeToFile(ESP_LOG_ERROR, TAG, "OTA download: Invalid package size");
            otaPullError = "invalid package size";
        }
        else {
            file = fopen(FILE_OTA_STAGED_PACKAGE, "wb");
            if (file == NULL) {
                LogFile.writeToFile(ESP_LOG_ERROR, TAG, "OTA download: Failed to open staging file");
            }
            else {
                size_t total = 0;
                int read = 0;
                while ((read = esp_http_client_read(httpClient, buffer, SERVER_OTA_SCRATCH_BUFSIZE)) > 0) {
                    if (fwrite(buffer, 1, read, file) != (size_t)read) {
                        LogFile.writeToFile(ESP_LOG_ERROR, TAG, "OTA download: SD card write failed");
                        otaPullError = "SD card write failed";
                        break;
                    }
                    total += read;
                }
                success = (read == 0 && total == (size_t)contentLength);
                if (!success) {
                    otaPullError = "incomplete download";
                }
                LogFile.writeToFile(success ? ESP_LOG_INFO : ESP_LOG_ERROR, TAG,
                                    "OTA download: " + std::to_string(total) + " of " + std::to_string(contentLength) + " byte written");
            }
        }
    }

    if (file) {
        fclose(file);
    }
    if (buffer) {
        free(buffer);
    }
    esp_http_client_close(httpClient);
    esp_http_client_cleanup(httpClient);

    if (!success) {
        deleteAllFilesInDirectory(DIR_OTA_STAGED, true);
        return false;
    }

    if (detectPackageType(FILE_OTA_STAGED_PACKAGE) != "ZIP") {
        LogFile.writeToFile(ESP_LOG_ERROR, TAG, "OTA download: Package is not a valid ZIP file");
        otaPullError = "package is not a ZIP";
        deleteAllFilesInDirectory(DIR_OTA_STAGED, true);
        return false;
    }

    return true;
}


// Downloads a release package in the background and reboots into the OTA process
static void taskOtaPull(void *pvParameter)
{
    std::string url = *(std::string *)pvParameter;
    delete (std::string *)pvParameter;

    setStatusLed(AP_OR_OTA, 1, true);

    otaPullError.clear();

    // No URL given (e.g. Home Assistant): resolve the newest release for this board
    if (url.empty()) {
        std::string tag;
        size_t assetSize = 0;
        if (!otaGetLatestRelease(toLower(getBoardType()), tag, url, assetSize)) {
            setOtaPullStatus("failed: no matching release asset");
            otaPullActive = false;
            vTaskDelete(NULL);
            return;
        }
        LogFile.writeToFile(ESP_LOG_INFO, TAG, "OTA pull: Resolved latest release " + tag);
    }

    if (!otaDownloadPackage(url)) {
        setOtaPullStatus("failed: " + (otaPullError.empty() ? std::string("download failed, see device log") : otaPullError));
        otaPullActive = false;
        vTaskDelete(NULL);
        return;
    }

    setOtaPullStatus("done");
    LogFile.writeToFile(ESP_LOG_INFO, TAG, "OTA package downloaded, rebooting to install");
    vTaskDelay(pdMS_TO_TICKS(500));
    doReboot();
    vTaskDelete(NULL);
}


// Periodic check: install a newer release automatically when the device runs a release build
static void taskOtaAutoUpdate(void *pvParameter)
{
    vTaskDelay(pdMS_TO_TICKS(OTA_AUTO_UPDATE_DELAY_MS));

    for (;;) {
        const std::string boardType = toLower(getBoardType());
        const std::string installedVersion = getFwVersion();
        std::string tag;
        std::string assetUrl;
        size_t assetSize = 0;

        if (!otaGetLatestRelease(boardType, tag, assetUrl, assetSize)) {
            LogFile.writeToFile(ESP_LOG_WARN, TAG, "Auto update: No matching release asset for board " + boardType);
        }
        else if (!otaIsReleaseVersion(installedVersion)) {
            LogFile.writeToFile(ESP_LOG_INFO, TAG,
                                "Auto update: Latest release is " + tag + " | Installed build is no release version, skipped");
        }
        else if (!otaIsNewerVersion(tag, installedVersion)) {
            LogFile.writeToFile(ESP_LOG_INFO, TAG, "Auto update: Installed version is up to date (" + tag + ")");
        }
        else if (otaPullActive) {
            LogFile.writeToFile(ESP_LOG_INFO, TAG, "Auto update: Another download is running, skipped");
        }
        else if (assetSize > MAX_FILE_SIZE) {
            LogFile.writeToFile(ESP_LOG_ERROR, TAG, "Auto update: Release package exceeds maximum file size");
        }
        else {
            LogFile.writeToFile(ESP_LOG_INFO, TAG, "Auto update: Installing " + tag + " (installed: " + installedVersion + ")");
            otaPullError.clear();
            setOtaPullStatus("running: auto update " + tag);
            otaPullActive = true;

            if (otaDownloadPackage(assetUrl)) {
                setOtaPullStatus("done");
                doReboot();
                vTaskDelete(NULL);
            }

            setOtaPullStatus("failed: " + (otaPullError.empty() ? std::string("download failed, see device log") : otaPullError));
            otaPullActive = false;
        }

        vTaskDelay(pdMS_TO_TICKS(OTA_AUTO_UPDATE_INTERVAL_MS));
    }
}


void startOtaAutoUpdate(void)
{
    if (xTaskCreate(&taskOtaAutoUpdate, "taskOTAAutoUpdate", 12288, NULL, tskIDLE_PRIORITY + 2, NULL) != pdPASS) {
        LogFile.writeToFile(ESP_LOG_WARN, TAG, "Auto update: Failed to create task");
    }
}


esp_err_t handler_ota_pull_post(httpd_req_t *req)
{
    char query[700];
    char value[600];
    std::string url;

    // Without a URL the device resolves the latest release itself (used by Home Assistant)
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "url", value, sizeof(value)) == ESP_OK) {
        url = urlDecode(value);

        // Only release downloads of this fork are fetched (no arbitrary URLs on the device)
        if (url.rfind(OTA_RELEASE_DOWNLOAD_PREFIX, 0) != 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "URL not allowed. Only comdata release downloads are supported");
            return ESP_FAIL;
        }
    }

    if (otaPullActive) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Download already running");
        return ESP_FAIL;
    }

    std::string *urlCopy = new std::string(url);
    if (xTaskCreate(&taskOtaPull, "taskOTAPull", 12288, urlCopy, tskIDLE_PRIORITY + 5, NULL) != pdPASS) {
        delete urlCopy;
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to start download");
        return ESP_FAIL;
    }

    otaPullActive = true;
    setOtaPullStatus("running");

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "started");
    return ESP_OK;
}


esp_err_t handler_ota_pull_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_sendstr(req, otaPullStatus);
    return ESP_OK;
}


// Uploads the OTA package, stages it and reboot
esp_err_t handler_ota(httpd_req_t *req)
{
    static const char *uriPrefix = "/ota/";
    if (strncmp(req->uri, uriPrefix, strlen(uriPrefix)) != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Use POST /ota/<filename.xyz>");
        return ESP_FAIL;
    }

    const std::string sanitizedFile = getFileName(req->uri + strlen(uriPrefix));
    if (sanitizedFile.empty()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid or missing filename");
        return ESP_FAIL;
    }

    if (req->content_len == 0 || req->content_len > MAX_FILE_SIZE) {
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "Invalid file size, must be > 0 and < " MAX_FILE_SIZE_STR);
        return ESP_FAIL;
    }

    makeDir(DIR_OTA_STAGED);
    deleteAllFilesInDirectory(DIR_OTA_STAGED, true);

    LogFile.writeToFile(ESP_LOG_DEBUG, TAG, "OTA upload | File: " + sanitizedFile);

    if (receiveRequestBodyToFile(req, FILE_OTA_STAGED_PACKAGE) != ESP_OK) {
        deleteAllFilesInDirectory(DIR_OTA_STAGED, true);
        return ESP_FAIL;
    }

    LogFile.writeToFile(ESP_LOG_DEBUG, TAG, "OTA upload completed | File: " + sanitizedFile);

    const std::string fileType = detectPackageType(FILE_OTA_STAGED_PACKAGE);
    if (fileType != "ZIP" && fileType != "BIN") {
        deleteAllFilesInDirectory(DIR_OTA_STAGED, true);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unsupported file type (supported: ZIP, ESP firmware BIN)");
        return ESP_FAIL;
    }

    // Send response before triggering reboot to guarantee delivery
    httpd_resp_sendstr(req, "success: Upload successful. Device reboots to process OTA package");

    vTaskDelay(pdMS_TO_TICKS(500));
    doReboot(); // mandatory reboot before the actual OTA is performed
    return ESP_OK;
}


static void forceReboot()
{
    const esp_task_wdt_config_t twdtConfig = {
        .timeout_ms = 100,
        .idle_core_mask = (1 << SOC_CPU_CORES_NUM) - 1,
        .trigger_panic = true,
    };

    esp_err_t err = esp_task_wdt_init(&twdtConfig);
    if (err == ESP_ERR_INVALID_STATE) { // Already initialized
        err = esp_task_wdt_reconfigure(&twdtConfig);
    }

    if (err != ESP_OK) {
        abort();
    }

    err = esp_task_wdt_add(NULL);
    if (err != ESP_OK) {
        abort();
    }

    while (true) {
        // Wait for watchdog reset
    }
}


static void taskReboot(void *DeleteMainFlow)
{
    markPlannedReboot();

    // Kill main task if executed in extra task, if not don't kill parent task to force reboot
    if ((bool)DeleteMainFlow) {
        deleteMainFlowTask();
    }

/* Stop service tasks */
#ifdef ENABLE_MQTT
    deinitMqttClient(true);
#endif // ENABLE_MQTT

    cameraCtrl.setFlashlight(false);
    forceStatusLedOff();
    cameraCtrl.deinitCam();

    destroyGpioHandler();

    httpd_stop(server);

    vTaskDelay(pdMS_TO_TICKS(3000));
    deinitNetwork();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    // Only reached if esp_restart() unexpectedly returns.
    vTaskDelay(pdMS_TO_TICKS(5000));
    forceReboot();
}


void doReboot()
{
    LogFile.writeToFile(ESP_LOG_INFO, TAG, "Reboot triggered by software");
    LogFile.writeToFile(ESP_LOG_WARN, TAG, "Reboot in 5 seconds");

    BaseType_t xReturned = xTaskCreate(&taskReboot, "taskReboot", configMINIMAL_STACK_SIZE * 4, (void *)true, 10, NULL);
    if (xReturned != pdPASS) {
        LogFile.writeToFile(ESP_LOG_ERROR, TAG, "taskReboot not created -> force reboot without killing flow");
        taskReboot((void *)false);
    }
}


void doRebootOTA()
{
    LogFile.writeToFile(ESP_LOG_WARN, TAG, "Reboot in 5 seconds");

    markPlannedReboot();

    cameraCtrl.setFlashlight(false);
    forceStatusLedOff();
    cameraCtrl.deinitCam();

    vTaskDelay(5000 / portTICK_PERIOD_MS);
    esp_restart();

    // Only reached if esp_restart() unexpectedly returns
    vTaskDelay(5000 / portTICK_PERIOD_MS);
    forceReboot();
}


esp_err_t handler_reboot(httpd_req_t *req)
{
    LogFile.writeToFile(ESP_LOG_DEBUG, TAG, "handler_reboot");

    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Reboot initiated");

    doReboot();

    return ESP_OK;
}


void registerOtaRebootUri(httpd_handle_t server)
{
    ESP_LOGI(TAG, "Registering URI handlers");

    httpd_uri_t camuri = {};
    camuri.method = HTTP_POST;
    camuri.uri = "/ota/pull"; // has to be registered before the wildcard /ota/* route
    camuri.handler = HTTP_AUTH_BASIC(handler_ota_pull_post);
    camuri.user_ctx = httpServerData;
    httpd_register_uri_handler(server, &camuri);

    camuri.method = HTTP_GET;
    camuri.uri = "/ota/pull";
    camuri.handler = HTTP_AUTH_BASIC(handler_ota_pull_get);
    camuri.user_ctx = httpServerData;
    httpd_register_uri_handler(server, &camuri);

    camuri.method = HTTP_POST;
    camuri.uri = "/ota/*";
    camuri.handler = HTTP_AUTH_BASIC(handler_ota);
    camuri.user_ctx = httpServerData;
    httpd_register_uri_handler(server, &camuri);

    camuri.method = HTTP_GET;
    camuri.uri = "/reboot";
    camuri.handler = HTTP_AUTH_BASIC(handler_reboot);
    camuri.user_ctx = NULL;
    httpd_register_uri_handler(server, &camuri);
}
