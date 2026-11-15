/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * main_ogc.c - GameCube / Wii / Wii U entry point. Boot order follows the 360's
 * (xbox360_port/xdk/src/main_xdk.cpp, the build that plays), which follows the
 * Original Xbox and PC ones:
 *
 *   filesystem -> log -> config (+ console overrides) -> GTE self-test ->
 *   CD image -> GPU -> pad -> memory card -> file table -> game data ->
 *   SPU + audio out -> libgpu reset -> file queue -> MainLoop
 *
 * Fs_InitFileTableForRegion must run before anything reads a file, or every
 * startSector is 0 and every read returns the ISO volume descriptor. The boot
 * trace is flushed before MainLoop, which never returns, so a hang inside it
 * still leaves the log on the card.
 *
 * Deliberately free of libogc headers: it includes game headers, whose `bool`
 * is the decomp's enum (see ogc_port/include/ogc_prelude.h).
 */
#include "ogc_prelude.h"
#include <stdio.h>
#include <string.h>

#include "sh_log.h"
#include "fs_xbox360.h"
#include "psx_memory.h"
#include "pc_config.h"

extern const char* g_ShLogPath;
extern void        SH_DebugLogFlush(void);

extern char g_CdBinPath[];
extern void Cd_XboxInit(void);
extern int  Cd_XboxSelfTest(char* idOut, int idOutSize);

extern void PcConfig_Load(const char* path);
extern void XboxConfig_ApplyOverrides(void);
extern void Gte_SelfTest(void);
extern void GpuNv2a_Init(void);
extern void GpuNv2a_FrameBegin(void);
extern void Gpu360_SelfTest(void);
extern void Pad_XboxInit(void);
extern void Mcard_XboxInit(void);
extern void Fs_InitFileTableForRegion(int region);
extern void SpuInit(void);
extern void Audio360_Init(void);
extern void ResetGraph(int mode);
extern void SetGraphDebug(int level);
extern void Fs_QueueInitialize(void);
extern void MainLoop(void);
extern int  g_PcWidescreenMode;

extern void* g_OvlDynamic;
extern void* g_OvlBodyprog;
typedef struct s_DemoFrameData s_DemoFrameData;
extern s_DemoFrameData* g_Demo_PlayFileBufferPtr;

extern void PcPort_InitCharaAnimInfo(void);
extern void PcPort_InitSdBuffers(void);
extern void AsRodata_Reformat(void);
extern void GroanerAnimInfos_Init(void);
extern void BloodsuckerAnimInfos_Init(void);
extern void BloodyLisaAnimInfos_Init(void);
extern void AlessaAnimInfos_Init(void);
extern void GhostChildAlessaAnimInfos_Init(void);
extern void LisaAnimInfos_Init(void);
extern void KaufmannAnimInfos_Init(void);
extern void DahliaAnimInfos_Init(void);
extern void CatAnimInfos_Init(void);
extern void PuppetNurseData_Init(void);
extern void LarvalStalkerAnimInfos_Init(void);
extern void HangedScratcherAnimInfos_Init(void);
extern void CreeperAnimInfos_Init(void);
extern void SplitHeadAnimInfos_Init(void);
extern void RomperAnimInfos_Init(void);
extern void LockerDeadBodyAnimInfos_Init(void);
extern void TwinfeelerAnimInfos_Init(void);
extern void FloatstingerAnimInfos_Init(void);
extern void MonsterCybilAnimInfos_Init(void);
extern void FlaurosAnimInfos_Init(void);
extern void ParasiteAnimInfos_Init(void);
extern void GhostDoctorAnimInfos_Init(void);
extern void BloodyIncubatorAnimInfos_Init(void);
extern void IncubatorAnimInfos_Init(void);
extern void LittleIncubusAnimInfos_Init(void);
extern void IncubusAnimInfos_Init(void);
extern void Unkkown23AnimInfos_Init(void);
extern void Map6S04ExtraAnimInfos_Init(void);

/* The toolchains reject function pointers in static initialisers, so the PC
 * and console ports build these tables at runtime (main_pc.c / main_xbox.c). */
static void Sh_InitGameData(void)
{
    PsxMemory_Init();                    /* everything below is g_PsxRam-relative */
    SH_DBG("[BOOT] PSX RAM @ %p", (void*)g_PsxRam);

    PcPort_InitCharaAnimInfo();
    PcPort_InitSdBuffers();
    AsRodata_Reformat();

    GroanerAnimInfos_Init();
    BloodsuckerAnimInfos_Init();
    BloodyLisaAnimInfos_Init();
    AlessaAnimInfos_Init();
    GhostChildAlessaAnimInfos_Init();
    LisaAnimInfos_Init();
    KaufmannAnimInfos_Init();
    DahliaAnimInfos_Init();
    CatAnimInfos_Init();
    PuppetNurseData_Init();
    LarvalStalkerAnimInfos_Init();
    HangedScratcherAnimInfos_Init();
    CreeperAnimInfos_Init();
    SplitHeadAnimInfos_Init();
    RomperAnimInfos_Init();
    LockerDeadBodyAnimInfos_Init();
    TwinfeelerAnimInfos_Init();
    FloatstingerAnimInfos_Init();
    MonsterCybilAnimInfos_Init();
    FlaurosAnimInfos_Init();
    ParasiteAnimInfos_Init();
    GhostDoctorAnimInfos_Init();
    BloodyIncubatorAnimInfos_Init();
    IncubatorAnimInfos_Init();
    LittleIncubusAnimInfos_Init();
    IncubusAnimInfos_Init();
    Unkkown23AnimInfos_Init();
    Map6S04ExtraAnimInfos_Init();

    /* Overlay base pointers into emulated PSX RAM (USA addresses, per main_pc.c). */
    g_OvlDynamic             = PSX_ADDR(0x000C9578);
    g_OvlBodyprog            = PSX_ADDR(0x00024B60);
    g_Demo_PlayFileBufferPtr = (s_DemoFrameData*)PSX_ADDR(0x000F5E00);

    SH_DBG("[BOOT] game data initialised");
}

static int FileExists(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

int main(void)
{
    char cfgPath[SH360_PATH_MAX * 2];
    char pvd[8];
    int  haveBin;

#if defined(SH_WIIU_PORT)
    { extern void WiiU_Boot(void); WiiU_Boot(); }   /* ProcUI first (pad_wiiu.c) */
#endif
#if defined(SH_GC_PORT)
    /* Keep the top 64 KB of MEM1 out of the heap: a GX texture/TLUT fetch that
     * runs past the last allocation must stay inside the 24 MB. Without it
     * Dolphin itself crashed (access violation) on the first busy town frame
     * once the packet arenas grew (2026-11-14). */
    { extern void* SYS_GetArena1Hi(void); extern void SYS_SetArena1Hi(void*);
      SYS_SetArena1Hi((char*)SYS_GetArena1Hi() - 64 * 1024); }
#endif
    haveBin = Sh360Fs_Init();
    SH_DebugLogInit();
#if defined(SH_WIIU_PORT)
    SH_DBG("[BOOT] Silent Hill - Wii U");
#elif defined(SH_GC_PORT)
    SH_DBG("[BOOT] Silent Hill - GameCube");
#else
    SH_DBG("[BOOT] Silent Hill - Wii");
#endif
    SH_DBG("[BOOT] root='%s' bin='%s' found=%d log='%s'",
           Sh360Fs_DataRoot(), Sh360Fs_BinName(), haveBin, g_ShLogPath);
    SH_DebugLogFlush();

    /* No launcher here: silenthill.cfg next to the disc image is the only
     * way to tune anything. XboxConfig_ApplyOverrides then pins the settings
     * whose PC defaults assume a desktop's memory (texture-pack caches,
     * global chara pool, ...). The three the 360 keeps from the cfg are kept
     * here too. */
    snprintf(cfgPath, sizeof(cfgPath), "%ssilenthill.cfg", Sh360Fs_DataRoot());
    SH_DBG("[CFG] config file: %s", cfgPath);
    g_PcConfig.disableCulling = -1;
#if defined(SH_WIIU_PORT)
    g_PcConfig.minimap = 0;              /* the GamePad shows the map instead */
#endif
    PcConfig_Load(cfgPath);
    if (g_PcConfig.disableCulling < 0)
        g_PcConfig.disableCulling = (g_PcConfig.widescreenMode == 1) ? 1 : 0;
    {
        const int preload = g_PcConfig.preloadChunks;
        const int noCull  = g_PcConfig.disableCulling;
        int       wide    = g_PcConfig.widescreenMode;
        char      startMap[sizeof(g_PcConfig.mapName)];
        /* `map = <name>` (the new-game start map) survives the overrides: the
         * cfg only exists here if someone wrote one, and test runs use it to
         * start a new game in a later area. */
        memcpy(startMap, g_PcConfig.mapName, sizeof(startMap));
        XboxConfig_ApplyOverrides();
        if (startMap[0]) memcpy(g_PcConfig.mapName, startMap, sizeof(startMap));
#if defined(SH_WIIU_PORT)
        g_PcConfig.preloadChunks = preload;  /* 1 GB: the cfg decides */
#else
        /* GameCube / Wii: always stream chunks like the PSX. Preloading a
         * whole map's exteriors (the PC and 360 default) ran the Wii's 88 MB
         * out within one map: every [IPD]/[LM] model calloc after that failed,
         * so the town had no floor to draw or stand on (Dolphin, 2026-11-11). */
        (void)preload;
        g_PcConfig.preloadChunks = 0;
#endif
#if defined(SH_GC_PORT)
        wide = 0;                        /* 4:3 only; no widescreen setting */
#endif
        g_PcConfig.disableCulling = noCull;
        g_PcConfig.widescreenMode = wide;
        g_PcWidescreenMode = (wide == 1) ? 1 : 0;
#if defined(SH_WIIU_PORT)
        g_PcConfig.windowWidth  = g_PcWidescreenMode ? 1920 : 1440;
        g_PcConfig.windowHeight = 1080;
#else
        g_PcConfig.windowWidth  = g_PcWidescreenMode ? 854 : 640;
        g_PcConfig.windowHeight = 480;
#endif
        SH_DBG("[CFG] preload_chunks=%d disable_culling=%d widescreen_mode=%d",
               g_PcConfig.preloadChunks, noCull, g_PcWidescreenMode);
    }

    /* Fixed-point/GTE sanity on a new architecture before anything rides on it. */
    Gte_SelfTest();

    Cd_XboxInit();
    SH_DBG("[CD] BIN: %s", g_CdBinPath);
    if (Cd_XboxSelfTest(pvd, sizeof(pvd)))
        SH_DBG("[CD] PVD id='%s' (expect CD001)", pvd);
    else
        SH_DBG("[CD] BIN NOT OPEN - the game cannot load anything");

    GpuNv2a_Init();
    Pad_XboxInit();
    Mcard_XboxInit();

    Fs_InitFileTableForRegion(0 /* Region_USA */);

    Sh_InitGameData();
    SpuInit();
    Audio360_Init();
    ResetGraph(0);
    SetGraphDebug(0);
    Fs_QueueInitialize();

    SH_DBG("[BOOT] subsystems up; entering MainLoop");
    SH_DebugLogFlush();

    /* <root>/gputest: draw the known test image for a few seconds first. */
    snprintf(cfgPath, sizeof(cfgPath), "%sgputest", Sh360Fs_DataRoot());
    if (FileExists(cfgPath))
        Gpu360_SelfTest();

    GpuNv2a_FrameBegin();
    MainLoop();

    SH_DBG("[BOOT] MainLoop returned (unexpected)");
    SH_DebugLogFlush();
    return 0;
}
