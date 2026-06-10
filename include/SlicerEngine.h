#pragma once
// =============================================================================
// SlicerEngine.h  —  FDM 3D Printing Slicer Core
// =============================================================================
// Implements the full slicing pipeline:
//   1. Plane-mesh intersection  → raw line segments per layer
//   2. Contour reconstruction   → ordered closed loops (Clipper-free, pure C++)
//   3. Perimeter generation     → inset shells (offset by extrusion width)
//   4. Infill pattern           → rectilinear (raster) or honeycomb
//   5. Support generation       → overhang detection + support grid
//   6. Skirt generation         → outer perimeter priming lines
//   7. Raft generation          → multi-layer adhesion base
//   8. Layer data output        → SliceLayer with all path types
//
// All geometry is 2D (XY plane) per layer; Z is the layer height.
// No external geometry library required beyond GLM.
// =============================================================================

#include <vector>
#include <array>
#include <string>
#include <cmath>
#include <algorithm>
#include <unordered_map>
#include <functional>
#include <glm/glm.hpp>

// ---------------------------------------------------------------------------
// Basic 2D types
// ---------------------------------------------------------------------------
using Vec2 = glm::vec2;
using Vec3 = glm::vec3;

struct Seg2 { Vec2 a, b; };          // a line segment in 2D
using Loop2 = std::vector<Vec2>;     // an ordered closed polygon loop
using Path2 = std::vector<Vec2>;     // an open polyline (infill / travel)

// ---------------------------------------------------------------------------
// SliceLayer — result of slicing one horizontal plane
// ---------------------------------------------------------------------------
struct SliceLayer {
    float   z;                       // layer Z height (mm)
    int     index;                   // 0-based layer index

    // Contours: outer boundary + holes (even-odd winding)
    std::vector<Loop2> contours;     // [0] = outermost, [1..] = holes

    // Perimeter shells (inset by extrusion width * shell_index)
    std::vector<std::vector<Loop2>> shells;  // shells[shellIdx][loopIdx]

    // Infill paths (open polylines, alternating direction per layer)
    std::vector<Path2> infillPaths;

    // Top/bottom solid fill paths
    std::vector<Path2> solidPaths;

    // Support paths (grid lines below overhangs)
    std::vector<Path2> supportPaths;

    // Skirt paths (outer priming loops, first layer only)
    std::vector<Loop2> skirtLoops;

    // Raft paths (base adhesion layers, index < raftLayers)
    std::vector<Path2> raftPaths;
    bool               isRaftLayer = false;  // true for raft-only layers

    // Bounding box of this layer
    Vec2 bbMin, bbMax;
};

// ---------------------------------------------------------------------------
// SlicerParams — user-configurable printing parameters
// ---------------------------------------------------------------------------
struct SlicerParams {
    float layerHeight       = 0.2f;   // mm
    float firstLayerHeight  = 0.3f;   // mm
    float nozzleDiameter    = 0.4f;   // mm
    float extrusionWidth    = 0.45f;  // mm (typically 1.05–1.2 × nozzle)
    int   numShells         = 2;      // number of perimeter shells
    float infillDensity     = 0.20f;  // 0.0–1.0
    int   topLayers         = 3;      // solid top layers
    int   bottomLayers      = 3;      // solid bottom layers
    bool  useHoneycomb      = false;  // false = rectilinear infill
    float printSpeed        = 60.0f;  // mm/s
    float travelSpeed       = 120.0f; // mm/s
    float infillSpeed       = 80.0f;  // mm/s
    float firstLayerSpeed   = 30.0f;  // mm/s
    float extruderTemp      = 200.0f; // °C  (PLA default)
    float bedTemp           = 60.0f;  // °C
    float filamentDiameter  = 1.75f;  // mm
    float retractionLength  = 1.0f;   // mm
    float retractionSpeed   = 45.0f;  // mm/s

    // ---- Skirt ----
    bool  enableSkirt       = true;   // generate skirt
    int   skirtLoopCount    = 2;      // number of skirt loops
    float skirtDistance     = 3.0f;   // mm gap from model
    int   skirtLayers       = 1;      // number of layers to print skirt on

    // ---- Raft ----
    bool  enableRaft        = false;  // generate raft
    int   raftBaseLayers    = 1;      // dense base layers
    int   raftInterfaceLayers = 1;    // interface layers (finer)
    float raftMargin        = 3.0f;   // mm expansion beyond model footprint
    float raftAirGap        = 0.2f;   // mm gap between raft top and model bottom
    float raftBaseSpeed     = 25.0f;  // mm/s (slow for adhesion)
    float raftInterfaceSpeed= 40.0f;  // mm/s

    // ---- Support ----
    bool  enableSupport     = false;  // generate support structures
    float supportAngle      = 45.0f;  // degrees — overhang threshold
    float supportDensity    = 0.20f;  // support infill density (0–1)
    float supportOffset     = 0.2f;   // mm gap between support and model
    float supportSpeed      = 40.0f;  // mm/s
    bool  supportEverywhere = false;  // false = build plate only
};

// ---------------------------------------------------------------------------
// SliceResult — complete slicer output
// ---------------------------------------------------------------------------
struct SliceResult {
    std::vector<SliceLayer> layers;
    SlicerParams            params;
    Vec3                    meshMin, meshMax;  // bounding box of input mesh
    int                     totalLayers = 0;
    float                   estimatedTime = 0.0f;  // seconds
    float                   estimatedFilament = 0.0f; // mm
    std::string             statusMsg;
    bool                    success = false;
};

// ---------------------------------------------------------------------------
// SlicerEngine
// ---------------------------------------------------------------------------
class SlicerEngine {
    // Internal triangle type — declared first so it is visible in all methods
    struct Tri { Vec3 v[3]; };

public:
    // Progress callback: (currentLayer, totalLayers, message)
    using ProgressCB = std::function<void(int, int, const std::string&)>;

    SlicerEngine() = default;

    // Main entry point: slice the mesh defined by (vertices, indices)
    // vertices: flat array [x0,y0,z0, x1,y1,z1, ...]
    // indices:  flat array of triangle vertex indices (3 per triangle)
    SliceResult slice(
        const std::vector<float>&    vertices,
        const std::vector<uint32_t>& indices,
        const SlicerParams&          params,
        ProgressCB                   progress = nullptr)
    {
        SliceResult result;
        result.params = params;

        if (vertices.empty() || indices.empty()) {
            result.statusMsg = "Empty mesh";
            return result;
        }

        // --- 1. Compute mesh bounding box ---
        Vec3 bbMin( 1e30f), bbMax(-1e30f);
        for (size_t i = 0; i + 2 < vertices.size(); i += 3) {
            Vec3 v(vertices[i], vertices[i+1], vertices[i+2]);
            bbMin = glm::min(bbMin, v);
            bbMax = glm::max(bbMax, v);
        }
        result.meshMin = bbMin;
        result.meshMax = bbMax;

        // --- 2. Determine layer Z positions ---
        // If raft is enabled, prepend raft layers below the model
        std::vector<float> zLevels;
        float modelZStart = bbMin.z;

        if (params.enableRaft) {
            // Raft base layers
            float raftH = params.firstLayerHeight;
            float zRaft = bbMin.z - params.raftAirGap
                          - (params.raftBaseLayers + params.raftInterfaceLayers) * raftH;
            for (int r = 0; r < params.raftBaseLayers; ++r) {
                zLevels.push_back(zRaft + r * raftH + raftH * 0.5f);
            }
            // Raft interface layers (slightly finer)
            float zIface = zRaft + params.raftBaseLayers * raftH;
            float ifaceH = raftH * 0.7f;
            for (int r = 0; r < params.raftInterfaceLayers; ++r) {
                zLevels.push_back(zIface + r * ifaceH + ifaceH * 0.5f);
            }
        }

        int raftLayerCount = (int)zLevels.size();

        // Model layers
        zLevels.push_back(bbMin.z + params.firstLayerHeight);
        float z = bbMin.z + params.firstLayerHeight + params.layerHeight;
        while (z <= bbMax.z + 1e-4f) {
            zLevels.push_back(z);
            z += params.layerHeight;
        }
        result.totalLayers = (int)zLevels.size();

        // --- 3. Build triangle list ---
        size_t numTris = indices.size() / 3;
        std::vector<Tri> tris(numTris);
        for (size_t t = 0; t < numTris; ++t) {
            for (int k = 0; k < 3; ++k) {
                uint32_t idx = indices[t*3+k];
                tris[t].v[k] = Vec3(vertices[idx*3], vertices[idx*3+1], vertices[idx*3+2]);
            }
        }

        // --- 4. Compute model footprint for skirt/raft ---
        // Use the first model layer contours as footprint
        std::vector<Loop2> footprint;
        {
            std::vector<Seg2> segs;
            intersectPlane(tris, zLevels[raftLayerCount], segs);
            chainSegments(segs, footprint);
        }
        Vec2 footBBMin( 1e30f), footBBMax(-1e30f);
        for (auto& loop : footprint)
            for (auto& p : loop) {
                footBBMin = glm::min(footBBMin, p);
                footBBMax = glm::max(footBBMax, p);
            }

        // --- 5. Slice each layer ---
        result.layers.reserve(zLevels.size());
        int totalWork = (int)zLevels.size();

        for (int li = 0; li < (int)zLevels.size(); ++li) {
            float zl = zLevels[li];
            if (progress) progress(li, totalWork, "Slicing layer " + std::to_string(li));

            bool isRaft = (li < raftLayerCount);
            int  modelLi = li - raftLayerCount;  // model-relative layer index

            if (isRaft) {
                // Generate raft layer
                SliceLayer layer;
                layer.z         = zl;
                layer.index     = li;
                layer.isRaftLayer = true;
                layer.bbMin     = footBBMin - Vec2(params.raftMargin);
                layer.bbMax     = footBBMax + Vec2(params.raftMargin);
                generateRaftLayer(layer, params, li, raftLayerCount, footprint);
                result.layers.push_back(std::move(layer));
                continue;
            }

            // 5a. Plane-triangle intersection → raw segments
            std::vector<Seg2> segs;
            segs.reserve(256);
            intersectPlane(tris, zl, segs);
            if (segs.empty()) continue;

            // 5b. Chain segments into closed loops
            std::vector<Loop2> contours;
            chainSegments(segs, contours);
            if (contours.empty()) continue;

            // 5c. Compute bounding box
            Vec2 lMin( 1e30f), lMax(-1e30f);
            for (auto& loop : contours)
                for (auto& p : loop) { lMin = glm::min(lMin,p); lMax = glm::max(lMax,p); }

            SliceLayer layer;
            layer.z        = zl;
            layer.index    = li;
            layer.contours = std::move(contours);
            layer.bbMin    = lMin;
            layer.bbMax    = lMax;

            // 5d. Generate perimeter shells
            generateShells(layer, params);

            // 5e. Generate infill
            bool isSolid = (modelLi < params.bottomLayers) ||
                           (modelLi >= (result.totalLayers - raftLayerCount) - params.topLayers);
            if (isSolid)
                generateSolidFill(layer, params, modelLi);
            else
                generateInfill(layer, params, modelLi);

            // 5f. Generate skirt (first skirtLayers model layers only)
            if (params.enableSkirt && modelLi < params.skirtLayers) {
                generateSkirt(layer, params, footprint);
            }

            result.layers.push_back(std::move(layer));
        }

        // --- 6. Generate support structures ---
        if (params.enableSupport && !tris.empty()) {
            generateSupport(result, tris, params);
        }

        // --- 7. Estimate print time and filament ---
        estimatePrintStats(result);

        result.success   = true;
        int tSec = (int)result.estimatedTime;
        result.statusMsg = "Sliced " + std::to_string(result.layers.size()) +
                           " layers in " +
                           std::to_string(tSec / 60) + "m " +
                           std::to_string(tSec % 60) + "s";
        return result;
    }

private:
    // =========================================================================
    // Plane-triangle intersection
    // =========================================================================
    static void intersectPlane(const std::vector<Tri>& tris,
                                float z,
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
    // Chain disconnected segments into closed loops
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

            if (loop.size() >= 3)
                loops.push_back(std::move(loop));
        }
    }

    // =========================================================================
    // Generate perimeter shells by inward offsetting contours
    // =========================================================================
    static void generateShells(SliceLayer& layer, const SlicerParams& p)
    {
        layer.shells.resize(p.numShells);
        for (int s = 0; s < p.numShells; ++s) {
            float offset = -(s + 0.5f) * p.extrusionWidth;
            for (auto& loop : layer.contours) {
                Loop2 shell = offsetLoop(loop, offset);
                if (shell.size() >= 3)
                    layer.shells[s].push_back(std::move(shell));
            }
        }
    }

    // Simple polygon offset: move each vertex along averaged edge normals
    static Loop2 offsetLoop(const Loop2& loop, float d)
    {
        int n = (int)loop.size();
        Loop2 result(n);
        for (int i = 0; i < n; ++i) {
            Vec2 prev = loop[(i-1+n)%n];
            Vec2 curr = loop[i];
            Vec2 next = loop[(i+1)%n];

            Vec2 e1 = glm::normalize(curr - prev);
            Vec2 e2 = glm::normalize(next - curr);

            Vec2 n1( e1.y, -e1.x);
            Vec2 n2( e2.y, -e2.x);
            Vec2 nm = glm::normalize(n1 + n2);

            float dot = glm::dot(nm, n1);
            float miter = (std::abs(dot) > 1e-4f) ? d / dot : d;
            miter = std::max(-3.0f * std::abs(d), std::min(3.0f * std::abs(d), miter));

            result[i] = curr + nm * miter;
        }
        return result;
    }

    // =========================================================================
    // Rectilinear infill
    // =========================================================================
    static void generateInfill(SliceLayer& layer, const SlicerParams& p, int layerIdx)
    {
        if (p.infillDensity < 0.01f) return;

        float spacing = p.extrusionWidth / p.infillDensity;
        bool  horiz   = (layerIdx % 2 == 0);

        if (p.useHoneycomb) {
            generateHoneycombInfill(layer, p, layerIdx, spacing);
            return;
        }

        Vec2 bbMin = layer.bbMin;
        Vec2 bbMax = layer.bbMax;

        if (horiz) {
            float y = bbMin.y + spacing * 0.5f;
            while (y <= bbMax.y) {
                Path2 line = { Vec2(bbMin.x - 1, y), Vec2(bbMax.x + 1, y) };
                clipPathToContours(line, layer.contours, layer.infillPaths);
                y += spacing;
            }
        } else {
            float x = bbMin.x + spacing * 0.5f;
            while (x <= bbMax.x) {
                Path2 line = { Vec2(x, bbMin.y - 1), Vec2(x, bbMax.y + 1) };
                clipPathToContours(line, layer.contours, layer.infillPaths);
                x += spacing;
            }
        }
    }

    // =========================================================================
    // Solid fill (top/bottom layers) — dense rectilinear
    // =========================================================================
    static void generateSolidFill(SliceLayer& layer, const SlicerParams& p, int layerIdx)
    {
        float spacing = p.extrusionWidth * 0.95f;
        bool  horiz   = (layerIdx % 2 == 0);
        Vec2  bbMin   = layer.bbMin;
        Vec2  bbMax   = layer.bbMax;

        if (horiz) {
            float y = bbMin.y + spacing * 0.5f;
            while (y <= bbMax.y) {
                Path2 line = { Vec2(bbMin.x - 1, y), Vec2(bbMax.x + 1, y) };
                clipPathToContours(line, layer.contours, layer.solidPaths);
                y += spacing;
            }
        } else {
            float x = bbMin.x + spacing * 0.5f;
            while (x <= bbMax.x) {
                Path2 line = { Vec2(x, bbMin.y - 1), Vec2(x, bbMax.y + 1) };
                clipPathToContours(line, layer.contours, layer.solidPaths);
                x += spacing;
            }
        }
    }

    // =========================================================================
    // Honeycomb infill
    // =========================================================================
    static void generateHoneycombInfill(SliceLayer& layer, const SlicerParams& p,
                                         int layerIdx, float spacing)
    {
        float hw = spacing;
        float hh = spacing * 0.866f;

        Vec2 bbMin = layer.bbMin;
        Vec2 bbMax = layer.bbMax;

        bool even = (layerIdx % 2 == 0);
        float x = bbMin.x - hw;
        while (x <= bbMax.x + hw) {
            Path2 path;
            float y = bbMin.y - hh;
            int seg = 0;
            while (y <= bbMax.y + hh) {
                float xOff = (even ? 1 : -1) * ((seg % 2 == 0) ? hw * 0.5f : -hw * 0.5f);
                path.push_back(Vec2(x + xOff, y));
                y += hh;
                ++seg;
            }
            if (path.size() >= 2)
                clipPathToContours(path, layer.contours, layer.infillPaths);
            x += hw * 1.5f;
        }
    }

    // =========================================================================
    // Skirt generation
    // =========================================================================
    // Generates skirtLoopCount outward-offset loops around the model footprint.
    // The first loop is at skirtDistance from the outermost contour.
    static void generateSkirt(SliceLayer& layer,
                               const SlicerParams& p,
                               const std::vector<Loop2>& footprint)
    {
        // Use the layer's own contours if available, else fall back to footprint
        const std::vector<Loop2>& base = layer.contours.empty() ? footprint : layer.contours;
        if (base.empty()) return;

        for (int k = 0; k < p.skirtLoopCount; ++k) {
            // Offset outward: positive offset = outward
            float offset = p.skirtDistance + (k + 0.5f) * p.extrusionWidth;
            for (auto& loop : base) {
                Loop2 skirt = offsetLoop(loop, offset);
                if (skirt.size() >= 3)
                    layer.skirtLoops.push_back(std::move(skirt));
            }
        }
    }

    // =========================================================================
    // Raft layer generation
    // =========================================================================
    // Generates a dense grid fill for the raft footprint (model footprint + margin).
    static void generateRaftLayer(SliceLayer& layer,
                                   const SlicerParams& p,
                                   int li,
                                   int totalRaftLayers,
                                   const std::vector<Loop2>& footprint)
    {
        // Raft bounding box (footprint + margin)
        Vec2 bbMin = layer.bbMin;
        Vec2 bbMax = layer.bbMax;

        // Determine spacing: base layers are coarser, interface layers are finer
        bool isBase = (li < p.raftBaseLayers);
        float spacing = isBase ? (p.extrusionWidth * 2.5f) : (p.extrusionWidth * 1.2f);
        bool horiz = (li % 2 == 0);

        // Generate dense rectilinear fill over the raft bounding box
        // (no contour clipping needed — raft is a solid rectangle)
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

        // Also add a border loop around the raft
        if (li == 0) {
            Loop2 border = {
                bbMin,
                Vec2(bbMax.x, bbMin.y),
                bbMax,
                Vec2(bbMin.x, bbMax.y)
            };
            layer.skirtLoops.push_back(border);  // reuse skirtLoops for raft border
        }
    }

    // =========================================================================
    // Support structure generation
    // =========================================================================
    // Algorithm:
    //   For each layer i (from top to bottom):
    //     1. Compute overhang area = contour[i] minus (contour[i-1] expanded by tan(angle)*layerH)
    //     2. Accumulate support area downward (union with previous support)
    //     3. Clip support area against model contour (avoid printing inside model)
    //     4. Fill support area with sparse grid
    // =========================================================================
    static void generateSupport(SliceResult& result,
                                 const std::vector<Tri>& tris,
                                 const SlicerParams& p)
    {
        if (result.layers.empty()) return;

        // Overhang threshold: horizontal distance a layer can extend per layer height
        float maxOverhang = p.layerHeight * std::tan((90.0f - p.supportAngle) * (float)M_PI / 180.0f);

        // Collect model-only layers (skip raft layers)
        std::vector<SliceLayer*> modelLayers;
        for (auto& layer : result.layers)
            if (!layer.isRaftLayer) modelLayers.push_back(&layer);

        if (modelLayers.size() < 2) return;

        // For each layer, compute support area as AABB-based overhang approximation
        // We use a simplified approach: for each contour point in layer[i],
        // check if it is supported by layer[i-1] (expanded by maxOverhang).
        // Unsupported points define the support region.

        // Build per-layer bounding boxes for quick lookup
        std::vector<Vec2> layerBBMin(modelLayers.size()), layerBBMax(modelLayers.size());
        for (size_t i = 0; i < modelLayers.size(); ++i) {
            layerBBMin[i] = modelLayers[i]->bbMin;
            layerBBMax[i] = modelLayers[i]->bbMax;
        }

        // Process from top to bottom, accumulating support regions
        // Support region per layer: list of AABB cells that need support
        struct SupportCell {
            Vec2 center;
            float size;
        };

        float supportSpacing = p.extrusionWidth / std::max(0.05f, p.supportDensity);

        for (int li = (int)modelLayers.size() - 1; li >= 1; --li) {
            SliceLayer* cur  = modelLayers[li];
            SliceLayer* prev = modelLayers[li - 1];

            if (cur->contours.empty() || prev->contours.empty()) continue;

            // For each point in current layer contours, check if it is
            // within maxOverhang of the previous layer's contour region.
            // Simplified: check if point is inside prev contours expanded by maxOverhang.

            // Collect overhang points: points in cur that are NOT inside prev (expanded)
            std::vector<Vec2> overhangPts;
            for (auto& loop : cur->contours) {
                for (auto& pt : loop) {
                    // Check if pt is inside prev contours (with expansion)
                    Vec2 expanded_pt = pt; // test point
                    bool supported = false;

                    // Quick AABB check with expansion
                    Vec2 expMin = prev->bbMin - Vec2(maxOverhang);
                    Vec2 expMax = prev->bbMax + Vec2(maxOverhang);
                    if (pt.x >= expMin.x && pt.x <= expMax.x &&
                        pt.y >= expMin.y && pt.y <= expMax.y) {
                        // More precise: check if pt is inside any expanded contour
                        // We approximate by checking if pt is inside the expanded AABB
                        // For a more accurate check, we test against the original contour
                        // with a tolerance equal to maxOverhang
                        float minDist = 1e30f;
                        for (auto& ploop : prev->contours) {
                            int n = (int)ploop.size();
                            for (int k = 0; k < n; ++k) {
                                Vec2 a = ploop[k], b = ploop[(k+1)%n];
                                // Distance from pt to segment ab
                                Vec2 ab = b - a, ap = pt - a;
                                float t = glm::clamp(glm::dot(ap, ab) / std::max(glm::dot(ab,ab), 1e-10f), 0.0f, 1.0f);
                                float d = glm::distance(pt, a + t * ab);
                                minDist = std::min(minDist, d);
                            }
                        }
                        // If inside prev contour OR within maxOverhang of its boundary
                        if (pointInContours(pt, prev->contours) || minDist <= maxOverhang)
                            supported = true;
                    }

                    if (!supported)
                        overhangPts.push_back(pt);
                }
            }

            if (overhangPts.empty()) continue;

            // Compute bounding box of overhang points
            Vec2 supMin( 1e30f), supMax(-1e30f);
            for (auto& pt : overhangPts) {
                supMin = glm::min(supMin, pt);
                supMax = glm::max(supMax, pt);
            }
            // Expand slightly
            supMin -= Vec2(p.supportOffset + p.extrusionWidth);
            supMax += Vec2(p.supportOffset + p.extrusionWidth);

            // Generate support grid in the overhang bounding box,
            // clipped to NOT be inside the model contour (avoid printing inside model)
            bool horiz = (li % 2 == 0);
            if (horiz) {
                float y = supMin.y;
                while (y <= supMax.y) {
                    Path2 line = { Vec2(supMin.x, y), Vec2(supMax.x, y) };
                    // Clip: keep parts that are NOT inside the model contour
                    // (support is outside the model)
                    clipPathOutsideContours(line, cur->contours, cur->supportPaths, p.supportOffset);
                    y += supportSpacing;
                }
            } else {
                float x = supMin.x;
                while (x <= supMax.x) {
                    Path2 line = { Vec2(x, supMin.y), Vec2(x, supMax.y) };
                    clipPathOutsideContours(line, cur->contours, cur->supportPaths, p.supportOffset);
                    x += supportSpacing;
                }
            }
        }
    }

    // =========================================================================
    // Clip a polyline: keep only segments OUTSIDE the contours (for support)
    // offset: minimum distance from contour boundary
    // =========================================================================
    static void clipPathOutsideContours(const Path2& path,
                                         const std::vector<Loop2>& contours,
                                         std::vector<Path2>& out,
                                         float offset = 0.0f)
    {
        if (path.size() < 2 || contours.empty()) {
            if (path.size() >= 2) out.push_back(path);
            return;
        }

        for (size_t si = 0; si + 1 < path.size(); ++si) {
            Vec2 a = path[si], b = path[si+1];

            std::vector<float> ts;
            ts.push_back(0.0f);
            ts.push_back(1.0f);

            for (auto& loop : contours) {
                int n = (int)loop.size();
                for (int i = 0; i < n; ++i) {
                    Vec2 c = loop[i], d = loop[(i+1)%n];
                    float t, u;
                    if (segIntersect(a, b, c, d, t, u)) {
                        if (t > 1e-6f && t < 1.0f - 1e-6f)
                            ts.push_back(t);
                    }
                }
            }

            std::sort(ts.begin(), ts.end());
            ts.erase(std::unique(ts.begin(), ts.end(),
                [](float x, float y){ return std::abs(x-y) < 1e-6f; }), ts.end());

            for (size_t ti = 0; ti + 1 < ts.size(); ++ti) {
                float tmid = (ts[ti] + ts[ti+1]) * 0.5f;
                Vec2 mid = a + tmid * (b - a);
                // Keep segment if midpoint is OUTSIDE contours
                if (!pointInContours(mid, contours)) {
                    Vec2 p0 = a + ts[ti]   * (b - a);
                    Vec2 p1 = a + ts[ti+1] * (b - a);
                    if (glm::distance(p0, p1) > 1e-4f)
                        out.push_back({p0, p1});
                }
            }
        }
    }

    // =========================================================================
    // Clip a polyline against contour polygons (keep inside portions)
    // =========================================================================
    static void clipPathToContours(const Path2& path,
                                    const std::vector<Loop2>& contours,
                                    std::vector<Path2>& out)
    {
        if (path.size() < 2 || contours.empty()) return;

        for (size_t si = 0; si + 1 < path.size(); ++si) {
            Vec2 a = path[si], b = path[si+1];

            std::vector<float> ts;
            ts.push_back(0.0f);
            ts.push_back(1.0f);

            for (auto& loop : contours) {
                int n = (int)loop.size();
                for (int i = 0; i < n; ++i) {
                    Vec2 c = loop[i], d = loop[(i+1)%n];
                    float t, u;
                    if (segIntersect(a, b, c, d, t, u)) {
                        if (t > 1e-6f && t < 1.0f - 1e-6f)
                            ts.push_back(t);
                    }
                }
            }

            std::sort(ts.begin(), ts.end());
            ts.erase(std::unique(ts.begin(), ts.end(),
                [](float x, float y){ return std::abs(x-y) < 1e-6f; }), ts.end());

            for (size_t ti = 0; ti + 1 < ts.size(); ++ti) {
                float tmid = (ts[ti] + ts[ti+1]) * 0.5f;
                Vec2 mid = a + tmid * (b - a);
                if (pointInContours(mid, contours)) {
                    Vec2 p0 = a + ts[ti]   * (b - a);
                    Vec2 p1 = a + ts[ti+1] * (b - a);
                    if (glm::distance(p0, p1) > 1e-4f)
                        out.push_back({p0, p1});
                }
            }
        }
    }

    // Segment-segment intersection
    static bool segIntersect(Vec2 a, Vec2 b, Vec2 c, Vec2 d, float& t, float& u)
    {
        Vec2 ab = b - a, cd = d - c, ac = c - a;
        float denom = ab.x * cd.y - ab.y * cd.x;
        if (std::abs(denom) < 1e-10f) return false;
        t = (ac.x * cd.y - ac.y * cd.x) / denom;
        u = (ac.x * ab.y - ac.y * ab.x) / denom;
        return (u >= 0.0f && u <= 1.0f);
    }

    // Point-in-polygon test (ray casting)
    static bool pointInContours(Vec2 p, const std::vector<Loop2>& contours)
    {
        int crossings = 0;
        for (auto& loop : contours) {
            int n = (int)loop.size();
            for (int i = 0; i < n; ++i) {
                Vec2 a = loop[i], b = loop[(i+1)%n];
                if ((a.y <= p.y && b.y > p.y) || (b.y <= p.y && a.y > p.y)) {
                    float xIntersect = a.x + (p.y - a.y) / (b.y - a.y) * (b.x - a.x);
                    if (p.x < xIntersect) ++crossings;
                }
            }
        }
        return (crossings % 2) == 1;
    }

    // =========================================================================
    // Estimate print time and filament usage
    // =========================================================================
    static void estimatePrintStats(SliceResult& result)
    {
        const SlicerParams& p = result.params;
        float totalLen = 0.0f;

        auto addLoopLen = [&](const Loop2& loop) {
            if (loop.size() < 2) return;
            for (size_t i = 0; i < loop.size(); ++i) {
                float d = glm::distance(loop[i], loop[(i+1) % loop.size()]);
                if (std::isfinite(d)) totalLen += d;
            }
        };
        auto addPathLen = [&](const Path2& path) {
            for (size_t i = 0; i + 1 < path.size(); ++i) {
                float d = glm::distance(path[i], path[i+1]);
                if (std::isfinite(d)) totalLen += d;
            }
        };

        for (auto& layer : result.layers) {
            for (auto& shellGroup : layer.shells)
                for (auto& loop : shellGroup) addLoopLen(loop);
            for (auto& path : layer.infillPaths)  addPathLen(path);
            for (auto& path : layer.solidPaths)   addPathLen(path);
            for (auto& path : layer.supportPaths) addPathLen(path);
            for (auto& loop : layer.skirtLoops)   addLoopLen(loop);
            for (auto& path : layer.raftPaths)    addPathLen(path);
        }

        float crossSection = (float)M_PI * (p.nozzleDiameter * 0.5f) * (p.nozzleDiameter * 0.5f);
        float filamentCrossSection = (float)M_PI * (p.filamentDiameter * 0.5f) * (p.filamentDiameter * 0.5f);
        float filamentLen = (filamentCrossSection > 0.0f)
            ? (totalLen * crossSection * p.layerHeight / filamentCrossSection)
            : 0.0f;
        result.estimatedFilament = std::isfinite(filamentLen) ? filamentLen : 0.0f;

        float avgSpeed = p.printSpeed * 0.8f;
        float t = (avgSpeed > 0.0f) ? (totalLen / avgSpeed) : 0.0f;
        result.estimatedTime = std::isfinite(t) ? t : 0.0f;
    }
};
