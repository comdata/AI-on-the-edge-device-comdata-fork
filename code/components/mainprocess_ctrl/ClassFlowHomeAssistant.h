#include "../../include/defines.h"

#ifndef CLASSFLOWHOMEASSISTANT_H
#define CLASSFLOWHOMEASSISTANT_H

#include <string>

#include "ClassFlow.h"
#include "ClassFlowPostProcessing.h"
#include "interface_homeassistant.h"


// Publishes the processed values to the Home Assistant REST API
// (POST /api/states/<entity_id>, authenticated with a long-lived access token).
class ClassFlowHomeAssistant : public ClassFlow
{
  protected:
    bool haEnable = false;
    HomeAssistantConfig haConfig;

    bool publishSequence(SequenceData *sequence);
    cJSON *buildValueAttributes(SequenceData *sequence);

  public:
    ClassFlowHomeAssistant();
    virtual ~ClassFlowHomeAssistant();

    bool loadParameter();
    bool doFlow(std::string time);
    void doPostProcessEventHandling();

    std::string name() { return "ClassFlowHomeAssistant"; };
};

#endif // CLASSFLOWHOMEASSISTANT_H
