#pragma once
#include "../Module.hpp"
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

class ThirdPersonNametagModule : public Module {
private:
    bool m_patched;
    uint8_t m_originalBytes[4];
    void* m_patchTarget;

    bool m_nameTagHooked = false;
    bool m_tickSubscribed = false;

    // Options. The mod menu follows the order they are saved in saveConfig(),
    // so "Show Health" appears directly below "Name".
    bool m_name = true;        // "Name" – show the player name in the nametag
    bool m_showHealth = true;  // "Show Health" – append live health to player nametags

    // Live health snapshot, refreshed on the game (tick) thread. The render
    // thread only reads this cache, so no game/ECS calls happen in the hook.
    std::mutex m_healthMutex;
    std::unordered_map<void*, float> m_healthCache;

    void applyPatch();
    void removePatch();

public:
    ThirdPersonNametagModule();
    ~ThirdPersonNametagModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    void onLocalPlayerTick(void* localPlayer);

    bool isShowName() const { return m_name; }
    bool isShowHealth() const { return m_showHealth; }
    float healthForActor(void* actor);
};
