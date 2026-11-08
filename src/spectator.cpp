// EDF6 Spectator: while you're down waiting for a revive, your camera follows your teammates.
//
// Every player soldier gets a CharacterGhostCamera that follows it through a weak_ptr kept at
// +0x350 / +0x358. The camera's per-frame update (vtable slot 4) locks that weak_ptr and rebuilds
// its view from the followed object: world matrix, camera block, player options. Pointing the
// weak_ptr at a teammate's soldier makes the camera follow that teammate with their own camera
// setup. Nothing is sent over the network: the change only lives in the local camera.

#include <windows.h>
#include <xinput.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include <MinHook.h>
#include <PluginAPI.h>

namespace {

constexpr char kCameraRtti[] = ".?AVCharacterGhostCamera@@";
constexpr char kSoldierRtti[] = ".?AVSoldierBase@@";
constexpr char kHumanRtti[] = ".?AVHumanBase@@";
constexpr int kDtorSlot = 0;
constexpr int kUpdateSlot = 4;

// CharacterGhostCamera
constexpr int32_t kCamTargetPtr = 0x350;
constexpr int32_t kCamTargetCtrl = 0x358;
// Game objects (GameObjectBase): enable_shared_from_this, team and player number.
constexpr int32_t kSelfWeakPtr = 0x28;
constexpr int32_t kSelfWeakCtrl = 0x30;
constexpr int32_t kTeamOffset = 0x314;
constexpr int32_t kPlayerNoOffset = 0x338;
// MSVC _Ref_count_base
constexpr int32_t kCtrlUses = 0x8;
constexpr int32_t kCtrlWeaks = 0xC;
// Team manager: a CRITICAL_SECTION at +8 and a table of 0x38-byte entries at +0x38, one per team.
// Each entry starts with the head of a std::set<GameObject *>; +0x28 is the number of teams.
constexpr int32_t kTeamLock = 0x8;
constexpr int32_t kTeamTable = 0x38;
constexpr int32_t kTeamStride = 0x38;
constexpr int32_t kTeamCount = 0x28;
// std::set node
constexpr int32_t kNodeLeft = 0x0;
constexpr int32_t kNodeParent = 0x8;
constexpr int32_t kNodeRight = 0x10;
constexpr int32_t kNodeIsNil = 0x19;
constexpr int32_t kNodeValue = 0x20;

constexpr int kPlayerTeam = 0;
constexpr int kAllyNpcTeam = 2;
constexpr int kMaxWatches = 8;
constexpr int kMaxTargets = 32;
constexpr ULONGLONG kRescanMs = 1000;
constexpr ULONGLONG kPadPollMs = 15;
constexpr DWORD kWaitForGameDllMs = 30000;

// `cmp dword ptr [rbx+314h], -1 / je / mov rcx, [rip+teamManager] / mov rdx, rbx / call`: the
// team change of a game object (today EDF.dll+0x54EEC1) loads the team manager right there.
const uint8_t kTeamManagerLoad[] = {0x83, 0xBB, 0x14, 0x03, 0x00, 0x00, 0xFF, 0x74, 0x00, 0x48,
                                    0x8B, 0x0D, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xD3, 0xE8};
const char kTeamManagerLoadMask[] = "xxxxxxxx?xxx????xxxx";
// The HP write of the damage function (same block EDF6-EnemyHp hooks): addss / minss with max HP /
// maxss / movss into HP.
const uint8_t kHpWriteBlock[] = {
    0xF3, 0x0F, 0x58, 0x87, 0x00, 0x00, 0x00, 0x00,
    0xF3, 0x0F, 0x5D, 0x87, 0x00, 0x00, 0x00, 0x00,
    0xF3, 0x0F, 0x5F, 0x87, 0x00, 0x00, 0x00, 0x00,
    0xF3, 0x0F, 0x11, 0x87, 0x00, 0x00, 0x00, 0x00,
};
const char kHpWriteBlockMask[] = "xxxx??xxxxxx??xxxxxx??xxxxxx??xx";
// `lea rdx, [rdi+350h]`: the update locks the followed object's weak_ptr.
const uint8_t kUpdateLocksTarget[] = {0x48, 0x8D, 0x97, 0x50, 0x03, 0x00, 0x00};
constexpr size_t kUpdateLockWithin = 0x100;

using UpdateFn = void(__fastcall *)(uintptr_t camera, const float *dt);
using DtorFn = uintptr_t(__fastcall *)(uintptr_t camera, uint32_t flags);
using XInputGetStateFn = DWORD(WINAPI *)(DWORD, XINPUT_STATE *);

struct Config {
    float delaySeconds = 2.0f;
    int nextKey = VK_LBUTTON;
    int prevKey = VK_RBUTTON;
    int selfKey = 0;
    bool gamepad = true;
    bool debug = false;
    int debugKey = VK_F8;
};

// Edge detection per camera, so two cameras updating in the same frame both see a press.
enum Button { kNext, kPrev, kSelf, kDebug, kButtons };

struct Watch {
    uintptr_t camera = 0;
    ULONGLONG downSince = 0;
    bool spectating = false;
    bool forced = false;      // started with the debug key while alive
    bool manualSelf = false;  // the player picked their own body on purpose
    uintptr_t ownPtr = 0;     // the camera's original target; we hold a weak reference to it
    uintptr_t ownCtrl = 0;
    uintptr_t viewPtr = 0;    // what we last put in the camera
    ULONGLONG nextRescan = 0;
    bool held[kButtons] = {};
};

struct Target {
    uintptr_t ptr = 0;
    uintptr_t ctrl = 0;
    int playerNo = -1;
};

Config config;
uintptr_t gameBase = 0;
uintptr_t rdataStart = 0;
uintptr_t rdataEnd = 0;
uintptr_t *teamManagerSlot = nullptr;
int32_t hpOffset = 0;
uintptr_t soldierDescriptor = 0;
uintptr_t humanDescriptor = 0;
UpdateFn originalUpdate = nullptr;
DtorFn originalDtor = nullptr;
Watch watches[kMaxWatches];
bool disabled = false;

XInputGetStateFn xinputGetState = nullptr;
ULONGLONG padPolledAt = 0;
WORD padButtons = 0;

std::string GamePath(const char *relative) {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char *slash = strrchr(path, '\\');
    if (slash) {
        *(slash + 1) = '\0';
    }
    return std::string(path) + relative;
}

void Log(const char *message) {
    FILE *fh = nullptr;
    if (fopen_s(&fh, GamePath("Spectator.log").c_str(), "a") == 0 && fh) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(fh, "[%02d:%02d:%02d] %s\n", t.wHour, t.wMinute, t.wSecond, message);
        fclose(fh);
    }
}

void LogF(const char *fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Log(buf);
}

std::string Trim(std::string s) {
    s.erase(0, s.find_first_not_of(" \t\r"));
    s.erase(s.find_last_not_of(" \t\r") + 1);
    return s;
}

int ParseKey(const std::string &value, int fallback) {
    const long vk = strtol(value.c_str(), nullptr, 0);
    return vk >= 0 && vk < 256 ? (int)vk : fallback;
}

void ReadConfig() {
    std::ifstream in(GamePath("Mods\\Spectator\\config.ini"));
    std::string line;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }
        const size_t equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        const std::string key = Trim(line.substr(0, equals));
        const std::string value = Trim(line.substr(equals + 1));
        if (key == "delay") {
            const float seconds = strtof(value.c_str(), nullptr);
            if (seconds >= 0.0f && seconds <= 60.0f) {
                config.delaySeconds = seconds;
            }
        } else if (key == "next") {
            config.nextKey = ParseKey(value, config.nextKey);
        } else if (key == "prev") {
            config.prevKey = ParseKey(value, config.prevKey);
        } else if (key == "self") {
            config.selfKey = ParseKey(value, config.selfKey);
        } else if (key == "gamepad") {
            config.gamepad = atoi(value.c_str()) != 0;
        } else if (key == "debug") {
            config.debug = atoi(value.c_str()) != 0;
        } else if (key == "debug_key") {
            config.debugKey = ParseKey(value, config.debugKey);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Memory helpers

template <typename T> T Read(uintptr_t address) { return *reinterpret_cast<const T *>(address); }

bool Alive(uintptr_t ctrl) {
    return ctrl && *reinterpret_cast<volatile const LONG *>(ctrl + kCtrlUses) > 0;
}

void WeakAddRef(uintptr_t ctrl) {
    if (ctrl) {
        InterlockedIncrement(reinterpret_cast<volatile LONG *>(ctrl + kCtrlWeaks));
    }
}

// Same as std::weak_ptr's destructor: the last weak reference deletes the control block.
void WeakRelease(uintptr_t ctrl) {
    if (ctrl && InterlockedDecrement(reinterpret_cast<volatile LONG *>(ctrl + kCtrlWeaks)) == 0) {
        auto vtable = Read<void (**)(uintptr_t)>(ctrl);
        vtable[1](ctrl);
    }
}

void SetCameraTarget(uintptr_t camera, uintptr_t ptr, uintptr_t ctrl) {
    WeakAddRef(ctrl);
    const uintptr_t old = Read<uintptr_t>(camera + kCamTargetCtrl);
    *reinterpret_cast<uintptr_t *>(camera + kCamTargetPtr) = ptr;
    *reinterpret_cast<uintptr_t *>(camera + kCamTargetCtrl) = ctrl;
    WeakRelease(old);
}

bool InRdata(uintptr_t p) { return p >= rdataStart && p + 0x18 <= rdataEnd; }

// MSVC RTTI: vtable[-1] is the complete object locator, whose class hierarchy descriptor lists
// every base class. True when `descriptor` (a type descriptor) is among them.
bool HierarchyHas(uintptr_t vtable, uintptr_t descriptor) {
    if (!InRdata(vtable)) {
        return false;
    }
    const uintptr_t locator = Read<uintptr_t>(vtable - 8);
    if (!InRdata(locator) || Read<uint32_t>(locator) != 1) {
        return false;
    }
    const uintptr_t hierarchy = gameBase + Read<uint32_t>(locator + 0x10);
    const uint32_t count = Read<uint32_t>(hierarchy + 0x8);
    const uintptr_t array = gameBase + Read<uint32_t>(hierarchy + 0xC);
    for (uint32_t i = 0; i < count && i < 64; i++) {
        const uintptr_t base = gameBase + Read<uint32_t>(array + 4 * i);
        if (gameBase + Read<uint32_t>(base) == descriptor) {
            return true;
        }
    }
    return false;
}

bool IsKindOf(uintptr_t object, uintptr_t descriptor) {
    struct Entry {
        uintptr_t vtable;
        uintptr_t descriptor;
        bool result;
    };
    static Entry cache[64];
    static int next = 0;
    if (!descriptor) {
        return false;
    }
    const uintptr_t vtable = Read<uintptr_t>(object);
    for (const Entry &e : cache) {
        if (e.vtable == vtable && e.descriptor == descriptor) {
            return e.result;
        }
    }
    const bool result = HierarchyHas(vtable, descriptor);
    cache[next] = Entry{vtable, descriptor, result};
    next = (next + 1) % 64;
    return result;
}

float Hp(uintptr_t object) { return Read<float>(object + hpOffset); }
int Team(uintptr_t object) { return Read<int32_t>(object + kTeamOffset); }
int PlayerNo(uintptr_t object) { return Read<int32_t>(object + kPlayerNoOffset); }

bool IsPlayerSoldier(uintptr_t object) {
    return object && Team(object) == kPlayerTeam && PlayerNo(object) != -1 && IsKindOf(object, soldierDescriptor);
}

// Debug only: allied NPC soldiers, so the camera swap can be tried offline.
bool IsAllyNpc(uintptr_t object) {
    const int team = Team(object);
    return (team == kPlayerTeam || team == kAllyNpcTeam) && PlayerNo(object) == -1 &&
           IsKindOf(object, humanDescriptor);
}

uintptr_t NextNode(uintptr_t node) {
    uintptr_t right = Read<uintptr_t>(node + kNodeRight);
    if (!Read<uint8_t>(right + kNodeIsNil)) {
        node = right;
        for (uintptr_t left = Read<uintptr_t>(node + kNodeLeft); !Read<uint8_t>(left + kNodeIsNil);
             left = Read<uintptr_t>(node + kNodeLeft)) {
            node = left;
        }
        return node;
    }
    uintptr_t parent = Read<uintptr_t>(node + kNodeParent);
    while (!Read<uint8_t>(parent + kNodeIsNil) && node == Read<uintptr_t>(parent + kNodeRight)) {
        node = parent;
        parent = Read<uintptr_t>(node + kNodeParent);
    }
    return parent;
}

void AddTarget(uintptr_t object, int playerNo, Target *out, int *count) {
    if (*count >= kMaxTargets) {
        return;
    }
    const uintptr_t ptr = Read<uintptr_t>(object + kSelfWeakPtr);
    const uintptr_t ctrl = Read<uintptr_t>(object + kSelfWeakCtrl);
    if (!ptr || !Alive(ctrl) || Hp(ptr) <= 0.0f) {
        return;
    }
    out[(*count)++] = Target{ptr, ctrl, playerNo};
}

// Living teammates other than `own`, players first by player number, then (debug) allied NPCs.
int GatherTargets(uintptr_t own, Target *out) {
    const uintptr_t manager = teamManagerSlot ? *teamManagerSlot : 0;
    if (!manager) {
        return 0;
    }
    int count = 0;
    auto lock = reinterpret_cast<CRITICAL_SECTION *>(manager + kTeamLock);
    EnterCriticalSection(lock);
    const uintptr_t table = Read<uintptr_t>(manager + kTeamTable);
    const size_t teams = table ? Read<size_t>(table + kTeamCount) : 0;
    const int wanted[] = {kPlayerTeam, kAllyNpcTeam};
    for (int pass = 0; pass < (config.debug ? 2 : 1); pass++) {
        for (int team : wanted) {
            if ((size_t)team >= teams) {
                continue;
            }
            const uintptr_t head = Read<uintptr_t>(table + team * kTeamStride);
            int guard = 0;
            for (uintptr_t node = Read<uintptr_t>(head + kNodeLeft); node != head && guard < 4096;
                 node = NextNode(node), guard++) {
                const uintptr_t object = Read<uintptr_t>(node + kNodeValue);
                if (!object || Read<uintptr_t>(object + kSelfWeakPtr) == own) {
                    continue;
                }
                if (pass == 0 && IsPlayerSoldier(object)) {
                    AddTarget(object, PlayerNo(object), out, &count);
                } else if (pass == 1 && IsAllyNpc(object)) {
                    AddTarget(object, -1, out, &count);
                }
            }
        }
    }
    LeaveCriticalSection(lock);
    // Players by number; NPCs keep the set order after them.
    for (int i = 1; i < count; i++) {
        for (int j = i; j > 0 && out[j].playerNo != -1 &&
                        (out[j - 1].playerNo == -1 || out[j - 1].playerNo > out[j].playerNo);
             j--) {
            const Target t = out[j];
            out[j] = out[j - 1];
            out[j - 1] = t;
        }
    }
    return count;
}

// ---------------------------------------------------------------------------------------------
// Input

bool GameHasFocus() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

WORD PadButtons(ULONGLONG now) {
    if (!config.gamepad || !xinputGetState) {
        return 0;
    }
    if (now - padPolledAt >= kPadPollMs) {
        padPolledAt = now;
        padButtons = 0;
        XINPUT_STATE state;
        for (DWORD pad = 0; pad < XUSER_MAX_COUNT; pad++) {
            if (xinputGetState(pad, &state) == ERROR_SUCCESS) {
                padButtons |= state.Gamepad.wButtons;
            }
        }
    }
    return padButtons;
}

bool KeyDown(int vk) { return vk > 0 && (GetAsyncKeyState(vk) & 0x8000) != 0; }

// True on the frame the button goes down (keyboard / mouse key or gamepad button).
bool Pressed(Watch *w, Button button, ULONGLONG now) {
    bool down = false;
    if (GameHasFocus()) {
        const WORD pad = PadButtons(now);
        switch (button) {
        case kNext: down = KeyDown(config.nextKey) || (pad & XINPUT_GAMEPAD_RIGHT_SHOULDER); break;
        case kPrev: down = KeyDown(config.prevKey) || (pad & XINPUT_GAMEPAD_LEFT_SHOULDER); break;
        case kSelf: down = KeyDown(config.selfKey); break;
        case kDebug: down = config.debug && KeyDown(config.debugKey); break;
        default: break;
        }
    }
    const bool edge = down && !w->held[button];
    w->held[button] = down;
    return edge;
}

void SyncButtons(Watch *w, ULONGLONG now) {
    for (int b = 0; b < kButtons; b++) {
        Pressed(w, (Button)b, now);
    }
}

// ---------------------------------------------------------------------------------------------
// Spectating

Watch *FindWatch(uintptr_t camera) {
    for (Watch &w : watches) {
        if (w.camera == camera) {
            return &w;
        }
    }
    return nullptr;
}

Watch *AllocWatch(uintptr_t camera, ULONGLONG now) {
    for (Watch &w : watches) {
        if (!w.camera) {
            w = Watch{};
            w.camera = camera;
            SyncButtons(&w, now);
            return &w;
        }
    }
    return nullptr;
}

void ForgetWatch(Watch *w) {
    if (w->spectating) {
        WeakRelease(w->ownCtrl);
    }
    *w = Watch{};
}

void StartSpectating(Watch *w, uintptr_t ptr, uintptr_t ctrl, bool forced) {
    WeakAddRef(ctrl);
    w->spectating = true;
    w->forced = forced;
    w->manualSelf = false;
    w->ownPtr = ptr;
    w->ownCtrl = ctrl;
    w->viewPtr = ptr;
    w->nextRescan = 0;
    LogF("player %d: spectating%s", PlayerNo(ptr), forced ? " (debug key)" : "");
}

void StopSpectating(Watch *w, const char *why) {
    if (Read<uintptr_t>(w->camera + kCamTargetPtr) != w->ownPtr && Alive(w->ownCtrl)) {
        SetCameraTarget(w->camera, w->ownPtr, w->ownCtrl);
    }
    LogF("back to own body: %s", why);
    ForgetWatch(w);
}

void Show(Watch *w, uintptr_t ptr, uintptr_t ctrl, int playerNo) {
    if (Read<uintptr_t>(w->camera + kCamTargetPtr) != ptr) {
        SetCameraTarget(w->camera, ptr, ctrl);
    }
    w->viewPtr = ptr;
    if (ptr == w->ownPtr) {
        Log("watching own body");
    } else if (playerNo >= 0) {
        LogF("watching player %d", playerNo);
    } else {
        LogF("watching ally %p", reinterpret_cast<void *>(ptr));
    }
}

void TickIdle(uintptr_t camera, Watch *w, ULONGLONG now) {
    const uintptr_t ptr = Read<uintptr_t>(camera + kCamTargetPtr);
    const uintptr_t ctrl = Read<uintptr_t>(camera + kCamTargetCtrl);
    if (!ptr || !Alive(ctrl) || !IsPlayerSoldier(ptr)) {
        if (w) {
            w->downSince = 0;
        }
        return;
    }
    if (!w && (Hp(ptr) <= 0.0f || config.debug)) {
        w = AllocWatch(camera, now);
    }
    if (!w) {
        return;
    }
    if (Pressed(w, kDebug, now)) {
        StartSpectating(w, ptr, ctrl, true);
        return;
    }
    SyncButtons(w, now);
    if (Hp(ptr) > 0.0f) {
        w->downSince = 0;
        return;
    }
    if (!w->downSince) {
        w->downSince = now;
        LogF("player %d is down", PlayerNo(ptr));
    }
    if (now - w->downSince >= (ULONGLONG)(config.delaySeconds * 1000.0f)) {
        StartSpectating(w, ptr, ctrl, false);
    }
}

void TickSpectating(Watch *w, ULONGLONG now) {
    const uintptr_t camera = w->camera;
    const uintptr_t current = Read<uintptr_t>(camera + kCamTargetPtr);
    const uintptr_t currentCtrl = Read<uintptr_t>(camera + kCamTargetCtrl);
    if (current != w->viewPtr) {
        // The game pointed the camera somewhere else (respawn, vehicle, cutscene): let it be.
        Log("the game moved the camera; spectating ends");
        ForgetWatch(w);
        return;
    }
    if (!Alive(w->ownCtrl)) {
        Log("own body is gone; spectating ends");
        ForgetWatch(w);
        return;
    }
    if (Pressed(w, kDebug, now)) {
        StopSpectating(w, "debug key");
        return;
    }
    if (!w->forced && Hp(w->ownPtr) > 0.0f) {
        StopSpectating(w, "revived");
        return;
    }

    int step = 0;
    if (Pressed(w, kNext, now)) {
        step = 1;
    }
    if (Pressed(w, kPrev, now)) {
        step = -1;
    }
    const bool wantsSelf = Pressed(w, kSelf, now);
    const bool viewingSelf = current == w->ownPtr;
    const bool viewLost = !viewingSelf && (!Alive(currentCtrl) || Hp(current) <= 0.0f);
    const bool rescan = viewingSelf && !w->manualSelf && now >= w->nextRescan;
    if (!step && !wantsSelf && !viewLost && !rescan) {
        return;
    }

    if (wantsSelf) {
        w->manualSelf = true;
        if (!viewingSelf) {
            Show(w, w->ownPtr, w->ownCtrl, PlayerNo(w->ownPtr));
        }
        return;
    }

    Target targets[kMaxTargets];
    const int count = GatherTargets(w->ownPtr, targets);
    // The cycle is every living teammate and then your own body.
    int index = count;
    for (int i = 0; i < count; i++) {
        if (targets[i].ptr == current) {
            index = i;
        }
    }
    if (step) {
        index = (index + step + count + 1) % (count + 1);
        w->manualSelf = index == count;
    } else if (viewLost) {
        index = count ? 0 : count;
    } else {
        w->nextRescan = now + kRescanMs;
        if (!count) {
            return;
        }
        index = 0;
    }
    if (index == count) {
        if (!viewingSelf) {
            Show(w, w->ownPtr, w->ownCtrl, PlayerNo(w->ownPtr));
        }
    } else if (targets[index].ptr != current) {
        Show(w, targets[index].ptr, targets[index].ctrl, targets[index].playerNo);
    }
}

void Tick(uintptr_t camera) {
    const ULONGLONG now = GetTickCount64();
    Watch *w = FindWatch(camera);
    if (w && w->spectating) {
        TickSpectating(w, now);
    } else {
        TickIdle(camera, w, now);
    }
}

int SafeTick(uintptr_t camera) {
    __try {
        Tick(camera);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return (int)GetExceptionCode();
    }
}

void __fastcall HookedUpdate(uintptr_t camera, const float *dt) {
    if (!disabled) {
        const int code = SafeTick(camera);
        if (code) {
            disabled = true;
            LogF("exception 0x%08X while following the camera; the mod turns itself off", (unsigned)code);
        }
    }
    originalUpdate(camera, dt);
}

uintptr_t __fastcall HookedDtor(uintptr_t camera, uint32_t flags) {
    if (Watch *w = FindWatch(camera)) {
        ForgetWatch(w);
    }
    return originalDtor(camera, flags);
}

// ---------------------------------------------------------------------------------------------
// Locating the game's pieces

bool FindSection(HMODULE module, const char *name, const uint8_t **start, size_t *size) {
    auto base = reinterpret_cast<const uint8_t *>(module);
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt);
    const size_t length = strlen(name);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, section++) {
        if (memcmp(section->Name, name, length) == 0 && (length == 8 || section->Name[length] == '\0')) {
            *start = base + section->VirtualAddress;
            *size = section->Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

const uint8_t *FindPattern(const uint8_t *start, size_t size, const uint8_t *pattern, const char *mask) {
    const size_t length = strlen(mask);
    for (size_t i = 0; i + length <= size; i++) {
        size_t j = 0;
        while (j < length && (mask[j] == '?' || start[i + j] == pattern[j])) {
            j++;
        }
        if (j == length) {
            return start + i;
        }
    }
    return nullptr;
}

// The type descriptor keeps the mangled class name 0x10 bytes in.
uintptr_t LocateTypeDescriptor(HMODULE module, const char *mangledName) {
    const uint8_t *data = nullptr;
    size_t dataSize = 0;
    if (!FindSection(module, ".data", &data, &dataSize)) {
        return 0;
    }
    const size_t nameLength = strlen(mangledName) + 1;
    for (const uint8_t *p = data; p + nameLength <= data + dataSize; p++) {
        if (memcmp(p, mangledName, nameLength) == 0) {
            return reinterpret_cast<uintptr_t>(p) - 0x10;
        }
    }
    return 0;
}

// The complete object locator of the primary vtable points at the descriptor, and the vtable
// starts right after the pointer to its locator.
const uintptr_t *LocateVtable(HMODULE module, uintptr_t descriptor) {
    auto base = reinterpret_cast<const uint8_t *>(module);
    const uint8_t *rdata = nullptr;
    size_t rdataSize = 0;
    if (!descriptor || !FindSection(module, ".rdata", &rdata, &rdataSize)) {
        return nullptr;
    }
    const uint32_t descriptorRva = static_cast<uint32_t>(descriptor - reinterpret_cast<uintptr_t>(base));
    const uint8_t *locator = nullptr;
    for (const uint8_t *p = rdata; p + 24 <= rdata + rdataSize && !locator; p += 4) {
        const uint32_t *col = reinterpret_cast<const uint32_t *>(p);
        if (col[0] == 1 && col[1] == 0 && col[3] == descriptorRva && col[5] == static_cast<uint32_t>(p - base)) {
            locator = p;
        }
    }
    if (!locator) {
        return nullptr;
    }
    for (const uint8_t *p = rdata; p + 8 <= rdata + rdataSize; p += 8) {
        if (*reinterpret_cast<const uintptr_t *>(p) == reinterpret_cast<uintptr_t>(locator)) {
            return reinterpret_cast<const uintptr_t *>(p + 8);
        }
    }
    return nullptr;
}

unsigned long long Rva(const void *p) {
    return static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(p) - gameBase);
}

bool Locate(HMODULE gameDll) {
    gameBase = reinterpret_cast<uintptr_t>(gameDll);
    const uint8_t *text = nullptr;
    const uint8_t *rdata = nullptr;
    size_t textSize = 0;
    size_t rdataSize = 0;
    if (!FindSection(gameDll, ".text", &text, &textSize) || !FindSection(gameDll, ".rdata", &rdata, &rdataSize)) {
        Log("EDF.dll has no .text or .rdata section");
        return false;
    }
    rdataStart = reinterpret_cast<uintptr_t>(rdata);
    rdataEnd = rdataStart + rdataSize;

    const uint8_t *hpWrite = FindPattern(text, textSize, kHpWriteBlock, kHpWriteBlockMask);
    if (!hpWrite || *reinterpret_cast<const int32_t *>(hpWrite + 4) != *reinterpret_cast<const int32_t *>(hpWrite + 28)) {
        Log("couldn't find where HP is written (did the game change?)");
        return false;
    }
    hpOffset = *reinterpret_cast<const int32_t *>(hpWrite + 4);

    const uint8_t *teamLoad = FindPattern(text, textSize, kTeamManagerLoad, kTeamManagerLoadMask);
    if (!teamLoad) {
        Log("couldn't find the team manager (did the game change?)");
        return false;
    }
    teamManagerSlot = reinterpret_cast<uintptr_t *>(const_cast<uint8_t *>(teamLoad + 16) +
                                                   *reinterpret_cast<const int32_t *>(teamLoad + 12));

    soldierDescriptor = LocateTypeDescriptor(gameDll, kSoldierRtti);
    humanDescriptor = LocateTypeDescriptor(gameDll, kHumanRtti);
    if (!soldierDescriptor) {
        Log("SoldierBase type information not found");
        return false;
    }
    const uintptr_t *vtable = LocateVtable(gameDll, LocateTypeDescriptor(gameDll, kCameraRtti));
    if (!vtable) {
        Log("CharacterGhostCamera vtable not found");
        return false;
    }
    const uint8_t *update = reinterpret_cast<const uint8_t *>(vtable[kUpdateSlot]);
    bool locksTarget = false;
    for (size_t i = 0; i + sizeof(kUpdateLocksTarget) <= kUpdateLockWithin && !locksTarget; i++) {
        locksTarget = memcmp(update + i, kUpdateLocksTarget, sizeof(kUpdateLocksTarget)) == 0;
    }
    if (!locksTarget) {
        LogF("the camera update at EDF.dll+0x%llX doesn't follow its target at +0x%X", Rva(update),
             kCamTargetPtr);
        return false;
    }
    LogF("HP at +0x%X, team manager at EDF.dll+0x%llX, camera vtable at EDF.dll+0x%llX", hpOffset,
         Rva(teamManagerSlot), Rva(vtable));
    return true;
}

bool Install() {
    HMODULE gameDll = nullptr;
    for (DWORD waited = 0; !(gameDll = GetModuleHandleA("EDF.dll")) && waited < kWaitForGameDllMs; waited += 100) {
        Sleep(100);
    }
    if (!gameDll) {
        Log("EDF.dll didn't load");
        return false;
    }
    if (!Locate(gameDll)) {
        return false;
    }
    const uintptr_t *vtable = LocateVtable(gameDll, LocateTypeDescriptor(gameDll, kCameraRtti));
    void *update = reinterpret_cast<void *>(vtable[kUpdateSlot]);
    void *dtor = reinterpret_cast<void *>(vtable[kDtorSlot]);
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        LogF("MH_Initialize failed (%d)", init);
        return false;
    }
    if (MH_CreateHook(dtor, reinterpret_cast<void *>(&HookedDtor), reinterpret_cast<void **>(&originalDtor)) != MH_OK ||
        MH_CreateHook(update, reinterpret_cast<void *>(&HookedUpdate),
                      reinterpret_cast<void **>(&originalUpdate)) != MH_OK ||
        MH_EnableHook(dtor) != MH_OK || MH_EnableHook(update) != MH_OK) {
        Log("couldn't hook the camera");
        return false;
    }
    LogF("hooked camera update EDF.dll+0x%llX and destructor EDF.dll+0x%llX", Rva(update), Rva(dtor));
    return true;
}

DWORD WINAPI StartThread(LPVOID) {
    ReadConfig();
    if (config.gamepad) {
        HMODULE xinput = LoadLibraryA("xinput1_4.dll");
        if (!xinput) {
            xinput = LoadLibraryA("xinput9_1_0.dll");
        }
        if (xinput) {
            xinputGetState = reinterpret_cast<XInputGetStateFn>(GetProcAddress(xinput, "XInputGetState"));
        }
    }
    LogF("config: delay %.1f s, next 0x%02X, prev 0x%02X, self 0x%02X, gamepad %s%s", config.delaySeconds,
         config.nextKey, config.prevKey, config.selfKey, xinputGetState ? "on" : "off",
         config.debug ? ", DEBUG on" : "");
    Install();
    return 0;
}

}

extern "C" BOOL __declspec(dllexport) EML6_Load(PluginInfo *pluginInfo) {
    pluginInfo->infoVersion = PluginInfo::MaxInfoVer;
    pluginInfo->name = "Spectator";
    pluginInfo->version = PLUG_VER(0, 1, 0, 0);
    static bool started = false;
    if (started) {
        return TRUE;
    }
    started = true;
    CreateThread(nullptr, 0, StartThread, nullptr, 0, nullptr);
    return TRUE;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
