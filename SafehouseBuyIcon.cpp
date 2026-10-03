// SafehouseBuyIcon.asi - GTA Vice City PC v1.0
// Mostra o icone de compra (sprite "property") no minimapa e no mapa nas safehouses
// que ainda nao foram compradas, ao mesmo tempo que os demais icones do radar
// (lojas de armas, Pay'n'Spray, etc.). Ao comprar, o jogo cria o icone de salvamento
// no mesmo lugar e o icone de compra some no mesmo instante.
//
// Como funciona (tudo confirmado no gta-vc.exe 1.0 e no main.scm do VC 1.0):
//  - Na inicializacao (novo jogo e ao carregar um save) o script cria um blip sprite 25
//    (property) em cada safehouse e o remove logo em seguida (REMOVE_BLIP).
//    Este plugin intercepta so essa remocao (call em 0x453F5E) e mantem o blip: ele nasce
//    junto com os blips das lojas, criado pelo proprio script.
//  - Quando o script compra/cria o blip de salvamento (sprite 19) nas mesmas coordenadas
//    (calls em 0x457239 / 0x4572FD), o blip de compra e removido na hora.
//  - Lista de blips do jogo: 75 entradas de 0x38 bytes em 0x7D7D38.
//  - Os comandos de blip do VC (ADD_SPRITE_BLIP_FOR_COORD, 0x0570) criam o blip pela funcao
//    0x4C3C00, que chama SetCoordBlip (0x4C3C80) e marca o blip como "curto alcance"
//    (aparece no radar so quando o jogador esta perto, igual as lojas). O plugin usa a mesma.
//  - Ao CARREGAR um save o script nao roda essa parte de novo (os blips vem do proprio save).
//    Nesse caso o gancho no desenho do radar (call em 0x4C45AF) cria o icone de compra
//    ja no primeiro frame em que o radar e desenhado, ou seja, junto com os demais icones.
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

// ---- Enderecos (VC 1.0) ----
static const DWORD VERSION_ADDR   = 0x601048;
static const DWORD VERSION_VC_1_0 = 0x53FF1B8B;
static const DWORD FN_CLEARBLIP   = 0x4C3990;
static const DWORD FN_SETSPRITE   = 0x4C3780;
static const DWORD FN_DRAWSPRITE  = 0x4C2D00;
static const DWORD CALL_REMOVE    = 0x453F5E;   // call ClearBlip   (REMOVE_BLIP)
static const DWORD CALL_SPRITE_A  = 0x457239;   // call SetBlipSprite
static const DWORD CALL_SPRITE_B  = 0x4572FD;   // call SetBlipSprite
static const DWORD CALL_SPRITE_C  = 0x630459;   // call SetBlipSprite (comando de blip do VC, tipo 5)
static const DWORD CALL_SPRITE_D  = 0x63105B;   // call SetBlipSprite (ADD_SPRITE_BLIP_FOR_COORD 0x0570, tipo 4)
static const DWORD FN_COORDBLIP_SHORT = 0x4C3C00; // wrapper: SetCoordBlip + curto alcance (usado pelo script)
static const DWORD FN_COORDBLIP_LONG  = 0x4C3C80; // SetCoordBlip puro
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
static const int   SPRITE_PROPERTY = 25;        // property (icone de compra do script)

typedef int  (__cdecl *SetCoordBlip_t)(int type, float x, float y, float z, int flag, int scale);
typedef void (__cdecl *SetBlipDisplay_t)(int blip, int display);
typedef void (__cdecl *ClearBlip_t)(int blip);
typedef void (__cdecl *SetBlipSprite_t)(int blip, int sprite);
typedef void (__cdecl *Draw_t)(int, int, int, int);

static SetBlipDisplay_t FnSetBlipDisplay = (SetBlipDisplay_t)0x4C3840;
static ClearBlip_t     OrigClear  = (ClearBlip_t)FN_CLEARBLIP;
static SetBlipSprite_t OrigSprite = (SetBlipSprite_t)FN_SETSPRITE;
static Draw_t          OrigDraw   = NULL;

// ---- Safehouses (coordenadas extraidas do main.scm do VC 1.0) ----
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
static int     gSprite = SPRITE_PROPERTY;   // sprite mostrado (padrao 25)
static int     gDisplay = 2;                // 2 = so radar/mapa
static int     gDelayMs = 0;                // espera antes de criar o icone (0 = imediato)
static int     gShortRange = 1;             // 1 = curto alcance (igual as lojas do jogo)
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

// Entrada do blip se o handle ainda for valido (em uso e mesma geracao), senao NULL
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

// ---- Gancho 1: REMOVE_BLIP do script ----
// Primeira remocao de um blip "property" numa safehouse = a do script na inicializacao:
// nao remove, mantem o blip (sera o icone de compra).
static void __cdecl HookRemove(int h)
{
    unsigned char* e = EntryOf(h);
    if (e && gLogged < 40 && *(unsigned short*)(e + OFF_SPRITE) == SPRITE_PROPERTY)
    {
        gLogged++;
        Log("REMOVE_BLIP handle=%08X tipo=%d sprite=%d x=%.1f y=%.1f", h, *(int*)(e + OFF_TYPE),
            *(unsigned short*)(e + OFF_SPRITE), *(float*)(e + OFF_X), *(float*)(e + OFF_Y));
    }
    if (e && *(int*)(e + OFF_TYPE) == BLIP_COORD && *(unsigned short*)(e + OFF_SPRITE) == SPRITE_PROPERTY)
    {
        Safehouse* s = Match(*(float*)(e + OFF_X), *(float*)(e + OFF_Y));
        if (s && s->enabled)
        {
            if (s->handle == h)
            {
                s->handle = -1;                       // o script realmente quer remover (ja estava mantido)
                Log("%s: script removeu o blip mantido", s->key);
            }
            else if (!TrackedAlive(*s))
            {
                s->handle = h;                        // mantem o blip criado pelo script
                if (gSprite != SPRITE_PROPERTY) OrigSprite(h, gSprite);
                Log("%s: blip de compra mantido (handle %08X)", s->key, h);
                return;                               // NAO remove
            }
            // ja existe outro blip de compra nesta safehouse: remove este normalmente (duplicata)
        }
    }
    OrigClear(h);
}

// ---- Gancho 2: SetBlipSprite do script ----
// Sprite 19 (salvamento) criado numa safehouse = foi comprada: remove o icone de compra.
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
        Log("%s comprada: icone de compra removido", s->key);
        s->handle = -1;
    }
}

// ---- Gancho 3: desenho do radar (so limpeza de seguranca) ----
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

        if (s.handle != -1 && !TrackedAlive(s)) { s.handle = -1; s.firstMiss = 0; }  // limpo pelo jogo (load/new game)

        if (SaveBlipExists(s))
        {
            // Comprada (ou ja comprada no save): garante que o icone de compra nao exista
            if (s.handle != -1) { OrigClear(s.handle); s.handle = -1; Log("%s: icone de compra removido (salvamento presente)", s.key); }
            s.firstMiss = 0;
            continue;
        }

        if (s.handle != -1) continue;                         // ja temos o icone de compra
        if (s.firstMiss == 0) s.firstMiss = now;
        if ((int)(now - s.firstMiss) < gDelayMs) continue;

        // Mesma chamada que o script usa para ADD_SPRITE_BLIP_FOR_COORD
        SetCoordBlip_t create = (SetCoordBlip_t)(gShortRange ? FN_COORDBLIP_SHORT : FN_COORDBLIP_LONG);
        int h = create(BLIP_COORD, s.x, s.y, s.z, 5, 3);
        if (h == -1) { s.firstMiss = 0; continue; }
        OrigSprite(h, gSprite);
        FnSetBlipDisplay(h, gDisplay);
        s.handle = h;
        Log("%s: icone de compra criado no radar (handle %08X)", s.key, h);
    }
}

static void __cdecl HookRadar(int a, int b, int c, int d)
{
    Update();               // antes do desenho: o icone ja pode sair neste mesmo frame
    OrigDraw(a, b, c, d);
}

// ---- Instalacao ----
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
        { CALL_SPRITE_A, FN_SETSPRITE,  (void*)HookSprite },
        { CALL_SPRITE_B, FN_SETSPRITE,  (void*)HookSprite },
        { CALL_SPRITE_C, FN_SETSPRITE,  (void*)HookSprite },
        { CALL_SPRITE_D, FN_SETSPRITE,  (void*)HookSprite },
        { CALL_RADAR,    FN_DRAWSPRITE, (void*)HookRadar  },
    };
    const int n = sizeof(sites) / sizeof(sites[0]);

    // Confere todos os pontos antes de alterar qualquer coisa
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
    if (!GetPrivateProfileIntA("Main", "Enable", 1, gIni)) { Log("Desativado no .ini"); return; }

    if (IsBadReadPtr((void*)VERSION_ADDR, 4) || *(DWORD*)VERSION_ADDR != VERSION_VC_1_0)
    { Log("Nao e o GTA VC 1.0 - mod desativado"); return; }

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

    if (!Install()) { Log("Codigo do jogo diferente do esperado - mod desativado"); return; }
    Log("Ativo: sprite=%d display=%d delay=%dms curtoAlcance=%d", gSprite, gDisplay, gDelayMs, gShortRange);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        hSelf = hModule;
        GetModuleFileNameA(hModule, gIni, MAX_PATH);
        char* dot = strrchr(gIni, '.');
        if (dot) strcpy(dot, ".ini"); else strcat(gIni, ".ini");
        Init();   // sincrono: os ganchos precisam estar prontos antes do script do jogo rodar
    }
    return TRUE;
}
