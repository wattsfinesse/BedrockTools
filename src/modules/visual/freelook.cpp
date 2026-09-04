#include "freelook.hpp"
#include <bedrocktools/sdk/Types.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>
#include "core/memory/Hooks.hpp"
#include <atomic>

namespace {
using Vec2 = bedrocktools::sdk::Vec2;
using Vec3 = bedrocktools::sdk::Vec3;

// Supplied Apollon 1.21.111 ARM64 offset: Actor::intersects.
// This is intentionally version-specific; do not reuse it for another Bedrock build.
constexpr uintptr_t kActorIntersects_1_21_111 = 0xC94727C;
using IntersectsFn = void*(*)(void*, Vec3*, Vec3*);

std::atomic<bool> g_enabled{false};
IntersectsFn g_original = nullptr;
FreeLookModule* g_mod = nullptr;
void* g_local = nullptr;
Vec2 g_saved{0.f, 0.f};
bool g_savedValid = false;

static void onTick(void* actor) {
    g_local = actor;
    if (!g_enabled.load(std::memory_order_relaxed) || !actor) return;
    auto* component = bedrocktools::sdk::field<void*>(actor, bedrocktools::sdk::offsets::Actor::mActorRotationComponent);
    if (!component) return;
    if (!g_savedValid) {
        g_saved = bedrocktools::sdk::field<Vec2>(component, 0);
        g_savedValid = true;
    }
}

static void* intersectsHook(void* self, Vec3* a2, Vec3* a3) {
    if (!g_original || !g_enabled.load(std::memory_order_relaxed) || self != g_local || !g_savedValid)
        return g_original ? g_original(self, a2, a3) : nullptr;

    auto* component = bedrocktools::sdk::field<void*>(self, bedrocktools::sdk::offsets::Actor::mActorRotationComponent);
    if (!component) return g_original(self, a2, a3);

    Vec2* live = reinterpret_cast<Vec2*>(component);
    const Vec2 current = *live;
    // Apollon's FreeLook preserves the player/body rotation only during this
    // gameplay calculation. The view can still rotate freely in the client.
    *live = g_saved;
    void* result = g_original(self, a2, a3);
    *live = current;
    return result;
}
}

FreeLookModule::FreeLookModule() : Module("FreeLook", "Apollon-style freelook: free camera rotation while preserving stored body rotation for Actor::intersects.") { showInMenu = true; g_mod = this; }
FreeLookModule::~FreeLookModule() { g_enabled.store(false); if (g_mod == this) g_mod = nullptr; }

void FreeLookModule::installHook() {
    if (m_hooked) return;
    g_local = nullptr;
    g_original = nullptr;
    const uintptr_t address = kActorIntersects_1_21_111;
    auto handle = bedrocktools::hooks::install(reinterpret_cast<void*>(address), reinterpret_cast<void*>(&intersectsHook), reinterpret_cast<void**>(&g_original));
    m_hooked = handle && g_original;
}

void FreeLookModule::onInit() {
    installHook();
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>([](auto& e){ onTick(e.player); });
}
void FreeLookModule::onEnable() { g_savedValid = false; installHook(); g_enabled.store(m_hooked, std::memory_order_release); }
void FreeLookModule::onDisable() { g_enabled.store(false, std::memory_order_release); g_savedValid = false; }
void FreeLookModule::loadConfig(const nlohmann::json& j) { Module::loadConfig(j); restoreOnDisable = j.value("restoreOnDisable", restoreOnDisable); }
void FreeLookModule::saveConfig(nlohmann::json& j) { Module::saveConfig(j); j["restoreOnDisable"] = restoreOnDisable; }
