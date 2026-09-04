#pragma once
#include "../Module.hpp"
#include <cstdint>

class ESPModule : public Module {
public:
    ESPModule();
    ~ESPModule() override;

    void onInit() override {}
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    bool playersOnly = true;
    bool box = true;
    bool tracers = true;
    bool nametag = true;
    bool distance = true;
    bool showSelf = false;
    bool ignoreInvisible = true;
    float range = 64.0f;
    float lineWidth = 2.0f;
    uint32_t color = 0xFF00FFFF;
};

// Shared state used by HitboxModule's single RenderLevel hook. Keeping one
// render hook prevents ESP and Hitbox from fighting over the same engine hook.
ESPModule* getESPModule();
