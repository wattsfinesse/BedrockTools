#include "oreoutline.hpp"
#include <GLES3/gl3.h>
#include "core/memory/Hooks.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/render/Block.hpp>
#include <bedrocktools/events/EventBus.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <array>
#include <vector>

namespace {
struct BlockPos { int x, y, z; };
using BeginFn = void(*)(void*, void*, int, int, int);
using ColorFn = void(*)(void*, float, float, float, float);
using VertexFn = void(*)(void*, float, float, float);
using RenderFn = void(*)(void*, void*, void*, char*);
using GetBlockFn = void*(*)(void*, const BlockPos&);

struct HashedString { uint64_t hash; std::string value; mutable const HashedString* last; };
struct MaterialPtr { void* data[2]{nullptr,nullptr}; explicit operator bool() const { return data[0] != nullptr; } };

OreOutlineModule* g_module = nullptr;
void* g_player = nullptr;
bedrocktools::sdk::Vec3 g_pos{};
BeginFn beginFn = nullptr; ColorFn colorFn = nullptr; VertexFn vertexFn = nullptr; RenderFn renderFn = nullptr; GetBlockFn getBlockFn = nullptr;
void* originalRenderLevel = nullptr;
uintptr_t materialGroup = 0;
MaterialPtr outlineMaterial{};

uintptr_t resolveADRP(uint32_t* insns, size_t count, uint32_t reg) {
    for (size_t i=0;i<count;i++) { uint32_t insn=insns[i]; if ((insn&0x1F)!=reg) continue;
        if ((insn&0x9F000000)==0x90000000) {
            uintptr_t page=((uintptr_t)&insns[i]&~0xFFFULL)+((int64_t)((uint64_t)(((insn>>3)&0x1FFFFC)|((insn>>29)&3))<<43)>>31);
            for(size_t j=i+1;j<count;j++){uint32_t a=insns[j]; if((a&0xFF000000)==0x91000000&&((a>>5)&0x1F)==reg&&(a&0x1F)==reg){uint32_t imm=(a>>10)&0xFFF;if(a&0x400000)imm<<=12;return page+imm;} if((a&0x1F)==reg)break;}
        }
    } return 0;
}
void* getMaterial(const char* name) {
    if(!materialGroup) return nullptr;
    HashedString hs{}; hs.value=name; hs.last=nullptr;
    constexpr uint64_t off=0xCBF29CE484222325ULL, prime=0x100000001B3ULL; uint64_t h=off;
    for(unsigned char c:hs.value) h=static_cast<uint64_t>(c)^(prime*h); hs.hash=h;
    void** vt=*reinterpret_cast<void***>(materialGroup); if(!vt||!vt[bedrocktools::sdk::offsets::VTable::RenderMaterialGroup_getMaterial]) return nullptr;
    using Fn=MaterialPtr(*)(void*,const HashedString*);
    MaterialPtr result=reinterpret_cast<Fn>(vt[bedrocktools::sdk::offsets::VTable::RenderMaterialGroup_getMaterial])((void*)materialGroup,&hs);
    if(!result) return nullptr;
    outlineMaterial=result; return &outlineMaterial;
}

uint32_t oreColor(const std::string& n) {
    if(n.find("diamond_ore")!=std::string::npos) return 0xFF42E8F5;
    if(n.find("emerald_ore")!=std::string::npos) return 0xFF35D66F;
    if(n.find("gold_ore")!=std::string::npos) return 0xFFFFD447;
    if(n.find("iron_ore")!=std::string::npos) return 0xFFD9B49A;
    if(n.find("copper_ore")!=std::string::npos) return 0xFFE58B55;
    if(n.find("coal_ore")!=std::string::npos) return 0xFF777777;
    if(n.find("redstone_ore")!=std::string::npos) return 0xFFFF4545;
    if(n.find("lapis_ore")!=std::string::npos) return 0xFF4169E1;
    if(n.find("quartz_ore")!=std::string::npos) return 0xFFFFF0DA;
    if(n.find("ancient_debris")!=std::string::npos) return 0xFFB9785A;
    return 0;
}
void addCubeLines(std::vector<bedrocktools::sdk::Vec3>& vertices, int x,int y,int z,float cx,float cy,float cz) {
    const bedrocktools::sdk::Vec3 p[8]={{(float)x-cx,(float)y-cy,(float)z-cz},{x+1.f-cx,(float)y-cy,(float)z-cz},{x+1.f-cx,y+1.f-cy,(float)z-cz},{(float)x-cx,y+1.f-cy,(float)z-cz},
    {(float)x-cx,(float)y-cy,z+1.f-cz},{x+1.f-cx,(float)y-cy,z+1.f-cz},{x+1.f-cx,y+1.f-cy,z+1.f-cz},{(float)x-cx,y+1.f-cy,z+1.f-cz}};
    constexpr int e[12][2]={{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
    for(auto& edge:e){vertices.push_back(p[edge[0]]);vertices.push_back(p[edge[1]]);}
}
void renderHook(void* self, void* screenContext, void* a3) {
    using Orig=void(*)(void*,void*,void*); auto orig=reinterpret_cast<Orig>(originalRenderLevel);
    if(orig) orig(self,screenContext,a3);
    if(!g_module||!g_module->enabled||!g_player||!screenContext||!beginFn||!colorFn||!vertexFn||!renderFn||!getBlockFn) return;
    uintptr_t tessPtr=*(uintptr_t*)((uintptr_t)screenContext+bedrocktools::sdk::offsets::ScreenContext::mTessellator);
    uintptr_t lrp=*(uintptr_t*)((uintptr_t)self+bedrocktools::sdk::offsets::LevelRenderer::mLevelRendererPlayer);
    if(!tessPtr||tessPtr<0x1000||!lrp||lrp<0x1000) return;
    auto* tess=(void*)tessPtr;
    float cx=*(float*)(lrp+bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos), cy=*(float*)(lrp+bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos+4), cz=*(float*)(lrp+bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos+8);
    uintptr_t dim=*(uintptr_t*)((uintptr_t)g_player+bedrocktools::sdk::offsets::Actor::mDimension); if(!dim) return;
    uintptr_t region=*(uintptr_t*)(dim+bedrocktools::sdk::offsets::Dimension::mBlockSource); if(!region||region<0x1000) return;
    if(!outlineMaterial) getMaterial("selection_box");
    void* mat=outlineMaterial? (void*)&outlineMaterial : (void*)(lrp+bedrocktools::sdk::offsets::LevelRendererPlayer::mSelectionOverlayMaterial);
    int r=std::clamp(g_module->radius,4,32); BlockPos center{(int)std::floor(g_pos.x),(int)std::floor(g_pos.y),(int)std::floor(g_pos.z)};
    constexpr uint32_t colors[] = {0xFF42E8F5,0xFF35D66F,0xFFFFD447,0xFFD9B49A,0xFFE58B55,0xFF777777,0xFFFF4545,0xFF4169E1,0xFFFFF0DA,0xFFB9785A};
    std::array<std::vector<bedrocktools::sdk::Vec3>,10> batches;
    for(int x=-r;x<=r;x++) for(int y=-r;y<=r;y++) for(int z=-r;z<=r;z++) {
        BlockPos bp{center.x+x,center.y+y,center.z+z}; void* raw=getBlockFn((void*)region,bp); if(!raw|| (uintptr_t)raw<0x1000) continue;
        auto* block=reinterpret_cast<bedrocktools::sdk::Block*>(raw); const std::string* full=block->fullName(); if(!full||full->empty()) continue;
        uint32_t col=oreColor(*full); if(!col) continue;
        size_t index=0; while(index<10 && colors[index]!=col) ++index; if(index<10) addCubeLines(batches[index],bp.x,bp.y,bp.z,cx,cy,cz);
    }
    bool any=false; for(const auto& batch:batches) if(!batch.empty()){any=true;break;} if(!any) return;
    uintptr_t ch=*(uintptr_t*)((uintptr_t)screenContext+bedrocktools::sdk::offsets::ScreenContext::mColorHolder); float saved[4]{}; float* holder=nullptr;
    if(ch&&ch>=0x1000){holder=(float*)ch;std::copy(holder,holder+4,saved);std::fill(holder,holder+4,1.f);}
    // This hook runs on the active world-render thread. Disable only depth testing
    // while submitting our line batches so terrain cannot occlude ore outlines.
    // Restore the previous GL state immediately afterwards to avoid affecting the HUD.
    const GLboolean depthWasEnabled = glIsEnabled(GL_DEPTH_TEST);
    glDisable(GL_DEPTH_TEST);
    for(size_t i=0;i<batches.size();++i){auto& batch=batches[i];if(batch.empty())continue;auto col=colors[i];colorFn(tess,((col>>16)&255)/255.f,((col>>8)&255)/255.f,(col&255)/255.f,((col>>24)&255)/255.f);beginFn(tess,nullptr,4,(int)batch.size(),0);for(auto& v:batch)vertexFn(tess,v.x,v.y,v.z);char pad[0x58]{};renderFn(screenContext,tess,mat,pad);}
    if(depthWasEnabled) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if(holder)std::copy(saved,saved+4,holder);
}
void tick(void* player){g_player=player;if(!player)return;uintptr_t sv=*(uintptr_t*)((uintptr_t)player+bedrocktools::sdk::offsets::Actor::mStateVectorComponent);if(sv)g_pos=*(bedrocktools::sdk::Vec3*)sv;}
}

OreOutlineModule::OreOutlineModule():Module("Ore Outline","Draws colored wireframe outlines around nearby ores through terrain." ){showInMenu=true;g_module=this;}
OreOutlineModule::~OreOutlineModule(){if(g_module==this)g_module=nullptr;}
void OreOutlineModule::onInit(){m_patchTarget=(void*)bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RenderLevel);beginFn=(BeginFn)bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorBegin);colorFn=(ColorFn)bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorColor);vertexFn=(VertexFn)bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorVertex);renderFn=(RenderFn)bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::MeshHelpersRenderMeshImmediately2);if(!renderFn)renderFn=(RenderFn)bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::MeshHelpersRenderMeshImmediately);getBlockFn=(GetBlockFn)bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::BlockSourceGetBlock);uintptr_t rg=bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RenderMaterialGroupCommon);if(rg){uintptr_t group=resolveADRP((uint32_t*)rg,2,0);if(group)materialGroup=group+bedrocktools::sdk::offsets::MaterialGroup::mRenderMaterialGroupOffset;}bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>([](auto& e){tick(e.player);});}
void OreOutlineModule::applyPatch(){if(m_patched||!m_patchTarget)return;bedrocktools::hooks::install(m_patchTarget,(void*)renderHook,&originalRenderLevel);m_patched=true;}
void OreOutlineModule::onEnable(){applyPatch();}
void OreOutlineModule::onDisable(){}
void OreOutlineModule::loadConfig(const nlohmann::json& j){Module::loadConfig(j);radius=j.value("radius",radius);}
void OreOutlineModule::saveConfig(nlohmann::json& j){Module::saveConfig(j);j["radius"]=radius;}
