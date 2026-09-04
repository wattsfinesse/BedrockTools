#pragma once

#include "../Module.hpp"

class XrayModule : public Module {
public:
    XrayModule();
    ~XrayModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    bool oresOnly = true;
    bool diamond = true;
    bool emerald = true;
    bool gold = true;
    bool iron = true;
    bool copper = true;
    bool coal = true;
    bool redstone = true;
    bool lapis = true;
    bool quartz = true;
    bool ancientDebris = true;
    bool amethyst = true;
    bool obsidian = false;

private:
    bool m_hooked = false;
    void installHooks();
    void applyConfig();
};
