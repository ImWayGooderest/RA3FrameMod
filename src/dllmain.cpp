// RA3FrameMod — raises the render rate of Red Alert 3 / Uprising without changing game speed.
//
// Loaded as a dsound.dll proxy from the game's Data folder. See docs/RE_NOTES.md for the
// reverse-engineering background. Summary:
//   * Logic ticks at LogicFPS (15). Each tick is split into 6 sub-steps; every drawn frame
//     processes 6/ratio of them, where ratio = MaxUpdateFPS / LogicFPS (30/15 = 2 vanilla).
//   * We point only the frame-cadence code at our own MaxUpdateFPS value. The engine's
//     global constant is left untouched because gamelogic code also reads it.
//   * Only ratios dividing 6 keep the sub-step scheduler correct, so 30/45/90 fps are valid.
//
// Every patch site is located by signature and verified before anything is written; if any
// signature is missing or ambiguous, nothing is patched and the game runs vanilla.

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <share.h>
#include <math.h>

// ---------------------------------------------------------------------------------------
// dsound.dll forwarding

static HMODULE g_realDsound;
static FARPROC g_real[12];
static const char* const kDsoundNames[12] = {
    "DirectSoundCreate", "DirectSoundEnumerateA", "DirectSoundEnumerateW", "DllCanUnloadNow",
    "DllGetClassObject", "DirectSoundCaptureCreate", "DirectSoundCaptureEnumerateA",
    "DirectSoundCaptureEnumerateW", "GetDeviceID", "DirectSoundFullDuplexCreate",
    "DirectSoundCreate8", "DirectSoundCaptureCreate8",
};

extern "C" void __cdecl ResolveDsound(int index)
{
    if (g_real[index])
        return;
    if (!g_realDsound) {
        char path[MAX_PATH];
        UINT n = GetSystemDirectoryA(path, MAX_PATH);
        strcpy_s(path + n, MAX_PATH - n, "\\dsound.dll");
        g_realDsound = LoadLibraryA(path);
    }
    if (g_realDsound)
        g_real[index] = GetProcAddress(g_realDsound, kDsoundNames[index]);
}

// Each export resolves the real function on first use, then tail-jumps to it with the
// caller's stack untouched, so signatures and calling conventions don't matter here.
#define DSOUND_PROXY(name, index)                                   \
    extern "C" __declspec(naked) void Proxy_##name()                \
    {                                                               \
        __asm pushad                                                \
        __asm push index                                            \
        __asm call ResolveDsound                                    \
        __asm add esp, 4                                            \
        __asm popad                                                 \
        __asm jmp dword ptr [g_real + index * 4]                    \
    }

DSOUND_PROXY(DirectSoundCreate, 0)
DSOUND_PROXY(DirectSoundEnumerateA, 1)
DSOUND_PROXY(DirectSoundEnumerateW, 2)
DSOUND_PROXY(DllCanUnloadNow, 3)
DSOUND_PROXY(DllGetClassObject, 4)
DSOUND_PROXY(DirectSoundCaptureCreate, 5)
DSOUND_PROXY(DirectSoundCaptureEnumerateA, 6)
DSOUND_PROXY(DirectSoundCaptureEnumerateW, 7)
DSOUND_PROXY(GetDeviceID, 8)
DSOUND_PROXY(DirectSoundFullDuplexCreate, 9)
DSOUND_PROXY(DirectSoundCreate8, 10)
DSOUND_PROXY(DirectSoundCaptureCreate8, 11)

// ---------------------------------------------------------------------------------------
// Logging and config

static FILE* g_log;
static char g_dir[MAX_PATH];

static void Log(const char* fmt, ...)
{
    if (!g_log)
        return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static int IniInt(const char* key, int def)
{
    char path[MAX_PATH];
    sprintf_s(path, "%sRA3FrameMod.ini", g_dir);
    return GetPrivateProfileIntA("RA3FrameMod", key, def, path);
}

// ---------------------------------------------------------------------------------------
// Signature scanning

struct Module {
    uint8_t* text;
    size_t textSize;
};

static bool FindText(Module& m)
{
    uint8_t* base = (uint8_t*)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (memcmp(sec->Name, ".text", 6) == 0) {
            m.text = base + sec->VirtualAddress;
            m.textSize = sec->Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

// pattern: bytes; mask: 'x' = must match, '?' = wildcard. Returns hit count, fills hits[].
static int Scan(const Module& m, const uint8_t* pattern, const char* mask, uint8_t** hits, int maxHits)
{
    size_t len = strlen(mask);
    int count = 0;
    for (size_t i = 0; i + len <= m.textSize; i++) {
        size_t j = 0;
        while (j < len && (mask[j] == '?' || m.text[i + j] == pattern[j]))
            j++;
        if (j == len) {
            if (count < maxHits)
                hits[count] = m.text + i;
            count++;
        }
    }
    return count;
}

static void Write(uint8_t* at, const void* bytes, size_t n)
{
    DWORD old;
    VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy(at, bytes, n);
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
}

// Replaces a 6-byte instruction with `call stub; nop`.
static void WriteCall6(uint8_t* at, void* stub)
{
    uint8_t code[6] = { 0xE8, 0, 0, 0, 0, 0x90 };
    int32_t rel = (int32_t)((uint8_t*)stub - (at + 5));
    memcpy(code + 1, &rel, 4);
    Write(at, code, sizeof(code));
}

// ---------------------------------------------------------------------------------------
// Patches

static int g_maxUpdateFps = 30;       // replaces the engine's MaxUpdateFPS at cadence sites
static int g_stepFps = 30;            // read by the sub-step function; lowered to catch up
static int g_logicFps = 15;
static bool g_catchUp;
static volatile LONG g_drawnFrames;    // incremented once per main-loop iteration
static volatile LONG g_catchUpFrames;  // frames that processed extra sub-steps
static uint32_t* g_theGameLogic;       // address of the TheGameLogic pointer global
static uint32_t* g_theNetwork;         // address of the network interface pointer (null offline)

// Catch-up state: sub-steps the simulation should have run by now vs. has run.
static LARGE_INTEGER g_qpcFreq, g_anchorTime;
static int64_t g_anchorDone, g_lastDone;
static bool g_anchored, g_haveLastDone;
static float g_animDt = 1.0f / 30;     // replaces the engine's 1/30 s per-frame animation step
static int g_animMs = 33;              // replaces the engine's 33 ms per-frame animation clock step
static double g_animClockMs;
static int64_t g_animClockInt;

static uint32_t ReadLogicFrame(bool* ok)
{
    *ok = false;
    __try {
        if (g_theGameLogic && *g_theGameLogic) {
            *ok = true;
            return *(uint32_t*)(*g_theGameLogic + 0x50);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return 0;
}

// Runs once per frame, just before the next GameEngine::update(). When the last frames took
// longer than 1/90 s, the next frame processes 2, 3 or 6 sub-steps (the engine's own valid
// step sizes) so the simulation keeps real-time speed and dips cost only smoothness.
extern "C" void __cdecl OnFrame(uint8_t* engine)
{
    g_drawnFrames++;
    bool ok;
    uint32_t logicFrame = ReadLogicFrame(&ok);
    int64_t done = (int64_t)logicFrame * 6 + *(int*)(engine + 0x58);

    // The client advances its animation clock by a fixed "one 30 fps frame" per drawn frame.
    // Give it the simulation time actually covered instead: sub-steps since last frame /
    // (6 x LogicFPS). With no sub-steps (menus) fall back to real time per frame; when the
    // game is paused the engine multiplies the clock step by zero itself.
    int64_t stepped = done - g_lastDone;
    double seconds;
    if (ok && g_haveLastDone && stepped >= 1 && stepped <= 6)
        seconds = stepped / (double)(g_logicFps * 6);
    else
        seconds = 1.0 / g_maxUpdateFps;
    g_haveLastDone = ok;
    g_animDt = (float)seconds;
    g_animClockMs += seconds * 1000.0;
    int64_t clockInt = (int64_t)g_animClockMs;
    g_animMs = (int)(clockInt - g_animClockInt);   // whole ms this frame; remainder carries over
    g_animClockInt = clockInt;

    if (!g_catchUp) {
        g_lastDone = done;
        return;
    }
    g_stepFps = g_maxUpdateFps;
    if (!ok || (g_theNetwork && *g_theNetwork)) {   // online: the network layer paces the game
        g_anchored = false;
        g_lastDone = done;
        return;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    // No progress at all last frame means the game is paused or waiting; don't build debt.
    if (g_anchored && done >= g_lastDone && done != g_lastDone) {
        int64_t expected = g_anchorDone +
            (now.QuadPart - g_anchorTime.QuadPart) * (g_logicFps * 6) / g_qpcFreq.QuadPart;
        int64_t behind = expected - done;
        // More than 2 ticks behind (loading, pause, stall) or running ahead: forget the debt
        // rather than fast-forwarding.
        if (behind > 12 || behind < -6) {
            g_anchored = false;
        } else if (behind >= 2) {
            // step() runs from the next sub-step to the end of its aligned block of 6/ratio,
            // so pick the ratio whose block covers the most sub-steps without exceeding the debt.
            int counter = *(int*)(engine + 0x58);
            int next = counter >= 6 ? 1 : counter + 1;
            int maxRatio = g_maxUpdateFps / g_logicFps;
            int bestRatio = maxRatio, bestCount = 1;
            static const int kRatios[] = { 6, 3, 2, 1 };
            for (int r : kRatios) {
                if (r > maxRatio)
                    continue;
                int block = 6 / r;
                int count = ((next - 1) / block + 1) * block - next + 1;
                if (count <= behind && count > bestCount) {
                    bestCount = count;
                    bestRatio = r;
                }
            }
            g_stepFps = bestRatio * g_logicFps;
            if (bestCount > 1)
                g_catchUpFrames++;
        }
    } else {
        g_anchored = false;
    }
    if (!g_anchored) {
        g_anchorTime = now;
        g_anchorDone = done;
        g_anchored = true;
    }
    g_lastDone = done;
}

// Main loop, replaces `mov esi, [MaxUpdateFPS]`; edi is the GameEngine. Must preserve flags
// because the following jle consumes the preceding cmp.
static __declspec(naked) void StubMainLoop()
{
    __asm {
        pushfd
        pushad
        push edi
        call OnFrame
        add esp, 4
        popad
        popfd
        mov esi, dword ptr [g_maxUpdateFps]
        ret
    }
}

// Adaptive ratio (network games only), replaces `mov [edi+0x17c], eax`. The sub-step
// scheduler only stays in step for ratios that divide 6, so round 4/5 down to 3 and cap at 6.
static __declspec(naked) void StubRatioStore()
{
    __asm {
        cmp eax, 6
        jge six
        cmp eax, 4
        jl store
        mov eax, 3
        jmp store
    six:
        mov eax, 6
    store:
        mov dword ptr [edi + 0x17c], eax
        ret
    }
}

// ---------------------------------------------------------------------------------------
// Widescreen FOV
//
// Camera::setFOV(float hfov, float vfov) is called with vfov = -1, meaning "derive vertical
// from aspect": tan(v/2) = tan(h/2) / aspect. The horizontal FOV is fixed (50deg x 1.33 on
// widescreen), so screens wider than 16:9 lose vertical view and look zoomed in. We widen
// hfov so the vertical view matches 16:9 (Hor+). The camera's aspect is at +0x210.

static uint8_t* g_setFovResume;       // original setFOV + 10
static float g_fovBaseAspect = 16.0f / 9.0f;
static float g_fovScale = 1.0f;        // extra multiplier on the corrected tan(h/2)

extern "C" void __cdecl AdjustFov(float* args, uint8_t* camera)
{
    if (args[1] != -1.0f)
        return;
    float aspect = *(float*)(camera + 0x210);
    if (!(aspect > g_fovBaseAspect + 0.01f) && g_fovScale == 1.0f)
        return;
    float widen = aspect > g_fovBaseAspect ? aspect / g_fovBaseAspect : 1.0f;
    float t = tanf(args[0] * 0.5f) * widen * g_fovScale;
    float h = 2.0f * atanf(t);
    const float maxH = 150.0f * 3.14159265f / 180.0f;
    args[0] = h < maxH ? h : maxH;
}

// Replaces the first 10 bytes of setFOV (`fld [esp+4]; movss xmm0,[esp+8]`), which are
// re-executed here after adjusting the arguments in place.
static __declspec(naked) void StubSetFov()
{
    __asm {
        pushfd
        pushad
        push ecx
        lea eax, [esp + 4 + 36 + 4]
        push eax
        call AdjustFov
        add esp, 8
        popad
        popfd
        fld dword ptr [esp + 4]
        movss xmm0, dword ptr [esp + 8]
        jmp dword ptr [g_setFovResume]
    }
}

static void ApplyFovPatch(Module& m)
{
    static const uint8_t pat[] = { 0xD9, 0x44, 0x24, 0x04, 0xF3, 0x0F, 0x10, 0x44, 0x24, 0x08, 0xD9, 0x05,
                                   0, 0, 0, 0, 0x0F, 0x2E, 0x05, 0, 0, 0, 0, 0xDC, 0xC9, 0x9F, 0xF6, 0xC4, 0x44 };
    uint8_t* hit[2];
    int n = Scan(m, pat, "xxxxxxxxxxxx????xxx????xxxxxx", hit, 2);
    if (n != 1) {
        Log("widescreen FOV: setFOV signature found %d times; FOV fix not applied", n);
        return;
    }
    g_fovScale = IniInt("FOVPercent", 100) / 100.0f;
    g_setFovResume = hit[0] + 10;
    uint8_t code[10] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90 };
    int32_t rel = (int32_t)((uint8_t*)StubSetFov - (hit[0] + 5));
    memcpy(code + 1, &rel, 4);
    Write(hit[0], code, sizeof(code));
    Log("widescreen FOV: Hor+ fix applied at %08X (FOVPercent=%d)", (uint32_t)hit[0], IniInt("FOVPercent", 100));
}

static int ValidFps(int requested, int logicFps)
{
    int ratio = requested / logicFps;
    if (ratio < 1)
        ratio = 1;
    while (ratio > 1 && (ratio > 6 || 6 % ratio != 0))
        ratio--;
    return ratio * logicFps;
}

static bool ApplyPatches()
{
    Module m;
    if (!FindText(m)) {
        Log("ERROR: no .text section");
        return false;
    }

    // Anchor: the sub-step function's `mov eax,[MaxUpdateFPS]; xor edx,edx; div [LogicFPS];
    // mov edi,eax; cmp edi,6; jge`. Gives us both constants' addresses.
    static const uint8_t anchorPat[] = { 0xA1, 0, 0, 0, 0, 0x33, 0xD2, 0xF7, 0x35, 0, 0, 0, 0,
                                         0x8B, 0xF8, 0x83, 0xFF, 0x06, 0x0F, 0x8D };
    uint8_t* hits[8];
    if (Scan(m, anchorPat, "x????xxxx????xxxxxxx", hits, 8) != 1) {
        Log("ERROR: sub-step anchor not found exactly once; unsupported game build");
        return false;
    }
    uint32_t maxFpsAddr = *(uint32_t*)(hits[0] + 1);
    uint32_t logicFpsAddr = *(uint32_t*)(hits[0] + 9);
    int logicFps = *(int*)logicFpsAddr;
    int vanillaMax = *(int*)maxFpsAddr;
    Log("MaxUpdateFPS @%08X = %d, LogicFPS @%08X = %d", maxFpsAddr, vanillaMax, logicFpsAddr, logicFps);
    if (logicFps != 15 || vanillaMax != 30) {
        Log("ERROR: unexpected engine constants; not patching");
        return false;
    }

    // All `mov eax,[MaxUpdateFPS]; xor edx,edx; div [LogicFPS]` ratio computations.
    uint8_t ratioPat[13] = { 0xA1, 0, 0, 0, 0, 0x33, 0xD2, 0xF7, 0x35 };
    memcpy(ratioPat + 1, &maxFpsAddr, 4);
    memcpy(ratioPat + 9, &logicFpsAddr, 4);
    uint8_t* ratioSites[8];
    int nRatio = Scan(m, ratioPat, "xxxxxxxxxxxxx", ratioSites, 8);

    // Main loop: `mov ecx,[edi+0x174]; cmp ecx,ebp; mov esi,[MaxUpdateFPS]`.
    uint8_t loopPat[14] = { 0x8B, 0x8F, 0x74, 0x01, 0x00, 0x00, 0x3B, 0xCD, 0x8B, 0x35 };
    memcpy(loopPat + 10, &maxFpsAddr, 4);
    uint8_t* loopSite[2];
    int nLoop = Scan(m, loopPat, "xxxxxxxxxxxxxx", loopSite, 2);

    // Adaptive ratio store: `imul ecx; sar edx,2; ...; mov [edi+0x17c],eax`.
    static const uint8_t storePat[] = { 0xF7, 0xE9, 0xC1, 0xFA, 0x02, 0x8B, 0xC2, 0xC1, 0xE8, 0x1F,
                                        0x03, 0xC2, 0x89, 0x87, 0x7C, 0x01, 0x00, 0x00 };
    uint8_t* storeSite[2];
    int nStore = Scan(m, storePat, "xxxxxxxxxxxxxxxxxx", storeSite, 2);

    // TheGameLogic pointer, used only for the stats log: `mov eax,[TheGameLogic];
    // mov eax,[eax+0x50]; mov ecx,[..]; ... mov esi,[edi+0x58]`.
    static const uint8_t logicPat[] = { 0xA1, 0, 0, 0, 0, 0x8B, 0x40, 0x50, 0x8B, 0x0D, 0, 0, 0, 0,
                                        0x8B, 0x77, 0x58 };
    uint8_t* logicSite[2];
    int nLogic = Scan(m, logicPat, "x????xxxxx????xxx", logicSite, 2);

    // Network interface pointer (null when offline): `cmp [TheNetwork],ebp; mov ecx,[LogicFPS]`
    // in the main loop's adaptive-ratio block.
    uint8_t netPat[12] = { 0x39, 0x2D, 0, 0, 0, 0, 0x8B, 0x0D };
    memcpy(netPat + 8, &logicFpsAddr, 4);
    uint8_t* netSite[2];
    int nNet = Scan(m, netPat, "xx????xxxxxx", netSite, 2);

    Log("signatures: ratio=%d (want 5) loop=%d (want 1) store=%d (want 1) logic=%d (want 1) net=%d (want 1)",
        nRatio, nLoop, nStore, nLogic, nNet);
    if (nRatio != 5 || nLoop != 1 || nStore != 1) {
        Log("ERROR: signature mismatch; not patching");
        return false;
    }
    if (nLogic == 1)
        g_theGameLogic = *(uint32_t**)(logicSite[0] + 1);
    if (nNet == 1)
        g_theNetwork = *(uint32_t**)(netSite[0] + 2);

    int requested = IniInt("TargetFPS", 90);
    g_logicFps = logicFps;
    g_maxUpdateFps = ValidFps(requested, logicFps);
    g_stepFps = g_maxUpdateFps;
    if (g_maxUpdateFps != requested)
        Log("TargetFPS %d is not a valid rate (must be 15 x 1/2/3/6); using %d", requested, g_maxUpdateFps);

    // Catch-up needs to read the logic frame and to know when we're online.
    g_catchUp = IniInt("CatchUp", 1) && g_theGameLogic && g_theNetwork;
    QueryPerformanceFrequency(&g_qpcFreq);

    // The anchor is the sub-step function's ratio site; it reads the per-frame g_stepFps.
    uint32_t ours = (uint32_t)&g_maxUpdateFps;
    uint32_t step = (uint32_t)&g_stepFps;
    for (int i = 0; i < nRatio; i++)
        Write(ratioSites[i] + 1, ratioSites[i] == hits[0] ? &step : &ours, 4);
    WriteCall6(loopSite[0] + 8, StubMainLoop);
    WriteCall6(storeSite[0] + 12, StubRatioStore);

    // Draw-module animation loop: `fld [SecondsPerFrame]; push ecx; mov ecx,[esi+0x64];
    // fstp [esp]; call advance; add esi,0xcc`.
    static const uint8_t animPat[] = { 0xD9, 0x05, 0, 0, 0, 0, 0x51, 0x8B, 0x4E, 0x64, 0xD9, 0x1C, 0x24,
                                       0xE8, 0, 0, 0, 0, 0x81, 0xC6, 0xCC, 0x00, 0x00, 0x00 };
    uint8_t* animSite[2];
    int nAnim = Scan(m, animPat, "xx????xxxxxxxx????xxxxxx", animSite, 2);
    if (nAnim == 1 && IniInt("FixAnimationSpeed", 1)) {
        uint32_t dt = (uint32_t)&g_animDt;
        Write(animSite[0] + 2, &dt, 4);
        Log("animation speed fix applied at %08X", (uint32_t)animSite[0]);
    } else {
        Log("animation speed fix not applied (signature found %d times)", nAnim);
    }

    // Client animation clock: `mov ecx,[MsPerFrame]; mov eax,[AnimTime]; imul ecx,esi;
    // add eax,ecx; push eax; mov [AnimTime],eax` (normal path), plus the special-mode path
    // `mov eax,[AnimTime]; add eax,[MsPerFrame]; push eax; mov [AnimTime],eax`.
    static const uint8_t clockPat[] = { 0x8B, 0x0D, 0, 0, 0, 0, 0xA1, 0, 0, 0, 0, 0x0F, 0xAF, 0xCE,
                                        0x03, 0xC1, 0x50, 0xA3 };
    uint8_t* clockSite[2];
    int nClock = Scan(m, clockPat, "xx????x????xxxxxxx", clockSite, 2);
    if (nClock == 1 && IniInt("FixAnimationSpeed", 1)) {
        uint32_t msPerFrameAddr = *(uint32_t*)(clockSite[0] + 2);
        uint32_t animTimeAddr = *(uint32_t*)(clockSite[0] + 7);
        uint8_t altPat[14] = { 0xA1, 0, 0, 0, 0, 0x03, 0x05, 0, 0, 0, 0, 0x50, 0xA3 };
        memcpy(altPat + 1, &animTimeAddr, 4);
        memcpy(altPat + 7, &msPerFrameAddr, 4);
        uint8_t* altSite[2];
        int nAlt = Scan(m, altPat, "xxxxxxxxxxxxx", altSite, 2);
        uint32_t ms = (uint32_t)&g_animMs;
        Write(clockSite[0] + 2, &ms, 4);
        if (nAlt == 1)
            Write(altSite[0] + 7, &ms, 4);
        Log("animation clock fix applied at %08X (alt path %s)", (uint32_t)clockSite[0], nAlt == 1 ? "too" : "not found");
    } else {
        Log("animation clock fix not applied (signature found %d times)", nClock);
    }

    Log("patched: render cadence %d fps, logic %d Hz (%d sub-steps per frame), catch-up %s",
        g_maxUpdateFps, logicFps, 6 / (g_maxUpdateFps / logicFps), g_catchUp ? "on (offline games)" : "off");
    return true;
}

// Logs measured draw rate and logic rate so game speed can be verified from the log alone.
static DWORD WINAPI StatsThread(void*)
{
    LONG lastFrames = g_drawnFrames;
    LONG lastCatchUp = g_catchUpFrames;
    uint32_t lastLogic = 0;
    bool haveLogic = false;
    DWORD lastTime = GetTickCount();
    while (g_log) {
        Sleep(5000);
        DWORD now = GetTickCount();
        double dt = (now - lastTime) / 1000.0;
        LONG frames = g_drawnFrames;
        LONG catchUp = g_catchUpFrames;
        double fps = (frames - lastFrames) / dt;

        bool logicOk;
        uint32_t logic = ReadLogicFrame(&logicOk);
        if (logicOk && haveLogic && logic >= lastLogic)
            Log("draw %.1f fps | logic %.2f Hz | catch-up frames %ld", fps, (logic - lastLogic) / dt,
                catchUp - lastCatchUp);
        else
            Log("draw %.1f fps | logic n/a", fps);

        lastCatchUp = catchUp;
        lastFrames = frames;
        lastLogic = logic;
        haveLogic = logicOk;
        lastTime = now;
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;
    DisableThreadLibraryCalls(inst);

    GetModuleFileNameA(inst, g_dir, MAX_PATH);
    char* slash = strrchr(g_dir, '\\');
    if (slash)
        slash[1] = 0;

    if (IniInt("Log", 1)) {
        char path[MAX_PATH];
        sprintf_s(path, "%sRA3FrameMod.log", g_dir);
        g_log = _fsopen(path, "w", _SH_DENYWR);
    }

    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    Log("RA3FrameMod loaded into %s", exe);

    if (!IniInt("Enabled", 1)) {
        Log("disabled in ini");
        return TRUE;
    }
    if (ApplyPatches() && g_log && IniInt("LogStats", 1))
        CloseHandle(CreateThread(NULL, 0, StatsThread, NULL, 0, NULL));
    Module m;
    if (IniInt("WidescreenFix", 1) && FindText(m))
        ApplyFovPatch(m);
    return TRUE;
}
