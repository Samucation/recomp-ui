// launcher_input.c — live gamepad enumeration (SDL2 + SDL3).

#include "launcher_input.h"
#include "launcher_platform.h"   // pulls the right SDL header for this build

#include <stdio.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#endif

/* Bateria do Xbox pelo XInput do Windows (19/11/2026). No adaptador sem fio e no Bluetooth o
 * driver HIDAPI do SDL não informa a carga ("SEM LEITURA" no jogo do Samuel); o XInput informa.
 * Usado só para controles Xbox e só quando o SDL não sabe. Sem DLL ou sem controle XInput: 0. */
#if defined(_WIN32)
/* Do runtime (psxrecomp/runtime/src/wgi_battery.c), ligado no mesmo executável. */
int psx_wgi_battery(unsigned short vid, unsigned short pid, int *pct, int *power);
int psx_win_bt_battery(unsigned short vid, unsigned short pid);
typedef struct { unsigned char type, level; } XBatInfo;
typedef unsigned long (__stdcall *XBatFn)(unsigned long, unsigned char, XBatInfo*);
static int xinput_battery(int* pct, int* power) {
    static XBatFn fn = NULL;
    static int tried = 0;
    if (!tried) {
        tried = 1;
        HMODULE h = LoadLibraryA("xinput1_4.dll");
        if (!h) h = LoadLibraryA("xinput1_3.dll");
        if (h) fn = (XBatFn)(void*)GetProcAddress(h, "XInputGetBatteryInformation");
    }
    if (!fn) return 0;
    for (unsigned long i = 0; i < 4; ++i) {
        XBatInfo b = {0, 0};
        if (fn(i, 0 /* BATTERY_DEVTYPE_GAMEPAD */, &b) != 0) continue;
        if (b.type == 0 /* DISCONNECTED */ || b.type == 0xFF /* UNKNOWN */) continue;
        if (b.type == 1 /* WIRED */) { *power = 4; *pct = -1; return 1; }
        static const int lv[4] = { 5, 25, 60, 100 };
        *power = 1; *pct = lv[b.level & 3];
        return 1;
    }
    return 0;
}
#endif

typedef struct {
    uint32_t id;
#if defined(LNG_SDL3)
    SDL_Gamepad* handle;
#else
    SDL_GameController* handle;
#endif
    int has_gyro;
} OpenPad;

static OpenPad s_open[LNG_MAX_PADS];

static OpenPad* find_open(uint32_t id) {
    for (int i = 0; i < LNG_MAX_PADS; ++i)
        if (s_open[i].handle && s_open[i].id == id) return &s_open[i];
    return NULL;
}

static OpenPad* free_open_slot(void) {
    for (int i = 0; i < LNG_MAX_PADS; ++i)
        if (!s_open[i].handle) return &s_open[i];
    return NULL;
}

static int id_is_live(const LauncherPad* pads, int count, uint32_t id) {
    for (int i = 0; i < count; ++i)
        if (pads[i].id == id) return 1;
    return 0;
}

static void close_open(OpenPad* pad) {
    if (!pad || !pad->handle) return;
#if defined(LNG_SDL3)
    if (pad->has_gyro)
        SDL_SetGamepadSensorEnabled(pad->handle, SDL_SENSOR_GYRO, false);
    SDL_CloseGamepad(pad->handle);
#else
#if SDL_VERSION_ATLEAST(2, 0, 14)
    if (pad->has_gyro)
        SDL_GameControllerSetSensorEnabled(
            pad->handle, SDL_SENSOR_GYRO, SDL_FALSE);
#endif
    SDL_GameControllerClose(pad->handle);
#endif
    memset(pad, 0, sizeof(*pad));
}

void launcher_input_shutdown(void) {
    for (int i = 0; i < LNG_MAX_PADS; ++i) close_open(&s_open[i]);
}

/* Recuperação do controle (20/11/2026, pedido do Samuel): um controle Xbox sem fio que perde
 * o sinal pode voltar sem que o SDL o enumere de novo (só fechando e abrindo o programa).
 * O XInput percebe a volta. Se o XInput vê mais controles ligados do que o SDL conhece por
 * 2 s seguidos, pede para reiniciar o subsistema de controles do SDL (o chamador fecha antes
 * os controles que mantém abertos). Limite: 3 tentativas seguidas, depois uma a cada 30 s. */
#if defined(_WIN32) && defined(LNG_SDL3)
typedef struct { unsigned long pkt; unsigned short btn; unsigned char lt, rt; short lx, ly, rx, ry; } XPadState;
typedef unsigned long (__stdcall *XStateFn)(unsigned long, XPadState*);
static int xinput_connected_count(void) {
    static XStateFn fn = NULL;
    static int tried = 0;
    if (!tried) {
        tried = 1;
        HMODULE h = LoadLibraryA("xinput1_4.dll");
        if (!h) h = LoadLibraryA("xinput1_3.dll");
        if (h) fn = (XStateFn)(void*)GetProcAddress(h, "XInputGetState");
    }
    if (!fn) return -1;
    int n = 0;
    for (unsigned long i = 0; i < 4; ++i) {
        XPadState st;
        if (fn(i, &st) == 0) ++n;
    }
    return n;
}
#endif

int launcher_input_rescan_due(void) {
#if defined(_WIN32) && defined(LNG_SDL3)
    static Uint64 last_check = 0, mismatch_since = 0, last_restart = 0;
    static int in_row = 0;
    const Uint64 now = SDL_GetTicks();
    if (last_check && now - last_check < 1000) return 0;
    last_check = now;
    const int xin = xinput_connected_count();
    int sdl_n = 0;
    SDL_JoystickID* ids = SDL_GetJoysticks(&sdl_n);
    SDL_free(ids);
    if (xin <= sdl_n) { mismatch_since = 0; in_row = 0; return 0; }
    if (!mismatch_since) mismatch_since = now ? now : 1;
    if (now - mismatch_since < 2000) return 0;
    if (last_restart && now - last_restart < (in_row < 3 ? 5000u : 30000u)) return 0;
    last_restart = now;
    ++in_row;
    fprintf(stderr, "launcher: XInput ve %d controle(s), SDL ve %d: reenumerando\n", xin, sdl_n);
    return 1;
#else
    return 0;
#endif
}

void launcher_input_restart_sdl(void) {
#if defined(LNG_SDL3)
    launcher_input_shutdown();
    int ng = 0, nj = 0;
    while (SDL_WasInit(SDL_INIT_GAMEPAD) && ng < 16) { SDL_QuitSubSystem(SDL_INIT_GAMEPAD); ++ng; }
    while (SDL_WasInit(SDL_INIT_JOYSTICK) && nj < 16) { SDL_QuitSubSystem(SDL_INIT_JOYSTICK); ++nj; }
    for (int i = 0; i < nj; ++i) (void)SDL_InitSubSystem(SDL_INIT_JOYSTICK);
    if (ng == 0) ng = 1;
    for (int i = 0; i < ng; ++i) (void)SDL_InitSubSystem(SDL_INIT_GAMEPAD);
#endif
}

int launcher_input_gamepad_at_rest(uint32_t id) {
    OpenPad* pad = find_open(id);
    if (!pad || !pad->handle) return 1;  // no handle → don't block capture
    // Stick rest band; well below the capture commit threshold (20000).
    const int kRest = 8000;
#if defined(LNG_SDL3)
    for (int b = 0; b < (int)SDL_GAMEPAD_BUTTON_COUNT; ++b) {
        if (SDL_GetGamepadButton(pad->handle, (SDL_GamepadButton)b))
            return 0;
    }
    for (int a = 0; a < (int)SDL_GAMEPAD_AXIS_COUNT; ++a) {
        const int v = (int)SDL_GetGamepadAxis(pad->handle, (SDL_GamepadAxis)a);
        if (v <= -kRest || v >= kRest) return 0;
    }
#else
    for (int b = 0; b < (int)SDL_CONTROLLER_BUTTON_MAX; ++b) {
        if (SDL_GameControllerGetButton(
                pad->handle, (SDL_GameControllerButton)b))
            return 0;
    }
    for (int a = 0; a < (int)SDL_CONTROLLER_AXIS_MAX; ++a) {
        const int v = (int)SDL_GameControllerGetAxis(
            pad->handle, (SDL_GameControllerAxis)a);
        if (v <= -kRest || v >= kRest) return 0;
    }
#endif
    return 1;
}

uint32_t launcher_input_gamepad_button_mask(uint32_t id) {
    OpenPad* pad = find_open(id);
    uint32_t mask = 0;
    if (!pad || !pad->handle) return 0;
#if defined(LNG_SDL3)
    const int max_buttons = (int)SDL_GAMEPAD_BUTTON_COUNT;
    for (int b = 0; b < max_buttons && b < 32; ++b) {
        if (SDL_GetGamepadButton(pad->handle, (SDL_GamepadButton)b))
            mask |= (uint32_t)1u << b;
    }
#else
    const int max_buttons = (int)SDL_CONTROLLER_BUTTON_MAX;
    for (int b = 0; b < max_buttons && b < 32; ++b) {
        if (SDL_GameControllerGetButton(
                pad->handle, (SDL_GameControllerButton)b))
            mask |= (uint32_t)1u << b;
    }
#endif
    return mask;
}

int launcher_input_poll(LauncherPad* out, int max, int enable_gyro) {
    int n = 0;
    if (!out || max <= 0) return 0;
    if (max > LNG_MAX_PADS) max = LNG_MAX_PADS;
    memset(out, 0, (size_t)max * sizeof(*out));

#if defined(LNG_SDL3)
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (ids) {
        for (int i = 0; i < count && n < max; ++i) {
            out[n].id = (uint32_t)ids[i];
            const char* nm = SDL_GetGamepadNameForID(ids[i]);
            snprintf(out[n].name, sizeof(out[n].name), "%s", nm ? nm : "Gamepad");
            out[n].guid[0] = '\0';
            {
                SDL_GUID g = SDL_GetGamepadGUIDForID(ids[i]);
                SDL_GUIDToString(g, out[n].guid, (int)sizeof(out[n].guid));
            }
            OpenPad* opened = find_open(out[n].id);
            if (!opened) {
                opened = free_open_slot();
                if (opened) {
                    opened->handle = SDL_OpenGamepad(ids[i]);
                    opened->id = out[n].id;
                    if (enable_gyro && opened->handle &&
                        SDL_GamepadHasSensor(opened->handle, SDL_SENSOR_GYRO) &&
                        SDL_SetGamepadSensorEnabled(
                            opened->handle, SDL_SENSOR_GYRO, true)) {
                        opened->has_gyro = 1;
                    }
                }
            }
            if (!enable_gyro && opened && opened->handle && opened->has_gyro) {
                SDL_SetGamepadSensorEnabled(
                    opened->handle, SDL_SENSOR_GYRO, false);
                opened->has_gyro = 0;
            } else if (enable_gyro && opened && opened->handle &&
                       !opened->has_gyro &&
                       SDL_GamepadHasSensor(
                           opened->handle, SDL_SENSOR_GYRO) &&
                       SDL_SetGamepadSensorEnabled(
                           opened->handle, SDL_SENSOR_GYRO, true)) {
                opened->has_gyro = 1;
            }
            if (enable_gyro && opened && opened->handle && opened->has_gyro) {
                float rate[3] = {0};
                if (SDL_GetGamepadSensorData(
                        opened->handle, SDL_SENSOR_GYRO, rate, 3)) {
                    out[n].has_gyro = 1;
                    out[n].gyro_z = rate[2];
                }
            }
            out[n].battery_pct = -1;
            if (opened && opened->handle) {
                int pct = -1;
                switch (SDL_GetGamepadPowerInfo(opened->handle, &pct)) {
                    case SDL_POWERSTATE_ON_BATTERY: out[n].power = 1; break;
                    case SDL_POWERSTATE_CHARGING:   out[n].power = 2; break;
                    case SDL_POWERSTATE_CHARGED:    out[n].power = 3; break;
                    case SDL_POWERSTATE_NO_BATTERY: out[n].power = 4; break;
                    default:                        out[n].power = 0; break;
                }
                out[n].battery_pct = pct >= 0 && pct <= 100 ? pct : -1;
#if defined(_WIN32)
                /* 21/11/2026: a carga do Bluetooth do Windows (a das Configurações) vem antes de
                 * todas; no Xbox Series por BLE o Windows.Gaming.Input dava 10% com o Windows em 73%.
                 * Do SDL fica só o estado (carregando/carregado/com fio). */
                if (out[n].power != 4) {
                    const int bt = psx_win_bt_battery(SDL_GetGamepadVendor(opened->handle),
                                                      SDL_GetGamepadProduct(opened->handle));
                    if (bt >= 0) {
                        out[n].battery_pct = bt;
                        if (out[n].power != 2 && out[n].power != 3) out[n].power = 1;
                    }
                }
                /* Xbox Series por Bluetooth: só o Windows.Gaming.Input informa (20/11/2026). */
                if (out[n].power == 0 && out[n].battery_pct < 0)
                    (void)psx_wgi_battery(SDL_GetGamepadVendor(opened->handle),
                                          SDL_GetGamepadProduct(opened->handle),
                                          &out[n].battery_pct, &out[n].power);
                if (out[n].power == 0 && out[n].battery_pct < 0 &&
                    (SDL_strcasestr(out[n].name, "xbox") || SDL_strcasestr(out[n].name, "xinput")))
                    (void)xinput_battery(&out[n].battery_pct, &out[n].power);
#endif
            }
            ++n;
        }
        SDL_free(ids);
    }
#else
    // SDL2 uses a device-index model. SDL_NumJoysticks() re-scans on each call
    // (fed by SDL's event pump), so a pad powered on after launch shows up here
    // without a relaunch — same hot-plug behaviour as the SDL3 path.
    const int count = SDL_NumJoysticks();
    for (int i = 0; i < count && n < max; ++i) {
        if (!SDL_IsGameController(i)) continue;   // mapped gamepads only
        // Report the stable instance id, not the volatile device index, so a
        // selection survives other pads connecting/disconnecting.
        SDL_JoystickID inst = SDL_JoystickGetDeviceInstanceID(i);
        if (inst < 0) continue;

        out[n].id = (uint32_t)inst;
        const char* nm = SDL_GameControllerNameForIndex(i);
        snprintf(out[n].name, sizeof(out[n].name), "%s", nm ? nm : "Gamepad");
        out[n].guid[0] = '\0';
        {
            SDL_JoystickGUID g = SDL_JoystickGetDeviceGUID(i);
            SDL_JoystickGetGUIDString(g, out[n].guid, (int)sizeof(out[n].guid));
        }
        OpenPad* opened = find_open(out[n].id);
        if (!opened) {
            opened = free_open_slot();
            if (opened) {
                opened->handle = SDL_GameControllerOpen(i);
                opened->id = out[n].id;
#if SDL_VERSION_ATLEAST(2, 0, 14)
                if (enable_gyro && opened->handle &&
                    SDL_GameControllerHasSensor(
                        opened->handle, SDL_SENSOR_GYRO) &&
                    SDL_GameControllerSetSensorEnabled(
                        opened->handle, SDL_SENSOR_GYRO, SDL_TRUE) == 0) {
                    opened->has_gyro = 1;
                }
#endif
            }
        }
#if SDL_VERSION_ATLEAST(2, 0, 14)
        if (!enable_gyro && opened && opened->handle && opened->has_gyro) {
            SDL_GameControllerSetSensorEnabled(
                opened->handle, SDL_SENSOR_GYRO, SDL_FALSE);
            opened->has_gyro = 0;
        } else if (enable_gyro && opened && opened->handle &&
                   !opened->has_gyro &&
                   SDL_GameControllerHasSensor(
                       opened->handle, SDL_SENSOR_GYRO) &&
                   SDL_GameControllerSetSensorEnabled(
                       opened->handle, SDL_SENSOR_GYRO, SDL_TRUE) == 0) {
            opened->has_gyro = 1;
        }
        if (enable_gyro && opened && opened->handle && opened->has_gyro) {
            float rate[3] = {0};
            if (SDL_GameControllerGetSensorData(
                    opened->handle, SDL_SENSOR_GYRO, rate, 3) == 0) {
                out[n].has_gyro = 1;
                out[n].gyro_z = rate[2];
            }
        }
#endif
        out[n].battery_pct = -1;
        if (opened && opened->handle) {
            /* SDL2 só dá o nível em faixas: vira uma porcentagem aproximada */
            switch (SDL_JoystickCurrentPowerLevel(SDL_GameControllerGetJoystick(opened->handle))) {
                case SDL_JOYSTICK_POWER_EMPTY:  out[n].power = 1; out[n].battery_pct = 5;   break;
                case SDL_JOYSTICK_POWER_LOW:    out[n].power = 1; out[n].battery_pct = 20;  break;
                case SDL_JOYSTICK_POWER_MEDIUM: out[n].power = 1; out[n].battery_pct = 55;  break;
                case SDL_JOYSTICK_POWER_FULL:   out[n].power = 1; out[n].battery_pct = 100; break;
                case SDL_JOYSTICK_POWER_WIRED:  out[n].power = 4; break;
                default:                        out[n].power = 0; break;
            }
        }
        ++n;
    }
#endif

    for (int i = 0; i < LNG_MAX_PADS; ++i)
        if (s_open[i].handle && !id_is_live(out, n, s_open[i].id))
            close_open(&s_open[i]);

    return n;
}
