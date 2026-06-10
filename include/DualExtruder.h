#pragma once
// =============================================================================
// DualExtruder.h  —  Dual Extruder Support for FDM Slicer
// =============================================================================
//
// Architecture (inspired by BambuStudio ExtruderManager + PrintConfig):
//
//   ExtruderConfig        — per-extruder parameters (temp, material, nozzle, offset)
//   DualExtruderParams    — dual-head configuration and assignment strategy
//   ExtruderAssignment    — per-layer path-to-extruder assignment
//   PrimeTower            — wipe/prime tower geometry generator
//   ToolChangePlan        — planned tool changes for a SliceResult
//
// Extruder assignment strategies:
//   ByFeature   — T0: outer shell + solid; T1: support + infill (soluble support)
//   ByColor     — T0: model body; T1: accent color regions (multi-color)
//   ByMaterial  — T0: structural material; T1: soluble/flexible material
//   Manual      — user-defined per-layer assignment
//
// Prime Tower algorithm:
//   - Placed at user-defined position (default: near build plate corner)
//   - Each tool change prints N loops of the tower at current layer
//   - Tower shrinks inward each layer (constant outer radius, shrinking inner)
//   - Wipe path: spiral from outer to inner radius
//   - Tower is added to SliceLayer::primeTowerPaths before G-code export
//
// Tool change sequence (per BambuStudio toolchange.cpp):
//   1. Finish current feature
//   2. Retract current extruder
//   3. Move to prime tower position
//   4. Emit Tx command (T0 or T1)
//   5. Apply extruder offset (G92 or firmware OFFSET_EXTRUDER)
//   6. Heat new extruder to target temp (M109)
//   7. Prime tower: print N loops to purge old material
//   8. Unretract new extruder
//   9. Resume printing
//
// =============================================================================

#include "SlicerEngine.h"
#include <array>
#include <map>
#include <set>
#include <optional>

// ---------------------------------------------------------------------------
// Extruder index
// ---------------------------------------------------------------------------
enum class ExtruderIdx : uint8_t { T0 = 0, T1 = 1 };

inline const char* extruderName(ExtruderIdx e) {
    return (e == ExtruderIdx::T0) ? "T0" : "T1";
}

// ---------------------------------------------------------------------------
// ExtruderConfig — per-extruder parameters
// ---------------------------------------------------------------------------
struct ExtruderConfig {
    int   index              = 0;       // 0 = T0, 1 = T1

    // Nozzle
    float nozzleDiameter     = 0.4f;    // mm
    float extrusionWidth     = 0.45f;   // mm
    float outerExtrusionWidth= 0.40f;   // mm (narrower for quality)

    // Temperature
    float extruderTemp       = 200.0f;  // °C — printing temperature
    float standbyTemp        = 150.0f;  // °C — standby temperature (when not active)
    float firstLayerTemp     = 215.0f;  // °C — first layer temperature

    // Material
    std::string material     = "PLA";   // PLA / PETG / ABS / TPU / PVA / HIPS
    float filamentDiameter   = 1.75f;   // mm
    glm::vec3 color          = {1.0f, 1.0f, 1.0f};  // display color (RGB)

    // Retraction (per extruder — different materials need different settings)
    float retractionLength   = 1.0f;    // mm
    float retractionSpeed    = 45.0f;   // mm/s
    float unretractSpeed     = 45.0f;   // mm/s
    float retractionExtra    = 0.0f;    // mm — extra length on unretract

    // Extruder offset (relative to T0, which is the reference)
    // T0 offset is always (0, 0); T1 offset is the physical nozzle separation
    glm::vec2 offset         = {0.0f, 0.0f};  // mm (X, Y)

    // Fan
    int   minFanSpeed        = 50;      // 0-255
    int   maxFanSpeed        = 255;     // 0-255
    int   firstFanLayer      = 3;

    // Pressure advance
    float pressureAdvance    = 0.04f;

    // Cooling
    float minLayerTime       = 8.0f;    // s
};

// ---------------------------------------------------------------------------
// Assignment strategy: which extruder prints which features
// ---------------------------------------------------------------------------
enum class DualAssignStrategy : uint8_t {
    ByFeature   = 0,  // T0=model, T1=support (soluble support workflow)
    ByColor     = 1,  // T0=body color, T1=accent color (multi-color)
    ByMaterial  = 2,  // T0=structural, T1=flexible/soluble
    Manual      = 3,  // user-defined per-layer
};

// Per-feature extruder assignment (used by ByFeature strategy)
struct FeatureExtruderMap {
    ExtruderIdx outerShell      = ExtruderIdx::T0;
    ExtruderIdx innerShell      = ExtruderIdx::T0;
    ExtruderIdx infill          = ExtruderIdx::T0;
    ExtruderIdx solidFill       = ExtruderIdx::T0;
    ExtruderIdx bridge          = ExtruderIdx::T0;
    ExtruderIdx support         = ExtruderIdx::T1;  // T1 = soluble support
    ExtruderIdx supportIface    = ExtruderIdx::T1;
    ExtruderIdx skirt           = ExtruderIdx::T0;
    ExtruderIdx raft            = ExtruderIdx::T0;
    ExtruderIdx primeTower      = ExtruderIdx::T0;  // alternates per layer
};

// ---------------------------------------------------------------------------
// Prime Tower configuration
// ---------------------------------------------------------------------------
struct PrimeTowerConfig {
    bool  enabled            = true;
    float posX               = 180.0f;  // mm — tower center X (near build plate edge)
    float posY               = 180.0f;  // mm — tower center Y
    float outerRadius        = 12.0f;   // mm — outer radius
    float innerRadius        = 8.0f;    // mm — inner radius (wipe zone)
    int   loopsPerChange     = 2;       // loops to print per tool change
    float purgeVolume        = 60.0f;   // mm³ — target purge volume per change
    float wipeSpeed          = 60.0f;   // mm/s — speed for prime tower printing
    bool  enableWipeWall     = true;    // print a wipe wall around the tower
    float wipeWallDist       = 2.0f;    // mm — wipe wall offset from tower
};

// ---------------------------------------------------------------------------
// DualExtruderParams — top-level dual extruder configuration
// ---------------------------------------------------------------------------
struct DualExtruderParams {
    bool  enabled            = false;
    DualAssignStrategy strategy = DualAssignStrategy::ByFeature;

    std::array<ExtruderConfig, 2> extruders;  // [0]=T0, [1]=T1

    FeatureExtruderMap featureMap;  // used when strategy == ByFeature

    PrimeTowerConfig primeTower;

    // Tool change behavior
    bool  heatStandbyExtruder  = true;   // keep non-active extruder at standby temp
    float toolChangeTravelSpeed= 200.0f; // mm/s — travel to prime tower
    bool  applyOffsetInFirmware= true;   // use firmware offset (vs G92 compensation)
    bool  enableToolChangeFan  = true;   // full fan during tool change cooling
    float toolChangeCoolTime   = 2.0f;   // s — dwell after tool change

    // Constructor: set sensible defaults for T0/T1
    DualExtruderParams() {
        extruders[0].index  = 0;
        extruders[0].color  = {0.9f, 0.9f, 0.9f};  // white
        extruders[0].material = "PLA";

        extruders[1].index  = 1;
        extruders[1].color  = {0.2f, 0.6f, 1.0f};  // blue
        extruders[1].material = "PVA";              // soluble support default
        extruders[1].extruderTemp = 185.0f;
        extruders[1].standbyTemp  = 140.0f;
        extruders[1].retractionLength = 1.5f;       // PVA needs more retraction
        extruders[1].offset = {18.0f, 0.0f};        // 18mm X offset (typical)
    }

    const ExtruderConfig& get(ExtruderIdx e) const {
        return extruders[(int)e];
    }
    ExtruderConfig& get(ExtruderIdx e) {
        return extruders[(int)e];
    }
};

// ---------------------------------------------------------------------------
// Per-layer extruder assignment result
// ---------------------------------------------------------------------------
struct LayerExtruderAssignment {
    int layerIndex = 0;

    // Which extruder prints each path set
    ExtruderIdx shellExtruder   = ExtruderIdx::T0;
    ExtruderIdx infillExtruder  = ExtruderIdx::T0;
    ExtruderIdx supportExtruder = ExtruderIdx::T1;
    ExtruderIdx raftExtruder    = ExtruderIdx::T0;
    ExtruderIdx skirtExtruder   = ExtruderIdx::T0;

    // Tool changes needed in this layer (ordered by print sequence)
    struct ToolChange {
        ExtruderIdx from;
        ExtruderIdx to;
        bool        needsPrimeTower = true;
        float       purgeVolume     = 60.0f;  // mm³
    };
    std::vector<ToolChange> toolChanges;

    bool hasToolChange() const { return !toolChanges.empty(); }
};

// ---------------------------------------------------------------------------
// Prime Tower geometry generator
// ---------------------------------------------------------------------------
class PrimeTowerGenerator {
public:
    // Generate prime tower loops for a single layer
    // Returns a list of closed loops (outer → inner spiral)
    static std::vector<Loop2> generateLayer(
        const PrimeTowerConfig& cfg,
        int   layerIndex,
        float layerHeight,
        float extrusionWidth,
        ExtruderIdx activeExtruder)
    {
        std::vector<Loop2> loops;
        if (!cfg.enabled) return loops;

        // Alternate between T0 and T1 fill direction each layer
        // (like BambuStudio prime tower alternating pattern)
        bool alternate = (layerIndex % 2 == 0);

        // Generate concentric loops from outer to inner
        float r = cfg.outerRadius;
        while (r > cfg.innerRadius + extrusionWidth * 0.5f) {
            Loop2 loop = makeCircleLoop(cfg.posX, cfg.posY, r, 32);
            loops.push_back(loop);
            r -= extrusionWidth * 1.05f;  // slight overlap for adhesion
        }

        // Add a wipe spiral at the innermost radius
        Loop2 wipe = makeCircleLoop(cfg.posX, cfg.posY, cfg.innerRadius, 16);
        loops.push_back(wipe);

        return loops;
    }

    // Generate a wipe wall (single loop around the tower)
    static Loop2 generateWipeWall(const PrimeTowerConfig& cfg) {
        return makeCircleLoop(cfg.posX, cfg.posY,
                              cfg.outerRadius + cfg.wipeWallDist, 32);
    }

    // Compute purge volume for a given number of loops
    static float computePurgeVolume(const PrimeTowerConfig& cfg,
                                     float layerHeight, float extrusionWidth)
    {
        float totalLen = 0;
        float r = cfg.outerRadius;
        while (r > cfg.innerRadius + extrusionWidth * 0.5f) {
            totalLen += 2.0f * (float)M_PI * r;
            r -= extrusionWidth * 1.05f;
        }
        float filamentArea = (float)M_PI * (1.75f * 0.5f) * (1.75f * 0.5f);
        return totalLen * extrusionWidth * layerHeight / filamentArea;
    }

private:
    static Loop2 makeCircleLoop(float cx, float cy, float r, int segs) {
        Loop2 loop;
        loop.reserve(segs);
        for (int i = 0; i < segs; ++i) {
            float a = 2.0f * (float)M_PI * i / segs;
            loop.push_back({cx + r * std::cos(a), cy + r * std::sin(a)});
        }
        return loop;
    }
};

// ---------------------------------------------------------------------------
// DualExtruderPlanner — assigns extruders to layers and plans tool changes
// ---------------------------------------------------------------------------
class DualExtruderPlanner {
public:
    // Plan extruder assignments for all layers in a SliceResult
    static std::vector<LayerExtruderAssignment> plan(
        const SliceResult&        result,
        const DualExtruderParams& dualParams)
    {
        std::vector<LayerExtruderAssignment> assignments;
        if (!dualParams.enabled) return assignments;

        assignments.resize(result.layers.size());

        ExtruderIdx prevActive = ExtruderIdx::T0;

        for (size_t li = 0; li < result.layers.size(); ++li) {
            auto& layer = result.layers[li];
            auto& asgn  = assignments[li];
            asgn.layerIndex = layer.index;

            // Determine per-feature extruder based on strategy
            switch (dualParams.strategy) {
            case DualAssignStrategy::ByFeature:
                assignByFeature(layer, dualParams.featureMap, asgn);
                break;
            case DualAssignStrategy::ByColor:
                // Simplified: T0 for all model, T1 for support
                asgn.shellExtruder   = ExtruderIdx::T0;
                asgn.infillExtruder  = ExtruderIdx::T0;
                asgn.supportExtruder = ExtruderIdx::T1;
                asgn.raftExtruder    = ExtruderIdx::T0;
                asgn.skirtExtruder   = ExtruderIdx::T0;
                break;
            default:
                asgn.shellExtruder   = ExtruderIdx::T0;
                asgn.infillExtruder  = ExtruderIdx::T0;
                asgn.supportExtruder = ExtruderIdx::T1;
                asgn.raftExtruder    = ExtruderIdx::T0;
                asgn.skirtExtruder   = ExtruderIdx::T0;
                break;
            }

            // Detect tool changes needed in this layer
            // Print order: skirt → support → supportIface → innerShell →
            //              outerShell → solid → bridge → infill
            std::vector<ExtruderIdx> printOrder;

            if (!layer.skirtLoops.empty())
                printOrder.push_back(asgn.skirtExtruder);
            if (!layer.supportPaths.empty() || !layer.supportInterfacePaths.empty())
                printOrder.push_back(asgn.supportExtruder);
            if (!layer.shells.empty())
                printOrder.push_back(asgn.shellExtruder);
            if (!layer.solidPaths.empty() || !layer.bridgePaths.empty() ||
                !layer.infillPaths.empty())
                printOrder.push_back(asgn.infillExtruder);

            // Deduplicate consecutive same-extruder entries
            ExtruderIdx cur = prevActive;
            for (auto e : printOrder) {
                if (e != cur) {
                    LayerExtruderAssignment::ToolChange tc;
                    tc.from            = cur;
                    tc.to              = e;
                    tc.needsPrimeTower = dualParams.primeTower.enabled;
                    tc.purgeVolume     = dualParams.primeTower.purgeVolume;
                    asgn.toolChanges.push_back(tc);
                    cur = e;
                }
            }

            prevActive = cur;
        }

        return assignments;
    }

    // Inject prime tower paths into SliceLayer based on assignment
    static void injectPrimeTower(
        SliceResult&              result,
        const DualExtruderParams& dualParams,
        const std::vector<LayerExtruderAssignment>& assignments)
    {
        if (!dualParams.primeTower.enabled) return;

        const auto& cfg = dualParams.primeTower;
        const float lh  = result.params.layerHeight;
        const float ew  = result.params.extrusionWidth;

        for (size_t li = 0; li < result.layers.size(); ++li) {
            if (li >= assignments.size()) break;
            auto& layer = result.layers[li];
            auto& asgn  = assignments[li];

            if (!asgn.hasToolChange()) continue;

            // Generate prime tower loops for this layer
            // Active extruder = the one that will print the tower
            // (alternates: T0 on even layers, T1 on odd layers)
            ExtruderIdx towerExtruder =
                (layer.index % 2 == 0) ? ExtruderIdx::T0 : ExtruderIdx::T1;

            auto loops = PrimeTowerGenerator::generateLayer(
                cfg, layer.index, lh, ew, towerExtruder);

            // Store in primeTowerPaths (converted to Path2 for renderer)
            for (auto& loop : loops) {
                Path2 p(loop.begin(), loop.end());
                p.push_back(loop[0]);  // close the loop
                layer.primeTowerPaths.push_back(p);
            }

            // Wipe wall
            if (cfg.enableWipeWall) {
                Loop2 wall = PrimeTowerGenerator::generateWipeWall(cfg);
                Path2 p(wall.begin(), wall.end());
                p.push_back(wall[0]);
                layer.primeTowerPaths.push_back(p);
            }
        }
    }

private:
    static void assignByFeature(const SliceLayer& layer,
                                  const FeatureExtruderMap& fmap,
                                  LayerExtruderAssignment& asgn)
    {
        asgn.shellExtruder   = fmap.outerShell;
        asgn.infillExtruder  = fmap.infill;
        asgn.supportExtruder = fmap.support;
        asgn.raftExtruder    = fmap.raft;
        asgn.skirtExtruder   = fmap.skirt;
    }
};

// ---------------------------------------------------------------------------
// Extend SliceLayer with prime tower paths
// (Added here to avoid modifying SlicerEngine.h header guard)
// ---------------------------------------------------------------------------
// NOTE: We add primeTowerPaths to SliceLayer via this extension pattern.
// In SlicerEngine.h, add:
//   std::vector<Path2> primeTowerPaths;
//   ExtruderIdx        activeExtruder = ExtruderIdx::T0;
// This is done by the patch below.
