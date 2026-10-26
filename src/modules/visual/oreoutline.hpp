#pragma once
#include "../Module.hpp"
#include <bedrocktools/sdk/Types.hpp>

class OreOutlineModule : public Module {
public:
    OreOutlineModule();
    ~OreOutlineModule() override;
    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;
    int radius = 16;
private:
    void applyPatch();
    bool m_patched = false;
    void* m_patchTarget = nullptr;
};
