#pragma once

#include "../Module.hpp"

class FreeLookModule : public Module {
public:
    FreeLookModule();
    ~FreeLookModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    float lockPitch = 0.0f;
    float lockYaw = 0.0f;
    bool restoreOnDisable = true;

private:
    bool m_hooked = false;
    void installHook();
};
