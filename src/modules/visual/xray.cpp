#include "xray.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include "core/memory/Hooks.hpp"
#include <bedrocktools/sdk/Types.hpp>
#include <bedrocktools/sdk/render/Block.hpp>
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/ClientInstanceUpdateEvent.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include <atomic>
#include <array>
#include <string>
#include <algorithm>
#include <cctype>

namespace {
using namespace bedrocktools;
using Vec3 = sdk::Vec3;

struct FaceHook { hooks::Handle handle{}; void (*original)(void*, void*, const void*, const Vec3*, const void*){}; };
std::array<FaceHook, 6> g_faces{};
XrayModule* g_xray = nullptr;
std::atomic<bool> g_enabled{false};
std::atomic<bool> g_rebuild{false};
using DirtyFn = void(*)(void*, bool, bool);
DirtyFn g_setAllDirty = nullptr;

constexpr std::array<memory::SignatureId, 6> kFaces = {
    memory::SignatureId::BlockTessellatorTessellateFaceDown,
    memory::SignatureId::BlockTessellatorTessellateFaceUp,
    memory::SignatureId::BlockTessellatorTessellateFaceNorth,
    memory::SignatureId::BlockTessellatorTessellateFaceSouth,
    memory::SignatureId::BlockTessellatorTessellateFaceWest,
    memory::SignatureId::BlockTessellatorTessellateFaceEast,
};

static std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    return s;
}

static bool selected(const std::string& raw) {
    if (!g_xray) return false;
    const std::string n = lower(raw);
    if (n.find("diamond") != std::string::npos) return g_xray->diamond;
    if (n.find("iron_ore") != std::string::npos || n.find("deepslate_iron") != std::string::npos) return g_xray->iron;
    if (n.find("gold_ore") != std::string::npos || n.find("deepslate_gold") != std::string::npos || n.find("nether_gold") != std::string::npos) return g_xray->gold;
    if (n.find("coal_ore") != std::string::npos || n.find("deepslate_coal") != std::string::npos) return g_xray->coal;
    if (n.find("copper_ore") != std::string::npos || n.find("deepslate_copper") != std::string::npos) return g_xray->copper;
    if (n.find("lapis_ore") != std::string::npos || n.find("deepslate_lapis") != std::string::npos) return g_xray->lapis;
    if (n.find("emerald") != std::string::npos) return g_xray->emerald;
    if (n.find("redstone_ore") != std::string::npos || n.find("deepslate_redstone") != std::string::npos) return g_xray->redstone;
    if (n.find("amethyst") != std::string::npos) return g_xray->amethyst;
    if (n.find("ancient_debris") != std::string::npos || n.find("netherite_block") != std::string::npos) return g_xray->netherite;
    if (n.find("quartz") != std::string::npos) return g_xray->quartz;
    if (n == "minecraft:obsidian" || n == "tile.obsidian" || n == "obsidian") return g_xray->obsidian;
    if (n.find("barrel") != std::string::npos) return g_xray->barrel;
    return false;
}

static bool renderable(const void* block) {
    if (!block) return false;
    const auto* b = reinterpret_cast<const sdk::Block*>(block);
    const auto* name = b->fullName();
    return name && !name->empty() && selected(*name);
}

template <size_t I>
void face(void* a0, void* a1, const void* block, const Vec3* pos, const void* tex) {
    auto& h = g_faces[I];
    if (!h.original) return;
    if (!g_enabled.load(std::memory_order_relaxed) || renderable(block))
        h.original(a0, a1, block, pos, tex);
}

static void* trampoline(size_t i) {
    switch(i) {
        case 0: return (void*)&face<0>;
        case 1: return (void*)&face<1>;
        case 2: return (void*)&face<2>;
        case 3: return (void*)&face<3>;
        case 4: return (void*)&face<4>;
        default: return (void*)&face<5>;
    }
}

static void requestRebuild() { g_rebuild.store(true, std::memory_order_release); }
static void tryRebuild(void* client) {
    if (!client || !g_setAllDirty) return;
    // The current BedrockTools offsets expose the coordinator hash table. Marking
    // every coordinator dirty reproduces the instant refresh used by Apollon.
    void* renderer = sdk::field<void*>(client, sdk::offsets::ClientInstance::mLevelRenderer);
    if (!renderer) return;
    void* node = sdk::field<void*>(renderer, sdk::offsets::LevelRenderer::mRenderChunkCoordinators + sdk::offsets::HashTable::mFirstNode);
    size_t count = 0;
    while (node && count++ < sdk::offsets::RenderChunkCoordinator::MaxNodes) {
        void* next = sdk::field<void*>(node, sdk::offsets::HashNode::mNext);
        void* coordinator = sdk::field<void*>(node, sdk::offsets::HashNode::mValuePointer);
        if (coordinator) g_setAllDirty(coordinator, true, false);
        node = next;
    }
    g_rebuild.store(false, std::memory_order_release);
}
}

XrayModule::XrayModule() : Module("Xray", "Apollon-style resource Xray: hides normal terrain and keeps selected resources visible.") { showInMenu = true; g_xray = this; }
XrayModule::~XrayModule() { g_enabled.store(false); if (g_xray == this) g_xray = nullptr; }

void XrayModule::installHooks() {
    if (m_hooked) return;
    g_setAllDirty = reinterpret_cast<DirtyFn>(memory::resolve(memory::SignatureId::RenderChunkCoordinatorSetAllDirty));
    size_t ok = 0;
    for (size_t i = 0; i < g_faces.size(); ++i) {
        uintptr_t address = memory::resolve(kFaces[i]);
        if (!address) continue;
        g_faces[i].handle = hooks::install(reinterpret_cast<void*>(address), trampoline(i), reinterpret_cast<void**>(&g_faces[i].original));
        if (g_faces[i].handle && g_faces[i].original) ++ok;
    }
    m_hooked = ok == g_faces.size();
}

void XrayModule::onInit() {
    installHooks();
    events::bus().subscribe<events::ClientInstanceUpdateEvent>([](auto& event){
        if (g_rebuild.load(std::memory_order_acquire)) tryRebuild(event.clientInstance);
    });
}
void XrayModule::onEnable() { installHooks(); g_enabled.store(m_hooked, std::memory_order_release); requestRebuild(); }
void XrayModule::onDisable() { g_enabled.store(false, std::memory_order_release); requestRebuild(); }

void XrayModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    diamond=j.value("diamond",diamond); iron=j.value("iron",iron); gold=j.value("gold",gold); coal=j.value("coal",coal);
    copper=j.value("copper",copper); lapis=j.value("lapis",lapis); emerald=j.value("emerald",emerald); redstone=j.value("redstone",redstone);
    amethyst=j.value("amethyst",amethyst); netherite=j.value("netherite",netherite); quartz=j.value("quartz",quartz); obsidian=j.value("obsidian",obsidian); barrel=j.value("barrel",barrel);
}
void XrayModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["diamond"]=diamond; j["iron"]=iron; j["gold"]=gold; j["coal"]=coal; j["copper"]=copper; j["lapis"]=lapis; j["emerald"]=emerald; j["redstone"]=redstone;
    j["amethyst"]=amethyst; j["netherite"]=netherite; j["quartz"]=quartz; j["obsidian"]=obsidian; j["barrel"]=barrel;
}
