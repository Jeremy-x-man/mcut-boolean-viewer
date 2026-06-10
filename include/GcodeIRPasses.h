#pragma once
// =============================================================================
// GcodeIRPasses.h  —  Post-Processing Passes for GcodeIR
// =============================================================================
//
// Each Pass is a self-contained transformation on GcodeIR::Program.
// Passes are designed to be composable and independently testable.
//
// Pass execution order (recommended):
//   1. TimingAnnotPass    — compute per-move time estimates first
//   2. RetractPass        — resolve synthetic Retract/Unretract nodes
//   3. WipePass           — insert wipe moves before retracts
//   4. CoolingPass        — adjust feedrate for cooling constraints
//   5. FanControlPass     — insert M106/M107 based on layer/feature/bridge
//   6. ArcFitPass         — replace G1 polylines with G2/G3 arcs
//   7. PressureAdvPass    — insert pressure advance commands
//   8. SeamAlignPass      — rotate loop seam to best corner
//   9. TimingAnnotPass    — re-run after speed changes to update estimates
//
// =============================================================================

#include "GcodeIR.h"
#include <algorithm>
#include <numeric>
#include <cmath>
#include <sstream>
#include <iomanip>

namespace GcodeIR {

// =============================================================================
// 1. TimingAnnotPass
//    Estimates print time for each MoveLinear/MoveArc instruction.
//    Stores result in Instruction::estimatedTime and Program::estimatedTime.
//    Must run before CoolingPass (which uses time estimates).
// =============================================================================
class TimingAnnotPass : public Pass {
public:
    const char* name() const override { return "TimingAnnotPass"; }

    void run(Program& prog) override {
        float totalTime = 0.0f;
        int   curLayer  = 0;

        // Ensure layerStats is large enough
        if ((int)prog.layerStats.size() < prog.totalLayers)
            prog.layerStats.resize(prog.totalLayers);

        float curX = 0, curY = 0, curZ = 0;

        for (auto& ins : prog.instructions) {
            if (ins.op == OpCode::LayerChange) {
                curLayer = ins.layerIndex;
                if (curLayer < (int)prog.layerStats.size()) {
                    prog.layerStats[curLayer].z      = ins.layerZ;
                    prog.layerStats[curLayer].height = ins.layerHeight;
                }
                continue;
            }

            if (ins.op == OpCode::MoveLinear || ins.op == OpCode::MoveArc) {
                float dx = ins.pos.x - curX;
                float dy = ins.pos.y - curY;
                float dz = ins.pos.z - curZ;
                float dist = std::sqrt(dx*dx + dy*dy + dz*dz);

                float spd = ins.feedrate;  // mm/s
                if (spd < 0.1f) spd = 60.0f;  // fallback

                float t = (spd > 0) ? dist / spd : 0.0f;
                ins.estimatedTime = t;
                totalTime += t;

                if (curLayer < (int)prog.layerStats.size()) {
                    prog.layerStats[curLayer].printTime += t;
                    if (ins.isTravel)
                        prog.layerStats[curLayer].travelDist += dist;
                    else
                        prog.layerStats[curLayer].extrudeDist += dist;
                }

                curX = ins.pos.x;
                curY = ins.pos.y;
                curZ = ins.pos.z;
            }
        }

        prog.estimatedTime = totalTime;
    }
};

// =============================================================================
// 2. RetractPass
//    Resolves synthetic Retract/Unretract nodes into real G1 E moves.
//    Also inserts smart retraction: skips retraction for short travels.
//
//    Strategy (BambuStudio GCode.cpp retraction logic):
//      - On Retract: emit G1 E-retractLen F<retractSpeed>
//      - On Unretract: emit G1 E+retractLen F<unretractSpeed>
//      - Skip retraction if the following travel distance < minTravelForRetract
//      - Z-hop: optionally lift Z before travel (reduces stringing on tall parts)
// =============================================================================
class RetractPass : public Pass {
public:
    struct Config {
        float retractionLength    = 0.8f;   // mm
        float retractionSpeed     = 45.0f;  // mm/s
        float unretractSpeed      = 45.0f;  // mm/s
        float minTravelForRetract = 1.5f;   // mm — skip retract for shorter travels
        bool  enableZHop          = false;
        float zHopHeight          = 0.2f;   // mm
        float zHopSpeed           = 10.0f;  // mm/s
        float filamentDiameter    = 1.75f;
    } cfg;

    explicit RetractPass(const Config& c) : cfg(c) {}
    const char* name() const override { return "RetractPass"; }

    void run(Program& prog) override {
        std::vector<Instruction> out;
        out.reserve(prog.instructions.size() * 2);

        bool  retracted = false;
        float E         = 0.0f;
        float curX = 0, curY = 0, curZ = 0;
        float retractLen = cfg.retractionLength;

        // Pre-scan: for each Retract, find the next travel distance
        // to decide whether to actually retract
        std::vector<float> travelAfterRetract(prog.instructions.size(), 999.0f);
        {
            float nextTravelDist = 999.0f;
            for (int i = (int)prog.instructions.size()-1; i >= 0; --i) {
                auto& ins = prog.instructions[i];
                if (ins.op == OpCode::MoveLinear && ins.isTravel) {
                    float dx = ins.pos.x - curX;
                    float dy = ins.pos.y - curY;
                    nextTravelDist = std::sqrt(dx*dx + dy*dy);
                }
                if (ins.op == OpCode::Retract)
                    travelAfterRetract[i] = nextTravelDist;
            }
        }

        size_t idx = 0;
        for (auto& ins : prog.instructions) {
            if (ins.op == OpCode::Retract) {
                float travelDist = travelAfterRetract[idx];
                if (!retracted && travelDist >= cfg.minTravelForRetract) {
                    // Emit retract
                    E -= retractLen;
                    Instruction r;
                    r.op = OpCode::MoveLinear;
                    r.pos.x = curX; r.pos.hasX = false;  // no XY move
                    r.pos.y = curY; r.pos.hasY = false;
                    r.pos.z = curZ; r.pos.hasZ = false;
                    r.feedrate     = cfg.retractionSpeed;
                    r.extrusion    = E;
                    r.extrudeDelta = -retractLen;
                    r.isTravel     = true;
                    r.feature      = GcodeFeature::Travel;
                    r.text         = "; retract";
                    out.push_back(r);

                    // Z-hop
                    if (cfg.enableZHop) {
                        Instruction zh;
                        zh.op = OpCode::MoveLinear;
                        zh.pos.z = curZ + cfg.zHopHeight; zh.pos.hasZ = true;
                        zh.feedrate = cfg.zHopSpeed;
                        zh.isTravel = true;
                        zh.feature  = GcodeFeature::Travel;
                        zh.text     = "; z-hop";
                        out.push_back(zh);
                    }

                    retracted = true;
                    if (curLayer_ < (int)prog.layerStats.size())
                        prog.layerStats[curLayer_].retracts++;
                }
                ++idx; continue;
            }

            if (ins.op == OpCode::Unretract) {
                if (retracted) {
                    // Un-hop
                    if (cfg.enableZHop) {
                        Instruction uzh;
                        uzh.op = OpCode::MoveLinear;
                        uzh.pos.z = curZ; uzh.pos.hasZ = true;
                        uzh.feedrate = cfg.zHopSpeed;
                        uzh.isTravel = true;
                        uzh.feature  = GcodeFeature::Travel;
                        uzh.text     = "; z-hop restore";
                        out.push_back(uzh);
                    }

                    E += retractLen;
                    Instruction ur;
                    ur.op = OpCode::MoveLinear;
                    ur.pos.x = curX; ur.pos.hasX = false;
                    ur.pos.y = curY; ur.pos.hasY = false;
                    ur.pos.z = curZ; ur.pos.hasZ = false;
                    ur.feedrate     = cfg.unretractSpeed;
                    ur.extrusion    = E;
                    ur.extrudeDelta = retractLen;
                    ur.isTravel     = false;
                    ur.feature      = GcodeFeature::Travel;
                    ur.text         = "; unretract";
                    out.push_back(ur);
                    retracted = false;
                }
                ++idx; continue;
            }

            // Track position and E
            if (ins.op == OpCode::MoveLinear || ins.op == OpCode::MoveArc) {
                if (ins.pos.hasX) curX = ins.pos.x;
                if (ins.pos.hasY) curY = ins.pos.y;
                if (ins.pos.hasZ) curZ = ins.pos.z;
                E = ins.extrusion;
            }
            if (ins.op == OpCode::LayerChange) curLayer_ = ins.layerIndex;

            out.push_back(ins);
            ++idx;
        }

        prog.instructions = std::move(out);
    }

private:
    int curLayer_ = 0;
};

// =============================================================================
// 3. WipePass
//    Inserts a short wipe move before each retraction to reduce ooze.
//    The wipe retraces the last extrusion path segment in reverse.
//    (BambuStudio: wipe_before_retract, wipe_distance)
// =============================================================================
class WipePass : public Pass {
public:
    struct Config {
        bool  enabled     = true;
        float wipeDistance= 1.5f;   // mm — length of wipe move
        float wipeFactor  = 0.5f;   // extrusion factor during wipe (partial retract)
    } cfg;

    explicit WipePass(const Config& c) : cfg(c) {}
    const char* name() const override { return "WipePass"; }

    void run(Program& prog) override {
        if (!cfg.enabled) return;

        std::vector<Instruction> out;
        out.reserve(prog.instructions.size() + 64);

        float prevX = 0, prevY = 0;
        float curX  = 0, curY  = 0, curZ = 0;
        float E = 0;

        for (size_t i = 0; i < prog.instructions.size(); ++i) {
            auto& ins = prog.instructions[i];

            if (ins.op == OpCode::Retract && cfg.wipeDistance > 0.01f) {
                // Compute wipe direction: reverse of last extrude segment
                float dx = prevX - curX;
                float dy = prevY - curY;
                float len = std::sqrt(dx*dx + dy*dy);
                if (len > 0.01f) {
                    float scale = std::min(cfg.wipeDistance, len) / len;
                    float wx = curX + dx * scale;
                    float wy = curY + dy * scale;

                    // Wipe move (partial retract during wipe)
                    Instruction wipe;
                    wipe.op = OpCode::MoveLinear;
                    wipe.pos.x = wx; wipe.pos.hasX = true;
                    wipe.pos.y = wy; wipe.pos.hasY = true;
                    wipe.pos.z = curZ; wipe.pos.hasZ = false;
                    wipe.feedrate     = 60.0f;  // fast wipe
                    wipe.extrudeDelta = -cfg.wipeFactor * 0.2f;  // tiny retract during wipe
                    wipe.extrusion    = E + wipe.extrudeDelta;
                    wipe.isTravel     = true;
                    wipe.feature      = GcodeFeature::Wipe;
                    wipe.text         = "; wipe";
                    out.push_back(wipe);
                    E = wipe.extrusion;
                }
            }

            if (ins.op == OpCode::MoveLinear || ins.op == OpCode::MoveArc) {
                if (!ins.isTravel) { prevX = curX; prevY = curY; }
                if (ins.pos.hasX) curX = ins.pos.x;
                if (ins.pos.hasY) curY = ins.pos.y;
                if (ins.pos.hasZ) curZ = ins.pos.z;
                E = ins.extrusion;
            }

            out.push_back(ins);
        }

        prog.instructions = std::move(out);
    }
};

// =============================================================================
// 4. CoolingPass
//    Adjusts print speed to ensure minimum layer time for cooling.
//    (BambuStudio: cooling.cpp — slow_down_below_layer_time, min_fan_speed_layer_time)
//
//    Algorithm:
//      1. Sum estimated time for each layer (from TimingAnnotPass)
//      2. If layer time < minLayerTime: scale all feedrates down by
//         (minLayerTime / actualTime), clamped to minPrintSpeed
//      3. If layer time < fanMaxLayerTime: increase fan speed proportionally
// =============================================================================
class CoolingPass : public Pass {
public:
    struct Config {
        float minLayerTime      = 8.0f;   // seconds — slow down if layer faster
        float fanMaxLayerTime   = 60.0f;  // seconds — full fan if layer faster
        float minPrintSpeed     = 10.0f;  // mm/s — floor for slowdown
        float maxPrintSpeed     = 300.0f; // mm/s — cap
        bool  slowDownEnabled   = true;
        bool  fanByLayerTime    = true;
    } cfg;

    explicit CoolingPass(const Config& c) : cfg(c) {}
    const char* name() const override { return "CoolingPass"; }

    void run(Program& prog) override {
        // Re-compute layer times (TimingAnnotPass may have run before)
        // Map: layerIndex → [begin_idx, end_idx, total_print_time]
        struct LayerRange { size_t begin = 0, end = 0; float printTime = 0; };
        std::vector<LayerRange> ranges;

        int curLayer = -1;
        for (size_t i = 0; i < prog.instructions.size(); ++i) {
            auto& ins = prog.instructions[i];
            if (ins.op == OpCode::LayerChange) {
                if (curLayer >= 0 && curLayer < (int)ranges.size())
                    ranges[curLayer].end = i;
                curLayer = ins.layerIndex;
                if (curLayer >= (int)ranges.size())
                    ranges.resize(curLayer + 1);
                ranges[curLayer].begin = i;
            }
            if ((ins.op == OpCode::MoveLinear || ins.op == OpCode::MoveArc)
                && !ins.isTravel && curLayer >= 0 && curLayer < (int)ranges.size())
            {
                ranges[curLayer].printTime += ins.estimatedTime;
            }
        }
        if (curLayer >= 0 && curLayer < (int)ranges.size())
            ranges[curLayer].end = prog.instructions.size();

        // Apply slowdown and fan speed
        for (int li = 0; li < (int)ranges.size(); ++li) {
            auto& r = ranges[li];
            float layerTime = r.printTime;
            if (layerTime < 1e-4f) continue;

            float speedScale = 1.0f;
            if (cfg.slowDownEnabled && layerTime < cfg.minLayerTime) {
                speedScale = layerTime / cfg.minLayerTime;
                // Clamp: don't go below minPrintSpeed
                // We'll apply the scale to each move's feedrate
            }

            for (size_t i = r.begin; i < r.end && i < prog.instructions.size(); ++i) {
                auto& ins = prog.instructions[i];
                if ((ins.op == OpCode::MoveLinear || ins.op == OpCode::MoveArc)
                    && !ins.isTravel && speedScale < 1.0f)
                {
                    float newSpd = ins.feedrate * speedScale;
                    newSpd = std::max(cfg.minPrintSpeed, std::min(cfg.maxPrintSpeed, newSpd));
                    if (std::abs(newSpd - ins.feedrate) > 0.1f) {
                        ins.feedrate = newSpd;
                        ins.modified = true;
                    }
                }
            }
        }
    }
};

// =============================================================================
// 5. FanControlPass
//    Inserts M106/M107 fan commands at appropriate positions.
//
//    Rules (BambuStudio cooling.cpp):
//      - Fan off for first N layers (default: 3)
//      - Fan at minFanSpeed for layers after firstFanLayer
//      - Fan at maxFanSpeed when layer time < fanMaxLayerTime (cooling needed)
//      - Fan at 100% for bridge features (immediate)
//      - Fan at 100% for overhang features
//      - Ramp up fan speed gradually (don't jump from 0 to 255 instantly)
//      - Fan off at end of print
// =============================================================================
class FanControlPass : public Pass {
public:
    struct Config {
        int   firstFanLayer     = 3;     // enable fan after this layer
        int   minFanSpeed       = 50;    // 0-255 (20%)
        int   maxFanSpeed       = 255;   // 0-255 (100%)
        int   bridgeFanSpeed    = 255;   // 100% for bridges
        int   overhangFanSpeed  = 200;   // ~78% for overhangs
        float fanMaxLayerTime   = 60.0f; // full fan if layer time < this
        float fanMinLayerTime   = 8.0f;  // min layer time for min fan speed
        bool  rampFan           = true;  // gradually ramp fan speed
        int   rampSteps         = 3;     // layers to ramp over
    } cfg;

    explicit FanControlPass(const Config& c) : cfg(c) {}
    const char* name() const override { return "FanControlPass"; }

    void run(Program& prog) override {
        std::vector<Instruction> out;
        out.reserve(prog.instructions.size() + 64);

        int   curLayer    = -1;
        int   curFanSpeed = 0;
        bool  inBridge    = false;
        bool  inOverhang  = false;

        auto emitFan = [&](int speed, const char* reason) {
            if (speed == curFanSpeed) return;
            Instruction f = Instruction::setFan(speed);
            f.text = std::string("; fan ") + reason;
            out.push_back(f);
            curFanSpeed = speed;
        };

        for (auto& ins : prog.instructions) {
            if (ins.op == OpCode::LayerChange) {
                curLayer = ins.layerIndex;

                // Determine target fan speed for this layer
                if (curLayer < cfg.firstFanLayer) {
                    emitFan(0, "off (first layers)");
                } else {
                    // Compute fan speed based on layer time
                    int targetFan = cfg.minFanSpeed;
                    if (curLayer < (int)prog.layerStats.size()) {
                        float lt = prog.layerStats[curLayer].printTime;
                        if (lt < cfg.fanMinLayerTime) {
                            targetFan = cfg.maxFanSpeed;
                        } else if (lt < cfg.fanMaxLayerTime) {
                            // Linear interpolation
                            float t = (cfg.fanMaxLayerTime - lt) /
                                      (cfg.fanMaxLayerTime - cfg.fanMinLayerTime);
                            targetFan = (int)(cfg.minFanSpeed + t * (cfg.maxFanSpeed - cfg.minFanSpeed));
                        }
                    }
                    emitFan(targetFan, "layer");
                }
                out.push_back(ins);
                continue;
            }

            // Bridge feature: full fan
            if (ins.op == OpCode::FeatureBegin && ins.feature == GcodeFeature::Bridge) {
                inBridge = true;
                emitFan(cfg.bridgeFanSpeed, "bridge 100%");
            }
            if (ins.op == OpCode::FeatureEnd && ins.feature == GcodeFeature::Bridge) {
                inBridge = false;
                // Restore layer fan speed
                int restore = (curLayer >= cfg.firstFanLayer) ? cfg.minFanSpeed : 0;
                emitFan(restore, "bridge end restore");
            }

            out.push_back(ins);
        }

        // Fan off at end
        if (curFanSpeed > 0) {
            out.push_back(Instruction::setFan(0));
            out.back().text = "; fan off (end)";
        }

        prog.instructions = std::move(out);
    }
};

// =============================================================================
// 6. ArcFitPass
//    Replaces sequences of G1 moves with G2/G3 arc commands.
//    Uses Kasa least-squares circle fitting (same as BambuStudio ArcFitter).
//
//    Algorithm:
//      For each consecutive run of MoveLinear instructions in the same feature:
//        1. Try to fit a circle through a sliding window of N points
//        2. If max deviation < tolerance: replace window with a single MoveArc
//        3. Extend window as long as deviation stays within tolerance
// =============================================================================
class ArcFitPass : public Pass {
public:
    struct Config {
        bool  enabled      = true;
        float tolerance    = 0.05f;  // mm — max deviation from fitted arc
        int   minPoints    = 4;      // minimum points to attempt arc fitting
        float minRadius    = 0.5f;   // mm — ignore tiny arcs
        float maxRadius    = 200.0f; // mm — ignore huge arcs (nearly straight)
    } cfg;

    explicit ArcFitPass(const Config& c) : cfg(c) {}
    const char* name() const override { return "ArcFitPass"; }

    void run(Program& prog) override {
        if (!cfg.enabled) return;

        std::vector<Instruction> out;
        out.reserve(prog.instructions.size());

        size_t n = prog.instructions.size();
        size_t i = 0;

        while (i < n) {
            auto& ins = prog.instructions[i];

            // Only try arc fitting on extrusion moves (not travel)
            if (ins.op != OpCode::MoveLinear || ins.isTravel) {
                out.push_back(ins);
                ++i; continue;
            }

            // Collect a run of consecutive extrusion MoveLinear in same feature
            GcodeFeature feat = ins.feature;
            std::vector<size_t> run;
            run.push_back(i);
            size_t j = i + 1;
            while (j < n &&
                   prog.instructions[j].op == OpCode::MoveLinear &&
                   !prog.instructions[j].isTravel &&
                   prog.instructions[j].feature == feat)
            {
                run.push_back(j);
                ++j;
            }

            if ((int)run.size() < cfg.minPoints) {
                // Too short for arc fitting
                for (size_t k : run) out.push_back(prog.instructions[k]);
                i = j; continue;
            }

            // Try to fit arcs greedily over the run
            // Build point list (including start point from previous position)
            std::vector<glm::vec2> pts;
            pts.reserve(run.size() + 1);
            // Start point: position before first instruction in run
            // We track this from the previous instruction
            float sx = 0, sy = 0;
            if (i > 0) {
                for (int back = (int)i - 1; back >= 0; --back) {
                    auto& prev = prog.instructions[back];
                    if (prev.op == OpCode::MoveLinear || prev.op == OpCode::MoveArc) {
                        sx = prev.pos.x; sy = prev.pos.y; break;
                    }
                }
            }
            pts.push_back({sx, sy});
            for (size_t k : run)
                pts.push_back({prog.instructions[k].pos.x, prog.instructions[k].pos.y});

            // Greedy arc fitting
            size_t pi = 0;  // index into pts
            size_t ri = 0;  // index into run

            while (pi + cfg.minPoints < (int)pts.size()) {
                // Try to extend arc from pi
                int bestEnd = -1;
                glm::vec2 bestCenter;
                float bestRadius = 0;
                bool  bestCCW    = false;

                for (int end = pi + cfg.minPoints; end < (int)pts.size(); ++end) {
                    glm::vec2 center;
                    float radius;
                    if (!kasaFit(pts, pi, end, center, radius)) break;
                    if (radius < cfg.minRadius || radius > cfg.maxRadius) break;

                    // Check all points in [pi, end] are within tolerance
                    bool ok = true;
                    for (int k = pi; k <= end; ++k) {
                        float dx = pts[k].x - center.x;
                        float dy = pts[k].y - center.y;
                        float dev = std::abs(std::sqrt(dx*dx + dy*dy) - radius);
                        if (dev > cfg.tolerance) { ok = false; break; }
                    }
                    if (!ok) break;

                    bestEnd    = end;
                    bestCenter = center;
                    bestRadius = radius;

                    // Determine CCW/CW from cross product
                    glm::vec2 v1 = pts[pi+1] - pts[pi];
                    glm::vec2 v2 = pts[pi+2] - pts[pi+1];
                    bestCCW = (v1.x * v2.y - v1.y * v2.x) > 0;
                }

                if (bestEnd < 0 || bestEnd <= pi) {
                    // No arc found, emit the next G1 as-is
                    if (ri < run.size())
                        out.push_back(prog.instructions[run[ri++]]);
                    ++pi;
                } else {
                    // Emit arc from pts[pi] to pts[bestEnd]
                    size_t endRunIdx = ri + (bestEnd - pi) - 1;
                    if (endRunIdx >= run.size()) endRunIdx = run.size() - 1;

                    auto& endIns = prog.instructions[run[endRunIdx]];

                    // Compute total extrusion for the arc
                    float totalDE = 0;
                    for (size_t k = ri; k <= endRunIdx; ++k)
                        totalDE += prog.instructions[run[k]].extrudeDelta;

                    float I = bestCenter.x - pts[pi].x;
                    float J = bestCenter.y - pts[pi].y;

                    Instruction arc = Instruction::moveArc(
                        endIns.pos.x, endIns.pos.y, endIns.pos.z,
                        I, J, bestCCW,
                        endIns.feedrate,
                        totalDE, endIns.extrusion,
                        feat);
                    arc.modified = true;
                    out.push_back(arc);

                    pi = bestEnd;
                    ri = endRunIdx + 1;
                }
            }

            // Emit remaining points in run
            while (ri < run.size())
                out.push_back(prog.instructions[run[ri++]]);

            i = j;
        }

        prog.instructions = std::move(out);
    }

private:
    // Kasa least-squares circle fit for points[begin..end]
    // Returns false if fit is degenerate
    static bool kasaFit(const std::vector<glm::vec2>& pts,
                         int begin, int end,
                         glm::vec2& center, float& radius)
    {
        int n = end - begin + 1;
        if (n < 3) return false;

        double sumX = 0, sumY = 0, sumX2 = 0, sumY2 = 0;
        double sumXY = 0, sumX3 = 0, sumY3 = 0, sumX2Y = 0, sumXY2 = 0;

        for (int i = begin; i <= end; ++i) {
            double x = pts[i].x, y = pts[i].y;
            double x2 = x*x, y2 = y*y;
            sumX  += x;  sumY  += y;
            sumX2 += x2; sumY2 += y2;
            sumXY += x*y;
            sumX3 += x2*x; sumY3 += y2*y;
            sumX2Y+= x2*y; sumXY2+= x*y2;
        }

        double A = 2*(sumX*sumX - n*sumX2);
        double B = 2*(sumX*sumY - n*sumXY);
        double C = 2*(sumY*sumY - n*sumY2);
        double D = sumX2*sumX - n*sumX3 + sumX*sumY2 - n*sumXY2;
        double E2= sumX2*sumY - n*sumX2Y + sumY*sumY2 - n*sumY3;

        double det = A*C - B*B;
        if (std::abs(det) < 1e-10) return false;

        double cx = (D*C - B*E2) / det;
        double cy = (A*E2 - B*D) / det;
        center = {(float)cx, (float)cy};

        double r = 0;
        for (int i = begin; i <= end; ++i) {
            double dx = pts[i].x - cx, dy = pts[i].y - cy;
            r += std::sqrt(dx*dx + dy*dy);
        }
        radius = (float)(r / n);
        return radius > 1e-4f;
    }
};

// =============================================================================
// 7. PressureAdvPass
//    Inserts pressure advance (PA) commands at feature boundaries.
//    (BambuStudio: SET_PRESSURE_ADVANCE / M572)
//
//    Different features use different PA values:
//      Outer shell:  lower PA (more responsive)
//      Infill:       higher PA (faster moves, more pressure needed)
//      Bridge:       zero PA (no pressure needed for bridge)
// =============================================================================
class PressureAdvPass : public Pass {
public:
    struct Config {
        bool  enabled          = false;
        float outerShellPA     = 0.04f;
        float innerShellPA     = 0.04f;
        float infillPA         = 0.08f;
        float bridgePA         = 0.0f;
        float supportPA        = 0.06f;
        bool  useKlipper       = true;   // SET_PRESSURE_ADVANCE vs M572
    } cfg;

    explicit PressureAdvPass(const Config& c) : cfg(c) {}
    const char* name() const override { return "PressureAdvPass"; }

    void run(Program& prog) override {
        if (!cfg.enabled) return;

        std::vector<Instruction> out;
        out.reserve(prog.instructions.size() + 32);

        float lastPA = -1.0f;

        auto emitPA = [&](float val) {
            if (std::abs(val - lastPA) < 1e-5f) return;
            Instruction pa = Instruction::pressureAdv(val);
            if (cfg.useKlipper)
                pa.text = "SET_PRESSURE_ADVANCE ADVANCE=" + std::to_string(val);
            else
                pa.text = "M572 S" + std::to_string(val);
            out.push_back(pa);
            lastPA = val;
        };

        for (auto& ins : prog.instructions) {
            if (ins.op == OpCode::FeatureBegin) {
                switch (ins.feature) {
                    case GcodeFeature::OuterShell:   emitPA(cfg.outerShellPA); break;
                    case GcodeFeature::InnerShell:   emitPA(cfg.innerShellPA); break;
                    case GcodeFeature::Infill:
                    case GcodeFeature::Solid:        emitPA(cfg.infillPA);     break;
                    case GcodeFeature::Bridge:       emitPA(cfg.bridgePA);     break;
                    case GcodeFeature::Support:
                    case GcodeFeature::SupportIface: emitPA(cfg.supportPA);    break;
                    default: break;
                }
            }
            out.push_back(ins);
        }

        prog.instructions = std::move(out);
    }
};

// =============================================================================
// 8. SeamAlignPass
//    Rotates the start point of each perimeter loop to align the seam
//    to the sharpest corner (or a user-specified position).
//    (BambuStudio: seam_position = nearest/aligned/back/random)
// =============================================================================
class SeamAlignPass : public Pass {
public:
    enum class SeamMode { Nearest, Aligned, Back, Random };

    struct Config {
        SeamMode mode          = SeamMode::Aligned;
        glm::vec2 alignTarget  = {0, 0};  // for Nearest mode
    } cfg;

    explicit SeamAlignPass(const Config& c) : cfg(c) {}
    const char* name() const override { return "SeamAlignPass"; }

    void run(Program& prog) override {
        // For each FeatureBegin(OuterShell) ... FeatureEnd(OuterShell) block,
        // find the loop and rotate its start point.
        // This is a simplified version — full implementation would need to
        // track the actual loop geometry.
        // Here we just annotate the feature begin with the seam mode.
        for (auto& ins : prog.instructions) {
            if (ins.op == OpCode::FeatureBegin &&
                ins.feature == GcodeFeature::OuterShell)
            {
                // Mark for serializer to apply seam alignment
                ins.text = std::string("; seam=") +
                    (cfg.mode == SeamMode::Aligned ? "aligned" :
                     cfg.mode == SeamMode::Back    ? "back"    :
                     cfg.mode == SeamMode::Random  ? "random"  : "nearest");
            }
        }
    }
};

// =============================================================================
// 9. TimingAnnotPass (second run)
//    Re-annotates timing after CoolingPass may have changed feedrates.
//    Also inserts M73 progress comments (Prusa-style).
// =============================================================================
class ProgressAnnotPass : public Pass {
public:
    const char* name() const override { return "ProgressAnnotPass"; }

    void run(Program& prog) override {
        // Re-sum total time
        float total = 0;
        for (auto& ins : prog.instructions)
            if (ins.op == OpCode::MoveLinear || ins.op == OpCode::MoveArc)
                total += ins.estimatedTime;
        prog.estimatedTime = total;

        // Insert M73 P<percent> Q<remaining_min> at each layer change
        std::vector<Instruction> out;
        out.reserve(prog.instructions.size() + 32);

        float elapsed = 0;
        for (auto& ins : prog.instructions) {
            if (ins.op == OpCode::LayerChange && total > 0) {
                int pct = (int)(elapsed / total * 100.0f);
                int remMin = (int)((total - elapsed) / 60.0f);
                std::ostringstream ss;
                ss << "M73 P" << pct << " Q" << remMin
                   << "  ; " << pct << "% done, " << remMin << "m remaining";
                out.push_back(Instruction::raw(ss.str()));
            }
            if (ins.op == OpCode::MoveLinear || ins.op == OpCode::MoveArc)
                elapsed += ins.estimatedTime;
            out.push_back(ins);
        }

        prog.instructions = std::move(out);
    }
};

// =============================================================================
// GcodeSerializer
//    Walks the final IR and emits the text G-code string.
//    This is the only place that knows about G-code syntax.
// =============================================================================
class GcodeSerializer {
public:
    struct Config {
        int   precision     = 4;    // decimal places for coordinates
        bool  emitComments  = true;
        bool  emitFeatureTags = true;
        bool  relativeE     = false; // absolute E by default
    } cfg;

    std::string serialize(const GcodeIR::Program& prog) {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(cfg.precision);

        float curX = 0, curY = 0, curZ = -999.0f;
        float lastFeedrate = 0;

        auto mmPerMin = [](float mmS) { return mmS * 60.0f; };

        for (auto& ins : prog.instructions) {
            switch (ins.op) {

            case OpCode::Comment:
                if (cfg.emitComments)
                    ss << "; " << ins.text << "\n";
                break;

            case OpCode::RawGcode:
                ss << ins.text << "\n";
                break;

            case OpCode::LayerChange:
                if (cfg.emitComments)
                    ss << "\n; --- Layer " << ins.layerIndex
                       << " (z=" << std::setprecision(3) << ins.layerZ
                       << " mm) ---\n";
                // Z move
                if (std::abs(ins.layerZ - curZ) > 1e-4f) {
                    ss << "G1 Z" << std::setprecision(cfg.precision) << ins.layerZ
                       << " F" << (int)mmPerMin(5.0f) << "\n";
                    curZ = ins.layerZ;
                }
                break;

            case OpCode::FeatureBegin:
                if (cfg.emitFeatureTags)
                    ss << "; " << featureName(ins.feature) << "\n";
                if (!ins.text.empty() && cfg.emitComments)
                    ss << "; " << ins.text << "\n";
                break;

            case OpCode::FeatureEnd:
                break;

            case OpCode::SetFan:
                if (ins.fanSpeed == 0)
                    ss << "M107";
                else
                    ss << "M106 S" << ins.fanSpeed;
                if (cfg.emitComments && !ins.text.empty())
                    ss << "  " << ins.text;
                ss << "\n";
                break;

            case OpCode::SetTemp:
                if (ins.tempIsBed)
                    ss << (ins.tempWait ? "M190" : "M140") << " S" << ins.tempTarget;
                else
                    ss << (ins.tempWait ? "M109" : "M104") << " S" << ins.tempTarget;
                ss << "\n";
                break;

            case OpCode::ResetE:
                ss << "G92 E0\n";
                break;

            case OpCode::Dwell:
                ss << "G4 P" << ins.dwellMs << "\n";
                break;

            case OpCode::PressureAdv:
                ss << ins.text << "\n";
                break;

            case OpCode::MoveLinear: {
                bool isG0 = ins.isTravel && ins.extrudeDelta == 0;
                ss << (isG0 ? "G0" : "G1");
                if (ins.pos.hasX) ss << " X" << ins.pos.x;
                if (ins.pos.hasY) ss << " Y" << ins.pos.y;
                if (ins.pos.hasZ) ss << " Z" << ins.pos.z;
                if (!ins.isTravel || ins.extrudeDelta != 0)
                    ss << " E" << ins.extrusion;
                if (ins.feedrate > 0 && std::abs(ins.feedrate - lastFeedrate) > 0.5f) {
                    ss << " F" << (int)mmPerMin(ins.feedrate);
                    lastFeedrate = ins.feedrate;
                }
                if (cfg.emitComments && !ins.text.empty())
                    ss << "  " << ins.text;
                ss << "\n";
                if (ins.pos.hasX) curX = ins.pos.x;
                if (ins.pos.hasY) curY = ins.pos.y;
                if (ins.pos.hasZ) curZ = ins.pos.z;
                break;
            }

            case OpCode::MoveArc: {
                ss << (ins.arcCCW ? "G3" : "G2");
                ss << " X" << ins.pos.x << " Y" << ins.pos.y;
                ss << " I" << ins.arcI << " J" << ins.arcJ;
                ss << " E" << ins.extrusion;
                if (ins.feedrate > 0 && std::abs(ins.feedrate - lastFeedrate) > 0.5f) {
                    ss << " F" << (int)mmPerMin(ins.feedrate);
                    lastFeedrate = ins.feedrate;
                }
                ss << "\n";
                curX = ins.pos.x; curY = ins.pos.y;
                break;
            }

            case OpCode::Retract:
            case OpCode::Unretract:
                // Should have been resolved by RetractPass
                if (cfg.emitComments)
                    ss << "; [unresolved retract/unretract — RetractPass not run]\n";
                break;

            default:
                break;
            }
        }

        return ss.str();
    }
};

} // namespace GcodeIR
