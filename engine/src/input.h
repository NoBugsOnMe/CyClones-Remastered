// Rebindable controls: every action can have up to two inputs, each a key, a mouse button or a
// turn of the mouse wheel. Saved in settings.ini as bind_<action>=<input>,<input>.
#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cctype>
#include <string>

enum class Action { Forward, Back, StrafeLeft, StrafeRight, Run, Jump, Fire, Use, BioKit, MechKit, NextWeapon, PrevWeapon, Map,
                    Inventory, Detonate, JumpJets, Count };
constexpr int kActionCount = (int)Action::Count;

// An input code: a keyboard scancode, a mouse button (kMouseBase + SDL button number) or the wheel.
constexpr int kMouseBase = 1000, kWheelUp = 1100, kWheelDown = 1101;

struct Bindings {
    std::array<std::array<int, 2>, kActionCount> in{};   // 0 = unbound

    static const char* label(Action a) {
        static const char* n[] = {"FORWARD", "BACK", "STRAFE LEFT", "STRAFE RIGHT", "RUN", "JUMP", "FIRE",
                                  "USE (DOORS, ITEMS)", "USE BIO-KIT", "USE MECH-KIT", "NEXT WEAPON", "PREVIOUS WEAPON", "AUTOMAP",
                                  "INVENTORY", "DETONATE DEMO PACKS", "JUMP JETS ON/OFF"};
        return n[(int)a];
    }
    static const char* key(Action a) {
        static const char* n[] = {"forward", "back", "strafe_left", "strafe_right", "run", "jump", "fire",
                                  "use", "bio_kit", "mech_kit", "next_weapon", "previous_weapon", "map", "inventory", "detonate",
                                  "jump_jets"};
        return n[(int)a];
    }
    // Holding these matters; the others act once per press.
    static bool held(Action a) { return a <= Action::Fire; }

    void defaults() {
        auto set = [&](Action a, int p, int s) { in[(int)a] = {p, s}; };
        set(Action::Forward, SDL_SCANCODE_W, SDL_SCANCODE_UP);
        set(Action::Back, SDL_SCANCODE_S, SDL_SCANCODE_DOWN);
        set(Action::StrafeLeft, SDL_SCANCODE_A, SDL_SCANCODE_LEFT);
        set(Action::StrafeRight, SDL_SCANCODE_D, SDL_SCANCODE_RIGHT);
        set(Action::Run, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_RSHIFT);
        set(Action::Jump, SDL_SCANCODE_SPACE, 0);
        set(Action::Fire, kMouseBase + SDL_BUTTON_LEFT, SDL_SCANCODE_LCTRL);
        set(Action::Use, kMouseBase + SDL_BUTTON_RIGHT, SDL_SCANCODE_RETURN);
        set(Action::BioKit, SDL_SCANCODE_Q, 0);
        set(Action::MechKit, SDL_SCANCODE_E, 0);
        set(Action::NextWeapon, kWheelDown, 0);
        set(Action::PrevWeapon, kWheelUp, 0);
        set(Action::Map, SDL_SCANCODE_TAB, 0);
        set(Action::Inventory, SDL_SCANCODE_I, 0);   // the game's own key
        set(Action::Detonate, SDL_SCANCODE_B, 0);
        set(Action::JumpJets, SDL_SCANCODE_J, 0);   // the game's own key (its data card: "J = toggle jump jet on/off")
    }

    static std::string upper(std::string s) {
        for (char& c : s) c = (char)std::toupper((unsigned char)c);
        return s;
    }
    static std::string inputName(int code) {
        if (code == kWheelUp) return "WHEEL UP";
        if (code == kWheelDown) return "WHEEL DOWN";
        if (code > kMouseBase && code < kMouseBase + 32) {
            int b = code - kMouseBase;
            return b == SDL_BUTTON_LEFT ? "MOUSE LEFT" : b == SDL_BUTTON_RIGHT ? "MOUSE RIGHT" : b == SDL_BUTTON_MIDDLE ? "MOUSE MIDDLE"
                                                                                                         : "MOUSE " + std::to_string(b);
        }
        if (code > 0 && code < SDL_SCANCODE_COUNT) {
            const char* n = SDL_GetScancodeName((SDL_Scancode)code);
            if (n && *n) return upper(n);
        }
        return "";
    }
    static int parseInput(std::string s) {
        while (!s.empty() && s.front() == ' ') s.erase(s.begin());
        while (!s.empty() && s.back() == ' ') s.pop_back();
        std::string u = upper(s);
        if (u.empty() || u == "NONE") return 0;
        if (u == "WHEEL UP") return kWheelUp;
        if (u == "WHEEL DOWN") return kWheelDown;
        if (u == "MOUSE LEFT") return kMouseBase + SDL_BUTTON_LEFT;
        if (u == "MOUSE RIGHT") return kMouseBase + SDL_BUTTON_RIGHT;
        if (u == "MOUSE MIDDLE") return kMouseBase + SDL_BUTTON_MIDDLE;
        if (u.rfind("MOUSE ", 0) == 0) {
            int b = std::atoi(u.c_str() + 6);
            return b > 0 && b < 32 ? kMouseBase + b : 0;
        }
        SDL_Scancode sc = SDL_GetScancodeFromName(s.c_str());
        return sc == SDL_SCANCODE_UNKNOWN ? 0 : (int)sc;
    }

    bool has(Action a, int code) const { return code && (in[(int)a][0] == code || in[(int)a][1] == code); }
    Action actionFor(int code) const {
        for (int a = 0; a < kActionCount; ++a)
            if (has((Action)a, code)) return (Action)a;
        return Action::Count;
    }
    // The new input becomes the first; the old first moves to second. An input does one thing.
    void bind(Action a, int code) {
        if (!code) return;
        for (auto& b : in)
            for (int& c : b)
                if (c == code) c = 0;
        auto& b = in[(int)a];
        if (!b[0]) b[0] = b[1], b[1] = 0;
        b[1] = b[0];
        b[0] = code;
    }
    void clear(Action a) { in[(int)a] = {0, 0}; }
    std::string describe(Action a) const {
        std::string s;
        for (int c : in[(int)a]) {
            std::string n = inputName(c);
            if (n.empty()) continue;
            if (!s.empty()) s += ", ";
            s += n;
        }
        return s.empty() ? "-" : s;
    }
    std::string save(Action a) const {
        std::string s;
        for (int c : in[(int)a]) {
            if (!c) continue;
            if (!s.empty()) s += ",";
            s += c >= kMouseBase ? inputName(c) : std::string(SDL_GetScancodeName((SDL_Scancode)c));
        }
        return s.empty() ? "none" : s;
    }
    void load(Action a, const std::string& value) {
        std::array<int, 2> b{0, 0};
        size_t p = 0;
        for (int k = 0; k < 2 && p <= value.size(); ++k) {
            size_t q = value.find(',', p);
            b[k] = parseInput(value.substr(p, q == std::string::npos ? std::string::npos : q - p));
            if (q == std::string::npos) break;
            p = q + 1;
        }
        in[(int)a] = b;
    }
};
