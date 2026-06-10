#pragma once
// =============================================================================
// GcodeIR.h  —  G-code Intermediate Representation
// =============================================================================
//
// Design philosophy (inspired by LLVM IR + BambuStudio GCode.cpp):
//
//   The G-code generation pipeline is split into two stages:
//
//   Stage 1 — Path Planning (GcodeExporter):
//     Converts SliceResult geometry into a flat list of GcodeIR::Instruction
//     nodes. This stage only deals with geometry and extrusion math; it knows
//     nothing about cooling, fan speed, retraction, or arc fitting.
//
//   Stage 2 — Post-Processing Pipeline (PostProcessPipeline):
//     A chain of composable Pass objects, each of which walks the IR and
//     transforms it. Passes are independent and can be reordered or disabled.
//
//     Built-in passes:
//       RetractPass        — insert retract/unretract moves at travel gaps
//       CoolingPass        — insert M73 progress, adjust speed for cooling
//       FanControlPass     — insert M106/M107 based on layer/bridge/overhang
//       ArcFitPass         — replace G1 polylines with G2/G3 arcs
//       PressureAdvPass    — insert M572/SET_PRESSURE_ADVANCE for Klipper
//       SeamAlignPass      — rotate loop start points to align seam to corner
//       WipePass           — insert wipe-before-retract moves
//       TimingAnnotPass    — annotate each layer with estimated time comment
//
//   Stage 3 — Serialization (GcodeSerializer):
//     Walks the final IR and emits the text G-code string.
//
// Instruction node types:
//   MOVE_LINEAR    G0/G1  (travel or extrude)
//   MOVE_ARC       G2/G3  (arc extrude, produced by ArcFitPass)
//   SET_TEMP       M104/M109/M140/M190
//   SET_FAN        M106/M107
//   SET_FEEDRATE   F override (virtual, merged into next MOVE)
//   RETRACT        synthetic retract (resolved by RetractPass)
//   UNRETRACT      synthetic unretract
//   LAYER_CHANGE   virtual node marking layer boundary
//   FEATURE_BEGIN  marks start of a feature (shell/infill/support/bridge…)
//   FEATURE_END    marks end of a feature
//   COMMENT        raw comment line
//   RAW_GCODE      verbatim G-code (startup/end sequences)
//
// =============================================================================

#include <vector>
#include <string>
#include <memory>
#include <functional>
#include <cmath>
#include <cassert>
#include <unordered_map>
#include <glm/glm.hpp>

// ---------------------------------------------------------------------------
// Instruction node
// ---------------------------------------------------------------------------
namespace GcodeIR {

// ---------------------------------------------------------------------------
// Feature type enum (matches SlicerRenderer pathType)
// ---------------------------------------------------------------------------
enum class GcodeFeature : uint8_t {
    None            = 0,
    OuterShell      = 1,
    Infill          = 2,
    Solid           = 3,
    Support         = 4,
    Skirt           = 5,
    Raft            = 6,
    Bridge          = 7,
    SupportIface    = 8,
    InnerShell      = 9,
    Travel          = 10,
    Wipe            = 11,
    Purge           = 12,
};

inline const char* featureName(GcodeFeature f) {
    switch (f) {
        case GcodeFeature::OuterShell:   return "Outer Shell";
        case GcodeFeature::InnerShell:   return "Inner Shell";
        case GcodeFeature::Infill:       return "Infill";
        case GcodeFeature::Solid:        return "Solid Fill";
        case GcodeFeature::Support:      return "Support";
        case GcodeFeature::SupportIface: return "Support Interface";
        case GcodeFeature::Bridge:       return "Bridge";
        case GcodeFeature::Skirt:        return "Skirt";
        case GcodeFeature::Raft:         return "Raft";
        case GcodeFeature::Travel:       return "Travel";
        case GcodeFeature::Wipe:         return "Wipe";
        case GcodeFeature::Purge:        return "Purge";
        default:                         return "Unknown";
    }
}

enum class OpCode : uint8_t {
    MoveLinear,     // G0 or G1
    MoveArc,        // G2 or G3
    SetTemp,        // M104/M109/M140/M190
    SetFan,         // M106/M107
    SetFeedrate,    // virtual feedrate (merged into next move on emit)
    Retract,        // synthetic — resolved by RetractPass
    Unretract,      // synthetic — resolved by RetractPass
    LayerChange,    // virtual layer boundary marker
    FeatureBegin,   // marks start of a feature region
    FeatureEnd,     // marks end of a feature region
    Comment,        // ; text
    RawGcode,       // verbatim gcode string
    ResetE,         // G92 E0
    Dwell,          // G4 P<ms>
    SetAccel,       // M204 S<val>
    PressureAdv,    // M572 / SET_PRESSURE_ADVANCE
    FanWait,        // virtual: wait for fan to reach speed
    ToolChange,     // Tx command (T0/T1) — resolved by ToolChangePass
    SetExtruder,    // virtual: set active extruder context
    PrimeTower,     // virtual: prime tower print sequence
    ExtruderOffset, // G92 / OFFSET_EXTRUDER for nozzle offset compensation
};

// Position in 3D space (mm)
struct Pos3 {
    float x = 0, y = 0, z = 0;
    bool  hasX = false, hasY = false, hasZ = false;
};

struct Instruction {
    OpCode      op;
    GcodeFeature feature = GcodeFeature::None;

    // --- MoveLinear / MoveArc fields ---
    Pos3   pos;             // target position
    float  feedrate = 0;    // mm/s (0 = inherit)
    float  extrusion = 0;   // absolute E value after this move (NaN = no E)
    float  extrudeDelta = 0;// relative dE for this move segment
    bool   isTravel = false;// true if no extrusion

    // --- MoveArc extra fields ---
    float  arcI = 0, arcJ = 0;  // center offset from current pos
    bool   arcCCW = false;       // G3 = true, G2 = false

    // --- SetTemp fields ---
    int    tempTarget = 0;
    bool   tempWait   = false;  // M109/M190 vs M104/M140
    bool   tempIsBed  = false;

    // --- SetFan fields ---
    int    fanSpeed = 0;    // 0-255

    // --- LayerChange fields ---
    int    layerIndex = -1;
    float  layerZ     = 0;
    float  layerHeight= 0;

    // --- FeatureBegin/End fields ---
    // (feature field above)

    // --- Comment / RawGcode ---
    std::string text;

    // --- Pressure advance ---
    float  pressureAdvValue = 0;

    // --- Dwell ---
    int    dwellMs = 0;

    // --- Accel ---
    float  accelPrint  = 0;
    float  accelTravel = 0;

    // --- ToolChange fields ---
    int    toolChangeFrom  = 0;  // extruder index switching from
    int    toolChangeTo    = 1;  // extruder index switching to
    float  purgeVolume     = 0;  // mm³ purge volume for prime tower
    glm::vec2 extruderOffset = {0, 0};  // nozzle offset (X, Y) mm

    // --- Metadata (set by passes) ---
    float  estimatedTime = 0;   // seconds for this move (set by TimingAnnotPass)
    bool   modified = false;    // flag for passes to mark changed nodes
    int    activeExtruder = 0;  // current active extruder (0=T0, 1=T1)

    // Constructors for common cases
    static Instruction moveLinear(float x, float y, float z,
                                   float feedMmS, float dE,
                                   float absE, bool travel,
                                   GcodeFeature feat = GcodeFeature::None)
    {
        Instruction ins;
        ins.op = OpCode::MoveLinear;
        ins.pos.x = x; ins.pos.hasX = true;
        ins.pos.y = y; ins.pos.hasY = true;
        ins.pos.z = z; ins.pos.hasZ = true;
        ins.feedrate    = feedMmS;
        ins.extrudeDelta= dE;
        ins.extrusion   = absE;
        ins.isTravel    = travel;
        ins.feature     = feat;
        return ins;
    }

    static Instruction moveArc(float x, float y, float z,
                                 float I, float J, bool ccw,
                                 float feedMmS, float dE, float absE,
                                 GcodeFeature feat = GcodeFeature::None)
    {
        Instruction ins;
        ins.op = OpCode::MoveArc;
        ins.pos.x = x; ins.pos.hasX = true;
        ins.pos.y = y; ins.pos.hasY = true;
        ins.pos.z = z; ins.pos.hasZ = true;
        ins.arcI = I; ins.arcJ = J; ins.arcCCW = ccw;
        ins.feedrate     = feedMmS;
        ins.extrudeDelta = dE;
        ins.extrusion    = absE;
        ins.isTravel     = false;
        ins.feature      = feat;
        return ins;
    }

    static Instruction layerChange(int idx, float z, float height) {
        Instruction ins;
        ins.op = OpCode::LayerChange;
        ins.layerIndex  = idx;
        ins.layerZ      = z;
        ins.layerHeight = height;
        return ins;
    }

    static Instruction featureBegin(GcodeFeature f) {
        Instruction ins;
        ins.op = OpCode::FeatureBegin;
        ins.feature = f;
        return ins;
    }

    static Instruction featureEnd(GcodeFeature f) {
        Instruction ins;
        ins.op = OpCode::FeatureEnd;
        ins.feature = f;
        return ins;
    }

    static Instruction comment(const std::string& s) {
        Instruction ins;
        ins.op = OpCode::Comment;
        ins.text = s;
        return ins;
    }

    static Instruction raw(const std::string& s) {
        Instruction ins;
        ins.op = OpCode::RawGcode;
        ins.text = s;
        return ins;
    }

    static Instruction setFan(int speed) {
        Instruction ins;
        ins.op = OpCode::SetFan;
        ins.fanSpeed = speed;
        return ins;
    }

    static Instruction setTemp(int temp, bool isBed, bool wait) {
        Instruction ins;
        ins.op = OpCode::SetTemp;
        ins.tempTarget = temp;
        ins.tempIsBed  = isBed;
        ins.tempWait   = wait;
        return ins;
    }

    static Instruction retract() {
        Instruction ins; ins.op = OpCode::Retract; return ins;
    }

    static Instruction unretract() {
        Instruction ins; ins.op = OpCode::Unretract; return ins;
    }

    static Instruction resetE() {
        Instruction ins; ins.op = OpCode::ResetE; return ins;
    }

    static Instruction pressureAdv(float val) {
        Instruction ins;
        ins.op = OpCode::PressureAdv;
        ins.pressureAdvValue = val;
        return ins;
    }

    static Instruction toolChange(int from, int to, float purgeVol = 60.0f,
                                   glm::vec2 offset = {0, 0}) {
        Instruction ins;
        ins.op = OpCode::ToolChange;
        ins.toolChangeFrom   = from;
        ins.toolChangeTo     = to;
        ins.purgeVolume      = purgeVol;
        ins.extruderOffset   = offset;
        ins.activeExtruder   = to;
        return ins;
    }

    static Instruction makeExtruderOffset(glm::vec2 offset) {
        Instruction ins;
        ins.op = OpCode::ExtruderOffset;
        ins.extruderOffset = offset;
        return ins;
    }
};

// ---------------------------------------------------------------------------
// The IR container: a flat list of instructions + metadata
// ---------------------------------------------------------------------------
struct Program {
    std::vector<Instruction> instructions;

    // Metadata
    std::string printerName;
    int         totalLayers   = 0;
    float       estimatedTime = 0;   // seconds (filled by TimingAnnotPass)
    float       totalFilament = 0;   // mm (filled by RetractPass/extrusion calc)

    // Per-layer statistics (indexed by layerIndex)
    struct LayerStats {
        float z          = 0;
        float height     = 0;
        float printTime  = 0;   // seconds
        float travelDist = 0;   // mm
        float extrudeDist= 0;   // mm
        int   retracts   = 0;
    };
    std::vector<LayerStats> layerStats;

    void reserve(size_t n) { instructions.reserve(n); }

    void push(Instruction ins) { instructions.push_back(std::move(ins)); }

    size_t size() const { return instructions.size(); }

    // Insert a list of instructions at position pos
    void insertAt(size_t pos, std::vector<Instruction> toInsert) {
        instructions.insert(instructions.begin() + pos,
                            toInsert.begin(), toInsert.end());
    }
};

// ---------------------------------------------------------------------------
// Pass base class
// ---------------------------------------------------------------------------
class Pass {
public:
    virtual ~Pass() = default;
    virtual const char* name() const = 0;
    // Run the pass on the program, modifying it in-place
    virtual void run(Program& prog) = 0;
    bool enabled = true;
};

// ---------------------------------------------------------------------------
// PostProcessPipeline: ordered chain of passes
// ---------------------------------------------------------------------------
class PostProcessPipeline {
public:
    // Add a pass to the end of the pipeline
    template<typename T, typename... Args>
    T* addPass(Args&&... args) {
        auto p = std::make_unique<T>(std::forward<Args>(args)...);
        T* ptr = p.get();
        m_passes.push_back(std::move(p));
        return ptr;
    }

    // Run all enabled passes in order
    void run(Program& prog) {
        for (auto& pass : m_passes) {
            if (pass->enabled)
                pass->run(prog);
        }
    }

    // Insert a pass at the front of the pipeline
    template<typename T, typename... Args>
    T* insertPassFront(Args&&... args) {
        auto p = std::make_unique<T>(std::forward<Args>(args)...);
        T* ptr = p.get();
        m_passes.insert(m_passes.begin(), std::move(p));
        return ptr;
    }

    // Insert a pass at a specific position (0-based index)
    template<typename T, typename... Args>
    T* insertPassAt(size_t idx, Args&&... args) {
        auto p = std::make_unique<T>(std::forward<Args>(args)...);
        T* ptr = p.get();
        if (idx >= m_passes.size())
            m_passes.push_back(std::move(p));
        else
            m_passes.insert(m_passes.begin() + idx, std::move(p));
        return ptr;
    }

    // Get pass by name (for runtime enable/disable)
    Pass* getPass(const char* name) {
        for (auto& p : m_passes)
            if (std::string(p->name()) == name)
                return p.get();
        return nullptr;
    }

    const std::vector<std::unique_ptr<Pass>>& passes() const { return m_passes; }

private:
    std::vector<std::unique_ptr<Pass>> m_passes;
};

} // namespace GcodeIR
