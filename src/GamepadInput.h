#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

struct GamepadState {
    bool connected = false;
    bool isXInput  = false;

    // Непрерывные оси [-1 .. +1]
    float moveX    = 0.0f; // Левый стик X (стрейф)
    float moveY    = 0.0f; // Левый стик Y (вперёд / назад)
    float lookX    = 0.0f; // Правый стик X (поворот камеры)
    float lookY    = 0.0f; // Правый стик Y (наклон камеры)
    float vertical = 0.0f; // RT / R2 / A — вверх (+1), LT / L2 / B — вниз (-1)
    bool  sprint   = false; // L3 или RB / R1

    // Однократные нажатия (фронт кнопки)
    bool toggleBiome     = false; // Y / Triangle
    bool togglePostFx    = false; // X / Square
    bool summonWhale     = false; // LB / L1
    bool toggleHud       = false; // Back / View / Share
    bool regenWorld      = false; // Start / Menu / Options
    bool toggleWireframe = false; // R3 (нажатие правого стика, аналог F)
    bool toggleShadows   = false; // D-Pad Left
    bool toggleMute      = false; // D-Pad Right
    bool volumeUp        = false; // D-Pad Up
    bool volumeDown      = false; // D-Pad Down
};

class GamepadInput {
public:
    GamepadInput() {
#ifdef _WIN32
        const char* xinputDlls[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
        for (const char* name : xinputDlls) {
            m_xinputLib = LoadLibraryA(name);
            if (m_xinputLib) {
                m_fnGetState = reinterpret_cast<FnXInputGetState>(
                    GetProcAddress(m_xinputLib, "XInputGetState"));
                m_fnSetState = reinterpret_cast<FnXInputSetState>(
                    GetProcAddress(m_xinputLib, "XInputSetState"));
                if (m_fnGetState) break;
                FreeLibrary(m_xinputLib);
                m_xinputLib = nullptr;
            }
        }
        m_winmmLib = LoadLibraryA("winmm.dll");
        if (m_winmmLib) {
            m_fnJoyGetPosEx = reinterpret_cast<FnJoyGetPosEx>(
                GetProcAddress(m_winmmLib, "joyGetPosEx"));
        }
#endif
    }

    ~GamepadInput() {
        setRumble(0.0f, 0.0f);
#ifdef _WIN32
        if (m_xinputLib) FreeLibrary(m_xinputLib);
        if (m_winmmLib)  FreeLibrary(m_winmmLib);
#endif
    }

    GamepadState poll(float dt) {
        GamepadState out{};
#ifdef _WIN32
        // Плавное затухание виброотклика
        if (m_rumbleTimer > 0.0f) {
            m_rumbleTimer -= dt;
            if (m_rumbleTimer <= 0.0f) {
                setRumble(0.0f, 0.0f);
            }
        }

        // 1. Приоритет — XInput (Xbox / 8BitDo / Steam Input / DS4Windows)
        if (m_fnGetState) {
            XInputStateRaw st{};
            int foundSlot = -1;
            if (m_activeSlot >= 0 && m_fnGetState(static_cast<DWORD>(m_activeSlot), &st) == ERROR_SUCCESS) {
                foundSlot = m_activeSlot;
            } else {
                for (int slot = 0; slot < 4; ++slot) {
                    if (m_fnGetState(static_cast<DWORD>(slot), &st) == ERROR_SUCCESS) {
                        foundSlot = slot;
                        break;
                    }
                }
            }

            if (foundSlot >= 0) {
                m_activeSlot = foundSlot;
                out.connected = true;
                out.isXInput  = true;

                const auto& gp = st.gamepad;
                applyRadialStick(gp.sThumbLX / 32767.0f, gp.sThumbLY / 32767.0f, 0.16f, out.moveX, out.moveY);
                applyRadialStick(gp.sThumbRX / 32767.0f, gp.sThumbRY / 32767.0f, 0.16f, out.lookX, out.lookY);
                out.lookX = applyLookCurve(out.lookX);
                out.lookY = applyLookCurve(out.lookY);

                float lt = (gp.bLeftTrigger  > 20) ? (gp.bLeftTrigger  - 20) / 235.0f : 0.0f;
                float rt = (gp.bRightTrigger > 20) ? (gp.bRightTrigger - 20) / 235.0f : 0.0f;
                const uint32_t btns = gp.wButtons;

                float vAxis = rt - lt;
                if (btns & 0x1000u) vAxis += 1.0f; // A
                if (btns & 0x2000u) vAxis -= 1.0f; // B
                out.vertical = std::clamp(vAxis, -1.0f, 1.0f);
                out.sprint   = ((btns & 0x0040u) != 0) || ((btns & 0x0200u) != 0); // L3 или RB

                const uint32_t pressed = (m_connected && m_lastWasXInput) ? (btns & ~m_prevButtons) : 0u;
                m_prevButtons   = btns;
                m_prevPovMask   = 0u;
                m_connected     = true;
                m_lastWasXInput = true;

                out.toggleBiome     = (pressed & 0x8000u) != 0; // Y
                out.togglePostFx    = (pressed & 0x4000u) != 0; // X
                out.summonWhale     = (pressed & 0x0100u) != 0; // LB
                out.toggleHud       = (pressed & 0x0020u) != 0; // Back / View
                out.regenWorld      = (pressed & 0x0010u) != 0; // Start / Menu
                out.toggleWireframe = (pressed & 0x0080u) != 0; // R3
                out.volumeUp        = (pressed & 0x0001u) != 0; // D-Pad Up
                out.volumeDown      = (pressed & 0x0002u) != 0; // D-Pad Down
                out.toggleShadows   = (pressed & 0x0004u) != 0; // D-Pad Left
                out.toggleMute      = (pressed & 0x0008u) != 0; // D-Pad Right
                return out;
            }
            m_activeSlot = -1;
        }

        // 2. Fallback — Windows Multimedia (DirectInput / USB DualSense без эмулятора)
        if (m_fnJoyGetPosEx) {
            JoyInfoExRaw ji{};
            int foundWinmmSlot = -1;

            auto tryReadWinmm = [&](int slot) -> bool {
                ji = {};
                ji.dwSize  = sizeof(JoyInfoExRaw);
                ji.dwFlags = 0x000000FFu; // JOY_RETURNALL
                return m_fnJoyGetPosEx(static_cast<UINT>(slot), &ji) == 0;
            };

            if (m_winmmSlot >= 0 && tryReadWinmm(m_winmmSlot)) {
                foundWinmmSlot = m_winmmSlot;
            } else {
                for (int slot = 0; slot < 4; ++slot) {
                    if (tryReadWinmm(slot)) {
                        foundWinmmSlot = slot;
                        break;
                    }
                }
            }

            if (foundWinmmSlot >= 0) {
                m_winmmSlot   = foundWinmmSlot;
                out.connected = true;
                out.isXInput  = false;

                float lx =  (static_cast<float>(ji.dwXpos) - 32767.5f) / 32767.5f;
                float ly = -(static_cast<float>(ji.dwYpos) - 32767.5f) / 32767.5f;
                float rx =  (static_cast<float>(ji.dwZpos) - 32767.5f) / 32767.5f;
                float ry = -(static_cast<float>(ji.dwRpos) - 32767.5f) / 32767.5f;
                applyRadialStick(lx, ly, 0.18f, out.moveX, out.moveY);
                applyRadialStick(rx, ry, 0.18f, out.lookX, out.lookY);
                out.lookX = applyLookCurve(out.lookX);
                out.lookY = applyLookCurve(out.lookY);

                const uint32_t btns = ji.dwButtons;
                float vAxis = 0.0f;
                if ((btns & (1u << 1)) || (btns & (1u << 7))) vAxis += 1.0f; // Cross / R2
                if ((btns & (1u << 2)) || (btns & (1u << 6))) vAxis -= 1.0f; // Circle / L2
                out.vertical = std::clamp(vAxis, -1.0f, 1.0f);
                out.sprint   = (btns & (1u << 5)) != 0 || (btns & (1u << 10)) != 0; // R1 / L3

                const uint8_t  povMask    = decodePovMask(ji.dwPOV);
                const uint32_t pressed    = (m_connected && !m_lastWasXInput) ? (btns & ~m_prevButtons) : 0u;
                const uint8_t  povPressed = (m_connected && !m_lastWasXInput)
                                               ? static_cast<uint8_t>(povMask & ~m_prevPovMask)
                                               : 0u;

                m_prevButtons   = btns;
                m_prevPovMask   = povMask;
                m_connected     = true;
                m_lastWasXInput = false;

                out.togglePostFx    = (pressed & (1u << 0))  != 0; // Square
                out.toggleBiome     = (pressed & (1u << 3))  != 0; // Triangle
                out.summonWhale     = (pressed & (1u << 4))  != 0; // L1
                out.toggleHud       = (pressed & (1u << 8))  != 0; // Share / Create
                out.regenWorld      = (pressed & (1u << 9))  != 0; // Options
                out.toggleWireframe = (pressed & (1u << 11)) != 0; // R3

                out.volumeUp        = (povPressed & 0x01u) != 0; // D-Pad Up
                out.toggleMute      = (povPressed & 0x02u) != 0; // D-Pad Right
                out.volumeDown      = (povPressed & 0x04u) != 0; // D-Pad Down
                out.toggleShadows   = (povPressed & 0x08u) != 0; // D-Pad Left
                return out;
            }
            m_winmmSlot = -1;
        }
#else
        (void)dt;
#endif
        m_connected     = false;
        m_lastWasXInput = false;
        m_prevButtons   = 0u;
        m_prevPovMask   = 0u;
        return out;
    }

    void triggerRumble(float lowMotor, float highMotor, float durationSec) {
        setRumble(lowMotor, highMotor);
        m_rumbleTimer = std::max(m_rumbleTimer, durationSec);
    }

    bool isConnected() const { return m_connected; }

private:
    static void applyRadialStick(float x, float y, float deadzone, float& outX, float& outY) {
        float mag = std::sqrt(x * x + y * y);
        if (mag <= deadzone) {
            outX = 0.0f;
            outY = 0.0f;
            return;
        }
        float norm = std::min(1.0f, (mag - deadzone) / (1.0f - deadzone));
        outX = (x / mag) * norm;
        outY = (y / mag) * norm;
    }

    // Нелинейная кривая правого стика: точное наведение у центра, быстрый разворот на краю
    static float applyLookCurve(float v) {
        const float av = std::abs(v);
        return (v >= 0.0f ? 1.0f : -1.0f) * (av * (0.45f + 0.55f * av));
    }

    // Преобразование угла POV (сотые доли градуса) в битовую маску D-Pad
    static uint8_t decodePovMask(uint32_t pov) {
        if (pov >= 36000u) return 0u; // JOY_POVCENTERED (0xFFFF)
        uint8_t mask = 0u;
        if (pov <= 4500u || pov >= 31500u) mask |= 0x01u; // Up
        if (pov >= 4500u  && pov <= 13500u) mask |= 0x02u; // Right
        if (pov >= 13500u && pov <= 22500u) mask |= 0x04u; // Down
        if (pov >= 22500u && pov <= 31500u) mask |= 0x08u; // Left
        return mask;
    }

    void setRumble(float lowMotor, float highMotor) {
#ifdef _WIN32
        if (!m_fnSetState || m_activeSlot < 0) return;
        XInputVibrationRaw vib{};
        vib.wLeftMotorSpeed  = static_cast<WORD>(std::clamp(lowMotor,  0.0f, 1.0f) * 65535.0f);
        vib.wRightMotorSpeed = static_cast<WORD>(std::clamp(highMotor, 0.0f, 1.0f) * 65535.0f);
        m_fnSetState(static_cast<DWORD>(m_activeSlot), &vib);
#else
        (void)lowMotor;
        (void)highMotor;
#endif
    }

#ifdef _WIN32
    struct XInputGamepadRaw {
        WORD  wButtons;
        BYTE  bLeftTrigger;
        BYTE  bRightTrigger;
        SHORT sThumbLX;
        SHORT sThumbLY;
        SHORT sThumbRX;
        SHORT sThumbRY;
    };
    struct XInputStateRaw {
        DWORD            dwPacketNumber;
        XInputGamepadRaw gamepad;
    };
    struct XInputVibrationRaw {
        WORD wLeftMotorSpeed;
        WORD wRightMotorSpeed;
    };
    struct JoyInfoExRaw {
        DWORD dwSize;
        DWORD dwFlags;
        DWORD dwXpos, dwYpos, dwZpos, dwRpos, dwUpos, dwVpos;
        DWORD dwButtons, dwButtonNumber, dwPOV, dwReserved1, dwReserved2;
    };

    using FnXInputGetState = DWORD(WINAPI*)(DWORD, XInputStateRaw*);
    using FnXInputSetState = DWORD(WINAPI*)(DWORD, XInputVibrationRaw*);
    using FnJoyGetPosEx    = UINT(WINAPI*)(UINT, JoyInfoExRaw*);

    HMODULE          m_xinputLib     = nullptr;
    HMODULE          m_winmmLib      = nullptr;
    FnXInputGetState m_fnGetState    = nullptr;
    FnXInputSetState m_fnSetState    = nullptr;
    FnJoyGetPosEx    m_fnJoyGetPosEx = nullptr;
#endif

    int      m_activeSlot    = -1;
    int      m_winmmSlot     = -1;
    bool     m_connected     = false;
    bool     m_lastWasXInput = false;
    uint32_t m_prevButtons   = 0u;
    uint8_t  m_prevPovMask   = 0u;
    float    m_rumbleTimer   = 0.0f;
};
