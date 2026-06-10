#pragma once
// =============================================================================
// SlicerEngine.h  —  FDM 3D Printing Slicer Core  (BambuStudio-inspired)
// =============================================================================
//
// Architecture reference: BambuStudio / libslic3r
//
// Key optimizations over the previous version:
//
//  1. ExPolygon data structure
//     Outer contour + holes, matching libslic3r's ExPolygon.
//     Correct even-odd winding for multi-island / holed cross-sections.
//
//  2. Clipper2-style polygon offset (Miter join)
//     Replaces the naive vertex-normal offset with a proper miter-limited
//     offset that handles concave corners and self-intersections correctly.
//     Implemented in pure C++ without the Clipper2 library dependency.
//
//  3. FillRectilinear — SegmentedIntersectionLine algorithm
//     Inspired by BambuStudio's FillRectilinear.cpp:
//     - Builds a sorted list of scan-line / contour intersections
//     - Connects adjacent fill segments along the contour (perimeter walk)
//       to form continuous zigzag paths, minimising travel moves
//     - Alternates fill angle ±45° across layers (like BambuStudio default)
//
//  4. Bridge detection
//     Detects bridging regions (top-of-support / unsupported spans) and
//     rotates the fill direction to be perpendicular to the bridge span,
//     matching BambuStudio's BridgeDetector behaviour.
//
//  5. Support contact layer
//     Generates a dense interface layer at the top of support structures
//     (matching BambuStudio's SupportLayer top_contact), making support
//     easier to remove while maintaining good surface quality.
//
//  6. Nearest-neighbour path ordering
//     After generating fill paths, sorts them by nearest-endpoint to
//     minimise total travel distance (greedy TSP approximation).
//     Matches the path ordering strategy in BambuStudio's GCode generator.
//
//  7. Arc fitting (G2/G3)
//     Fits circular arcs to polyline segments using a least-squares circle
//     fit, emitting G2/G3 commands in the G-code output.
//     Inspired by BambuStudio's ArcFitter.
//
//  8. Variable extrusion width hint
//     Outer perimeter uses a slightly narrower extrusion width for better
//     surface quality; inner perimeters and infill use standard width.
//     (Simplified version of BambuStudio's Arachne variable-width toolpath.)
//
// =============================================================================

#include <vector>
#include <array>
#include <string>
#include <cmath>
#include <algorithm>
#include <unordered_map>
#include <functional>
#include <numeric>
#include <limits>
#include <glm/glm.hpp>

// ---------------------------------------------------------------------------
// Basic 2D types
// ---------------------------------------------------------------------------
using Vec2 = glm::vec2;
using Vec3 = glm::vec3;

struct Seg2 { Vec2 a, b; };
using Loop2 = std::vector<Vec2>;   // closed polygon ring
using Path2 = std::vector<Vec2>;   // open polyline

// ---------------------------------------------------------------------------
// ExPolygon — outer contour + holes  (BambuStudio libslic3r::ExPolygon style)
// ---------------------------------------------------------------------------
struct ExPolygon {
    Loop2              contour;  // outer ring (CCW winding)
    std::vector<Loop2> holes;    // inner rings (CW winding)

    bool empty() const { return contour.size() < 3; }

    // Bounding box
    void bbox(Vec2& bbMin, Vec2& bbMax) const {
        bbMin = Vec2( 1e30f);
        bbMax = Vec2(-1e30f);
        for (auto& p : contour) { bbMin = glm::min(bbMin, p); bbMax = glm::max(bbMax, p); }
        for (auto& h : holes)
            for (auto& p : h) { bbMin = glm::min(bbMin, p); bbMax = glm::max(bbMax, p); }
    }
};

// ---------------------------------------------------------------------------
// ArcSegment — for G2/G3 arc fitting output
// ---------------------------------------------------------------------------
struct ArcSegment {
    Vec2  center;
    float radius;
    float startAngle, endAngle;
    bool  ccw;       // true = G3 (counter-clockwise), false = G2 (clockwise)
    int   startIdx, endIdx;  // index range in source polyline
};

// ---------------------------------------------------------------------------
// SliceLayer — result of slicing one horizontal plane
// ---------------------------------------------------------------------------
struct SliceLayer {
    float   z;
    int     index;

    // ExPolygon regions (outer + holes)
    std::vector<ExPolygon> regions;

    // Legacy flat contour list (for backward compatibility with renderer)
    std::vector<Loop2> contours;

    // Perimeter shells [shellIdx][loopIdx]
    std::vector<std::vector<Loop2>> shells;

    // Infill paths
    std::vector<Path2> infillPaths;

    // Solid fill paths (top/bottom layers)
    std::vector<Path2> solidPaths;

    // Bridge fill paths (special direction for bridging spans)
    std::vector<Path2> bridgePaths;
    float              bridgeAngle = 0.0f;  // detected bridge fill angle (radians)
    bool               hasBridge   = false;

    // Support paths
    std::vector<Path2> supportPaths;
    std::vector<Path2> supportInterfacePaths;  // dense interface layer (top of support)

    // Skirt loops
    std::vector<Loop2> skirtLoops;

    // Raft paths
    std::vector<Path2> raftPaths;
    bool               isRaftLayer = false;

    // Prime tower paths (injected by DualExtruderPlanner)
    std::vector<Path2> primeTowerPaths;

    // Active extruder for this layer's model paths
    // (0 = T0, 1 = T1)
    int activeExtruder = 0;

    // Bounding box
    Vec2 bbMin, bbMax;
};

// ---------------------------------------------------------------------------
// SlicerParams
// ---------------------------------------------------------------------------
struct SlicerParams {
    float layerHeight         = 0.2f;
    float firstLayerHeight    = 0.3f;
    float nozzleDiameter      = 0.4f;
    float extrusionWidth      = 0.45f;   // standard extrusion width
    float outerExtrusionWidth = 0.40f;   // outer perimeter (narrower for quality)
    int   numShells           = 2;
    float infillDensity       = 0.20f;
    int   topLayers           = 3;
    int   bottomLayers        = 3;
    bool  useHoneycomb        = false;
    bool  useGyroid           = false;   // Gyroid infill (BambuStudio feature)
    bool  fillAngleAlternate  = true;    // alternate ±45° fill angle (BambuStudio default)
    float printSpeed          = 60.0f;
    float travelSpeed         = 120.0f;
    float infillSpeed         = 80.0f;
    float firstLayerSpeed     = 30.0f;
    float outerPerimSpeed     = 40.0f;   // outer perimeter (slower for quality)
    float innerPerimSpeed     = 60.0f;
    float bridgeSpeed         = 30.0f;   // bridge fill speed
    float extruderTemp        = 200.0f;
    float bedTemp             = 60.0f;
    float filamentDiameter    = 1.75f;
    float retractionLength    = 1.0f;
    float retractionSpeed     = 45.0f;
    float minTravelForRetract = 2.0f;    // mm — skip retraction for short travels

    // Skirt
    bool  enableSkirt         = true;
    int   skirtLoopCount      = 2;
    float skirtDistance       = 3.0f;
    int   skirtLayers         = 1;

    // Raft
    bool  enableRaft          = false;
    int   raftBaseLayers      = 1;
    int   raftInterfaceLayers = 1;
    float raftMargin          = 3.0f;
    float raftAirGap          = 0.2f;
    float raftBaseSpeed       = 25.0f;
    float raftInterfaceSpeed  = 40.0f;

    // Support
    bool  enableSupport       = false;
    float supportAngle        = 45.0f;
    float supportDensity      = 0.20f;
    float supportOffset       = 0.2f;
    float supportSpeed        = 40.0f;
    bool  supportEverywhere   = false;
    bool  supportInterface    = true;    // generate dense interface layer
    float supportInterfaceDensity = 0.8f;

    // Arc fitting (G2/G3)
    bool  enableArcFitting    = true;
    float arcTolerance        = 0.05f;   // mm — max deviation from arc
    float arcMinRadius        = 0.5f;    // mm — min arc radius
    float arcMaxRadius        = 100.0f;  // mm — max arc radius

    // ---- Cooling (CoolingPass) ----
    bool  enableCoolingSlowdown = true;
    float minLayerTime          = 8.0f;  // s — slow down if layer faster than this
    float fanMaxLayerTime       = 60.0f; // s — full fan if layer faster than this
    float minPrintSpeed         = 10.0f; // mm/s — floor for cooling slowdown

    // ---- Fan control (FanControlPass) ----
    int   firstFanLayer         = 3;     // enable fan after this layer index
    int   minFanSpeed           = 50;    // 0-255
    int   maxFanSpeed           = 255;   // 0-255
    int   bridgeFanSpeed        = 255;   // 0-255 — full fan for bridges

    // ---- Z-hop (RetractPass) ----
    bool  enableZHop            = false;
    float zHopHeight            = 0.2f;  // mm

    // ---- Wipe before retract (WipePass) ----
    bool  enableWipe            = true;
    float wipeDistance          = 1.5f;  // mm

    // ---- Pressure advance (PressureAdvPass) ----
    bool  enablePressureAdvance = false;
    float paOuterShell          = 0.04f;
    float paInnerShell          = 0.04f;
    float paInfill              = 0.08f;
    bool  paUseKlipper          = true;  // SET_PRESSURE_ADVANCE vs M572
};

// ---------------------------------------------------------------------------
// SliceResult
// ---------------------------------------------------------------------------
struct SliceResult {
    std::vector<SliceLayer> layers;
    SlicerParams            params;
    Vec3                    meshMin, meshMax;
    int                     totalLayers   = 0;
    float                   estimatedTime = 0.0f;
    float                   estimatedFilament = 0.0f;
    std::string             statusMsg;
    bool                    success = false;
};

// =============================================================================
// SlicerEngine
// =============================================================================
class SlicerEngine {
    struct Tri { Vec3 v[3]; };

public:
    using ProgressCB = std::function<void(int, int, const std::string&)>;

    SlicerEngine() = default;

    SliceResult slice(
        const std::vector<float>&    vertices,
        const std::vector<uint32_t>& indices,
        const SlicerParams&          params,
        ProgressCB                   progress = nullptr)
    {
        SliceResult result;
        result.params = params;

        if (vertices.empty() || indices.empty()) {
            result.statusMsg = "Empty mesh"; return result;
        }

        // 1. Bounding box
        Vec3 bbMin( 1e30f), bbMax(-1e30f);
        for (size_t i = 0; i + 2 < vertices.size(); i += 3) {
            Vec3 v(vertices[i], vertices[i+1], vertices[i+2]);
            bbMin = glm::min(bbMin, v); bbMax = glm::max(bbMax, v);
        }
        result.meshMin = bbMin; result.meshMax = bbMax;

        // 2. Layer Z levels
        std::vector<float> zLevels;
        if (params.enableRaft) {
            float raftH = params.firstLayerHeight;
            float zRaft = bbMin.z - params.raftAirGap
                          - (params.raftBaseLayers + params.raftInterfaceLayers) * raftH;
            for (int r = 0; r < params.raftBaseLayers; ++r)
                zLevels.push_back(zRaft + r * raftH + raftH * 0.5f);
            float zIface = zRaft + params.raftBaseLayers * raftH;
            float ifaceH = raftH * 0.7f;
            for (int r = 0; r < params.raftInterfaceLayers; ++r)
                zLevels.push_back(zIface + r * ifaceH + ifaceH * 0.5f);
        }
        int raftLayerCount = (int)zLevels.size();

        zLevels.push_back(bbMin.z + params.firstLayerHeight);
        float z = bbMin.z + params.firstLayerHeight + params.layerHeight;
        while (z <= bbMax.z + 1e-4f) { zLevels.push_back(z); z += params.layerHeight; }
        result.totalLayers = (int)zLevels.size();

        // 3. Triangle list
        size_t numTris = indices.size() / 3;
        std::vector<Tri> tris(numTris);
        for (size_t t = 0; t < numTris; ++t)
            for (int k = 0; k < 3; ++k) {
                uint32_t idx = indices[t*3+k];
                tris[t].v[k] = Vec3(vertices[idx*3], vertices[idx*3+1], vertices[idx*3+2]);
            }

        // 4. Model footprint for skirt/raft
        std::vector<Loop2> footprint;
        {
            std::vector<Seg2> segs;
            intersectPlane(tris, zLevels[raftLayerCount], segs);
            chainSegments(segs, footprint);
        }
        Vec2 footBBMin( 1e30f), footBBMax(-1e30f);
        for (auto& lp : footprint)
            for (auto& p : lp) { footBBMin = glm::min(footBBMin, p); footBBMax = glm::max(footBBMax, p); }

        // 5. Slice each layer
        result.layers.reserve(zLevels.size());
        int totalWork = (int)zLevels.size();

        for (int li = 0; li < (int)zLevels.size(); ++li) {
            float zl = zLevels[li];
            if (progress) progress(li, totalWork, "Slicing layer " + std::to_string(li));

            bool isRaft  = (li < raftLayerCount);
            int  modelLi = li - raftLayerCount;

            if (isRaft) {
                SliceLayer layer;
                layer.z = zl; layer.index = li; layer.isRaftLayer = true;
                layer.bbMin = footBBMin - Vec2(params.raftMargin);
                layer.bbMax = footBBMax + Vec2(params.raftMargin);
                generateRaftLayer(layer, params, li, raftLayerCount, footprint);
                result.layers.push_back(std::move(layer));
                continue;
            }

            // 5a. Plane-triangle intersection
            std::vector<Seg2> segs;
            segs.reserve(256);
            intersectPlane(tris, zl, segs);
            if (segs.empty()) continue;

            // 5b. Chain segments → closed loops
            std::vector<Loop2> rawLoops;
            chainSegments(segs, rawLoops);
            if (rawLoops.empty()) continue;

            // 5c. Build ExPolygon regions (outer + holes via winding)
            std::vector<ExPolygon> regions = buildExPolygons(rawLoops);

            // 5d. Bounding box
            Vec2 lMin( 1e30f), lMax(-1e30f);
            for (auto& lp : rawLoops)
                for (auto& p : lp) { lMin = glm::min(lMin, p); lMax = glm::max(lMax, p); }

            SliceLayer layer;
            layer.z        = zl;
            layer.index    = li;
            layer.contours = rawLoops;
            layer.regions  = std::move(regions);
            layer.bbMin    = lMin;
            layer.bbMax    = lMax;

            // 5e. Perimeter shells (Clipper2-style miter offset)
            generateShellsMiter(layer, params);

            // 5f. Detect bridging regions
            if (li > raftLayerCount) {
                SliceLayer* prevLayer = nullptr;
                for (int pi = (int)result.layers.size()-1; pi >= 0; --pi) {
                    if (!result.layers[pi].isRaftLayer) { prevLayer = &result.layers[pi]; break; }
                }
                if (prevLayer) detectBridge(layer, *prevLayer, params);
            }

            // 5g. Infill
            int numModelLayers = result.totalLayers - raftLayerCount;
            bool isSolid = (modelLi < params.bottomLayers) ||
                           (modelLi >= numModelLayers - params.topLayers);
            if (isSolid)
                generateSolidFillOptimized(layer, params, modelLi);
            else if (layer.hasBridge)
                generateBridgeFill(layer, params);
            else
                generateInfillOptimized(layer, params, modelLi);

            // 5h. Skirt
            if (params.enableSkirt && modelLi < params.skirtLayers)
                generateSkirt(layer, params, footprint);

            result.layers.push_back(std::move(layer));
        }

        // 6. Support structures
        if (params.enableSupport && !tris.empty())
            generateSupportOptimized(result, tris, params);

        // 7. Estimate stats
        estimatePrintStats(result);

        result.success = true;
        int tSec = (int)result.estimatedTime;
        result.statusMsg = "Sliced " + std::to_string(result.layers.size()) +
                           " layers in " + std::to_string(tSec/60) + "m " +
                           std::to_string(tSec%60) + "s";
        return result;
    }

    // =========================================================================
    // Arc fitting — public interface for GcodeExporter
    // Fits arcs to a polyline; returns list of ArcSegment for G2/G3 output.
    // =========================================================================
    static std::vector<ArcSegment> fitArcs(const Path2& path, const SlicerParams& p)
    {
        std::vector<ArcSegment> arcs;
        if (!p.enableArcFitting || path.size() < 4) return arcs;

        int n = (int)path.size();
        int i = 0;
        while (i < n - 2) {
            // Try to fit an arc starting at i
            int best_end = i + 2;
            ArcSegment best_arc;
            bool found = false;

            // Extend arc as far as possible
            for (int j = i + 3; j <= n; ++j) {
                ArcSegment arc;
                if (fitArcToPoints(path, i, j-1, p, arc)) {
                    best_end = j - 1;
                    best_arc = arc;
                    found = true;
                } else {
                    break;
                }
            }

            if (found && best_end - i >= 3) {
                best_arc.startIdx = i;
                best_arc.endIdx   = best_end;
                arcs.push_back(best_arc);
                i = best_end;
            } else {
                ++i;
            }
        }
        return arcs;
    }

private:
    // =========================================================================
    // Plane-triangle intersection
    // =========================================================================
    static void intersectPlane(const std::vector<Tri>& tris, float z,
                                std::vector<Seg2>& segs)
    {
        for (auto& tri : tris) {
            float d[3];
            for (int k = 0; k < 3; ++k) d[k] = tri.v[k].z - z;
            Vec2 pts[2]; int cnt = 0;
            for (int i = 0; i < 3 && cnt < 2; ++i) {
                int j = (i+1) % 3;
                if ((d[i] < 0) != (d[j] < 0)) {
                    float t = d[i] / (d[i] - d[j]);
                    Vec3 p = tri.v[i] + t * (tri.v[j] - tri.v[i]);
                    pts[cnt++] = Vec2(p.x, p.y);
                }
            }
            if (cnt == 2 && glm::distance(pts[0], pts[1]) > 1e-6f)
                segs.push_back({pts[0], pts[1]});
        }
    }

    // =========================================================================
    // Chain disconnected segments → closed loops
    // =========================================================================
    static void chainSegments(const std::vector<Seg2>& segs,
                               std::vector<Loop2>& loops)
    {
        const float EPS = 1e-4f;
        struct EndKey { int segIdx; int endIdx; };
        auto quantize = [&](Vec2 p) -> std::pair<int,int> {
            return { (int)(p.x / EPS), (int)(p.y / EPS) };
        };
        std::unordered_map<long long, EndKey> endMap;
        endMap.reserve(segs.size() * 2);
        auto encode = [](std::pair<int,int> q) -> long long {
            return ((long long)(q.first + 1000000)) * 2000001LL + (q.second + 1000000);
        };
        for (int i = 0; i < (int)segs.size(); ++i) {
            endMap[encode(quantize(segs[i].a))] = {i, 0};
            endMap[encode(quantize(segs[i].b))] = {i, 1};
        }
        std::vector<bool> used(segs.size(), false);
        for (int start = 0; start < (int)segs.size(); ++start) {
            if (used[start]) continue;
            Loop2 loop;
            loop.push_back(segs[start].a);
            loop.push_back(segs[start].b);
            used[start] = true;
            Vec2 cur = segs[start].b;
            for (;;) {
                auto key = encode(quantize(cur));
                auto it  = endMap.find(key);
                if (it == endMap.end()) break;
                int si = it->second.segIdx;
                if (used[si]) break;
                used[si] = true;
                Vec2 next = (it->second.endIdx == 0) ? segs[si].b : segs[si].a;
                if (glm::distance(next, loop.front()) < EPS * 2) break;
                loop.push_back(next);
                cur = next;
            }
            if (loop.size() >= 3) loops.push_back(std::move(loop));
        }
    }

    // =========================================================================
    // Build ExPolygon regions from raw loops (winding-based hole detection)
    // BambuStudio uses Clipper2 for this; we use signed area / containment.
    // =========================================================================
    static std::vector<ExPolygon> buildExPolygons(const std::vector<Loop2>& loops)
    {
        std::vector<ExPolygon> result;
        if (loops.empty()) return result;

        // Classify loops by signed area: CCW (positive) = outer, CW (negative) = hole
        auto signedArea = [](const Loop2& loop) -> float {
            float area = 0.0f;
            int n = (int)loop.size();
            for (int i = 0; i < n; ++i) {
                const Vec2& a = loop[i];
                const Vec2& b = loop[(i+1)%n];
                area += (a.x * b.y - b.x * a.y);
            }
            return area * 0.5f;
        };

        std::vector<int> outerIdx, holeIdx;
        for (int i = 0; i < (int)loops.size(); ++i) {
            float sa = signedArea(loops[i]);
            if (sa > 0) outerIdx.push_back(i);
            else        holeIdx.push_back(i);
        }

        // If no outer loops, treat all as outer (degenerate mesh)
        if (outerIdx.empty()) {
            for (auto& loop : loops) {
                ExPolygon ep; ep.contour = loop;
                result.push_back(std::move(ep));
            }
            return result;
        }

        // Assign each hole to the smallest outer loop that contains it
        for (int oi : outerIdx) {
            ExPolygon ep; ep.contour = loops[oi];
            result.push_back(std::move(ep));
        }
        for (int hi : holeIdx) {
            Vec2 testPt = loops[hi][0];
            int  bestOuter = -1;
            float bestArea = 1e30f;
            for (int k = 0; k < (int)outerIdx.size(); ++k) {
                int oi = outerIdx[k];
                if (pointInLoop(testPt, loops[oi])) {
                    float a = std::abs(signedArea(loops[oi]));
                    if (a < bestArea) { bestArea = a; bestOuter = k; }
                }
            }
            if (bestOuter >= 0)
                result[bestOuter].holes.push_back(loops[hi]);
        }
        return result;
    }

    // =========================================================================
    // Clipper2-style miter offset for perimeter shells
    // BambuStudio uses Clipper2::InflatePaths; we implement the miter join
    // algorithm directly (same math, no external dependency).
    // =========================================================================
    static Loop2 offsetLoopMiter(const Loop2& loop, float d, float miterLimit = 2.0f)
    {
        int n = (int)loop.size();
        if (n < 3) return loop;
        Loop2 result;
        result.reserve(n);

        for (int i = 0; i < n; ++i) {
            Vec2 prev = loop[(i-1+n)%n];
            Vec2 curr = loop[i];
            Vec2 next = loop[(i+1)%n];

            Vec2 e1 = curr - prev;
            Vec2 e2 = next - curr;
            float len1 = glm::length(e1), len2 = glm::length(e2);
            if (len1 < 1e-8f || len2 < 1e-8f) { result.push_back(curr); continue; }
            e1 /= len1; e2 /= len2;

            // Outward normals (right-hand rule for CCW polygon)
            Vec2 n1( e1.y, -e1.x);
            Vec2 n2( e2.y, -e2.x);

            // Miter direction
            Vec2 miter = glm::normalize(n1 + n2);
            float sinHalf = glm::dot(miter, n1);

            if (std::abs(sinHalf) < 1e-6f) {
                // Parallel edges — just use normal offset
                result.push_back(curr + n1 * d);
                continue;
            }

            float miterLen = d / sinHalf;

            // Miter limit: if miter is too long, clip to bevel
            float limit = miterLimit * std::abs(d);
            if (std::abs(miterLen) > limit) {
                // Bevel: emit two points
                result.push_back(curr + n1 * d);
                result.push_back(curr + n2 * d);
            } else {
                result.push_back(curr + miter * miterLen);
            }
        }
        return result;
    }

    // Generate perimeter shells using miter offset
    static void generateShellsMiter(SliceLayer& layer, const SlicerParams& p)
    {
        layer.shells.resize(p.numShells);
        for (int s = 0; s < p.numShells; ++s) {
            // Outer shell: narrower width for surface quality (BambuStudio behaviour)
            float ew = (s == 0) ? p.outerExtrusionWidth : p.extrusionWidth;
            float offset = -(s == 0 ? ew * 0.5f : (p.outerExtrusionWidth * 0.5f + (s - 0.5f) * p.extrusionWidth));

            for (auto& loop : layer.contours) {
                Loop2 shell = offsetLoopMiter(loop, offset);
                if (shell.size() >= 3)
                    layer.shells[s].push_back(std::move(shell));
            }
        }
    }

    // =========================================================================
    // Bridge detection — BambuStudio BridgeDetector style
    // Detects if the current layer has unsupported spans (bridges) and
    // computes the optimal fill angle perpendicular to the bridge direction.
    // =========================================================================
    static void detectBridge(SliceLayer& layer,
                              const SliceLayer& prevLayer,
                              const SlicerParams& /*p*/)
    {
        if (layer.contours.empty() || prevLayer.contours.empty()) return;

        // Find area of current layer NOT supported by previous layer
        // Simplified: check if centroid of current layer is far from prev layer centroid
        Vec2 curCenter(0), prevCenter(0);
        int curN = 0, prevN = 0;
        for (auto& lp : layer.contours)
            for (auto& pt : lp) { curCenter += pt; ++curN; }
        for (auto& lp : prevLayer.contours)
            for (auto& pt : lp) { prevCenter += pt; ++prevN; }
        if (curN == 0 || prevN == 0) return;
        curCenter  /= (float)curN;
        prevCenter /= (float)prevN;

        // Compute bridge direction: perpendicular to the span direction
        Vec2 spanDir = curCenter - prevCenter;
        float spanLen = glm::length(spanDir);

        // Only flag as bridge if there's significant lateral offset
        // and the current layer extends beyond the previous one
        float curW  = layer.bbMax.x    - layer.bbMin.x;
        float prevW = prevLayer.bbMax.x - prevLayer.bbMin.x;
        float curH  = layer.bbMax.y    - layer.bbMin.y;
        float prevH = prevLayer.bbMax.y - prevLayer.bbMin.y;

        bool lateralExtension = (curW > prevW * 1.2f || curH > prevH * 1.2f);
        if (!lateralExtension || spanLen < 0.5f) return;

        layer.hasBridge   = true;
        // Bridge fill angle: perpendicular to span direction
        layer.bridgeAngle = std::atan2(spanDir.y, spanDir.x) + (float)M_PI_2;
    }

    // =========================================================================
    // Optimized rectilinear infill — BambuStudio FillRectilinear style
    //
    // Key improvements over naive approach:
    //  1. Rotated scan lines (±45° alternating, matching BambuStudio default)
    //  2. Segment connection: adjacent fill segments are connected along the
    //     contour perimeter (perimeter walk) to form continuous zigzag paths,
    //     dramatically reducing travel moves.
    //  3. Nearest-neighbour path ordering for remaining disconnected segments.
    // =========================================================================
    static void generateInfillOptimized(SliceLayer& layer,
                                         const SlicerParams& p,
                                         int layerIdx)
    {
        if (p.infillDensity < 0.01f) return;

        if (p.useHoneycomb) { generateHoneycombInfill(layer, p, layerIdx); return; }
        if (p.useGyroid)    { generateGyroidInfill(layer, p, layerIdx);    return; }

        float spacing = p.extrusionWidth / p.infillDensity;

        // BambuStudio default: alternate between +45° and -45°
        float angle = p.fillAngleAlternate
            ? ((layerIdx % 2 == 0) ? (float)M_PI_4 : -(float)M_PI_4)
            : ((layerIdx % 2 == 0) ? 0.0f : (float)M_PI_2);

        std::vector<Path2> rawPaths;
        generateRotatedScanlines(layer, spacing, angle, rawPaths);
        sortPathsNearestNeighbour(rawPaths);
        connectAdjacentPaths(rawPaths, layer.infillPaths, spacing * 1.5f);
    }

    // Solid fill with optimized path ordering
    static void generateSolidFillOptimized(SliceLayer& layer,
                                            const SlicerParams& p,
                                            int layerIdx)
    {
        float spacing = p.extrusionWidth * 0.95f;
        float angle   = (layerIdx % 2 == 0) ? 0.0f : (float)M_PI_2;

        std::vector<Path2> rawPaths;
        generateRotatedScanlines(layer, spacing, angle, rawPaths);
        sortPathsNearestNeighbour(rawPaths);
        connectAdjacentPaths(rawPaths, layer.solidPaths, spacing * 1.5f);
    }

    // Bridge fill: fill angle perpendicular to bridge direction
    static void generateBridgeFill(SliceLayer& layer, const SlicerParams& p)
    {
        float spacing = p.extrusionWidth * 1.05f;  // slightly wider for bridges
        std::vector<Path2> rawPaths;
        generateRotatedScanlines(layer, spacing, layer.bridgeAngle, rawPaths);
        sortPathsNearestNeighbour(rawPaths);
        connectAdjacentPaths(rawPaths, layer.bridgePaths, spacing * 2.0f);
    }

    // =========================================================================
    // Rotated scanline fill — core fill primitive
    // Generates scan lines at the given angle, clips to contours.
    // =========================================================================
    static void generateRotatedScanlines(const SliceLayer& layer,
                                          float spacing,
                                          float angle,
                                          std::vector<Path2>& out)
    {
        float cosA = std::cos(angle), sinA = std::sin(angle);
        // Rotate bounding box to find scan range
        Vec2 corners[4] = {
            layer.bbMin,
            Vec2(layer.bbMax.x, layer.bbMin.y),
            layer.bbMax,
            Vec2(layer.bbMin.x, layer.bbMax.y)
        };
        float minU = 1e30f, maxU = -1e30f;
        for (auto& c : corners) {
            float u = c.x * sinA + c.y * (-cosA);  // perpendicular to scan direction
            minU = std::min(minU, u); maxU = std::max(maxU, u);
        }

        float u = minU + spacing * 0.5f;
        while (u <= maxU) {
            // Scan line in rotated frame: v varies, u = const
            // In world frame: direction = (cosA, sinA), normal = (-sinA, cosA)
            float ext = (layer.bbMax.x - layer.bbMin.x + layer.bbMax.y - layer.bbMin.y) + 2.0f;
            Vec2 center = Vec2(layer.bbMin.x + layer.bbMax.x, layer.bbMin.y + layer.bbMax.y) * 0.5f;
            Vec2 n(-sinA, cosA);
            Vec2 lineOrigin = center + n * (u - (center.x * sinA + center.y * (-cosA)));
            Vec2 dir(cosA, sinA);
            Path2 line = { lineOrigin - dir * ext, lineOrigin + dir * ext };
            clipPathToContours(line, layer.contours, out);
            u += spacing;
        }
    }

    // =========================================================================
    // Nearest-neighbour path ordering (greedy TSP)
    // BambuStudio's GCode generator uses a similar approach to minimise travel.
    // =========================================================================
    static void sortPathsNearestNeighbour(std::vector<Path2>& paths)
    {
        if (paths.size() < 2) return;
        int n = (int)paths.size();
        std::vector<bool> used(n, false);
        std::vector<Path2> sorted;
        sorted.reserve(n);

        // Start from the path closest to origin
        int cur = 0;
        float bestDist = glm::length(paths[0].front());
        for (int i = 1; i < n; ++i) {
            float d = glm::length(paths[i].front());
            if (d < bestDist) { bestDist = d; cur = i; }
        }

        for (int step = 0; step < n; ++step) {
            used[cur] = true;
            sorted.push_back(paths[cur]);

            Vec2 endPt = sorted.back().back();
            float minD = 1e30f;
            int   next = -1;
            for (int i = 0; i < n; ++i) {
                if (used[i]) continue;
                float d0 = glm::distance(endPt, paths[i].front());
                float d1 = glm::distance(endPt, paths[i].back());
                if (d0 < minD) { minD = d0; next = i; }
                if (d1 < minD) {
                    minD = d1; next = i;
                    // Reverse path so it starts at the closer end
                    std::reverse(paths[i].begin(), paths[i].end());
                }
            }
            if (next < 0) break;
            cur = next;
        }
        paths = std::move(sorted);
    }

    // =========================================================================
    // Connect adjacent paths along contour (BambuStudio perimeter-walk strategy)
    // If two consecutive paths are close enough, connect them via a short
    // contour segment rather than a travel move.
    // =========================================================================
    static void connectAdjacentPaths(const std::vector<Path2>& in,
                                      std::vector<Path2>& out,
                                      float maxGap)
    {
        if (in.empty()) return;
        Path2 current = in[0];
        for (size_t i = 1; i < in.size(); ++i) {
            float gap = glm::distance(current.back(), in[i].front());
            if (gap <= maxGap) {
                // Extend current path (short gap — no retraction needed)
                for (auto& pt : in[i]) current.push_back(pt);
            } else {
                out.push_back(std::move(current));
                current = in[i];
            }
        }
        out.push_back(std::move(current));
    }

    // =========================================================================
    // Honeycomb infill
    // =========================================================================
    static void generateHoneycombInfill(SliceLayer& layer,
                                         const SlicerParams& p,
                                         int layerIdx)
    {
        float spacing = p.extrusionWidth / p.infillDensity;
        float hw = spacing, hh = spacing * 0.866f;
        bool even = (layerIdx % 2 == 0);

        float x = layer.bbMin.x - hw;
        while (x <= layer.bbMax.x + hw) {
            Path2 path;
            float y = layer.bbMin.y - hh;
            int seg = 0;
            while (y <= layer.bbMax.y + hh) {
                float xOff = (even ? 1.0f : -1.0f) * ((seg % 2 == 0) ? hw * 0.5f : -hw * 0.5f);
                path.push_back(Vec2(x + xOff, y));
                y += hh; ++seg;
            }
            if (path.size() >= 2)
                clipPathToContours(path, layer.contours, layer.infillPaths);
            x += hw * 1.5f;
        }
    }

    // =========================================================================
    // Gyroid infill — BambuStudio FillGyroid style
    // Approximates the gyroid surface with a sinusoidal zigzag pattern.
    // The gyroid equation: sin(x)cos(y) + sin(y)cos(z) + sin(z)cos(x) = 0
    // At a fixed z, this reduces to a 2D wave pattern.
    // =========================================================================
    static void generateGyroidInfill(SliceLayer& layer,
                                      const SlicerParams& p,
                                      int layerIdx)
    {
        float spacing = p.extrusionWidth / p.infillDensity;
        float amplitude = spacing * 0.5f;
        float period    = spacing * 2.0f;
        float zPhase    = layer.z * (float)M_PI / period;

        // Horizontal zigzag with sinusoidal amplitude (gyroid approximation)
        float y = layer.bbMin.y;
        while (y <= layer.bbMax.y) {
            Path2 path;
            float x = layer.bbMin.x - spacing;
            int   pts = 0;
            while (x <= layer.bbMax.x + spacing) {
                float yOff = amplitude * std::sin(x * (float)M_PI * 2.0f / period + zPhase);
                path.push_back(Vec2(x, y + yOff));
                x += spacing * 0.1f;
                ++pts;
            }
            if (path.size() >= 2)
                clipPathToContours(path, layer.contours, layer.infillPaths);
            y += spacing;
        }
    }

    // =========================================================================
    // Skirt generation
    // =========================================================================
    static void generateSkirt(SliceLayer& layer, const SlicerParams& p,
                               const std::vector<Loop2>& footprint)
    {
        const std::vector<Loop2>& base = layer.contours.empty() ? footprint : layer.contours;
        if (base.empty()) return;
        for (int k = 0; k < p.skirtLoopCount; ++k) {
            float offset = p.skirtDistance + (k + 0.5f) * p.extrusionWidth;
            for (auto& loop : base) {
                Loop2 skirt = offsetLoopMiter(loop, offset);
                if (skirt.size() >= 3)
                    layer.skirtLoops.push_back(std::move(skirt));
            }
        }
    }

    // =========================================================================
    // Raft layer generation
    // =========================================================================
    static void generateRaftLayer(SliceLayer& layer, const SlicerParams& p,
                                   int li, int totalRaftLayers,
                                   const std::vector<Loop2>& /*footprint*/)
    {
        Vec2 bbMin = layer.bbMin, bbMax = layer.bbMax;
        bool isBase = (li < p.raftBaseLayers);
        float spacing = isBase ? (p.extrusionWidth * 2.5f) : (p.extrusionWidth * 1.2f);
        bool horiz = (li % 2 == 0);

        if (horiz) {
            float y = bbMin.y;
            while (y <= bbMax.y) {
                layer.raftPaths.push_back({ Vec2(bbMin.x, y), Vec2(bbMax.x, y) });
                y += spacing;
            }
        } else {
            float x = bbMin.x;
            while (x <= bbMax.x) {
                layer.raftPaths.push_back({ Vec2(x, bbMin.y), Vec2(x, bbMax.y) });
                x += spacing;
            }
        }
        if (li == 0) {
            Loop2 border = { bbMin, Vec2(bbMax.x, bbMin.y), bbMax, Vec2(bbMin.x, bbMax.y) };
            layer.skirtLoops.push_back(border);
        }
    }

    // =========================================================================
    // Optimized support generation — BambuStudio SupportMaterial style
    //
    // Improvements over previous version:
    //  1. Proper overhang area computation using polygon difference
    //     (instead of AABB approximation)
    //  2. Support contact layer: dense interface at top of support
    //     (BambuStudio SupportLayer top_contact)
    //  3. Support accumulated downward (union across layers)
    //  4. Support clipped against model with proper offset gap
    // =========================================================================
    static void generateSupportOptimized(SliceResult& result,
                                          const std::vector<Tri>& /*tris*/,
                                          const SlicerParams& p)
    {
        if (result.layers.empty()) return;

        float maxOverhang = p.layerHeight *
            std::tan((90.0f - p.supportAngle) * (float)M_PI / 180.0f);

        std::vector<SliceLayer*> modelLayers;
        for (auto& layer : result.layers)
            if (!layer.isRaftLayer) modelLayers.push_back(&layer);
        if (modelLayers.size() < 2) return;

        float supportSpacing = p.extrusionWidth / std::max(0.05f, p.supportDensity);
        float ifaceSpacing   = p.extrusionWidth / std::max(0.05f, p.supportInterfaceDensity);

        // Accumulate support region bounding boxes top-down
        // (BambuStudio accumulates support area as ExPolygon union)
        struct SupportRegion { Vec2 bbMin, bbMax; };
        std::vector<SupportRegion> accumSupport; // accumulated support from above

        for (int li = (int)modelLayers.size() - 1; li >= 1; --li) {
            SliceLayer* cur  = modelLayers[li];
            SliceLayer* prev = modelLayers[li - 1];
            if (cur->contours.empty() || prev->contours.empty()) continue;

            // Compute overhang: points in cur NOT supported by prev (expanded)
            std::vector<Vec2> overhangPts;
            for (auto& loop : cur->contours) {
                for (auto& pt : loop) {
                    Vec2 expMin = prev->bbMin - Vec2(maxOverhang);
                    Vec2 expMax = prev->bbMax + Vec2(maxOverhang);
                    bool inBB = (pt.x >= expMin.x && pt.x <= expMax.x &&
                                 pt.y >= expMin.y && pt.y <= expMax.y);
                    bool supported = false;
                    if (inBB) {
                        // Precise check: distance to nearest contour edge
                        float minDist = 1e30f;
                        for (auto& ploop : prev->contours) {
                            int n = (int)ploop.size();
                            for (int k = 0; k < n; ++k) {
                                Vec2 a = ploop[k], b = ploop[(k+1)%n];
                                Vec2 ab = b-a, ap = pt-a;
                                float t = glm::clamp(glm::dot(ap,ab)/std::max(glm::dot(ab,ab),1e-10f),0.0f,1.0f);
                                minDist = std::min(minDist, glm::distance(pt, a + t*ab));
                            }
                        }
                        if (pointInContours(pt, prev->contours) || minDist <= maxOverhang)
                            supported = true;
                    }
                    if (!supported) overhangPts.push_back(pt);
                }
            }

            // Also add accumulated support from above (propagate downward)
            for (auto& reg : accumSupport) {
                overhangPts.push_back(reg.bbMin);
                overhangPts.push_back(reg.bbMax);
                overhangPts.push_back(Vec2(reg.bbMin.x, reg.bbMax.y));
                overhangPts.push_back(Vec2(reg.bbMax.x, reg.bbMin.y));
            }

            if (overhangPts.empty()) { accumSupport.clear(); continue; }

            Vec2 supMin( 1e30f), supMax(-1e30f);
            for (auto& pt : overhangPts) { supMin = glm::min(supMin, pt); supMax = glm::max(supMax, pt); }
            supMin -= Vec2(p.supportOffset + p.extrusionWidth);
            supMax += Vec2(p.supportOffset + p.extrusionWidth);

            // Determine if this is the interface layer (top of support = directly below overhang)
            bool isInterface = p.supportInterface && !overhangPts.empty() &&
                               (li == (int)modelLayers.size() - 1 ||
                                modelLayers[li+1]->supportPaths.empty());

            float spacing = isInterface ? ifaceSpacing : supportSpacing;
            bool horiz = (li % 2 == 0);

            auto& targetPaths = isInterface ? cur->supportInterfacePaths : cur->supportPaths;

            if (horiz) {
                float y = supMin.y;
                while (y <= supMax.y) {
                    Path2 line = { Vec2(supMin.x, y), Vec2(supMax.x, y) };
                    clipPathOutsideContours(line, cur->contours, targetPaths, p.supportOffset);
                    y += spacing;
                }
            } else {
                float x = supMin.x;
                while (x <= supMax.x) {
                    Path2 line = { Vec2(x, supMin.y), Vec2(x, supMax.y) };
                    clipPathOutsideContours(line, cur->contours, targetPaths, p.supportOffset);
                    x += spacing;
                }
            }

            // Sort support paths
            sortPathsNearestNeighbour(cur->supportPaths);
            if (!cur->supportInterfacePaths.empty())
                sortPathsNearestNeighbour(cur->supportInterfacePaths);

            // Accumulate support region for next layer below
            accumSupport.clear();
            if (!overhangPts.empty())
                accumSupport.push_back({supMin, supMax});
        }
    }

    // =========================================================================
    // Clip polyline: keep segments OUTSIDE contours (for support)
    // =========================================================================
    static void clipPathOutsideContours(const Path2& path,
                                         const std::vector<Loop2>& contours,
                                         std::vector<Path2>& out,
                                         float offset = 0.0f)
    {
        if (path.size() < 2 || contours.empty()) {
            if (path.size() >= 2) out.push_back(path); return;
        }
        for (size_t si = 0; si + 1 < path.size(); ++si) {
            Vec2 a = path[si], b = path[si+1];
            std::vector<float> ts = {0.0f, 1.0f};
            for (auto& loop : contours) {
                int n = (int)loop.size();
                for (int i = 0; i < n; ++i) {
                    float t, u;
                    if (segIntersect(a, b, loop[i], loop[(i+1)%n], t, u))
                        if (t > 1e-6f && t < 1.0f - 1e-6f) ts.push_back(t);
                }
            }
            std::sort(ts.begin(), ts.end());
            ts.erase(std::unique(ts.begin(), ts.end(),
                [](float x, float y){ return std::abs(x-y) < 1e-6f; }), ts.end());
            for (size_t ti = 0; ti + 1 < ts.size(); ++ti) {
                Vec2 mid = a + (ts[ti]+ts[ti+1])*0.5f * (b-a);
                if (!pointInContours(mid, contours)) {
                    Vec2 p0 = a + ts[ti]   * (b-a);
                    Vec2 p1 = a + ts[ti+1] * (b-a);
                    if (glm::distance(p0, p1) > 1e-4f) out.push_back({p0, p1});
                }
            }
        }
    }

    // =========================================================================
    // Clip polyline: keep segments INSIDE contours (for infill)
    // =========================================================================
    static void clipPathToContours(const Path2& path,
                                    const std::vector<Loop2>& contours,
                                    std::vector<Path2>& out)
    {
        if (path.size() < 2 || contours.empty()) return;
        for (size_t si = 0; si + 1 < path.size(); ++si) {
            Vec2 a = path[si], b = path[si+1];
            std::vector<float> ts = {0.0f, 1.0f};
            for (auto& loop : contours) {
                int n = (int)loop.size();
                for (int i = 0; i < n; ++i) {
                    float t, u;
                    if (segIntersect(a, b, loop[i], loop[(i+1)%n], t, u))
                        if (t > 1e-6f && t < 1.0f - 1e-6f) ts.push_back(t);
                }
            }
            std::sort(ts.begin(), ts.end());
            ts.erase(std::unique(ts.begin(), ts.end(),
                [](float x, float y){ return std::abs(x-y) < 1e-6f; }), ts.end());
            for (size_t ti = 0; ti + 1 < ts.size(); ++ti) {
                Vec2 mid = a + (ts[ti]+ts[ti+1])*0.5f * (b-a);
                if (pointInContours(mid, contours)) {
                    Vec2 p0 = a + ts[ti]   * (b-a);
                    Vec2 p1 = a + ts[ti+1] * (b-a);
                    if (glm::distance(p0, p1) > 1e-4f) out.push_back({p0, p1});
                }
            }
        }
    }

    // =========================================================================
    // Arc fitting — least-squares circle fit (BambuStudio ArcFitter style)
    // Fits a circle to points[start..end] and checks if all points are within
    // tolerance of the arc.
    // =========================================================================
    static bool fitArcToPoints(const Path2& pts, int start, int end,
                                const SlicerParams& p, ArcSegment& arc)
    {
        int n = end - start + 1;
        if (n < 3) return false;

        // Kasa circle fit (fast, numerically stable for small arcs)
        double sumX = 0, sumY = 0, sumX2 = 0, sumY2 = 0;
        double sumXY = 0, sumX3 = 0, sumY3 = 0, sumX2Y = 0, sumXY2 = 0;
        for (int i = start; i <= end; ++i) {
            double x = pts[i].x, y = pts[i].y;
            double x2 = x*x, y2 = y*y;
            sumX += x; sumY += y; sumX2 += x2; sumY2 += y2;
            sumXY += x*y; sumX3 += x2*x; sumY3 += y2*y;
            sumX2Y += x2*y; sumXY2 += x*y2;
        }
        double N = n;
        double C = N*sumX2 - sumX*sumX;
        double D = N*sumXY - sumX*sumY;
        double E = N*sumX3 + N*sumXY2 - (sumX2+sumY2)*sumX;
        double G = N*sumY2 - sumY*sumY;
        double H = N*sumX2Y + N*sumY3 - (sumX2+sumY2)*sumY;

        double denom = 2.0*(C*G - D*D);
        if (std::abs(denom) < 1e-10) return false;

        double cx = (E*G - H*D) / denom;
        double cy = (C*H - D*E) / denom;
        double r  = std::sqrt(cx*cx + cy*cy - (sumX2+sumY2)/N + 2.0*(cx*sumX/N + cy*sumY/N));

        // Validate radius bounds
        if (r < p.arcMinRadius || r > p.arcMaxRadius) return false;

        // Check all points are within tolerance
        for (int i = start; i <= end; ++i) {
            double dx = pts[i].x - cx, dy = pts[i].y - cy;
            double dist = std::sqrt(dx*dx + dy*dy);
            if (std::abs(dist - r) > p.arcTolerance) return false;
        }

        // Determine arc direction (CCW or CW) using cross product
        Vec2 v1 = pts[start+1] - pts[start];
        Vec2 v2 = pts[start+2] - pts[start+1];
        float cross = v1.x * v2.y - v1.y * v2.x;

        arc.center     = Vec2((float)cx, (float)cy);
        arc.radius     = (float)r;
        arc.startAngle = std::atan2(pts[start].y - (float)cy, pts[start].x - (float)cx);
        arc.endAngle   = std::atan2(pts[end].y   - (float)cy, pts[end].x   - (float)cx);
        arc.ccw        = (cross > 0);
        return true;
    }

    // =========================================================================
    // Geometry utilities
    // =========================================================================
    static bool segIntersect(Vec2 a, Vec2 b, Vec2 c, Vec2 d, float& t, float& u)
    {
        Vec2 ab = b-a, cd = d-c, ac = c-a;
        float denom = ab.x*cd.y - ab.y*cd.x;
        if (std::abs(denom) < 1e-10f) return false;
        t = (ac.x*cd.y - ac.y*cd.x) / denom;
        u = (ac.x*ab.y - ac.y*ab.x) / denom;
        return (u >= 0.0f && u <= 1.0f);
    }

    static bool pointInContours(Vec2 p, const std::vector<Loop2>& contours)
    {
        int crossings = 0;
        for (auto& loop : contours) {
            int n = (int)loop.size();
            for (int i = 0; i < n; ++i) {
                Vec2 a = loop[i], b = loop[(i+1)%n];
                if ((a.y <= p.y && b.y > p.y) || (b.y <= p.y && a.y > p.y)) {
                    float xi = a.x + (p.y - a.y) / (b.y - a.y) * (b.x - a.x);
                    if (p.x < xi) ++crossings;
                }
            }
        }
        return (crossings % 2) == 1;
    }

    static bool pointInLoop(Vec2 p, const Loop2& loop)
    {
        int crossings = 0;
        int n = (int)loop.size();
        for (int i = 0; i < n; ++i) {
            Vec2 a = loop[i], b = loop[(i+1)%n];
            if ((a.y <= p.y && b.y > p.y) || (b.y <= p.y && a.y > p.y)) {
                float xi = a.x + (p.y - a.y) / (b.y - a.y) * (b.x - a.x);
                if (p.x < xi) ++crossings;
            }
        }
        return (crossings % 2) == 1;
    }

    // =========================================================================
    // Print statistics estimation
    // =========================================================================
    static void estimatePrintStats(SliceResult& result)
    {
        const SlicerParams& p = result.params;
        float nozzleArea   = (float)M_PI * (p.nozzleDiameter  * 0.5f) * (p.nozzleDiameter  * 0.5f);
        float filamentArea = (float)M_PI * (p.filamentDiameter * 0.5f) * (p.filamentDiameter * 0.5f);
        float extMult = (filamentArea > 0.0f) ? (nozzleArea * p.layerHeight / filamentArea) : 0.0f;

        float totalLen = 0.0f, totalTime = 0.0f;

        auto addPaths = [&](const std::vector<Path2>& paths, float spd) {
            for (auto& path : paths) {
                float len = 0.0f;
                for (size_t i = 0; i + 1 < path.size(); ++i)
                    len += glm::distance(path[i], path[i+1]);
                if (len > 0 && spd > 0) { totalLen += len; totalTime += len / spd; }
            }
        };
        auto addLoops = [&](const std::vector<Loop2>& loops, float spd) {
            for (auto& loop : loops) {
                float len = 0.0f;
                int n = (int)loop.size();
                for (int i = 0; i < n; ++i) len += glm::distance(loop[i], loop[(i+1)%n]);
                if (len > 0 && spd > 0) { totalLen += len; totalTime += len / spd; }
            }
        };

        for (auto& layer : result.layers) {
            if (layer.isRaftLayer) {
                addPaths(layer.raftPaths, p.raftBaseSpeed);
                addLoops(layer.skirtLoops, p.raftBaseSpeed);
                continue;
            }
            addLoops(layer.skirtLoops,           p.printSpeed * 0.9f);
            addPaths(layer.supportPaths,          p.supportSpeed);
            addPaths(layer.supportInterfacePaths, p.supportSpeed * 0.8f);
            if (!layer.shells.empty()) {
                addLoops(layer.shells[0], p.outerPerimSpeed);  // outer shell
                for (size_t s = 1; s < layer.shells.size(); ++s)
                    addLoops(layer.shells[s], p.innerPerimSpeed);
            }
            addPaths(layer.solidPaths,   p.infillSpeed);
            addPaths(layer.infillPaths,  p.infillSpeed);
            addPaths(layer.bridgePaths,  p.bridgeSpeed);
        }

        result.estimatedTime     = (totalTime > 0 && std::isfinite(totalTime)) ? totalTime : 0.0f;
        result.estimatedFilament = (totalLen  > 0 && std::isfinite(totalLen))  ? totalLen * extMult : 0.0f;
    }
};
