#pragma once
// =============================================================================
// ToolChangePass.h  —  Dual Extruder Tool Change IR Pass
// =============================================================================
//
// This pass processes the GcodeIR::Program and:
//   1. Inserts ToolChange (Tx) commands at feature boundaries
//   2. Generates prime tower G-code sequences
//   3. Applies extruder offset compensation (G92 or firmware)
//   4. Manages per-extruder temperature (standby / active)
//   5. Handles per-extruder retraction parameters
//   6. Tracks active extruder context for all subsequent passes
//
// Tool change sequence (per BambuStudio toolchange.cpp):
//   ┌─────────────────────────────────────────────────────────┐
//   │  1. Finish current feature (FeatureEnd)                 │
//   │  2. Retract current extruder (synthetic Retract node)   │
//   │  3. Travel to prime tower position                      │
//   │  4. Emit Tx command                                     │
//   │  5. Apply extruder offset (G92 or OFFSET_EXTRUDER)      │
//   │  6. Set new extruder to active temp (M109 if needed)    │
//   │  7. Set old extruder to standby temp (M104)             │
//   │  8. Print prime tower loops (purge old material)        │
//   │  9. Unretract new extruder                              │
//   │ 10. Resume printing at feature start position           │
//   └─────────────────────────────────────────────────────────┘
//
// =============================================================================

#include "GcodeIR.h"
#include "DualExtruder.h"
#include <sstream>
#include <iomanip>

namespace GcodeIR {

// =============================================================================
// ToolChangePass
// =============================================================================
class ToolChangePass : public Pass {
public:
    struct Config {
        bool                    enabled             = false;
        DualExtruderParams      dualParams;
        // Assignment plan (computed by DualExtruderPlanner::plan())
        std::vector<LayerExtruderAssignment> assignments;
    } cfg;

    explicit ToolChangePass(const Config& c) : cfg(c) {}
    const char* name() const override { return "ToolChangePass"; }

    void run(Program& prog) override {
        if (!cfg.enabled || !cfg.dualParams.enabled) return;

        std::vector<Instruction> out;
        out.reserve(prog.instructions.size() * 2);

        int   curExtruder = 0;
        int   curLayer    = -1;
        float curX = 0, curY = 0, curZ = 0;

        // Track E per extruder (each has its own E register)
        float E[2] = {0.0f, 0.0f};

        // Emit startup: set both extruders to standby temp
        emitStartup(out, cfg.dualParams);

        // Track which layer's assignment we've applied
        int lastAssignedLayer = -1;

        for (size_t i = 0; i < prog.instructions.size(); ++i) {
            auto& ins = prog.instructions[i];

            // Track layer
            if (ins.op == OpCode::LayerChange) {
                curLayer = ins.layerIndex;
                out.push_back(ins);

                // Check if this layer needs a tool change at start
                // (e.g., raft uses T0, first model layer switches to T1 for support)
                if (curLayer < (int)cfg.assignments.size()) {
                    auto& asgn = cfg.assignments[curLayer];
                    // Pre-layer tool change: if the first feature needs a different extruder
                    // This is handled at FeatureBegin below
                }
                continue;
            }

            // Track position
            if (ins.op == OpCode::MoveLinear || ins.op == OpCode::MoveArc) {
                if (ins.pos.hasX) curX = ins.pos.x;
                if (ins.pos.hasY) curY = ins.pos.y;
                if (ins.pos.hasZ) curZ = ins.pos.z;
                // Update E for current extruder
                E[curExtruder] = ins.extrusion;
                ins.activeExtruder = curExtruder;
                out.push_back(ins);
                continue;
            }

            // At FeatureBegin: check if we need to switch extruder
            if (ins.op == OpCode::FeatureBegin && curLayer >= 0) {
                int targetExtruder = getTargetExtruder(ins.feature, curLayer);
                if (targetExtruder != curExtruder) {
                    // Emit tool change sequence
                    emitToolChange(out, curExtruder, targetExtruder,
                                   curX, curY, curZ, E,
                                   cfg.dualParams, curLayer);
                    curExtruder = targetExtruder;
                }
            }

            // Propagate active extruder to all instructions
            ins.activeExtruder = curExtruder;
            out.push_back(ins);
        }

        // End: retract both extruders, set to standby
        emitShutdown(out, cfg.dualParams, curExtruder);

        prog.instructions = std::move(out);
    }

private:
    // Determine target extruder for a given feature at a given layer
    int getTargetExtruder(GcodeFeature feat, int layerIdx) const {
        if (layerIdx < (int)cfg.assignments.size()) {
            auto& asgn = cfg.assignments[layerIdx];
            switch (feat) {
            case GcodeFeature::OuterShell:
            case GcodeFeature::InnerShell:
                return (int)asgn.shellExtruder;
            case GcodeFeature::Infill:
            case GcodeFeature::Solid:
            case GcodeFeature::Bridge:
                return (int)asgn.infillExtruder;
            case GcodeFeature::Support:
            case GcodeFeature::SupportIface:
                return (int)asgn.supportExtruder;
            case GcodeFeature::Raft:
                return (int)asgn.raftExtruder;
            case GcodeFeature::Skirt:
                return (int)asgn.skirtExtruder;
            default:
                return 0;
            }
        }
        // Fallback: use feature map defaults
        const auto& fmap = cfg.dualParams.featureMap;
        switch (feat) {
        case GcodeFeature::Support:
        case GcodeFeature::SupportIface:
            return (int)fmap.support;
        default:
            return 0;
        }
    }

    // Emit startup sequence for dual extruder
    void emitStartup(std::vector<Instruction>& out,
                      const DualExtruderParams& dp) const
    {
        out.push_back(Instruction::comment("=== Dual Extruder Startup ==="));
        out.push_back(Instruction::comment(
            std::string("T0: ") + dp.extruders[0].material +
            " @ " + std::to_string((int)dp.extruders[0].extruderTemp) + "C"));
        out.push_back(Instruction::comment(
            std::string("T1: ") + dp.extruders[1].material +
            " @ " + std::to_string((int)dp.extruders[1].extruderTemp) + "C"));

        // Heat both extruders: T0 to active, T1 to standby
        out.push_back(Instruction::setTemp(
            (int)dp.extruders[0].extruderTemp, false, false));  // T0 active (no wait)
        out.push_back(Instruction::setTemp(
            (int)dp.extruders[1].standbyTemp, false, false));   // T1 standby (no wait)

        // Select T0 as initial extruder
        Instruction tc;
        tc.op = OpCode::RawGcode;
        tc.text = "T0  ; select initial extruder";
        out.push_back(tc);

        // Wait for T0 to reach temp
        out.push_back(Instruction::setTemp(
            (int)dp.extruders[0].extruderTemp, false, true));   // M109 wait
    }

    // Emit full tool change sequence
    void emitToolChange(std::vector<Instruction>& out,
                         int fromIdx, int toIdx,
                         float curX, float curY, float curZ,
                         float E[2],
                         const DualExtruderParams& dp,
                         int layerIdx) const
    {
        const auto& fromCfg = dp.extruders[fromIdx];
        const auto& toCfg   = dp.extruders[toIdx];
        const auto& ptCfg   = dp.primeTower;

        std::ostringstream ss;
        ss << std::fixed << std::setprecision(3);

        out.push_back(Instruction::comment(
            std::string("=== Tool Change T") + std::to_string(fromIdx) +
            " -> T" + std::to_string(toIdx) + " (layer " +
            std::to_string(layerIdx) + ") ==="));

        // 1. Retract current extruder
        {
            float newE = E[fromIdx] - fromCfg.retractionLength;
            auto retract = Instruction::moveLinear(curX, curY, curZ,
                fromCfg.retractionSpeed, -fromCfg.retractionLength, newE,
                true, GcodeFeature::Travel);
            retract.pos.hasX = false; retract.pos.hasY = false;
            retract.text = "; retract T" + std::to_string(fromIdx) + " before tool change";
            out.push_back(retract);
            E[fromIdx] = newE;
        }

        // 2. Travel to prime tower position
        if (ptCfg.enabled) {
            auto travel = Instruction::moveLinear(
                ptCfg.posX, ptCfg.posY, curZ,
                dp.toolChangeTravelSpeed, 0, E[fromIdx], true,
                GcodeFeature::Travel);
            travel.text = "; travel to prime tower";
            out.push_back(travel);
        }

        // 3. Emit Tx command
        {
            Instruction tc;
            tc.op = OpCode::RawGcode;
            tc.text = "T" + std::to_string(toIdx) +
                      "  ; switch to extruder " + std::to_string(toIdx);
            tc.activeExtruder = toIdx;
            out.push_back(tc);
        }

        // 4. Apply extruder offset compensation
        if (!dp.applyOffsetInFirmware) {
            // Software offset via G92
            glm::vec2 off = toCfg.offset;
            ss.str("");
            ss << "G92 X" << (curX - off.x) << " Y" << (curY - off.y)
               << "  ; apply extruder offset T" << toIdx;
            Instruction offIns;
            offIns.op   = OpCode::RawGcode;
            offIns.text = ss.str();
            out.push_back(offIns);
        } else {
            // Firmware handles offset (e.g., Marlin HOTEND_OFFSET_X/Y)
            out.push_back(Instruction::comment(
                "Extruder offset applied by firmware"));
        }

        // 5. Reset E for new extruder
        out.push_back(Instruction::resetE());

        // 6. Set old extruder to standby temp
        if (dp.heatStandbyExtruder) {
            out.push_back(Instruction::setTemp(
                (int)fromCfg.standbyTemp, false, false));
            out.back().text = "; T" + std::to_string(fromIdx) + " -> standby " +
                               std::to_string((int)fromCfg.standbyTemp) + "C";
        }

        // 7. Heat new extruder to active temp (wait)
        out.push_back(Instruction::setTemp(
            (int)toCfg.extruderTemp, false, true));
        out.back().text = "; T" + std::to_string(toIdx) + " -> active " +
                           std::to_string((int)toCfg.extruderTemp) + "C";

        // 8. Fan control during tool change
        if (dp.enableToolChangeFan) {
            out.push_back(Instruction::setFan(255));
            out.back().text = "; full fan during tool change";
        }

        // 9. Print prime tower (purge old material)
        if (ptCfg.enabled) {
            emitPrimeTower(out, toIdx, ptCfg, toCfg, E[toIdx], layerIdx);
        }

        // 10. Dwell for cooling
        if (dp.toolChangeCoolTime > 0.1f) {
            Instruction dwell;
            dwell.op = OpCode::Dwell;
            dwell.dwellMs = (int)(dp.toolChangeCoolTime * 1000);
            out.push_back(dwell);
        }

        // 11. Unretract new extruder
        {
            float newE = E[toIdx] + toCfg.retractionLength + toCfg.retractionExtra;
            auto unretract = Instruction::moveLinear(
                ptCfg.enabled ? ptCfg.posX : curX,
                ptCfg.enabled ? ptCfg.posY : curY,
                curZ,
                toCfg.unretractSpeed,
                toCfg.retractionLength + toCfg.retractionExtra,
                newE, false, GcodeFeature::Travel);
            unretract.pos.hasX = false; unretract.pos.hasY = false;
            unretract.text = "; unretract T" + std::to_string(toIdx);
            out.push_back(unretract);
            E[toIdx] = newE;
        }

        out.push_back(Instruction::comment(
            "=== Tool Change Complete ==="));
    }

    // Emit prime tower print sequence
    void emitPrimeTower(std::vector<Instruction>& out,
                         int extruderIdx,
                         const PrimeTowerConfig& ptCfg,
                         const ExtruderConfig& extCfg,
                         float& E,
                         int layerIdx) const
    {
        out.push_back(Instruction::featureBegin(GcodeFeature::Purge));
        out.push_back(Instruction::comment(
            "Prime tower — purge " + std::to_string((int)ptCfg.purgeVolume) + " mm3"));

        float filamentArea = (float)M_PI * (extCfg.filamentDiameter * 0.5f) *
                                           (extCfg.filamentDiameter * 0.5f);
        float ew = extCfg.extrusionWidth;
        float lh = 0.2f;  // use standard layer height for tower
        float em = (filamentArea > 1e-8f) ? (ew * lh / filamentArea) : 0.01f;

        // Generate concentric loops from outer to inner radius
        float r = ptCfg.outerRadius;
        int   loopCount = 0;
        while (r > ptCfg.innerRadius + ew * 0.5f && loopCount < ptCfg.loopsPerChange * 2) {
            int segs = std::max(16, (int)(2.0f * (float)M_PI * r / ew));
            float cx = ptCfg.posX, cy = ptCfg.posY;

            // Travel to loop start
            float startX = cx + r;
            float startY = cy;
            auto travel = Instruction::moveLinear(startX, startY, 0,
                ptCfg.wipeSpeed, 0, E, true, GcodeFeature::Travel);
            travel.pos.hasZ = false;
            travel.text = "; prime tower loop travel";
            out.push_back(travel);

            // Print the loop
            for (int s = 1; s <= segs; ++s) {
                float a  = 2.0f * (float)M_PI * s / segs;
                float px = cx + r * std::cos(a);
                float py = cy + r * std::sin(a);
                float dx = px - (cx + r * std::cos(2.0f * (float)M_PI * (s-1) / segs));
                float dy = py - (cy + r * std::sin(2.0f * (float)M_PI * (s-1) / segs));
                float segLen = std::sqrt(dx*dx + dy*dy);
                float dE = segLen * em;
                E += dE;
                auto extrude = Instruction::moveLinear(px, py, 0,
                    ptCfg.wipeSpeed, dE, E, false, GcodeFeature::Purge);
                extrude.pos.hasZ = false;
                out.push_back(extrude);
            }

            r -= ew * 1.05f;
            ++loopCount;
        }

        out.push_back(Instruction::featureEnd(GcodeFeature::Purge));
    }

    // Emit shutdown sequence
    void emitShutdown(std::vector<Instruction>& out,
                       const DualExtruderParams& dp,
                       int activeExtruder) const
    {
        out.push_back(Instruction::comment("=== Dual Extruder Shutdown ==="));
        // Turn off both extruders
        out.push_back(Instruction::setTemp(0, false, false));
        out.back().text = "; T0 off";
        // Switch to T0 and turn off T1
        Instruction t0;
        t0.op = OpCode::RawGcode;
        t0.text = "T0  ; select T0 for shutdown";
        out.push_back(t0);
    }
};

// =============================================================================
// ExtruderContextPass
//    Propagates activeExtruder context through the IR after ToolChangePass.
//    Needed by GcodeSerializer to emit correct E values per extruder.
// =============================================================================
class ExtruderContextPass : public Pass {
public:
    const char* name() const override { return "ExtruderContextPass"; }

    void run(Program& prog) override {
        int curExtruder = 0;
        for (auto& ins : prog.instructions) {
            if (ins.op == OpCode::ToolChange || ins.op == OpCode::RawGcode) {
                if (ins.op == OpCode::ToolChange)
                    curExtruder = ins.toolChangeTo;
                else if (ins.text.size() >= 2 && ins.text[0] == 'T' &&
                         std::isdigit(ins.text[1]))
                    curExtruder = ins.text[1] - '0';
            }
            ins.activeExtruder = curExtruder;
        }
    }
};

} // namespace GcodeIR
