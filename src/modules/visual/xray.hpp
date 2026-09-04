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

    // Matches the Apollon resource toggles rather than a generic ore switch.
    bool diamond = true;
    bool iron = true;
    bool gold = true;
    bool coal = true;
    bool copper = false;
    bool lapis = true;
    bool emerald = true;
    bool redstone = true;
    bool amethyst = false;
    bool netherite = true;
    bool quartz = false;
    bool obsidian = false;
    bool barrel = false;

private:
    bool m_hooked = false;
    void installHooks();
};
