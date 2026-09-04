#include "esp.hpp"

static ESPModule* g_esp = nullptr;

ESPModule::ESPModule()
    : Module("ESP", "Apollon-style player ESP with boxes, tracers, nametags and distance.") {
    showInMenu = true;
    g_esp = this;
}

ESPModule::~ESPModule() {
    if (g_esp == this) g_esp = nullptr;
}

void ESPModule::onEnable() {}
void ESPModule::onDisable() {}

void ESPModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    playersOnly = j.value("playersOnly", playersOnly);
    box = j.value("box", box);
    tracers = j.value("tracers", tracers);
    nametag = j.value("nametag", nametag);
    distance = j.value("distance", distance);
    showSelf = j.value("showSelf", showSelf);
    ignoreInvisible = j.value("ignoreInvisible", ignoreInvisible);
    range = j.value("range", range);
    lineWidth = j.value("lineWidth", lineWidth);
    if (j.contains("color")) {
        if (j["color"].is_number_unsigned() || j["color"].is_number_integer())
            color = j["color"].get<uint32_t>();
    }
}

void ESPModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["playersOnly"] = playersOnly;
    j["box"] = box;
    j["tracers"] = tracers;
    j["nametag"] = nametag;
    j["distance"] = distance;
    j["showSelf"] = showSelf;
    j["ignoreInvisible"] = ignoreInvisible;
    j["range"] = range;
    j["lineWidth"] = lineWidth;
    j["color"] = color;
}

ESPModule* getESPModule() { return g_esp; }
