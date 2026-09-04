#include "xray.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include "core/memory/Hooks.hpp"
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Types.hpp>
#include <bedrocktools/sdk/render/Block.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/ClientInstanceUpdateEvent.hpp>
#include <atomic>
#include <cctype>
#include <array>
#include <string>
#include <algorithm>

namespace {
using Vec3Raw = bedrocktools::sdk::Vec3;
using BlockPosRaw = bedrocktools::sdk::BlockPos;
using FaceFn = void(*)(void*, void*, const void*, const Vec3Raw*, const void*);

std::atomic<bool> g_enabled{false};
XrayModule* g_module = nullptr;
using SetAllDirtyFn = void(*)(void*, bool, bool);
SetAllDirtyFn g_setAllDirty = nullptr;
std::atomic<bool> g_rebuildPending{false};

struct FaceHook { bedrocktools::hooks::Handle handle = nullptr; FaceFn original = nullptr; };
std::array<FaceHook, 6> g_hooks{};

static bool isOre(const std::string& raw) {
    std::string name = raw;
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (name.find("diamond") != std::string::npos) return g_module && g_module->diamond;
    if (name.find("emerald") != std::string::npos) return g_module && g_module->emerald;
    if (name.find("gold_ore") != std::string::npos || name.find("deepslate_gold") != std::string::npos) return g_module && g_module->gold;
    if (name.find("iron_ore") != std::string::npos || name.find("deepslate_iron") != std::string::npos) return g_module && g_module->iron;
    if (name.find("copper_ore") != std::string::npos || name.find("deepslate_copper") != std::string::npos) return g_module && g_module->copper;
    if (name.find("coal_ore") != std::string::npos || name.find("deepslate_coal") != std::string::npos) return g_module && g_module->coal;
    if (name.find("redstone_ore") != std::string::npos || name.find("deepslate_redstone") != std::string::npos) return g_module && g_module->redstone;
    if (name.find("lapis_ore") != std::string::npos || name.find("deepslate_lapis") != std::string::npos) return g_module && g_module->lapis;
    if (name.find("nether_quartz") != std::string::npos) return g_module && g_module->quartz;
    if (name.find("ancient_debris") != std::string::npos) return g_module && g_module->ancientDebris;
    if (name.find("amethyst") != std::string::npos) return g_module && g_module->amethyst;
    if (name == "minecraft:obsidian" || name == "obsidian") return g_module && g_module->obsidian;
    return false;
}

static bool shouldRender(const void* block) {
    if (!block) return false;
    const auto* b = reinterpret_cast<const bedrocktools::sdk::Block*>(block);
    const auto* fullName = b->fullName();
    if (!fullName || fullName->empty()) return false;
    return isOre(*fullName);
}

template <size_t I>
void faceHook(void* a0, void* a1, const void* block, const Vec3Raw* pos, const void* tex) {
    auto& hook = g_hooks[I];
    if (!hook.original) return;
    if (!g_enabled.load(std::memory_order_relaxed)) {
        hook.original(a0, a1, block, pos, tex);
        return;
    }
    // Only emit selected blocks. This gives the classic ore-through-terrain X-ray effect
    // using the game's own block tessellation path; it does not alter networking.
    if (shouldRender(block)) hook.original(a0, a1, block, pos, tex);
}

constexpr std::array<bedrocktools::memory::SignatureId, 6> kSigIds = {
    bedrocktools::memory::SignatureId::BlockTessellatorTessellateFaceDown,
    bedrocktools::memory::SignatureId::BlockTessellatorTessellateFaceUp,
    bedrocktools::memory::SignatureId::BlockTessellatorTessellateFaceNorth,
    bedrocktools::memory::SignatureId::BlockTessellatorTessellateFaceSouth,
    bedrocktools::memory::SignatureId::BlockTessellatorTessellateFaceWest,
    bedrocktools::memory::SignatureId::BlockTessellatorTessellateFaceEast,
};

static bool rebuildRenderChunks(void* clientInstance) {
    if (!clientInstance || !g_setAllDirty) return false;
    void* levelRenderer = bedrocktools::sdk::field<void*>(clientInstance, bedrocktools::sdk::offsets::ClientInstance::mLevelRenderer);
    if (!levelRenderer) return false;
    void* node = bedrocktools::sdk::field<void*>(
        levelRenderer,
        bedrocktools::sdk::offsets::LevelRenderer::mRenderChunkCoordinators
            + bedrocktools::sdk::offsets::HashTable::mFirstNode
    );
    bool rebuilt = false;
    size_t visited = 0;
    while (node && visited++ < bedrocktools::sdk::offsets::RenderChunkCoordinator::MaxNodes) {
        void* next = bedrocktools::sdk::field<void*>(node, bedrocktools::sdk::offsets::HashNode::mNext);
        void* coordinator = bedrocktools::sdk::field<void*>(node, bedrocktools::sdk::offsets::HashNode::mValuePointer);
        if (coordinator) {
            g_setAllDirty(coordinator, true, false);
            rebuilt = true;
        }
        node = next;
    }
    return rebuilt;
}

static void* trampolineFor(size_t i) {
    switch (i) {
        case 0: return reinterpret_cast<void*>(&faceHook<0>);
        case 1: return reinterpret_cast<void*>(&faceHook<1>);
        case 2: return reinterpret_cast<void*>(&faceHook<2>);
        case 3: return reinterpret_cast<void*>(&faceHook<3>);
        case 4: return reinterpret_cast<void*>(&faceHook<4>);
        default: return reinterpret_cast<void*>(&faceHook<5>);
    }
}
}

XrayModule::XrayModule() : Module("Xray", "Hides normal terrain during block rendering and keeps selected resources visible.") {
    showInMenu = true;
    g_module = this;
}

XrayModule::~XrayModule() {
    g_enabled.store(false, std::memory_order_relaxed);
    if (g_module == this) g_module = nullptr;
}

void XrayModule::applyConfig() {}

void XrayModule::installHooks() {
    if (m_hooked) return;
    g_setAllDirty = reinterpret_cast<SetAllDirtyFn>(bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RenderChunkCoordinatorSetAllDirty));
    for (size_t i = 0; i < g_hooks.size(); ++i) {
        const auto address = bedrocktools::memory::resolve(kSigIds[i]);
        if (!address) continue;
        g_hooks[i].handle = bedrocktools::hooks::install(
            reinterpret_cast<void*>(address), trampolineFor(i), reinterpret_cast<void**>(&g_hooks[i].original));
    }
    m_hooked = true;
    for (const auto& h : g_hooks) m_hooked = m_hooked && h.handle && h.original;
}

void XrayModule::onInit() {
    installHooks();
    bedrocktools::events::bus().subscribe<bedrocktools::events::ClientInstanceUpdateEvent>([](auto& event) {
        if (g_rebuildPending.load(std::memory_order_acquire) && rebuildRenderChunks(event.clientInstance))
            g_rebuildPending.store(false, std::memory_order_release);
    });
}
void XrayModule::onEnable() {
    applyConfig();
    installHooks();
    g_enabled.store(m_hooked, std::memory_order_release);
    g_rebuildPending.store(m_hooked, std::memory_order_release);
}
void XrayModule::onDisable() { g_enabled.store(false, std::memory_order_release); g_rebuildPending.store(true, std::memory_order_release); }

void XrayModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    oresOnly = j.value("oresOnly", oresOnly);
    diamond = j.value("diamond", diamond);
    emerald = j.value("emerald", emerald);
    gold = j.value("gold", gold);
    iron = j.value("iron", iron);
    copper = j.value("copper", copper);
    coal = j.value("coal", coal);
    redstone = j.value("redstone", redstone);
    lapis = j.value("lapis", lapis);
    quartz = j.value("quartz", quartz);
    ancientDebris = j.value("ancientDebris", ancientDebris);
    amethyst = j.value("amethyst", amethyst);
    obsidian = j.value("obsidian", obsidian);
}

void XrayModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["oresOnly"] = oresOnly;
    j["diamond"] = diamond;
    j["emerald"] = emerald;
    j["gold"] = gold;
    j["iron"] = iron;
    j["copper"] = copper;
    j["coal"] = coal;
    j["redstone"] = redstone;
    j["lapis"] = lapis;
    j["quartz"] = quartz;
    j["ancientDebris"] = ancientDebris;
    j["amethyst"] = amethyst;
    j["obsidian"] = obsidian;
}
