#include "thirdpersonnametag.hpp"
#include "core/memory/Hooks.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Types.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <bedrocktools/sdk/world/Level.hpp>

#include <entt/entt.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace {

using GetRuntimeActorList_t = std::vector<void*>(*)(void*);
using ActorIsPlayer_t = bool(*)(void*);
using ActorGetNameTag_t = std::string(*)(void*);

static GetRuntimeActorList_t s_getRuntimeActorList = nullptr;
static ActorIsPlayer_t s_actorIsPlayer = nullptr;
static ActorGetNameTag_t s_actorGetNameTagOrig = nullptr;
static ThirdPersonNametagModule* g_nametagMod = nullptr;

// This is the same EntityContext layout already used by Keystrokes and the
// Health Indicator module in BedrockTools.
enum class EntityId : uint32_t {};

struct EntityIdTraits {
    using value_type = EntityId;
    using entity_type = uint32_t;
    using version_type = uint16_t;
    static constexpr uint32_t entity_mask = 0x3FFFF;
    static constexpr uint32_t version_mask = 0x3FFF;
};

} // namespace

namespace entt {
template<>
struct entt_traits<EntityId> : basic_entt_traits<EntityIdTraits> {
    static constexpr std::size_t page_size = ENTT_SPARSE_PAGE;
};
} // namespace entt

namespace {

struct EntityContext {
    void* mRegistry;
    entt::basic_registry<EntityId>& mEnTTRegistry;
    EntityId mEntity;

    entt::basic_registry<EntityId>& registry() { return mEnTTRegistry; }
};

struct HealthSample {
    float current = 0.0f;
    float maximum = 0.0f;
    bool valid = false;
};

struct HealthLayout {
    enum class Kind : uint8_t { None, AttributeMap, RawPair };

    Kind kind = Kind::None;
    uint32_t typeHash = 0;
    int currentOffset = -1;
    int maxOffset = -1;
    bool floatValues = true;
    bool valid = false;
};

static HealthLayout s_layout;

static bool isFiniteHealth(float value) {
    return std::isfinite(value) && value >= 0.0f && value <= 10000.0f;
}

static int scoreCandidate(float current, float maximum, int offset) {
    if (!isFiniteHealth(current) || !isFiniteHealth(maximum)) return -100000;
    if (maximum < 0.5f || maximum > 1000.0f) return -100000;
    if (current < -0.001f || current > maximum + 0.25f) return -100000;

    int score = 0;
    if (offset == 0) score += 40;
    else if (offset == 4) score += 30;
    else if (offset <= 16) score += 15;

    for (float common : {20.0f, 30.0f, 40.0f, 100.0f}) {
        if (std::fabs(maximum - common) < 0.01f) score += 25;
    }

    if (std::fabs(current - std::round(current)) < 0.01f) score += 5;
    if (std::fabs(maximum - std::round(maximum)) < 0.01f) score += 5;
    return score;
}

// ---------------------------------------------------------------------------
// Health reading. Modern Bedrock clients no longer keep a dedicated
// "HealthComponent" in the client ECS - health lives inside
// AttributesComponent, whose only member is a BaseAttributeMap:
//
//   BaseAttributeMap {
//       +0x00 std::vector<uint> keys   (attribute id values)
//       +0x18 std::vector<AttributeInstance> values
//       ...
//   }
//
//   AttributeInstance (0x80 bytes on 64-bit) {
//       +0x00 vptr
//       +0x08 const Attribute* mAttribute
//       ...
//       last 24 bytes: float mDefaultValues[3]  (min, max, default)
//                      float mCurrentValues[3]  (min, max, current)
//   }
//
// This is the same parser the Health Indicator module uses.
// ---------------------------------------------------------------------------

constexpr std::size_t kAttrKeysVectorOffset = 0x00;
constexpr std::size_t kAttrValuesVectorOffset = 0x18;
constexpr std::size_t kAttributePtrOffset = 0x08;
constexpr std::size_t kAttributeIdOffset = 0x04;
constexpr std::size_t kAttributeNameStringOffset = 0x10; // HashedString hash(8) + string
constexpr uint32_t kHealthAttributeId = 7;               // legacy "minecraft:health" id
constexpr std::size_t kMaxAttributeCount = 64;
constexpr std::size_t kMinInstanceStride = 0x40;
constexpr std::size_t kMaxInstanceStride = 0x200;

template <typename T>
static T readAt(const void* base, std::size_t offset) {
    T value{};
    std::memcpy(&value, static_cast<const std::byte*>(base) + offset, sizeof(T));
    return value;
}

static bool readHealthFromAttributeMap(const void* component, HealthSample& out) {
    if (!component) return false;

    const auto keysBegin = readAt<uintptr_t>(component, kAttrKeysVectorOffset);
    const auto keysEnd = readAt<uintptr_t>(component, kAttrKeysVectorOffset + sizeof(void*));
    const auto valuesBegin = readAt<uintptr_t>(component, kAttrValuesVectorOffset);
    const auto valuesEnd = readAt<uintptr_t>(component, kAttrValuesVectorOffset + sizeof(void*));

    if (!keysBegin || !valuesBegin || keysEnd <= keysBegin || valuesEnd <= valuesBegin) return false;

    const std::size_t count = (keysEnd - keysBegin) / sizeof(uint32_t);
    if (count == 0 || count > kMaxAttributeCount) return false;

    const std::size_t stride = (valuesEnd - valuesBegin) / count;
    if (stride < kMinInstanceStride || stride > kMaxInstanceStride || stride % 8 != 0) return false;
    if (valuesBegin + stride * count != valuesEnd) return false;

    for (std::size_t i = 0; i < count; ++i) {
        const void* instance = reinterpret_cast<const void*>(valuesBegin + stride * i);
        const auto* attribute = readAt<const std::byte*>(instance, kAttributePtrOffset);
        if (!attribute) continue;

        bool isHealth = false;
        const auto* name = reinterpret_cast<const std::string*>(attribute + kAttributeNameStringOffset);
        if (name->size() > 0 && name->size() < 64) {
            isHealth = (*name == "minecraft:health");
        }
        if (!isHealth) {
            // Fall back to the well-known legacy id in case the name string
            // moved between releases.
            const auto id = readAt<uint32_t>(attribute, kAttributeIdOffset);
            isHealth = (id == kHealthAttributeId);
        }
        if (!isHealth) continue;

        // mCurrentValues[3] = {min, max, current} are the last three floats
        // of the instance.
        const float maximum = readAt<float>(instance, stride - 2 * sizeof(float));
        const float current = readAt<float>(instance, stride - sizeof(float));
        if (!isFiniteHealth(current) || !isFiniteHealth(maximum) || maximum <= 0.0f) return false;

        out.current = std::clamp(current, 0.0f, maximum);
        out.maximum = maximum;
        out.valid = true;
        return true;
    }
    return false;
}

static bool findHealthLayout(entt::basic_registry<EntityId>& registry, EntityId entity, HealthLayout& out) {
    int bestScore = -100000;
    HealthLayout best{};

    for (auto&& [typeId, storage] : registry.storage()) {
        (void)typeId;
        const auto name = storage.type().name();
        if (name.empty()) continue;
        if (!storage.contains(entity)) continue;

        const auto* raw = storage.value(entity);
        if (!raw) continue;

        // Preferred: modern clients store health inside AttributesComponent.
        if (name.find("AttributesComponent") != std::string_view::npos) {
            HealthSample sample{};
            if (readHealthFromAttributeMap(raw, sample)) {
                out = {};
                out.kind = HealthLayout::Kind::AttributeMap;
                out.typeHash = storage.type().hash();
                out.valid = true;
                return true;
            }
            continue;
        }

        std::string lower(name.begin(), name.end());
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (lower.find("health") == std::string::npos) continue;

        // Legacy fallback: a small dedicated health component. Try both float
        // and integer pairs while keeping all reads inside the first 32 bytes.
        for (int off = 0; off <= 24; off += 4) {
            float current = 0.0f, maximum = 0.0f;
            std::memcpy(&current, static_cast<const std::byte*>(raw) + off, sizeof(float));
            std::memcpy(&maximum, static_cast<const std::byte*>(raw) + off + 4, sizeof(float));
            int score = scoreCandidate(current, maximum, off);
            if (score > bestScore) {
                bestScore = score;
                best = {};
                best.kind = HealthLayout::Kind::RawPair;
                best.typeHash = storage.type().hash();
                best.currentOffset = off;
                best.maxOffset = off + 4;
                best.floatValues = true;
                best.valid = true;
            }

            int32_t icurrent = 0, imaximum = 0;
            std::memcpy(&icurrent, static_cast<const std::byte*>(raw) + off, sizeof(icurrent));
            std::memcpy(&imaximum, static_cast<const std::byte*>(raw) + off + 4, sizeof(imaximum));
            score = scoreCandidate(static_cast<float>(icurrent), static_cast<float>(imaximum), off) - 3;
            if (score > bestScore) {
                bestScore = score;
                best = {};
                best.kind = HealthLayout::Kind::RawPair;
                best.typeHash = storage.type().hash();
                best.currentOffset = off;
                best.maxOffset = off + 4;
                best.floatValues = false;
                best.valid = true;
            }
        }
    }

    if (best.kind == HealthLayout::Kind::RawPair && bestScore < 30) return false;
    if (!best.valid) return false;
    out = best;
    return true;
}

static HealthSample readHealth(void* actor) {
    if (!actor) return {};

    auto* ctx = reinterpret_cast<EntityContext*>(reinterpret_cast<uintptr_t>(actor) + bedrocktools::sdk::offsets::Actor::mEntityContext);
    if (!ctx) return {};

    auto& registry = ctx->registry();
    const EntityId entity = ctx->mEntity;

    entt::basic_sparse_set<EntityId>* healthStorage = nullptr;
    if (s_layout.valid) {
        for (auto&& [typeId, storage] : registry.storage()) {
            (void)typeId;
            if (storage.type().hash() == s_layout.typeHash && storage.contains(entity)) {
                healthStorage = &storage;
                break;
            }
        }
    }

    if (!healthStorage) {
        HealthLayout discovered{};
        if (!findHealthLayout(registry, entity, discovered)) return {};
        s_layout = discovered;
        for (auto&& [typeId, storage] : registry.storage()) {
            (void)typeId;
            if (storage.type().hash() == s_layout.typeHash && storage.contains(entity)) {
                healthStorage = &storage;
                break;
            }
        }
    }

    if (!healthStorage) return {};
    const auto* raw = healthStorage->value(entity);
    if (!raw) return {};

    HealthSample sample{};
    if (s_layout.kind == HealthLayout::Kind::AttributeMap) {
        if (!readHealthFromAttributeMap(raw, sample)) {
            // Layout stopped matching (e.g. new game version) - rediscover next tick.
            s_layout = {};
            return {};
        }
        return sample;
    }

    if (s_layout.floatValues) {
        std::memcpy(&sample.current, static_cast<const std::byte*>(raw) + s_layout.currentOffset, sizeof(float));
        std::memcpy(&sample.maximum, static_cast<const std::byte*>(raw) + s_layout.maxOffset, sizeof(float));
    } else {
        int32_t current = 0, maximum = 0;
        std::memcpy(&current, static_cast<const std::byte*>(raw) + s_layout.currentOffset, sizeof(current));
        std::memcpy(&maximum, static_cast<const std::byte*>(raw) + s_layout.maxOffset, sizeof(maximum));
        sample.current = static_cast<float>(current);
        sample.maximum = static_cast<float>(maximum);
    }

    sample.current = std::clamp(sample.current, 0.0f, sample.maximum);
    sample.valid = isFiniteHealth(sample.current) && isFiniteHealth(sample.maximum) && sample.maximum > 0.0f;
    return sample;
}

static float distanceSquared(const bedrocktools::sdk::Vec3& a, const bedrocktools::sdk::Vec3& b) {
    const float x = a.x - b.x;
    const float y = a.y - b.y;
    const float z = a.z - b.z;
    return x * x + y * y + z * z;
}

// ---------------------------------------------------------------------------
// "Show Health" option: hook Actor::getNameTag() (the string the vanilla
// nametag renderer displays) and append a red heart + current HP for every
// player - other players always, and the local player as soon as its nametag
// is visible (i.e. third person view thanks to the module's own patch).
// ---------------------------------------------------------------------------

// " §c❤ §f" – space, red heart, space, white text.
const std::string kHealthSuffixMarker = " \xC2\xA7" "c" "\xE2\x9D\xA4 \xC2\xA7" "f";

static std::string getNameTag_hook(void* actor) {
    std::string result;
    if (s_actorGetNameTagOrig) result = s_actorGetNameTagOrig(actor);

    if (!g_nametagMod || !g_nametagMod->enabled) return result;
    if (!s_actorIsPlayer || !s_actorIsPlayer(actor)) return result;

    const bool showName = g_nametagMod->isShowName();
    const bool showHealth = g_nametagMod->isShowHealth();

    if (showName && !showHealth) return result;        // plain vanilla nametag
    if (!showName && !showHealth) return std::string(); // everything hidden

    const float hp = g_nametagMod->healthForActor(actor);
    if (hp < 0.0f) {
        // Health could not be read: keep the plain name (or hide when names off).
        return showName ? result : std::string();
    }

    std::string out;
    if (showName) out = result;
    out += kHealthSuffixMarker;
    out += std::to_string(static_cast<int>(std::round(hp)));
    return out;
}

static void nametagTickCallback(void* localPlayer) {
    if (!g_nametagMod || !g_nametagMod->enabled) return;
    g_nametagMod->onLocalPlayerTick(localPlayer);
}

} // namespace

ThirdPersonNametagModule::ThirdPersonNametagModule() : Module("Third Person Nametag", "Shows your own nametag in third person view, with an optional live health display on player nametags.") {
    m_patched = false;
    m_patchTarget = nullptr;
    g_nametagMod = this;
}

ThirdPersonNametagModule::~ThirdPersonNametagModule() {
    removePatch();
    if (g_nametagMod == this) g_nametagMod = nullptr;
}

void ThirdPersonNametagModule::onInit() {
    if (!m_patchTarget) {
        uintptr_t addr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::Nametag);
        if (addr != 0) {
            m_patchTarget = (void*)(addr + bedrocktools::sdk::offsets::NameTag::mExtractNameTagsPatchOffset);
            memcpy(m_originalBytes, m_patchTarget, 4);
        }
    }

    s_getRuntimeActorList = reinterpret_cast<GetRuntimeActorList_t>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorManagerList));
    s_actorIsPlayer = reinterpret_cast<ActorIsPlayer_t>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorIsPlayer));

    if (!m_nameTagHooked) {
        uintptr_t nameTagAddr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorGetNameTag);
        if (nameTagAddr != 0) {
            bedrocktools::hooks::install((void*)nameTagAddr, (void*)getNameTag_hook, (void**)&s_actorGetNameTagOrig);
            m_nameTagHooked = true;
        }
    }

    if (!m_tickSubscribed) {
        bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
            [](auto& event) { nametagTickCallback(reinterpret_cast<void*>(event.player)); });
        m_tickSubscribed = true;
    }
}

void ThirdPersonNametagModule::applyPatch() {
    if (m_patched || !m_patchTarget) return;
    uint32_t nop = 0xD503201F;
    bedrocktools::sdk::patchMemory(m_patchTarget, &nop, 4);
    m_patched = true;
}

void ThirdPersonNametagModule::removePatch() {
    if (!m_patched || !m_patchTarget) return;
    bedrocktools::sdk::patchMemory(m_patchTarget, m_originalBytes, 4);
    m_patched = false;
}

void ThirdPersonNametagModule::onEnable() {
    applyPatch();
    s_layout = {};
}

void ThirdPersonNametagModule::onDisable() {
    removePatch();
    std::lock_guard<std::mutex> lock(m_healthMutex);
    m_healthCache.clear();
}

void ThirdPersonNametagModule::onLocalPlayerTick(void* localPlayer) {
    if (!localPlayer || !s_getRuntimeActorList) return;

    auto* localActor = reinterpret_cast<bedrocktools::sdk::Actor*>(localPlayer);
    auto* level = localActor->level();
    if (!level) return;
    auto* manager = level->actorManager();
    if (!manager) return;

    const auto actors = s_getRuntimeActorList(manager);
    const auto localPos = localActor->position();

    // Pre-filter on distance so the cache stays small and only contains
    // players that could actually enter the 64-block nametag render range.
    constexpr float prefilterDistance = 64.0f;
    constexpr float prefilterDistanceSq = prefilterDistance * prefilterDistance;

    std::unordered_map<void*, float> next;
    next.reserve(actors.size());

    for (void* actor : actors) {
        if (!actor) continue;

        auto* a = reinterpret_cast<bedrocktools::sdk::Actor*>(actor);
        const bool isSelf = (actor == localPlayer);

        if (!isSelf) {
            if (!s_actorIsPlayer || !s_actorIsPlayer(actor)) continue;
            const auto position = a->position();
            if (distanceSquared(position, localPos) > prefilterDistanceSq) continue;
        }

        const HealthSample sample = readHealth(actor);
        if (sample.valid) next[actor] = sample.current;
    }

    std::lock_guard<std::mutex> lock(m_healthMutex);
    m_healthCache.swap(next);
}

float ThirdPersonNametagModule::healthForActor(void* actor) {
    if (!actor) return -1.0f;
    std::lock_guard<std::mutex> lock(m_healthMutex);
    const auto it = m_healthCache.find(actor);
    return it != m_healthCache.end() ? it->second : -1.0f;
}

void ThirdPersonNametagModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("m_name")) m_name = j["m_name"].get<bool>();
    if (j.contains("m_showHealth")) m_showHealth = j["m_showHealth"].get<bool>();
}

void ThirdPersonNametagModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["m_name"] = m_name;
    j["m_showHealth"] = m_showHealth;
}
