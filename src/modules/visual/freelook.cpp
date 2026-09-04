#include "freelook.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>
#include <bedrocktools/sdk/Types.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include "core/memory/Hooks.hpp"
#include <dlfcn.h>
#include <link.h>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace {
using IntersectsFn = void*(*)(void*, bedrocktools::sdk::Vec3*, bedrocktools::sdk::Vec3*);

constexpr uintptr_t kApollon121111IntersectsOffset = 0xC94727C;
std::atomic<bool> g_enabled{false};
FreeLookModule* g_module = nullptr;
void* g_localPlayer = nullptr;
IntersectsFn g_original = nullptr;
bedrocktools::hooks::Handle g_hook = nullptr;
bedrocktools::sdk::Vec2 g_savedRotation{0.f, 0.f};
bool g_haveSavedRotation = false;

static uintptr_t minecraftBase() {
    struct State { uintptr_t base = 0; } state;
    dl_iterate_phdr([](struct dl_phdr_info* info, size_t, void* opaque) {
        if (!info || !info->dlpi_name || !opaque) return 0;
        if (std::strstr(info->dlpi_name, "libminecraftpe.so")) {
            auto* st = static_cast<State*>(opaque);
            st->base = static_cast<uintptr_t>(info->dlpi_addr);
            return 1;
        }
        return 0;
    }, &state);
    return state.base;
}

static void tick(void* player) {
    g_localPlayer = player;
    if (!g_enabled.load(std::memory_order_relaxed) || !player) return;
    auto* component = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(player) + bedrocktools::sdk::offsets::Actor::mActorRotationComponent);
    if (!component) return;
    auto rotation = *reinterpret_cast<bedrocktools::sdk::Vec2*>(component);
    if (!g_haveSavedRotation) {
        g_savedRotation = rotation;
        g_haveSavedRotation = true;
    }
}

static void* intersectsDetour(void* self, bedrocktools::sdk::Vec3* a2, bedrocktools::sdk::Vec3* a3) {
    if (!g_original) return nullptr;
    if (!g_enabled.load(std::memory_order_relaxed) || self != g_localPlayer || !g_haveSavedRotation) {
        return g_original(self, a2, a3);
    }

    auto* component = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(self) + bedrocktools::sdk::offsets::Actor::mActorRotationComponent);
    if (!component) return g_original(self, a2, a3);

    auto* rotation = reinterpret_cast<bedrocktools::sdk::Vec2*>(component);
    const auto live = *rotation;
    *rotation = g_savedRotation;
    void* result = g_original(self, a2, a3);
    *rotation = live;
    return result;
}
}

FreeLookModule::FreeLookModule()
    : Module("FreeLook", "Separates the player's stored body rotation during collision calculations.") {
    showInMenu = true;
    g_module = this;
}

FreeLookModule::~FreeLookModule() {
    g_enabled.store(false, std::memory_order_release);
    if (g_module == this) g_module = nullptr;
}

void FreeLookModule::installHook() {
    if (m_hooked) return;
    const uintptr_t base = minecraftBase();
    if (!base) return;
    const uintptr_t target = base + kApollon121111IntersectsOffset;
    g_hook = bedrocktools::hooks::install(reinterpret_cast<void*>(target), reinterpret_cast<void*>(&intersectsDetour), reinterpret_cast<void**>(&g_original));
    m_hooked = g_hook && g_original;
}

void FreeLookModule::onInit() {
    installHook();
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>([](auto& event) {
        tick(event.player);
    });
}

void FreeLookModule::onEnable() {
    g_haveSavedRotation = false;
    installHook();
    g_enabled.store(m_hooked, std::memory_order_release);
}

void FreeLookModule::onDisable() {
    g_enabled.store(false, std::memory_order_release);
    if (restoreOnDisable && g_localPlayer && g_haveSavedRotation) {
        auto* component = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(g_localPlayer) + bedrocktools::sdk::offsets::Actor::mActorRotationComponent);
        if (component) *reinterpret_cast<bedrocktools::sdk::Vec2*>(component) = g_savedRotation;
    }
    g_haveSavedRotation = false;
}

void FreeLookModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    lockPitch = j.value("lockPitch", lockPitch);
    lockYaw = j.value("lockYaw", lockYaw);
    restoreOnDisable = j.value("restoreOnDisable", restoreOnDisable);
}

void FreeLookModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["lockPitch"] = lockPitch;
    j["lockYaw"] = lockYaw;
    j["restoreOnDisable"] = restoreOnDisable;
}
