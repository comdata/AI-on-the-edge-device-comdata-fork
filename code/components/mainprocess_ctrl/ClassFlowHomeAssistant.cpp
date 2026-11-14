#include "ClassFlowHomeAssistant.h"

#include <esp_log.h>
#include <cJSON.h>

#include "ClassLogFile.h"
#include "helper.h"


static const char *TAG = "HOMEASSISTANT";


ClassFlowHomeAssistant::ClassFlowHomeAssistant()
{
    presetFlowStateHandler(true);
    haEnable = false;
}


bool ClassFlowHomeAssistant::loadParameter()
{
    if (!homeassistantLoadConfig(haConfig)) {
        haEnable = false;
        return true; // Not configured -> nothing to do, the flow stays enabled
    }

    haEnable = homeassistantInit(haConfig);
    return true;
}


cJSON *ClassFlowHomeAssistant::buildValueAttributes(SequenceData *sequence)
{
    cJSON *attributes = cJSON_CreateObject();
    if (attributes == NULL) {
        return NULL;
    }

    cJSON_AddStringToObject(attributes, "unit_of_measurement", "m³");
    cJSON_AddStringToObject(attributes, "device_class", "water");
    cJSON_AddStringToObject(attributes, "state_class", "total_increasing");
    cJSON_AddStringToObject(attributes, "icon", "mdi:water");
    cJSON_AddStringToObject(attributes, "friendly_name",
                            ("AI on the Edge " + sequence->sequenceName + " reading").c_str());
    cJSON_AddStringToObject(attributes, "raw_value", sequence->sRawValue.c_str());
    cJSON_AddStringToObject(attributes, "last_valid_value", sequence->sFallbackValue.c_str());
    cJSON_AddStringToObject(attributes, "value_status", sequence->sValueStatus.c_str());
    cJSON_AddStringToObject(attributes, "timestamp_processed", sequence->sTimeProcessed.c_str());
    cJSON_AddStringToObject(attributes, "timestamp_last_valid", sequence->sTimeFallbackValue.c_str());
    cJSON_AddStringToObject(attributes, "sequence", sequence->sequenceName.c_str());

    return attributes;
}


bool ClassFlowHomeAssistant::publishSequence(SequenceData *sequence)
{
    bool success = true;
    std::string entityBase = haConfig.entityPrefix + "_" + homeassistantSanitizeEntityPart(sequence->sequenceName);

    // Meter reading
    cJSON *attributes = buildValueAttributes(sequence);
    if (homeassistantPublishState("sensor." + entityBase + "_value", sequence->sActualValue, attributes) != ESP_OK) {
        success = false;
    }

    // Raw value
    attributes = cJSON_CreateObject();
    cJSON_AddStringToObject(attributes, "unit_of_measurement", "m³");
    cJSON_AddStringToObject(attributes, "state_class", "total_increasing");
    cJSON_AddStringToObject(attributes, "icon", "mdi:water-outline");
    cJSON_AddStringToObject(attributes, "friendly_name",
                            ("AI on the Edge " + sequence->sequenceName + " raw value").c_str());
    cJSON_AddStringToObject(attributes, "value_status", sequence->sValueStatus.c_str());
    if (homeassistantPublishState("sensor." + entityBase + "_raw_value", sequence->sRawValue, attributes) != ESP_OK) {
        success = false;
    }

    // Value status of the post-processing
    attributes = cJSON_CreateObject();
    cJSON_AddStringToObject(attributes, "icon", "mdi:check-decagram-outline");
    cJSON_AddStringToObject(attributes, "friendly_name",
                            ("AI on the Edge " + sequence->sequenceName + " value status").c_str());
    cJSON_AddStringToObject(attributes, "timestamp_processed", sequence->sTimeProcessed.c_str());
    if (homeassistantPublishState("sensor." + entityBase + "_value_status", sequence->sValueStatus, attributes) !=
        ESP_OK) {
        success = false;
    }

    // Flow rate in m³/h (the device reports m³/min)
    if (haConfig.publishRate) {
        char rate[24];
        snprintf(rate, sizeof(rate), "%.6f", sequence->ratePerMin * 60.0);

        attributes = cJSON_CreateObject();
        cJSON_AddStringToObject(attributes, "unit_of_measurement", "m³/h");
        cJSON_AddStringToObject(attributes, "device_class", "volume_flow_rate");
        cJSON_AddStringToObject(attributes, "state_class", "measurement");
        cJSON_AddStringToObject(attributes, "icon", "mdi:water-pump");
        cJSON_AddStringToObject(attributes, "friendly_name",
                                ("AI on the Edge " + sequence->sequenceName + " flow rate").c_str());
        if (homeassistantPublishState("sensor." + entityBase + "_rate", rate, attributes) != ESP_OK) {
            success = false;
        }
    }

    return success;
}


bool ClassFlowHomeAssistant::doFlow(std::string zwtime)
{
    if (!haEnable) {
        return true;
    }

    presetFlowStateHandler(false, zwtime);

    bool success = true;
    for (auto &sequence : sequenceData) {
        if (!publishSequence(sequence)) {
            success = false;
        }
    }

    if (!success) {
        // Publishing must never break the measurement: report a warning and continue
        setFlowStateHandlerEvent(1);
    }

    return true;
}


void ClassFlowHomeAssistant::doPostProcessEventHandling()
{
    // Nothing to do
}


ClassFlowHomeAssistant::~ClassFlowHomeAssistant()
{
    // Nothing to do
}
