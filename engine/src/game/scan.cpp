// SDILAB's security scan rooms: an addition, not in the game.
//
// Two rooms in SDILAB, (3, 25) and (3, 37), each run a script when the player steps on the cell
// in the middle: "...DANGER... Remain Motionless", half a second, "Conducting Security Scan", a
// second, "Scan Complete", a second, "APPROVED" and the way on opens (scripts 36 and 37; the waits
// are 400, 900 and 900 eighths of a tick, as in the game). In the game nothing else happens: the
// monitor on the room's wall (a block showing rows 25-60 of wall287) has an empty screen, drawn
// black. Here the screen switches on and stands by, scans (Renderer::scanFrame), then holds SCAN
// COMPLETE and APPROVED; while it scans, a band of green light sweeps the player from head to foot
// and back, in step with the line on the screen, and the cell's edges glow.
#include "world_internal.h"

#include <cctype>
#include <cstring>
#include <map>

namespace {

std::string actText(const ScriptAction& a) {
    std::string s;
    for (int i = 0; i < 20 && a.arg[i]; ++i) s += (char)std::toupper(a.arg[i]);
    return s;
}

}  // namespace

void World::initScans() {
    scans_.clear();
    if (!level_ || !renderer_) return;
    const Level& L = *level_;
    // The monitors: block sides showing wall287, counted as the renderer counts them.
    const int wall = renderer_->scanMonitorWall();
    struct Side { float x, z; };
    std::vector<Side> monitors;
    if (wall > 0)
        for (const auto& c : L.cubes) {
            auto u16 = [&](int k) { return (int)(c[k * 2] | c[k * 2 + 1] << 8); };
            float x0 = (float)u16(1), z0 = (float)u16(2), x1 = x0 + c[8], z1 = z0 + c[10];
            const Side mid[4] = {{(x0 + x1) / 2, z0}, {x1, (z0 + z1) / 2}, {(x0 + x1) / 2, z1}, {x0, (z0 + z1) / 2}};
            for (int k = 8; k <= 11; ++k)
                if (u16(k) == wall) monitors.push_back(mid[k - 8]);
        }
    for (size_t si = 0; si < scripts_.size(); ++si) {
        const Script& sc = scripts_[si];
        ScanRoom r;
        r.script = (int)si;
        int wait = 0, waits[3] = {0, 0, 0};
        for (size_t k = 0; k < sc.acts.size(); ++k) {
            const ScriptAction& a = sc.acts[k];
            std::string t = a.op == 32 || a.op == 48 ? actText(a) : "";
            if (a.op == 1) wait += a.arg[0] | a.arg[1] << 8;
            if (a.op == 32 && t.rfind("CONDUCTING", 0) == 0 && r.iScan < 0) { r.iScan = (int)k; waits[0] = wait; }
            else if (a.op == 32 && t.rfind("SCAN COM", 0) == 0 && r.iScan >= 0 && r.iDone < 0) { r.iDone = (int)k; waits[1] = wait; }
            else if (a.op == 48 && t.rfind("APPROVED", 0) == 0 && r.iDone >= 0 && r.iOk < 0) { r.iOk = (int)k; waits[2] = wait; }
        }
        if (r.iScan < 0 || r.iDone < 0 || r.iOk < 0) continue;
        int cellX = -1, cellZ = -1;
        for (const auto& c : sc.conds)
            if (c.op == 1) { cellX = c.arg[0]; cellZ = c.arg[1]; }
        if (cellX < 0) continue;
        // The waits come off at 800 a second (64 every 8 ticks).
        r.durStand = std::max(0.05f, waits[0] / 800.0f);
        r.durScan = std::max(0.05f, (waits[1] - waits[0]) / 800.0f);
        r.durDone = std::max(0.05f, (waits[2] - waits[1]) / 800.0f);
        r.cx = (cellX + 0.5f) * kCellSize;
        r.cz = (cellZ + 0.5f) * kCellSize;
        r.floorY = surfaceY(L.floor, 0, r.cx, r.cz);
        // Its monitor: the nearest within three cells.
        float best = 3.0f * kCellSize;
        for (size_t m = 0; m < monitors.size(); ++m) {
            float d = std::hypot(monitors[m].x - r.cx, monitors[m].z - r.cz);
            if (d < best) { best = d; r.monitor = (int)m; }
        }
        scans_.push_back(r);
    }
    scanTick();
    for (auto& r : scans_) r.since = time_ - 10;   // a scan already done (a loaded game) shows its end

    // The missile consoles: scripts run from a cell's north or west wall (conditions 4, 5) that
    // print "Missile ...", where that wall shows wall360.
    consoles_.clear();
    std::map<int, int> walls;
    const int cw = renderer_->consoleWall();
    if (cw > 0)
        for (size_t c = 0; c < cellScripts_.size(); ++c)
            for (int si : cellScripts_[c]) {
                if (si < 0 || si >= (int)scripts_.size()) continue;
                const Script& sc = scripts_[si];
                int side = -1;
                for (const auto& k : sc.conds)
                    if (k.op == 4 || k.op == 5) side = k.op == 4 ? 1 : 0;
                bool missile = false;
                for (const auto& a : sc.acts)
                    if (a.op == 32 && actText(a).rfind("MISSILE", 0) == 0) missile = true;
                if (side < 0 || !missile) continue;
                const EdgeLayer& e = side ? L.north : L.west;
                if (e.texFront[c] != cw && e.texBack[c] != cw) continue;
                if (walls.count((int)c * 2 + side) || (int)consoles_.size() >= Renderer::kConsoleScreens) continue;
                MissileConsole m;
                m.script = si;
                m.index = (int)consoles_.size();
                walls[(int)c * 2 + side] = m.index;
                consoles_.push_back(m);
            }
    renderer_->setScreenWalls(walls);

    // SDI2's launch computer: the walls showing its halves, if a script prints "Missile Launch".
    launch_ = MissileConsole();
    for (size_t si = 0; si < scripts_.size() && launch_.script < 0; ++si)
        for (const auto& a : scripts_[si].acts)
            if (a.op == 32 && actText(a).rfind("MISSILE LAUNCH", 0) == 0) { launch_.script = (int)si; break; }
    std::set<int> launchEdges;
    if (launch_.script >= 0)
        for (int c = 0; c < kGrid * kGrid; ++c)
            for (int side = 0; side < 2; ++side) {
                const EdgeLayer& e = side ? L.north : L.west;
                for (int h = 0; h < 2; ++h) {
                    int w = renderer_->launchWall(h);
                    if (w > 0 && (e.texFront[c] == w || e.texBack[c] == w)) launchEdges.insert(c * 2 + side);
                }
            }
    if (launchEdges.empty()) launch_.script = -1;
    renderer_->setLaunchWalls(launchEdges);

    // The computers' screens run (an addition, Renderer::kIdleScreen), but for the walls a readied
    // item is used on (conditions 11 and 12: a wall in cell a0, a1), such as BIOLAB's data
    // transmitters, which stay as they are until a data stack goes in.
    // The aliens' pod computers (wall93) there are another matter: they wait for the pod, take it,
    // and show it is in (Renderer::kPodScreen).
    std::set<int> still;
    std::map<int, int> podWalls;
    pods_.clear();
    const int pw = renderer_->podWall();
    for (size_t si = 0; si < scripts_.size(); ++si)
        for (const auto& k : scripts_[si].conds)
            if ((k.op == 11 || k.op == 12) && k.arg[0] < kGrid && k.arg[1] < kGrid) {
                int c = k.arg[1] * kGrid + k.arg[0];
                bool pod = false;
                for (int side = 0; side < 2; ++side) {
                    const EdgeLayer& e = side ? L.north : L.west;
                    if (pw > 0 && (e.texFront[c] == pw || e.texBack[c] == pw) && pods_.size() < Renderer::kPodScreens &&
                        !podWalls.count(c * 2 + side)) {
                        MissileConsole m;
                        m.script = (int)si;
                        m.index = (int)pods_.size();
                        podWalls[c * 2 + side] = m.index;
                        pods_.push_back(m);
                        pod = true;
                    }
                }
                if (!pod) {
                    still.insert(c * 2);
                    still.insert(c * 2 + 1);
                }
                if (SDL_getenv("CYCLONES_LIST"))
                    SDL_Log("dbg %s at %d,%d (an item is used there)", pod ? "pod computer" : "still screens", k.arg[0], k.arg[1]);
            }
    renderer_->setStillWalls(still);
    renderer_->setPodWalls(podWalls);
}

// The phase each room is in, from where its script has got to; and each console's, from when its
// script ran (it is over in well under a second; the arming shows for kArming).
void World::scanTick() {
    for (auto& m : consoles_) {
        if (m.script < 0 || m.script >= (int)runs_.size()) continue;
        const ScriptRun& run = runs_[m.script];
        bool done = (run.flags & 0x04) != 0, running = run.pc >= 0;
        if ((done || running) && !m.seen) {
            m.seen = true;
            // seen running: just used; seen only done (a saved game): armed long ago
            m.since = running ? time_ : time_ - 100;
        }
        if (!done && !running) m.seen = false;
        m.phase = !m.seen ? 0 : time_ - m.since < kArming ? 1 : 2;
    }
    for (auto& m : pods_) {
        if (m.script < 0 || m.script >= (int)runs_.size()) continue;
        const ScriptRun& run = runs_[m.script];
        bool done = (run.flags & 0x04) != 0, running = run.pc >= 0;
        if ((done || running) && !m.seen) {
            // Its script is over in a few ticks, so it is seen done more often than running: done
            // when first looked at is a saved game's, long since; afterwards, it has just run.
            m.seen = true;
            m.since = !m.checked && !running ? time_ - 100 : time_;
        }
        m.checked = true;
        if (!done && !running) m.seen = false;
        m.phase = !m.seen ? 0 : time_ - m.since < kPodTake ? 1 : 2;
    }
    if (launch_.script >= 0 && launch_.script < (int)runs_.size()) {
        const ScriptRun& run = runs_[launch_.script];
        bool done = (run.flags & 0x04) != 0, running = run.pc >= 0;
        if ((done || running) && !launch_.seen) {
            launch_.seen = true;
            launch_.since = running ? time_ : time_ - 100;   // done already: a saved game, long since
        }
        launch_.phase = !launch_.seen ? 0 : time_ - launch_.since < Renderer::kLaunchShow ? 1 : 2;
    }
    for (auto& r : scans_) {
        if (r.script < 0 || r.script >= (int)runs_.size()) continue;
        const ScriptRun& run = runs_[r.script];
        int pc = run.pc, phase;
        if (run.flags & 0x04) phase = 4;
        else if (pc < 0) phase = 0;
        else if (pc > r.iOk) phase = 4;
        else if (pc > r.iDone) phase = 3;
        else if (pc > r.iScan) phase = 2;
        else phase = 1;
        if (phase != r.phase) {
            r.phase = phase;
            r.since = time_;
        }
    }
}

void World::addScanDraw(Scene& sc, std::vector<LightDraw>& lights) const {
    if (launch_.script >= 0) {
        ScanScreen s;
        s.kind = 2;
        s.phase = launch_.phase;
        s.t = launch_.phase == 2 ? time_ - launch_.since - Renderer::kLaunchShow : time_ - launch_.since;
        sc.scanScreens.push_back(s);
    }
    for (const auto& m : pods_) {
        ScanScreen s;
        s.kind = 3;
        s.monitor = m.index;
        s.phase = m.phase;
        s.t = time_ - m.since;
        s.clock = time_;
        sc.scanScreens.push_back(s);
    }
    for (const auto& m : consoles_) {
        ScanScreen s;
        s.kind = 1;
        s.monitor = m.index;
        s.phase = m.phase;
        s.t = time_ - m.since;
        s.dur = kArming;
        sc.scanScreens.push_back(s);
    }
    for (const auto& r : scans_) {
        float t = time_ - r.since;
        if (r.monitor >= 0) {
            ScanScreen s;
            s.monitor = r.monitor;
            s.phase = r.phase;
            s.t = t;
            s.dur = r.phase == 1 ? r.durStand : r.phase == 2 ? r.durScan : r.phase == 3 ? r.durDone : 1;
            sc.scanScreens.push_back(s);
        }
        if (r.phase < 1 || r.phase > 3) continue;
        // The cell's edges glow from the moment the scan begins until it is complete.
        float edge = r.phase == 1 ? std::min(1.0f, t / 0.25f) : r.phase == 2 ? 1.0f : std::max(0.0f, 1.0f - t / 0.6f);
        edge *= 0.55f + 0.25f * std::sin(time_ * 14.0f);
        if (edge > 0.01f) {
            const float h = kCellSize * 0.5f - 2, y = r.floorY + 2.0f;
            for (int side = 0; side < 4; ++side)
                for (int k = 0; k < 16; ++k) {
                    float u = -h + (k + 0.5f) * (2 * h / 16);
                    float x = side < 2 ? r.cx + u : r.cx + (side == 2 ? -h : h);
                    float z = side < 2 ? r.cz + (side == 0 ? -h : h) : r.cz + u;
                    sc.glow.push_back({x, y, z, 1.4f, 0.25f, 1.0f, 0.4f, 0.5f * edge, 0, 0, 0, 0});
                }
        }
        if (r.phase != 2) continue;
        // The band of light, in step with the screen's line: from the top of the head (58 up) to the
        // feet and back over the scan.
        float u = std::clamp(t / r.durScan, 0.0f, 1.0f);
        float s = 0.5f - 0.5f * std::cos(u * 2 * kPi);
        float fade = std::min(1.0f, std::min(t, r.durScan - t) / 0.12f);
        if (fade <= 0) continue;
        float y = r.floorY + 4 + (1 - s) * 54;
        // The player stands inside it, so it is a thin line of light a little way off all round.
        const int n = 120;
        const float rad = 27;
        for (int k = 0; k < n; ++k) {
            float a = k * 2 * kPi / n + time_ * 1.5f;
            float x = r.cx + std::cos(a) * rad, z = r.cz + std::sin(a) * rad;
            // smeared along the ring, so the dots run together into a line
            float vx = -std::sin(a) * 60, vz = std::cos(a) * 60;
            sc.glow.push_back({x, y, z, 0.7f, 0.35f, 1.0f, 0.5f, 0.6f * fade, vx, 0, vz, 0.05f});
            sc.glow.push_back({x, y, z, 1.8f, 0.15f, 0.8f, 0.3f, 0.08f * fade, vx, 0, vz, 0.05f});
        }
        lights.push_back({r.cx, y, r.cz, 150, 0.3f, 1.0f, 0.45f, 40 * fade});
    }
}
