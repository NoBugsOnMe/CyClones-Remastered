// CyClones Remastered — Phase 1 launcher
// A native Windows x64 front-end that configures and starts the bundled
// DOSBox Staging engine with display, picture, input and audio settings
// tuned for modern monitors (high DPI, ultrawide, high refresh, VRR).
//
// Package layout (relative to this exe):
//   engine\dosbox.exe            DOSBox Staging (unmodified official build)
//   game\drive_c\CYCLONE\        installed game files from the user's own disc
//   game\CYCLONES.cue            generated at launch, points at the disc image
//   config\cyclones-base.conf    fixed engine settings for CyClones
//   config\session.conf          generated at launch from the launcher options
//   config\custom.conf           optional user overrides (loaded last)
//   config\launcher.ini          saved launcher options
//   config\last-run.log          engine log of the most recent session

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <map>
#include <cstdio>
#include <cwchar>
#include <algorithm>

// ---------------------------------------------------------------------------
// Constants

static const wchar_t* kAppTitle = L"CyClones Remastered";
static const unsigned long long kKnownImageSize = 503097504ULL; // 213,902 raw sectors

enum {
    IDC_MONITOR = 100, IDC_MODE, IDC_WINSIZE, IDC_FIT, IDC_SYNC, IDC_FITINFO,
    IDC_FILTER, IDC_GAMMA, IDC_GAMMA_VAL,
    IDC_CPU, IDC_MOUSE, IDC_MOUSE_VAL, IDC_CAPTURE, IDC_PAUSEBG,
    IDC_MUSIC, IDC_MUSIC_VAL, IDC_SFX, IDC_SFX_VAL, IDC_MUTEBG,
    IDC_DISC, IDC_BROWSE, IDC_STATUS,
    IDC_PLAY, IDC_QUIT, IDC_MANUAL, IDC_FOLDER, IDC_DEFAULTS,
};

static const UINT WM_APP_GAME_EXITED = WM_APP + 1;

// ---------------------------------------------------------------------------
// Settings

struct Settings {
    std::wstring monitor;   // GDI device name, e.g. \\.\DISPLAY1 (empty = primary)
    int mode = 0;           // 0 fullscreen, 1 windowed
    int winsize = 0;        // 0 auto, 1 small, 2 medium, 3 large
    int fit = 0;            // 0 4:3, 1 integer, 2 stretch
    int sync = 0;           // 0 auto/VRR, 1 vsync, 2 low latency
    int filter = 0;         // 0 sharp, 1 crt, 2 crt arcade, 3 bilinear, 4 nearest
    int gamma = 0;          // -30..30
    int cpu = 0;            // 0 fast, 1 pentium, 2 486, 3 max
    int mouse = 100;        // 25..300 %
    int capture = 1;
    int pausebg = 0;
    int music = 80;         // 0..100
    int sfx = 100;          // 0..100
    int mutebg = 0;
    std::wstring disc;      // path to .bin/.cue/.iso
};

struct MonitorInfo {
    std::wstring gdiName;
    std::wstring label;
    int w = 0, h = 0, hz = 0;
    bool primary = false;
};

struct Ctl {
    HWND hwnd;
    int x, y, w, h; // layout in 96-DPI units
};

// ---------------------------------------------------------------------------
// Globals

static HINSTANCE g_inst;
static HWND g_main;
static UINT g_dpi = 96;
static HFONT g_font, g_fontBold, g_fontTitle, g_fontSub;
static HBRUSH g_bodyBrush;
static std::vector<Ctl> g_ctls;
static std::map<int, HWND> g_byId;
static std::vector<MonitorInfo> g_monitors;
static Settings g_set;
static std::wstring g_root;
static bool g_statusError = false;
static HANDLE g_gameProcess = nullptr;

static const int kClientW = 720;
static const int kClientH = 560;
static const int kHeaderH = 76;

// ---------------------------------------------------------------------------
// Small helpers

static int S(int v) { return MulDiv(v, (int)g_dpi, 96); }

static std::wstring Join(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}

static bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static unsigned long long FileSize(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fad)) return 0;
    return ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
}

static std::wstring FullPath(const std::wstring& p) {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetFullPathNameW(p.c_str(), (DWORD)(sizeof(buf) / sizeof(buf[0])), buf, nullptr);
    if (n == 0 || n >= sizeof(buf) / sizeof(buf[0])) return p;
    return buf;
}

static std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

static std::wstring Ext(const std::wstring& p) {
    size_t dot = p.find_last_of(L'.');
    size_t slash = p.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return L"";
    return Lower(p.substr(dot));
}

static std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static bool WriteUtf8File(const std::wstring& path, const std::wstring& text) {
    std::string data = ToUtf8(text);
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(f, data.data(), (DWORD)data.size(), &written, nullptr);
    CloseHandle(f);
    return ok && written == data.size();
}

static std::wstring Fmt(const wchar_t* fmt, ...) {
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, sizeof(buf) / sizeof(buf[0]) - 1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) / sizeof(buf[0]) - 1] = 0;
    return buf;
}

static HWND Item(int id) { return g_byId[id]; }

// ---------------------------------------------------------------------------
// Paths

static std::wstring ExeDir() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    std::wstring p(buf, n);
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"." : p.substr(0, s);
}

static std::wstring ConfigDir() { return Join(g_root, L"config"); }
static std::wstring IniPath() { return Join(ConfigDir(), L"launcher.ini"); }
static std::wstring EnginePath() { return Join(Join(g_root, L"engine"), L"dosbox.exe"); }
static std::wstring GameDir() { return Join(Join(Join(g_root, L"game"), L"drive_c"), L"CYCLONE"); }

// ---------------------------------------------------------------------------
// Monitors — ordered the way SDL2 numbers displays on Windows
// (primary adapter first, then the other desktop-attached adapters).

static std::vector<MonitorInfo> EnumerateMonitors() {
    std::map<std::wstring, std::wstring> friendly;
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) == ERROR_SUCCESS && np > 0) {
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm > 0 ? nm : 1);
        if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) == ERROR_SUCCESS) {
            for (UINT32 i = 0; i < np; ++i) {
                DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {};
                src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                src.header.size = sizeof(src);
                src.header.adapterId = paths[i].sourceInfo.adapterId;
                src.header.id = paths[i].sourceInfo.id;
                DISPLAYCONFIG_TARGET_DEVICE_NAME tgt = {};
                tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
                tgt.header.size = sizeof(tgt);
                tgt.header.adapterId = paths[i].targetInfo.adapterId;
                tgt.header.id = paths[i].targetInfo.id;
                if (DisplayConfigGetDeviceInfo(&src.header) == ERROR_SUCCESS &&
                    DisplayConfigGetDeviceInfo(&tgt.header) == ERROR_SUCCESS &&
                    tgt.monitorFriendlyDeviceName[0]) {
                    friendly[src.viewGdiDeviceName] = tgt.monitorFriendlyDeviceName;
                }
            }
        }
    }

    std::vector<MonitorInfo> out;
    for (int pass = 0; pass < 2; ++pass) {
        for (DWORD i = 0;; ++i) {
            DISPLAY_DEVICEW dd = {};
            dd.cb = sizeof(dd);
            if (!EnumDisplayDevicesW(nullptr, i, &dd, 0)) break;
            if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
            bool prim = (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;
            if ((pass == 0) != prim) continue;
            MonitorInfo m;
            m.gdiName = dd.DeviceName;
            m.primary = prim;
            DEVMODEW dm = {};
            dm.dmSize = sizeof(dm);
            if (EnumDisplaySettingsW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm)) {
                m.w = (int)dm.dmPelsWidth;
                m.h = (int)dm.dmPelsHeight;
                m.hz = (int)dm.dmDisplayFrequency;
            }
            std::wstring name;
            auto it = friendly.find(m.gdiName);
            if (it != friendly.end()) name = it->second;
            if (name.empty()) {
                DISPLAY_DEVICEW md = {};
                md.cb = sizeof(md);
                if (EnumDisplayDevicesW(dd.DeviceName, 0, &md, 0)) name = md.DeviceString;
            }
            if (name.empty()) name = L"Display";
            std::wstring hz = m.hz > 1 ? Fmt(L" @ %d Hz", m.hz) : L"";
            m.label = Fmt(L"%d: %ls  —  %d × %d%ls%ls", (int)out.size() + 1, name.c_str(), m.w, m.h,
                          hz.c_str(), prim ? L"  (main)" : L"");
            out.push_back(m);
        }
    }
    if (out.empty()) {
        MonitorInfo m;
        m.primary = true;
        m.w = GetSystemMetrics(SM_CXSCREEN);
        m.h = GetSystemMetrics(SM_CYSCREEN);
        m.label = Fmt(L"1: Main display  —  %d × %d", m.w, m.h);
        out.push_back(m);
    }
    return out;
}

static int MonitorIndex(const std::wstring& gdi) {
    for (size_t i = 0; i < g_monitors.size(); ++i)
        if (!gdi.empty() && _wcsicmp(g_monitors[i].gdiName.c_str(), gdi.c_str()) == 0) return (int)i;
    return 0; // primary is always first
}

// ---------------------------------------------------------------------------
// Settings persistence

static int IniInt(const wchar_t* key, int def, int lo, int hi) {
    int v = (int)GetPrivateProfileIntW(L"launcher", key, def, IniPath().c_str());
    // GetPrivateProfileInt returns unsigned; negative values come back wrapped.
    wchar_t buf[32];
    GetPrivateProfileStringW(L"launcher", key, L"", buf, 32, IniPath().c_str());
    if (buf[0]) v = _wtoi(buf);
    return std::min(hi, std::max(lo, v));
}

static void LoadSettings() {
    Settings d;
    wchar_t buf[MAX_PATH * 4];
    GetPrivateProfileStringW(L"launcher", L"monitor", L"", buf, MAX_PATH, IniPath().c_str());
    g_set.monitor = buf;
    g_set.mode = IniInt(L"mode", d.mode, 0, 1);
    g_set.winsize = IniInt(L"window_size", d.winsize, 0, 3);
    g_set.fit = IniInt(L"fit", d.fit, 0, 2);
    g_set.sync = IniInt(L"sync", d.sync, 0, 2);
    g_set.filter = IniInt(L"filter", d.filter, 0, 4);
    g_set.gamma = IniInt(L"gamma", d.gamma, -30, 30);
    g_set.cpu = IniInt(L"cpu", d.cpu, 0, 3);
    g_set.mouse = IniInt(L"mouse", d.mouse, 25, 300);
    g_set.capture = IniInt(L"capture_mouse", d.capture, 0, 1);
    g_set.pausebg = IniInt(L"pause_in_background", d.pausebg, 0, 1);
    g_set.music = IniInt(L"music_volume", d.music, 0, 100);
    g_set.sfx = IniInt(L"effects_volume", d.sfx, 0, 100);
    g_set.mutebg = IniInt(L"mute_in_background", d.mutebg, 0, 1);
    GetPrivateProfileStringW(L"launcher", L"disc", L"", buf, (DWORD)(sizeof(buf) / sizeof(buf[0])), IniPath().c_str());
    g_set.disc = buf;
}

static void SaveSettings() {
    CreateDirectoryW(ConfigDir().c_str(), nullptr);
    auto W = [](const wchar_t* k, const std::wstring& v) {
        WritePrivateProfileStringW(L"launcher", k, v.c_str(), IniPath().c_str());
    };
    auto I = [&](const wchar_t* k, int v) { W(k, std::to_wstring(v)); };
    W(L"monitor", g_set.monitor);
    I(L"mode", g_set.mode);
    I(L"window_size", g_set.winsize);
    I(L"fit", g_set.fit);
    I(L"sync", g_set.sync);
    I(L"filter", g_set.filter);
    I(L"gamma", g_set.gamma);
    I(L"cpu", g_set.cpu);
    I(L"mouse", g_set.mouse);
    I(L"capture_mouse", g_set.capture);
    I(L"pause_in_background", g_set.pausebg);
    I(L"music_volume", g_set.music);
    I(L"effects_volume", g_set.sfx);
    I(L"mute_in_background", g_set.mutebg);
    W(L"disc", g_set.disc);
}

// ---------------------------------------------------------------------------
// Disc image discovery

static std::wstring FindDiscImage() {
    std::wstring parent = g_root;
    size_t s = parent.find_last_of(L"\\/");
    if (s != std::wstring::npos) parent = parent.substr(0, s);
    const std::wstring candidates[] = {
        Join(g_root, L"CYCLONES.cue"), Join(g_root, L"CYCLONES.bin"),
        Join(Join(g_root, L"disc"), L"CYCLONES.cue"), Join(Join(g_root, L"disc"), L"CYCLONES.bin"),
        Join(parent, L"CYCLONES.cue"), Join(parent, L"CYCLONES.bin"),
    };
    for (const auto& c : candidates)
        if (FileExists(c)) return FullPath(c);
    // Any .bin of the exact known size next to the package
    for (const auto& dir : {g_root, parent}) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(Join(dir, L"*.bin").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            unsigned long long sz = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            if (sz == kKnownImageSize) {
                FindClose(h);
                return FullPath(Join(dir, fd.cFileName));
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return L"";
}

// Track layout of the CyClones CD (1 data track + 29 Red Book audio tracks),
// detected from the raw 2352-byte-sector image.
static std::wstring MakeCue(const std::wstring& binPath) {
    static const int pregaps[] = {55953, 63371, 70707, 78327, 85363, 92698, 100290, 107889, 115480, 122560,
                                  129588, 136715, 143892, 151167, 158539, 165911, 173505, 181306, 182268,
                                  183293, 183895, 184479, 185384, 186751, 194155, 201491, 209128, 210849};
    auto msf = [](int l) { return Fmt(L"%02d:%02d:%02d", l / 4500, (l / 75) % 60, l % 75); };
    std::wstring c = L"FILE \"" + binPath + L"\" BINARY\r\n";
    c += L"  TRACK 01 MODE1/2352\r\n    INDEX 01 00:00:00\r\n";
    c += L"  TRACK 02 AUDIO\r\n    INDEX 00 " + msf(45142) + L"\r\n    INDEX 01 " + msf(45292) + L"\r\n";
    int t = 3;
    for (int p : pregaps) {
        c += Fmt(L"  TRACK %02d AUDIO\r\n", t++);
        c += L"    INDEX 00 " + msf(p) + L"\r\n    INDEX 01 " + msf(p + 150) + L"\r\n";
    }
    return c;
}

// ---------------------------------------------------------------------------
// Status line

static void SetStatus(const std::wstring& text, bool error) {
    g_statusError = error;
    SetWindowTextW(Item(IDC_STATUS), text.c_str());
    InvalidateRect(Item(IDC_STATUS), nullptr, TRUE);
}

static bool CheckReady(bool verbose) {
    if (!FileExists(EnginePath())) {
        SetStatus(L"Engine missing: engine\\dosbox.exe was not found next to the launcher.", true);
        return false;
    }
    if (!FileExists(Join(GameDir(), L"CYCLONE.EXE"))) {
        SetStatus(L"Game files missing: game\\drive_c\\CYCLONE\\CYCLONE.EXE was not found.", true);
        return false;
    }
    std::wstring d = g_set.disc;
    if (d.empty() || !FileExists(d)) {
        SetStatus(L"Choose your CyClones disc image (CYCLONES.bin or .cue) with Browse…", true);
        return false;
    }
    std::wstring e = Ext(d);
    if (e == L".bin") {
        unsigned long long sz = FileSize(d);
        if (sz != kKnownImageSize) {
            SetStatus(L"This .bin has a different layout from the known CyClones CD. Select its matching .cue file instead.", true);
            return false;
        }
        if (verbose) SetStatus(L"Ready  ·  disc image found (30 tracks, CD soundtrack included).", false);
    } else if (e == L".cue") {
        if (verbose) SetStatus(L"Ready  ·  using your .cue sheet.", false);
    } else if (e == L".iso") {
        if (verbose) SetStatus(L"Ready  ·  ISO image has no CD music tracks; the game will play without its soundtrack.", false);
    } else {
        SetStatus(L"Unsupported disc image type. Use CYCLONES.bin, a .cue sheet, or an .iso.", true);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Read UI -> settings, settings -> UI

static int ComboSel(int id) { return (int)SendMessageW(Item(id), CB_GETCURSEL, 0, 0); }
static int TrackPos(int id) { return (int)SendMessageW(Item(id), TBM_GETPOS, 0, 0); }
static bool Checked(int id) { return SendMessageW(Item(id), BM_GETCHECK, 0, 0) == BST_CHECKED; }

static std::wstring GetText(int id) {
    int n = GetWindowTextLengthW(Item(id));
    std::wstring s(n + 1, L'\0');
    GetWindowTextW(Item(id), &s[0], n + 1);
    s.resize(n);
    return s;
}

static void UpdateValueLabels() {
    SetWindowTextW(Item(IDC_GAMMA_VAL), (TrackPos(IDC_GAMMA) > 0 ? Fmt(L"+%d", TrackPos(IDC_GAMMA)) : std::to_wstring(TrackPos(IDC_GAMMA))).c_str());
    SetWindowTextW(Item(IDC_MOUSE_VAL), Fmt(L"%d%%", TrackPos(IDC_MOUSE)).c_str());
    SetWindowTextW(Item(IDC_MUSIC_VAL), Fmt(L"%d%%", TrackPos(IDC_MUSIC)).c_str());
    SetWindowTextW(Item(IDC_SFX_VAL), Fmt(L"%d%%", TrackPos(IDC_SFX)).c_str());
}

static void UpdateFitInfo() {
    int mi = ComboSel(IDC_MONITOR);
    if (mi < 0 || mi >= (int)g_monitors.size()) return;
    const MonitorInfo& m = g_monitors[mi];
    bool windowed = ComboSel(IDC_MODE) == 1;
    EnableWindow(Item(IDC_WINSIZE), windowed);
    if (windowed) {
        SetWindowTextW(Item(IDC_FITINFO), L"Windowed: the picture scales with the window; resize it freely.");
        return;
    }
    int W = m.w, H = m.h, fit = ComboSel(IDC_FIT);
    if (W <= 0 || H <= 0) { SetWindowTextW(Item(IDC_FITINFO), L""); return; }
    int iw, ih;
    std::wstring note;
    if (fit == 2) {
        iw = W; ih = H;
        note = L"stretched to fill";
    } else if (fit == 1) {
        int k = std::max(1, H / 200);
        ih = 200 * k; iw = ih * 4 / 3;
        while (iw > W && k > 1) { --k; ih = 200 * k; iw = ih * 4 / 3; }
        note = Fmt(L"exact %d\u00d7 scale", k);
    } else {
        ih = H; iw = H * 4 / 3;
        if (iw > W) { iw = W; ih = W * 3 / 4; }
        note = Fmt(L"%.1f\u00d7 scale", ih / 200.0);
    }
    std::wstring bars;
    if (iw < W && ih < H) bars = L"  \u00b7  framed";
    else if (iw < W) bars = L"  \u00b7  side bars";
    else if (ih < H) bars = L"  \u00b7  top/bottom bars";
    SetWindowTextW(Item(IDC_FITINFO), Fmt(L"\u2192 %d \u00d7 %d picture  \u00b7  %ls%ls", iw, ih, note.c_str(), bars.c_str()).c_str());
}

static void ReadUi() {
    int mi = ComboSel(IDC_MONITOR);
    if (mi >= 0 && mi < (int)g_monitors.size()) g_set.monitor = g_monitors[mi].gdiName;
    g_set.mode = std::max(0, ComboSel(IDC_MODE));
    g_set.winsize = std::max(0, ComboSel(IDC_WINSIZE));
    g_set.fit = std::max(0, ComboSel(IDC_FIT));
    g_set.sync = std::max(0, ComboSel(IDC_SYNC));
    g_set.filter = std::max(0, ComboSel(IDC_FILTER));
    g_set.gamma = TrackPos(IDC_GAMMA);
    g_set.cpu = std::max(0, ComboSel(IDC_CPU));
    g_set.mouse = TrackPos(IDC_MOUSE);
    g_set.capture = Checked(IDC_CAPTURE);
    g_set.pausebg = Checked(IDC_PAUSEBG);
    g_set.music = TrackPos(IDC_MUSIC);
    g_set.sfx = TrackPos(IDC_SFX);
    g_set.mutebg = Checked(IDC_MUTEBG);
    g_set.disc = GetText(IDC_DISC);
}

static void ApplyToUi() {
    SendMessageW(Item(IDC_MONITOR), CB_SETCURSEL, MonitorIndex(g_set.monitor), 0);
    SendMessageW(Item(IDC_MODE), CB_SETCURSEL, g_set.mode, 0);
    SendMessageW(Item(IDC_WINSIZE), CB_SETCURSEL, g_set.winsize, 0);
    SendMessageW(Item(IDC_FIT), CB_SETCURSEL, g_set.fit, 0);
    SendMessageW(Item(IDC_SYNC), CB_SETCURSEL, g_set.sync, 0);
    SendMessageW(Item(IDC_FILTER), CB_SETCURSEL, g_set.filter, 0);
    SendMessageW(Item(IDC_GAMMA), TBM_SETPOS, TRUE, g_set.gamma);
    SendMessageW(Item(IDC_CPU), CB_SETCURSEL, g_set.cpu, 0);
    SendMessageW(Item(IDC_MOUSE), TBM_SETPOS, TRUE, g_set.mouse);
    SendMessageW(Item(IDC_CAPTURE), BM_SETCHECK, g_set.capture ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(Item(IDC_PAUSEBG), BM_SETCHECK, g_set.pausebg ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(Item(IDC_MUSIC), TBM_SETPOS, TRUE, g_set.music);
    SendMessageW(Item(IDC_SFX), TBM_SETPOS, TRUE, g_set.sfx);
    SendMessageW(Item(IDC_MUTEBG), BM_SETCHECK, g_set.mutebg ? BST_CHECKED : BST_UNCHECKED, 0);
    SetWindowTextW(Item(IDC_DISC), g_set.disc.c_str());
    UpdateValueLabels();
    UpdateFitInfo();
}

// ---------------------------------------------------------------------------
// Session config + launch

static std::wstring BuildSessionConf(const std::wstring& cueRel) {
    static const wchar_t* shaders[] = {L"sharp", L"crt-auto", L"crt-auto-arcade", L"bilinear", L"nearest"};
    static const wchar_t* sizes[] = {L"default", L"small", L"medium", L"large"};
    static const wchar_t* cycles[] = {L"150000", L"60000", L"25000", L"max"};
    std::wstring c;
    c += L"# Generated by the CyClones Remastered launcher on every Play.\r\n";
    c += L"# Edits here are overwritten; put your own overrides in config\\custom.conf\r\n\r\n";
    c += L"[sdl]\r\n";
    c += Fmt(L"display = %d\r\n", MonitorIndex(g_set.monitor));
    c += Fmt(L"fullscreen = %ls\r\n", g_set.mode == 0 ? L"on" : L"off");
    c += Fmt(L"window_size = %ls\r\n", sizes[g_set.winsize]);
    c += Fmt(L"vsync = %ls\r\n", g_set.sync == 1 ? L"on" : L"off");
    c += Fmt(L"presentation_mode = %ls\r\n", g_set.sync == 2 ? L"host-rate" : L"auto");
    c += Fmt(L"pause_when_inactive = %ls\r\n", g_set.pausebg ? L"on" : L"off");
    c += Fmt(L"mute_when_inactive = %ls\r\n\r\n", g_set.mutebg ? L"on" : L"off");
    c += L"[render]\r\n";
    c += Fmt(L"shader = %ls\r\n", shaders[g_set.filter]);
    c += Fmt(L"aspect = %ls\r\n", g_set.fit == 2 ? L"stretch" : L"on");
    c += Fmt(L"integer_scaling = %ls\r\n", g_set.fit == 1 ? L"vertical" : (g_set.fit == 2 ? L"off" : L"auto"));
    c += Fmt(L"gamma = %d\r\n\r\n", g_set.gamma);
    c += L"[cpu]\r\n";
    c += Fmt(L"cpu_cycles_protected = %ls\r\n\r\n", cycles[g_set.cpu]);
    c += L"[mouse]\r\n";
    c += Fmt(L"mouse_capture = %ls\r\n", g_set.capture ? L"onstart" : L"onclick");
    c += Fmt(L"mouse_sensitivity = %d\r\n\r\n", g_set.mouse);
    c += L"[autoexec]\r\n";
    c += L"@echo off\r\n";
    c += L"mount c \"game\\drive_c\" > nul\r\n";
    c += L"mount d \"" + cueRel + L"\" -t cdrom > nul\r\n";
    c += Fmt(L"mixer cdaudio %d sb %d /noshow\r\n", g_set.music, g_set.sfx);
    c += L"c:\r\n";
    c += L"cd \\CYCLONE\r\n";
    c += L"cyclones\r\n";
    c += L"exit\r\n";
    return c;
}

static DWORD WINAPI WaitThread(LPVOID p) {
    HANDLE h = (HANDLE)p;
    WaitForSingleObject(h, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(h, &code);
    PostMessageW(g_main, WM_APP_GAME_EXITED, (WPARAM)code, 0);
    return 0;
}

static void Play() {
    ReadUi();
    if (!CheckReady(true)) return;
    SaveSettings();

    std::wstring disc = FullPath(g_set.disc);
    std::wstring e = Ext(disc);
    std::wstring mountTarget; // path given to DOSBox's mount, relative to the package root when possible
    if (e == L".bin") {
        std::wstring cue = Join(Join(g_root, L"game"), L"CYCLONES.cue");
        if (!WriteUtf8File(cue, MakeCue(disc))) {
            SetStatus(L"Could not write game\\CYCLONES.cue (is the folder read-only?).", true);
            return;
        }
        mountTarget = L"game\\CYCLONES.cue";
    } else {
        mountTarget = disc;
    }

    if (!WriteUtf8File(Join(ConfigDir(), L"session.conf"), BuildSessionConf(mountTarget))) {
        SetStatus(L"Could not write config\\session.conf (is the folder read-only?).", true);
        return;
    }

    std::wstring cmd = L"\"" + EnginePath() + L"\" --noprimaryconf --nolocalconf"
                       L" --conf \"config\\cyclones-base.conf\" --conf \"config\\session.conf\"";
    if (FileExists(Join(ConfigDir(), L"custom.conf"))) cmd += L" --conf \"config\\custom.conf\"";

    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE log = CreateFileW(Join(ConfigDir(), L"last-run.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    if (log != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = log;
        si.hStdError = log;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(0);
    BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, log != INVALID_HANDLE_VALUE,
                             CREATE_NO_WINDOW, nullptr, g_root.c_str(), &si, &pi);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (!ok) {
        SetStatus(Fmt(L"Could not start the engine (error %lu).", GetLastError()), true);
        return;
    }
    CloseHandle(pi.hThread);
    g_gameProcess = pi.hProcess;
    SetStatus(L"Game running…", false);
    ShowWindow(g_main, SW_HIDE);
    HANDLE t = CreateThread(nullptr, 0, WaitThread, pi.hProcess, 0, nullptr);
    if (t) CloseHandle(t);
}

// ---------------------------------------------------------------------------
// UI construction

static HWND Add(const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int w, int h,
                DWORD exStyle = 0) {
    HWND hw = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g_main,
                              (HMENU)(INT_PTR)id, g_inst, nullptr);
    g_ctls.push_back({hw, x, y, w, h});
    if (id > 0) g_byId[id] = hw;
    return hw;
}

static HWND AddCombo(int id, int x, int y, int w, std::initializer_list<const wchar_t*> items) {
    HWND c = Add(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, id, x, y, w, 300);
    for (auto s : items) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)s);
    return c;
}

static HWND AddTrack(int id, int valId, int x, int y, int w, int lo, int hi, int page) {
    HWND t = Add(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, id, x, y - 2, w - 52, 28);
    SendMessageW(t, TBM_SETRANGE, TRUE, MAKELPARAM(lo, hi));
    SendMessageW(t, TBM_SETPAGESIZE, 0, page);
    Add(WC_STATICW, L"", SS_RIGHT, valId, x + w - 48, y + 3, 48, 20);
    return t;
}

static void Label(const wchar_t* text, int x, int y, int w) { Add(WC_STATICW, text, 0, 0, x, y + 3, w, 20); }

static void BuildUi() {
    const int M = 16, gap = 12;
    const int colW = (kClientW - 2 * M - gap) / 2; // 338
    const int lx = M, rx = M + colW + gap;
    const int lab = 92, row = 32;
    int y0 = kHeaderH + 14;

    // ---- Display (left)
    int gh = 26 + row * 5 + 22;
    Add(WC_BUTTONW, L"Display", BS_GROUPBOX, 0, lx, y0, colW, gh);
    int cx = lx + 12 + lab, cw = colW - 24 - lab;
    int y = y0 + 24;
    Label(L"Monitor", lx + 12, y, lab);
    AddCombo(IDC_MONITOR, cx, y, cw, {});
    for (auto& m : g_monitors) SendMessageW(Item(IDC_MONITOR), CB_ADDSTRING, 0, (LPARAM)m.label.c_str());
    SendMessageW(Item(IDC_MONITOR), CB_SETDROPPEDWIDTH, S(420), 0);
    y += row;
    Label(L"Mode", lx + 12, y, lab);
    AddCombo(IDC_MODE, cx, y, cw, {L"Fullscreen (native resolution)", L"Windowed"});
    y += row;
    Label(L"Window size", lx + 12, y, lab);
    AddCombo(IDC_WINSIZE, cx, y, cw, {L"Automatic", L"Small", L"Medium", L"Large"});
    y += row;
    Label(L"Screen fit", lx + 12, y, lab);
    AddCombo(IDC_FIT, cx, y, cw, {L"Original 4:3 shape (recommended)", L"Pixel-perfect integer scaling", L"Stretch to fill the screen"});
    SendMessageW(Item(IDC_FIT), CB_SETDROPPEDWIDTH, S(300), 0);
    y += row;
    Label(L"Sync", lx + 12, y, lab);
    AddCombo(IDC_SYNC, cx, y, cw, {L"Auto  ·  best for G-Sync / FreeSync", L"VSync  ·  no tearing on fixed-rate", L"Low latency  ·  no sync"});
    SendMessageW(Item(IDC_SYNC), CB_SETDROPPEDWIDTH, S(300), 0);
    y += row;
    Add(WC_STATICW, L"", SS_LEFT | SS_ENDELLIPSIS, IDC_FITINFO, lx + 12, y + 1, colW - 24, 18);

    // ---- Picture (left)
    int y1 = y0 + gh + gap;
    int gh2 = 26 + row * 2 + 4;
    Add(WC_BUTTONW, L"Picture", BS_GROUPBOX, 0, lx, y1, colW, gh2);
    y = y1 + 24;
    Label(L"Filter", lx + 12, y, lab);
    AddCombo(IDC_FILTER, cx, y, cw, {L"Sharp pixels (recommended)", L"CRT monitor  ·  authentic 1994", L"CRT arcade  ·  thick scanlines", L"Smooth  ·  bilinear", L"Raw  ·  nearest neighbour"});
    SendMessageW(Item(IDC_FILTER), CB_SETDROPPEDWIDTH, S(260), 0);
    y += row;
    Label(L"Brightness", lx + 12, y, lab);
    AddTrack(IDC_GAMMA, IDC_GAMMA_VAL, cx, y, cw, -30, 30, 5);

    // ---- Gameplay (right)
    int gh3 = 26 + row * 4 + 4;
    Add(WC_BUTTONW, L"Gameplay", BS_GROUPBOX, 0, rx, y0, colW, gh3);
    int rcx = rx + 12 + lab, rcw = colW - 24 - lab;
    y = y0 + 24;
    Label(L"CPU speed", rx + 12, y, lab);
    AddCombo(IDC_CPU, rcx, y, rcw, {L"Fast  ·  smoothest (recommended)", L"Pentium  ·  late-90s PC", L"486 DX2/66  ·  original target", L"Maximum  ·  all your CPU can do"});
    SendMessageW(Item(IDC_CPU), CB_SETDROPPEDWIDTH, S(270), 0);
    y += row;
    Label(L"Mouse speed", rx + 12, y, lab);
    AddTrack(IDC_MOUSE, IDC_MOUSE_VAL, rcx, y, rcw, 25, 300, 25);
    y += row;
    Add(WC_BUTTONW, L"Capture the mouse as soon as the game starts", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_CAPTURE, rx + 12, y + 2, colW - 24, 22);
    y += row;
    Add(WC_BUTTONW, L"Pause when the game window is in the background", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_PAUSEBG, rx + 12, y + 2, colW - 24, 22);

    // ---- Audio (right)
    int y3 = y0 + gh3 + gap;
    int gh4 = (y1 + gh2) - y3;
    Add(WC_BUTTONW, L"Audio", BS_GROUPBOX, 0, rx, y3, colW, gh4);
    y = y3 + 24;
    Label(L"CD music", rx + 12, y, lab);
    AddTrack(IDC_MUSIC, IDC_MUSIC_VAL, rcx, y, rcw, 0, 100, 10);
    y += row;
    Label(L"Effects / voice", rx + 12, y, lab);
    AddTrack(IDC_SFX, IDC_SFX_VAL, rcx, y, rcw, 0, 100, 10);
    y += row;
    Add(WC_BUTTONW, L"Mute when the game window is in the background", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_MUTEBG, rx + 12, y + 2, colW - 24, 22);

    // ---- Disc (full width)
    int y5 = y1 + gh2 + gap;
    int fullW = kClientW - 2 * M;
    Add(WC_BUTTONW, L"Your CyClones CD image", BS_GROUPBOX, 0, M, y5, fullW, 82);
    y = y5 + 24;
    Add(WC_EDITW, L"", ES_AUTOHSCROLL | WS_TABSTOP | WS_BORDER, IDC_DISC, M + 12, y, fullW - 24 - 104, 24);
    Add(WC_BUTTONW, L"Browse…", BS_PUSHBUTTON | WS_TABSTOP, IDC_BROWSE, M + fullW - 12 - 96, y - 1, 96, 26);
    y += 30;
    Add(WC_STATICW, L"", SS_LEFT | SS_ENDELLIPSIS, IDC_STATUS, M + 12, y, fullW - 24, 20);

    // ---- Buttons
    int by = kClientH - M - 32;
    Add(WC_BUTTONW, L"Manual (PDF)", BS_PUSHBUTTON | WS_TABSTOP, IDC_MANUAL, M, by, 112, 32);
    Add(WC_BUTTONW, L"Saves folder", BS_PUSHBUTTON | WS_TABSTOP, IDC_FOLDER, M + 120, by, 104, 32);
    Add(WC_BUTTONW, L"Defaults", BS_PUSHBUTTON | WS_TABSTOP, IDC_DEFAULTS, M + 232, by, 84, 32);
    Add(WC_BUTTONW, L"Quit", BS_PUSHBUTTON | WS_TABSTOP, IDC_QUIT, kClientW - M - 96 - 8 - 140, by, 96, 32);
    Add(WC_BUTTONW, L"Play", BS_DEFPUSHBUTTON | WS_TABSTOP, IDC_PLAY, kClientW - M - 140, by, 140, 32);
}

static void MakeFonts() {
    if (g_font) DeleteObject(g_font);
    if (g_fontBold) DeleteObject(g_fontBold);
    if (g_fontTitle) DeleteObject(g_fontTitle);
    if (g_fontSub) DeleteObject(g_fontSub);
    auto mk = [](int pt, int weight, const wchar_t* face) {
        return CreateFontW(-MulDiv(pt, (int)g_dpi, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, face);
    };
    g_font = mk(9, FW_NORMAL, L"Segoe UI");
    g_fontBold = mk(10, FW_SEMIBOLD, L"Segoe UI");
    g_fontTitle = mk(22, FW_BOLD, L"Segoe UI");
    g_fontSub = mk(9, FW_NORMAL, L"Segoe UI");
}

static void Relayout() {
    for (auto& c : g_ctls) {
        SetWindowPos(c.hwnd, nullptr, S(c.x), S(c.y), S(c.w), S(c.h), SWP_NOZORDER | SWP_NOACTIVATE);
        SendMessageW(c.hwnd, WM_SETFONT, (WPARAM)(c.hwnd == Item(IDC_PLAY) ? g_fontBold : g_font), TRUE);
    }
    SendMessageW(Item(IDC_MONITOR), CB_SETDROPPEDWIDTH, S(420), 0);
    InvalidateRect(g_main, nullptr, TRUE);
}

static void PaintHeader(HDC dc) {
    RECT rc = {0, 0, S(kClientW), S(kHeaderH)};
    HBRUSH bg = CreateSolidBrush(RGB(12, 20, 34));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    RECT line = {0, S(kHeaderH) - S(3), S(kClientW), S(kHeaderH)};
    HBRUSH acc = CreateSolidBrush(RGB(64, 196, 255));
    FillRect(dc, &line, acc);
    DeleteObject(acc);

    // Reticle mark
    int cx = S(44), cy = S(kHeaderH / 2 - 1), r = S(17);
    HPEN pen = CreatePen(PS_SOLID, std::max(2, S(3)), RGB(64, 196, 255));
    HGDIOBJ op = SelectObject(dc, pen);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, cx - r, cy - r, cx + r, cy + r);
    MoveToEx(dc, cx, cy - r - S(6), nullptr); LineTo(dc, cx, cy - S(7));
    MoveToEx(dc, cx, cy + S(7), nullptr); LineTo(dc, cx, cy + r + S(6));
    MoveToEx(dc, cx - r - S(6), cy, nullptr); LineTo(dc, cx - S(7), cy);
    MoveToEx(dc, cx + S(7), cy, nullptr); LineTo(dc, cx + r + S(6), cy);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(pen);
    HBRUSH dot = CreateSolidBrush(RGB(255, 92, 72));
    RECT d = {cx - S(3), cy - S(3), cx + S(3), cy + S(3)};
    FillRect(dc, &d, dot);
    DeleteObject(dot);

    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ of = SelectObject(dc, g_fontTitle);
    SetTextColor(dc, RGB(236, 242, 248));
    RECT t = {S(84), S(10), S(kClientW - 16), S(48)};
    DrawTextW(dc, L"CyClones", -1, &t, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    SIZE sz;
    GetTextExtentPoint32W(dc, L"CyClones", 8, &sz);
    SelectObject(dc, g_fontBold);
    SetTextColor(dc, RGB(64, 196, 255));
    RECT t2 = {S(84) + sz.cx + S(10), S(10), S(kClientW - 16), S(50)};
    DrawTextW(dc, L"REMASTERED", -1, &t2, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    SelectObject(dc, g_fontSub);
    SetTextColor(dc, RGB(150, 170, 192));
    RECT t3 = {S(85), S(46), S(kClientW - 16), S(68)};
    DrawTextW(dc, L"Raven Software · SSI, 1994   ·   Windows 10/11 x64 edition  ·  Phase 1", -1, &t3, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    SelectObject(dc, of);
}

static void ResetDefaults() {
    std::wstring disc = GetText(IDC_DISC);
    g_set = Settings();
    g_set.disc = disc;
    ApplyToUi();
    CheckReady(true);
}

static void BrowseDisc() {
    wchar_t file[MAX_PATH * 4] = L"";
    std::wstring cur = GetText(IDC_DISC);
    if (!cur.empty() && cur.size() < MAX_PATH * 4) wcscpy(file, cur.c_str());
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = L"CD images (*.bin;*.cue;*.iso)\0*.bin;*.cue;*.iso\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH * 4;
    ofn.lpstrTitle = L"Select your CyClones disc image";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn)) {
        SetWindowTextW(Item(IDC_DISC), file);
        ReadUi();
        CheckReady(true);
    }
}

// ---------------------------------------------------------------------------
// Window procedure

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        PaintHeader(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetBkColor(dc, GetSysColor(COLOR_WINDOW));
        if ((HWND)lp == Item(IDC_STATUS)) SetTextColor(dc, g_statusError ? RGB(196, 43, 28) : RGB(16, 124, 64));
        else if ((HWND)lp == Item(IDC_FITINFO)) SetTextColor(dc, RGB(96, 104, 116));
        else SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        return (LRESULT)g_bodyBrush;
    }
    case WM_HSCROLL:
        UpdateValueLabels();
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (code == CBN_SELCHANGE && (id == IDC_MONITOR || id == IDC_MODE || id == IDC_FIT)) UpdateFitInfo();
        if (id == IDC_DISC && code == EN_KILLFOCUS) { ReadUi(); CheckReady(true); }
        switch (id) {
        case IDOK:
        case IDC_PLAY: Play(); break;
        case IDC_QUIT: ReadUi(); SaveSettings(); DestroyWindow(hwnd); break;
        case IDC_BROWSE: BrowseDisc(); break;
        case IDC_DEFAULTS: ResetDefaults(); break;
        case IDC_MANUAL: {
            std::wstring pdf = Join(GameDir(), L"CYCLONES.PDF");
            if (FileExists(pdf)) ShellExecuteW(hwnd, L"open", pdf.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            else SetStatus(L"The manual (CYCLONES.PDF) was not found in the game folder.", true);
            break;
        }
        case IDC_FOLDER:
            ShellExecuteW(hwnd, L"explore", GameDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        }
        return 0;
    }
    case WM_APP_GAME_EXITED: {
        if (g_gameProcess) { CloseHandle(g_gameProcess); g_gameProcess = nullptr; }
        ShowWindow(hwnd, SW_SHOW);
        SetForegroundWindow(hwnd);
        DWORD code = (DWORD)wp;
        if (code == 0) SetStatus(L"Welcome back. Settings are saved for next time.", false);
        else SetStatus(Fmt(L"The engine stopped with code %lu. Details: config\\last-run.log", code), true);
        return 0;
    }
    case WM_DPICHANGED: {
        g_dpi = HIWORD(wp);
        RECT* r = (RECT*)lp;
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        MakeFonts();
        Relayout();
        return 0;
    }
    case WM_CLOSE:
        ReadUi();
        SaveSettings();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    g_inst = inst;
    g_root = ExeDir();
    // Single instance per package folder
    std::wstring mutexName = L"CyClonesRemasteredLauncher";
    HANDLE mtx = CreateMutexW(nullptr, TRUE, mutexName.c_str());
    if (mtx && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(L"CyClonesRemasteredLauncher", nullptr);
        if (other) { ShowWindow(other, SW_RESTORE); SetForegroundWindow(other); }
        return 0;
    }

    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);
    g_bodyBrush = GetSysColorBrush(COLOR_WINDOW);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = g_bodyBrush;
    wc.lpszClassName = L"CyClonesRemasteredLauncher";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    RegisterClassExW(&wc);

    // Size the window for the DPI of the monitor it will open on.
    POINT pt = {0, 0};
    HMONITOR hm = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    UINT dx = 96, dy = 96;
    typedef HRESULT(WINAPI * GetDpiForMonitorFn)(HMONITOR, int, UINT*, UINT*);
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) {
        auto fn = (GetDpiForMonitorFn)(void*)GetProcAddress(shcore, "GetDpiForMonitor");
        if (fn && SUCCEEDED(fn(hm, 0, &dx, &dy))) g_dpi = dx;
    }
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r = {0, 0, S(kClientW), S(kClientH)};
    AdjustWindowRectEx(&r, style, FALSE, 0);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(hm, &mi);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - ww) / 2;
    int y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - wh) / 2;
    g_main = CreateWindowExW(0, wc.lpszClassName, kAppTitle, style, x, y, ww, wh, nullptr, nullptr, inst, nullptr);
    if (!g_main) return 1;
    UINT realDpi = GetDpiForWindow(g_main);
    if (realDpi && realDpi != g_dpi) {
        g_dpi = realDpi;
        RECT r2 = {0, 0, S(kClientW), S(kClientH)};
        AdjustWindowRectExForDpi(&r2, style, FALSE, 0, g_dpi);
        SetWindowPos(g_main, nullptr, 0, 0, r2.right - r2.left, r2.bottom - r2.top, SWP_NOMOVE | SWP_NOZORDER);
    }

    MakeFonts();
    g_monitors = EnumerateMonitors();
    BuildUi();
    Relayout();

    CreateDirectoryW(ConfigDir().c_str(), nullptr);
    LoadSettings();
    if (g_set.disc.empty() || !FileExists(g_set.disc)) {
        std::wstring found = FindDiscImage();
        if (!found.empty()) g_set.disc = found;
    }
    ApplyToUi();
    CheckReady(true);

    ShowWindow(g_main, show);
    UpdateWindow(g_main);
    SetFocus(Item(IDC_PLAY));

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        if (!IsDialogMessageW(g_main, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    if (mtx) CloseHandle(mtx);
    return 0;
}
