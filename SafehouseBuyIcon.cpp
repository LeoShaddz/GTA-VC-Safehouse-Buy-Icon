```cpp
// SafehouseBuyIcon.asi - GTA Vice City PC v1.0
// Shows the purchase icon (the "property" sprite) on the minimap and map for safehouses
// that have not yet been purchased, at the same time as the other radar icons
// (weapon shops, Pay'n'Spray, etc.). When purchased, the game creates the save icon
// in the same location and the purchase icon disappears immediately.
//
// How it works (all confirmed in gta-vc.exe 1.0 and VC 1.0 main.scm):
//  - During initialization (new game and when loading a save), the script creates a sprite blip 25
//    (property) at each safehouse and immediately removes it (REMOVE_BLIP).
//    This plugin intercepts only that removal (call at 0x453F5E) and keeps the blip: it is created
//    alongside the shop blips, by the script itself.
//  - When the script purchases/creates the save blip (sprite 19) at the same coordinates
//    (calls at 0x457239 / 0x4572FD), the purchase blip is removed immediately.
//  - Game blip list: 75 entries of 0x38 bytes at 0x7D7D38.
//  - VC blip commands (ADD_SPRITE_BLIP_FOR_COORD, 0x0570) create the blip through function
//    0x4C3C00, which calls SetCoordBlip (0x4C3C80) and marks the blip as "short range"
//    (appears on the radar only when the player is nearby, just like shops). The plugin uses the same.
//  - When LOADING a save, the script does not run this part again (the blips come from the save itself).
//    In this case, the radar drawing hook (call at 0x4C45AF) creates the purchase icon
//    on the first frame in which the radar is drawn, meaning it appears together with the other icons.
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

// ---- Addresses (VC 1.0) ----
static const DWORD VERSION_ADDR   = 0x601048;
static const DWORD VERSION_VC_1_0 = 0x53FF1B8B;
static const DWORD FN_CLEARBLIP   = 0x4C3990;
static const DWORD FN_SETSPRITE   = 0x4C3780;
static const DWORD FN_DRAWSPRITE  = 0x4C2D00;
static const DWORD CALL_REMOVE    = 0x453F5E;   // call ClearBlip   (REMOVE_BLIP)
static const DWORD CALL_SPRITE_A  = 0x457239;   // call SetBlipSprite
static const DWORD CALL_SPRITE_B  = 0x4572FD;   // call SetBlipSprite
static const DWORD CALL_SPRITE_C  = 0x630459;   // call SetBlipSprite (VC blip command, type 5)
static const DWORD CALL_SPRITE_D  = 0x63105B;   // call SetBlipSprite (ADD_SPRITE_BLIP_FOR_COORD 0x0570, type 4)
static const DWORD FN_COORDBLIP_SHORT = 0x4C3C00; // wrapper: SetCoordBlip + short range (used by the script)
static const DWORD FN_COORDBLIP_LONG  = 0x4C3C80; // pure SetCoordBlip
static const DWORD CALL_RADAR     = 0x4C45AF;   // call DrawRadarSprite
static const DWORD TRACE_BASE     = 0x7D7D38;
static const int   TRACE_COUNT    = 75;
static const int   TRACE_SIZE     = 0x38;
static const int   OFF_TYPE       = 0x04;
static const int   OFF_X          = 0x0C;
static const int   OFF_Y          = 0x10;
static const int   OFF_GEN        = 0x24;
static const int   OFF_INUSE      = 0x27;
static const int   OFF_SPRITE     = 0x34;
static const int   BLIP_COORD     = 4;
static const int   SPRITE_SAVE    = 19;         // radar_save
static const int   SPRITE_PROPERTY = 25;        // property (purchase icon used by the script)

typedef int  (__cdecl *SetCoordBlip_t)(int type, float x, float y, float z, int flag, int scale);
typedef void (__cdecl *SetBlipDisplay_t)(int blip, int display);
typedef void (__cdecl *ClearBlip_t)(int blip);
typedef void (__cdecl *SetBlipSprite_t)(int blip, int sprite);
typedef void (__cdecl *Draw_t)(int, int, int, int);

static SetBlipDisplay_t FnSetBlipDisplay = (SetBlipDisplay_t)0x4C3840;
static ClearBlip_t     OrigClear  = (ClearBlip_t)FN_CLEARBLIP;
static SetBlipSprite_t OrigSprite = (SetBlipSprite_t)FN_SETSPRITE;
static Draw_t          OrigDraw   = NULL;

// ---- Safehouses (coordinates extracted from VC 1.0 main.scm) ----
struct Safehouse { const char* key; float x, y, z; bool enabled; int handle; DWORD firstMiss; };
static Safehouse gSH[7] = {
    { "NBMN",  428.4f,   605.9f, 12.2f, true, -1, 0 },
    { "LNKV",  304.5f,   376.3f, 12.7f, true, -1, 0 },
    { "HYCO", -834.8f,  1306.9f, 11.0f, true, -1, 0 },
    { "OCHE",   14.0f, -1500.7f, 12.7f, true, -1, 0 },
    { "WASH",   88.5f,  -804.7f, 11.2f, true, -1, 0 },
    { "VCPT",  531.4f,  1273.7f, 17.6f, true, -1, 0 },
    { "SKUM", -560.1f,   703.6f, 20.5f, true, -1, 0 },
};

static HMODULE hSelf = NULL;
static char    gIni[MAX_PATH];
static bool    gLog = false;
static int     gSprite = SPRITE_PROPERTY;   // displayed sprite (default 25)
static int     gDisplay = 2;                // 2 = radar/map only
static int     gDelayMs = 0;                // delay before creating the icon (0 = immediate)
static int     gShortRange = 1;             // 1 = short range (same as the game's shops)
static int     gLogged = 0;

static void Log(const char* fmt, ...)
{
    if (!gLog) return;
    char path[MAX_PATH];
    if (!GetModuleFileNameA(hSelf, path, MAX_PATH)) return;
    char* dot = strrchr(path, '.');
    if (dot) strcpy(dot, ".log"); else strcat(path, ".log");
    FILE* f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "[%lu] ", GetTickCount());
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fprintf(f, "\n");
    fclose(f);
}

static inline unsigned char* EntryAt(int i) { return (unsigned char*)(TRACE_BASE + (DWORD)i * TRACE_SIZE); }

// Blip entry if the handle is still valid (in use and same generation), otherwise NULL
static unsigned char* EntryOf(int h)
{
    if (h == -1) return NULL;
    int idx = h & 0xFFFF, gen = (h >> 16) & 0xFFFF;
    if (idx >= TRACE_COUNT) return NULL;
    unsigned char* e = EntryAt(idx);
    if (!e[OFF_INUSE] || *(unsigned short*)(e + OFF_GEN) != gen) return NULL;
    return e;
}

static Safehouse* Match(float x, float y)
{
    for (int i = 0; i < 7; i++)
        if (fabsf(x - gSH[i].x) < 3.0f && fabsf(y - gSH[i].y) < 3.0f) return &gSH[i];
    return NULL;
}

static bool TrackedAlive(const Safehouse& s)
{
    unsigned char* e = EntryOf(s.handle);
    return e && *(int*)(e + OFF_TYPE) == BLIP_COORD && *(unsigned short*)(e + OFF_SPRITE) == gSprite;
}

// ---- Hook 1: script's REMOVE_BLIP ----
// First removal of a "property" blip at a safehouse = the one performed by the script during initialization:
// do not remove it; keep the blip (it will be the purchase icon).
static void __cdecl HookRemove(int h)
{
    unsigned char* e = EntryOf(h);
    if (e && gLogged < 40 && *(unsigned short*)(e + OFF_SPRITE) == SPRITE_PROPERTY)
    {
        gLogged++;
        Log("REMOVE_BLIP handle=%08X type=%d sprite=%d x=%.1f y=%.1f", h, *(int*)(e + OFF_TYPE),
            *(unsigned short*)(e + OFF_SPRITE), *(float*)(e + OFF_X), *(float*)(e + OFF_Y));
    }
    if (e && *(int*)(e + OFF_TYPE) == BLIP_COORD && *(unsigned short*)(e + OFF_SPRITE) == SPRITE_PROPERTY)
    {
        Safehouse* s = Match(*(float*)(e + OFF_X), *(float*)(e + OFF_Y));
        if (s && s->enabled)
        {
            if (s->handle == h)
            {
                s->handle = -1;                       // the script really wants to remove it (it was already kept)
                Log("%s: script removed the kept blip", s->key);
            }
            else if (!TrackedAlive(*s))
            {
                s->handle = h;                        // keep the blip created by the script
                if (gSprite != SPRITE_PROPERTY) OrigSprite(h, gSprite);
                Log("%s: purchase blip kept (handle %08X)", s->key, h);
                return;                               // DO NOT remove
            }
            // another purchase blip already exists at this safehouse: remove this one normally (duplicate)
        }
    }
    OrigClear(h);
}

// ---- Hook 2: script's SetBlipSprite ----
// Sprite 19 (save icon) created at a safehouse = it was purchased: remove the purchase icon.
static void __cdecl HookSprite(int blip, int sprite)
{
    OrigSprite(blip, sprite);
    if (sprite != SPRITE_SAVE) return;

    unsigned char* e = EntryOf(blip);
    if (!e) return;
    Safehouse* s = Match(*(float*)(e + OFF_X), *(float*)(e + OFF_Y));
    if (s && s->handle != -1)
    {
        if (TrackedAlive(*s)) OrigClear(s->handle);
        Log("%s purchased: purchase icon removed", s->key);
        s->handle = -1;
    }
}

// ---- Hook 3: radar drawing (safety cleanup only) ----
static bool SaveBlipExists(const Safehouse& s)
{
    for (int i = 0; i < TRACE_COUNT; i++)
    {
        unsigned char* e = EntryAt(i);
        if (!e[OFF_INUSE] || *(unsigned short*)(e + OFF_SPRITE) != SPRITE_SAVE) continue;
        if (fabsf(*(float*)(e + OFF_X) - s.x) < 3.0f && fabsf(*(float*)(e + OFF_Y) - s.y) < 3.0f) return true;
    }
    return false;
}

static void Update()
{
    DWORD now = GetTickCount();
    if (now == 0) now = 1;

    for (int i = 0; i < 7; i++)
    {
        Safehouse& s = gSH[i];
        if (!s.enabled) continue;

        if (s.handle != -1 && !TrackedAlive(s)) { s.handle = -1; s.firstMiss = 0; }  // cleared by the game (load/new game)

        if (SaveBlipExists(s))
        {
            // Purchased (or already purchased in the save): make sure the purchase icon does not exist
            if (s.handle != -1) { OrigClear(s.handle); s.handle = -1; Log("%s: purchase icon removed (save present)", s.key); }
            s.firstMiss = 0;
            continue;
        }

        if (s.handle != -1) continue;                         // we already have the purchase icon
        if (s.firstMiss == 0) s.firstMiss = now;
        if ((int)(now - s.firstMiss) < gDelayMs) continue;

        // Same call used by the script for ADD_SPRITE_BLIP_FOR_COORD
        SetCoordBlip_t create = (SetCoordBlip_t)(gShortRange ? FN_COORDBLIP_SHORT : FN_COORDBLIP_LONG);
        int h = create(BLIP_COORD, s.x, s.y, s.z, 5, 3);
        if (h == -1) { s.firstMiss = 0; continue; }
        OrigSprite(h, gSprite);
        FnSetBlipDisplay(h, gDisplay);
        s.handle = h;
        Log("%s: purchase icon created on radar (handle %08X)", s.key, h);
    }
}

static void __cdecl HookRadar(int a, int b, int c, int d)
{
    Update();               // before drawing: the icon can appear in this same frame
    OrigDraw(a, b, c, d);
}

// ---- Installation ----
static bool PatchCall(DWORD site, DWORD expectedTarget, void* newFn, DWORD* origOut)
{
    if (IsBadReadPtr((void*)site, 5) || *(unsigned char*)site != 0xE8) return false;
    DWORD target = site + 5 + *(int*)(site + 1);
    if (expectedTarget && target != expectedTarget) return false;
    if (origOut) *origOut = target;

    DWORD old;
    if (!VirtualProtect((void*)(site + 1), 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    *(int*)(site + 1) = (int)((DWORD)newFn - (site + 5));
    VirtualProtect((void*)(site + 1), 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)site, 5);
    return true;
}

static bool Install()
{
    struct Site { DWORD addr; DWORD expect; void* fn; };
    Site sites[] = {
        { CALL_REMOVE,   FN_CLEARBLIP,  (void*)HookRemove },
        { CALL_SPRITE_A, FN_SETSPRITE, (void*)HookSprite },
        { CALL_SPRITE_B, FN_SETSPRITE, (void*)HookSprite },
        { CALL_SPRITE_C, FN_SETSPRITE, (void*)HookSprite },
        { CALL_SPRITE_D, FN_SETSPRITE, (void*)HookSprite },
        { CALL_RADAR,    FN_DRAWSPRITE, (void*)HookRadar  },
    };
    const int n = sizeof(sites) / sizeof(sites[0]);

    // Verify all patch points before modifying anything
    for (int i = 0; i < n; i++)
    {
        if (IsBadReadPtr((void*)sites[i].addr, 5) || *(unsigned char*)sites[i].addr != 0xE8) return false;
        if (sites[i].addr + 5 + *(int*)(sites[i].addr + 1) != sites[i].expect) return false;
    }
    DWORD o;
    for (int i = 0; i < n; i++)
        if (!PatchCall(sites[i].addr, sites[i].expect, sites[i].fn, &o)) return false;
    OrigDraw = (Draw_t)FN_DRAWSPRITE;
    return true;
}

static void Init()
{
    gLog = GetPrivateProfileIntA("Main", "Log", 0, gIni) != 0;
    if (!GetPrivateProfileIntA("Main", "Enable", 1, gIni)) { Log("Disabled in .ini"); return; }

    if (IsBadReadPtr((void*)VERSION_ADDR, 4) || *(DWORD*)VERSION_ADDR != VERSION_VC_1_0)
    { Log("Not GTA VC 1.0 - mod disabled"); return; }

    gSprite  = GetPrivateProfileIntA("Main", "Sprite", SPRITE_PROPERTY, gIni);
    gDisplay = GetPrivateProfileIntA("Main", "Display", 2, gIni);
    gDelayMs = GetPrivateProfileIntA("Main", "DelayMs", 0, gIni);
    gShortRange = GetPrivateProfileIntA("Main", "ShortRange", 1, gIni);
    if (gDelayMs < 0) gDelayMs = 0;
    for (int i = 0; i < 7; i++)
    {
        char key[32];
        sprintf(key, "Show_%s", gSH[i].key);
        gSH[i].enabled = GetPrivateProfileIntA("Safehouses", key, 1, gIni) != 0;
    }

    if (!Install()) { Log("Game code differs from expected - mod disabled"); return; }
    Log("Active: sprite=%d display=%d delay=%dms shortRange=%d", gSprite, gDisplay, gDelayMs, gShortRange);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        hSelf = hModule;
        GetModuleFileNameA(hModule, gIni, MAX_PATH);
        char* dot = strrchr(gIni, '.');
        if (dot) strcpy(dot, ".ini"); else strcat(gIni, ".ini");
        Init();   // synchronous: hooks must be ready before the game's script runs
    }
    return TRUE;
}
```
