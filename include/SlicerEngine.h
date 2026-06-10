#pragma once
// =============================================================================
// SlicerEngine.h  —  FDM 3D Printing Slicer Core
// =============================================================================
// Implements the full slicing pipeline:
//   1. Plane-mesh intersection  → raw line segments per layer
//   2. Contour reconstruction   → ordered closed loops (Clipper-free, pure C++)
//   3. Perimeter generation     → inset shells (offset by extrusion width)
//   4. Infill pattern           → rectilinear (raster) or honeycomb
//   5. Layer data output        → SliceLayer with contours + infill paths
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
        std::vector<float> zLevels;
        float zStart = bbMin.z + params.firstLayerHeight * 0.5f;
        float zEnd   = bbMax.z - params.layerHeight * 0.5f;
        if (zStart > zEnd) {
            result.statusMsg = "Mesh too thin for given layer height";
            return result;
        }
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

        // --- 4. Slice each layer ---
        result.layers.reserve(zLevels.size());
        for (int li = 0; li < (int)zLevels.size(); ++li) {
            float zl = zLevels[li];
            if (progress) progress(li, result.totalLayers, "Slicing layer " + std::to_string(li));

            // 4a. Plane-triangle intersection → raw segments
            std::vector<Seg2> segs;
            segs.reserve(256);
            intersectPlane(tris, zl, segs);

            if (segs.empty()) continue;

            // 4b. Chain segments into closed loops
            std::vector<Loop2> contours;
            chainSegments(segs, contours);
            if (contours.empty()) continue;

            // 4c. Compute bounding box
            Vec2 lMin( 1e30f), lMax(-1e30f);
            for (auto& loop : contours)
                for (auto& p : loop) { lMin = glm::min(lMin,p); lMax = glm::max(lMax,p); }

            SliceLayer layer;
            layer.z        = zl;
            layer.index    = li;
            layer.contours = std::move(contours);
            layer.bbMin    = lMin;
            layer.bbMax    = lMax;

            // 4d. Generate perimeter shells
            generateShells(layer, params);

            // 4e. Generate infill
            bool isSolid = (li < params.bottomLayers) ||
                           (li >= result.totalLayers - params.topLayers);
            if (isSolid)
                generateSolidFill(layer, params, li);
            else
                generateInfill(layer, params, li);

            result.layers.push_back(std::move(layer));
        }

        // --- 5. Estimate print time and filament ---
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
            // Signed distances from plane z
            float d[3];
            for (int k = 0; k < 3; ++k) d[k] = tri.v[k].z - z;

            // Count sign changes
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
    // Uses a spatial hash for O(n) endpoint matching
    // =========================================================================
    static void chainSegments(const std::vector<Seg2>& segs,
                               std::vector<Loop2>& loops)
    {
        const float EPS = 1e-4f;

        // Build adjacency: endpoint → (seg_index, endpoint_index)
        struct EndKey {
            int   segIdx;
            int   endIdx; // 0=a, 1=b
        };
        // Quantize to grid for fast lookup
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

            // Walk forward from segs[start].b
            Vec2 cur = segs[start].b;
            for (;;) {
                auto key = encode(quantize(cur));
                auto it  = endMap.find(key);
                if (it == endMap.end()) break;
                int si = it->second.segIdx;
                if (used[si]) break;
                used[si] = true;
                Vec2 next = (it->second.endIdx == 0) ? segs[si].b : segs[si].a;
                if (glm::distance(next, loop.front()) < EPS * 2) break; // closed
                loop.push_back(next);
                cur = next;
            }

            if (loop.size() >= 3)
                loops.push_back(std::move(loop));
        }
    }

    // =========================================================================
    // Generate perimeter shells by inward offsetting contours
    // Simple vertex-normal offset (works well for convex/mildly concave shapes)
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

            // Inward normal (right-hand rule for CCW loops)
            Vec2 n1( e1.y, -e1.x);
            Vec2 n2( e2.y, -e2.x);
            Vec2 nm = glm::normalize(n1 + n2);

            // Miter length
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

        Vec2 bbMin = layer.bbMin;
        Vec2 bbMax = layer.bbMax;

        if (p.useHoneycomb) {
            generateHoneycombInfill(layer, p, layerIdx, spacing);
            return;
        }

        // Raster lines
        if (horiz) {
            float y = bbMin.y + spacing * 0.5f;
            while (y <= bbMax.y) {
                // Clip line against all contours (simple AABB clip for now)
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
        // Hexagonal grid with spacing as cell size
        float hw = spacing;          // hex half-width
        float hh = spacing * 0.866f; // hex half-height (sqrt(3)/2)

        Vec2 bbMin = layer.bbMin;
        Vec2 bbMax = layer.bbMax;

        // Generate zigzag lines that form hex pattern
        bool even = (layerIdx % 2 == 0);
        float step = hh * 2.0f;
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
            if (path.size() >= 2) {
                clipPathToContours(path, layer.contours, layer.infillPaths);
            }
            x += hw * 1.5f;
        }
    }

    // =========================================================================
    // Clip a polyline against contour polygons using point-in-polygon test
    // (Scanline intersection approach for each segment)
    // =========================================================================
    static void clipPathToContours(const Path2& path,
                                    const std::vector<Loop2>& contours,
                                    std::vector<Path2>& out)
    {
        if (path.size() < 2 || contours.empty()) return;

        // For a horizontal or vertical line segment, find intersections with contours
        // and output only the inside portions
        for (size_t si = 0; si + 1 < path.size(); ++si) {
            Vec2 a = path[si], b = path[si+1];

            // Collect all intersection t-values along segment a→b
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

            // For each sub-segment, test midpoint against contours
            for (size_t ti = 0; ti + 1 < ts.size(); ++ti) {
                float tmid = (ts[ti] + ts[ti+1]) * 0.5f;
                Vec2 mid = a + tmid * (b - a);
                if (pointInContours(mid, contours)) {
                    Vec2 p0 = a + ts[ti]   * (b - a);
                    Vec2 p1 = a + ts[ti+1] * (b - a);
                    if (glm::distance(p0, p1) > 1e-4f) {
                        out.push_back({p0, p1});
                    }
                }
            }
        }
    }

    // Segment-segment intersection: returns t along ab and u along cd
    static bool segIntersect(Vec2 a, Vec2 b, Vec2 c, Vec2 d, float& t, float& u)
    {
        Vec2 ab = b - a, cd = d - c, ac = c - a;
        float denom = ab.x * cd.y - ab.y * cd.x;
        if (std::abs(denom) < 1e-10f) return false;
        t = (ac.x * cd.y - ac.y * cd.x) / denom;
        u = (ac.x * ab.y - ac.y * ab.x) / denom;
        return (u >= 0.0f && u <= 1.0f);
    }

    // Point-in-polygon test (ray casting, handles multiple contours)
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

        for (auto& layer : result.layers) {
            // Perimeter length
            for (auto& shellGroup : layer.shells)
                for (auto& loop : shellGroup) {
                    if (loop.size() < 2) continue;
                    for (size_t i = 0; i < loop.size(); ++i) {
                        float d = glm::distance(loop[i], loop[(i+1) % loop.size()]);
                        if (std::isfinite(d)) totalLen += d;
                    }
                }

            // Infill length
            for (auto& path : layer.infillPaths)
                for (size_t i = 0; i + 1 < path.size(); ++i) {
                    float d = glm::distance(path[i], path[i+1]);
                    if (std::isfinite(d)) totalLen += d;
                }

            for (auto& path : layer.solidPaths)
                for (size_t i = 0; i + 1 < path.size(); ++i) {
                    float d = glm::distance(path[i], path[i+1]);
                    if (std::isfinite(d)) totalLen += d;
                }
        }

        // Filament volume = extrusion cross-section × path length
        float crossSection = (float)M_PI * (p.nozzleDiameter * 0.5f) * (p.nozzleDiameter * 0.5f);
        float filamentCrossSection = (float)M_PI * (p.filamentDiameter * 0.5f) * (p.filamentDiameter * 0.5f);
        float filamentLen = (filamentCrossSection > 0.0f)
            ? (totalLen * crossSection * p.layerHeight / filamentCrossSection)
            : 0.0f;
        result.estimatedFilament = std::isfinite(filamentLen) ? filamentLen : 0.0f;

        // Time estimate (rough: total length / average speed)
        // avgSpeed in mm/s, totalLen in mm → time in seconds
        float avgSpeed = p.printSpeed * 0.8f; // account for acceleration
        float t = (avgSpeed > 0.0f) ? (totalLen / avgSpeed) : 0.0f;
        result.estimatedTime = std::isfinite(t) ? t : 0.0f;
    }
};
