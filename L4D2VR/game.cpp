#include "game.h"
#include <Windows.h>
#include <iostream>
#include "sdk.h"
#include "vr.h"
#include "hooks.h"
#include "offsets.h"
#include "sigscanner.h"
#include "netvars.h"
#include "body.h"
#include "haptics.h"
#include "cinematic.h"
#include "interact.h"

Game::Game()
{
    while (!(m_BaseClient = (uintptr_t)GetModuleHandle("client.dll")))
        Sleep(50);
    while (!(m_BaseEngine = (uintptr_t)GetModuleHandle("engine.dll")))
        Sleep(50);
    while (!(m_BaseMaterialSystem = (uintptr_t)GetModuleHandle("materialsystem.dll")))
        Sleep(50);
    while (!(m_BaseServer = (uintptr_t)GetModuleHandle("server.dll")))
        Sleep(50);
    while (!(m_BaseVgui2 = (uintptr_t)GetModuleHandle("vgui2.dll")))
        Sleep(50);

    m_ClientEntityList = (IClientEntityList *)GetInterface("client.dll", "VClientEntityList003");
    m_EngineTrace = (IEngineTrace *)GetInterface("engine.dll", "EngineTraceClient004");
    m_EngineClient = (IEngineClient *)GetInterface("engine.dll", "VEngineClient015");
    m_MaterialSystem = (IMaterialSystem *)GetInterface("MaterialSystem.dll", "VMaterialSystem080");
    m_ClientViewRender = (IViewRender *)GetInterface("client.dll", "VEngineRenderView013");
    m_EngineViewRender = (IViewRender *)GetInterface("engine.dll", "VEngineRenderView013");
    m_ModelInfo = (IModelInfo *)GetInterface("engine.dll", "VModelInfoClient004");
    m_ModelRender = (IModelRender *)GetInterface("engine.dll", "VEngineModel016");
    m_VguiInput = (IInput *)GetInterface("vgui2.dll", "VGUI_InputInternal001");
    m_VguiSurface = (ISurface *)GetInterface("vguimatsurface.dll", "VGUI_Surface031");
    m_BaseClientDll = (IBaseClientDLL *)GetInterface("client.dll", "VClient016");

    m_Offsets = new Offsets();

    m_ClientMode = **(IClientMode***)(m_Offsets->g_pClientMode.address);

    std::cout << "Interfaces ready\n";
    if (m_BaseClientDll)
        Netvars::Init(m_BaseClientDll);

    m_VR = new VR(this);
    std::cout << "VR " << (m_VR->m_IsInitialized ? "ready" : "failed") << "\n";
    m_Hooks = new Hooks(this);
    std::cout << "Hooks ready\n";

    if (m_VR->m_IsInitialized)
    {
        Body::Install(this, m_VR);
        Haptics::Install(this, m_VR);
        Cinematic::Install(this, m_VR);
        Interact::Install(this, m_VR);
        m_VR->ApplyFeatureSettings();
        std::cout << "Body, haptics, interactions and cutscene camera ready\n";
    }

    m_Initialized = true;
}

void *Game::GetInterface(const char *dllname, const char *interfacename)
{
    tCreateInterface CreateInterface = (tCreateInterface)GetProcAddress(GetModuleHandle(dllname), "CreateInterface");

    int returnCode = 0;
    void *createdInterface = CreateInterface(interfacename, &returnCode);

    return createdInterface;
}

void Game::errorMsg(const char *msg)
{
    MessageBox(0, msg, "Portal 2 VR", MB_ICONERROR | MB_OK);
}

CBaseEntity *Game::GetClientEntity(int entityIndex)
{
    return (CBaseEntity *)(m_ClientEntityList->GetClientEntity(entityIndex));
}

void *Game::GetClientEntityFromHandle(int handle)
{
    constexpr int kEntityFromHandleSlot = 4;
    using tEntityFromHandle = void *(__thiscall *)(IClientEntityList *list, int handle);
    tEntityFromHandle entityFromHandle = reinterpret_cast<tEntityFromHandle>((*reinterpret_cast<void ***>(m_ClientEntityList))[kEntityFromHandleSlot]);
    return entityFromHandle(m_ClientEntityList, handle);
}

char *Game::getNetworkName(uintptr_t *entity)
{
    uintptr_t *IClientNetworkableVtable = (uintptr_t *)*(entity + 0x8);
    uintptr_t *GetClientClassPtr = (uintptr_t *)*(IClientNetworkableVtable + 0x8);
    uintptr_t *ClientClassPtr = (uintptr_t *)*(GetClientClassPtr + 0x1);
    char *m_pNetworkName = (char *)*(ClientClassPtr + 0x8);
    int classID = (int)*(ClientClassPtr + 0x10);
    std::cout << "ClassID: " << classID << std::endl;
    return m_pNetworkName;
}

void Game::ClientCmd(const char *szCmdString)
{
    m_EngineClient->ClientCmd(szCmdString);
}

void Game::ClientCmd_Unrestricted(const char *szCmdString)
{
    m_EngineClient->ClientCmd_Unrestricted(szCmdString);
}


