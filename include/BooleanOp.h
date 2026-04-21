#pragma once
#include <chrono>
#include <vector>
#include <string>
#include <memory>
#include <functional>
#include <iostream>

#include "mcut/mcut.h"
#include "RenderMesh.h"
#include "ObjLoader.h"

/**
 * @brief Enumeration of supported CSG boolean operations.
 */
enum class BoolOpType {
    UNION        = 0,  ///< A ∪ B
    INTERSECTION = 1,  ///< A ∩ B
    A_NOT_B      = 2,  ///< A − B
    B_NOT_A      = 3,  ///< B − A
    ALL          = 4,  ///< Compute all fragments (no filter)
};

static const char* BoolOpNames[] = {
    "Union (A ∪ B)",
    "Intersection (A ∩ B)",
    "Difference (A - B)",
    "Difference (B - A)",
    "All Fragments",
};

/**
 * @brief Wraps MCUT context and provides high-level boolean operation dispatch.
 *
 * Lifecycle:
 *   1. Construct → creates McContext
 *   2. setSourceMesh / setCutMesh → supply geometry
 *   3. execute(opType) → runs mcDispatch, populates resultMeshes
 *   4. Inspect resultMeshes for rendering
 */
class BooleanOpManager {
public:
    // Result connected components (ready for rendering)
    std::vector<std::unique_ptr<RenderMesh>> resultMeshes;

    // Timing info
    double lastDispatchTimeMs = 0.0;

    // Status
    std::string statusMessage;
    bool hasResult = false;

    // Log callback
    std::function<void(const std::string&)> logCallback;

    BooleanOpManager() = default;

    /** Call this after OpenGL/GLFW context is ready. */
    bool init() {
        if (m_context != MC_NULL_HANDLE) return true; // already initialized
        McResult err = mcCreateContext(&m_context, MC_DEBUG);
        if (err != MC_NO_ERROR) {
            std::cerr << "[MCUT] Failed to create context: " << (int)err << "\n";
            m_context = MC_NULL_HANDLE;
            return false;
        }
        // Register debug callback
        mcDebugMessageCallback(m_context, debugCallback, this);
        mcDebugMessageControl(m_context,
            MC_DEBUG_SOURCE_ALL, MC_DEBUG_TYPE_ALL, MC_DEBUG_SEVERITY_ALL, true);
        return true;
    }

    ~BooleanOpManager() {
        if (m_context != MC_NULL_HANDLE) {
            mcReleaseConnectedComponents(m_context, 0, nullptr);
            mcReleaseContext(m_context);
        }
    }

    bool setSourceMesh(const MeshModel& obj) {
        m_srcObj = obj;
        m_srcTranslation = {0.0, 0.0, 0.0};
        return obj.isValid();
    }

    bool setCutMesh(const MeshModel& obj) {
        m_cutObj = obj;
        m_cutTranslation = {0.0, 0.0, 0.0};
        return obj.isValid();
    }

    /**
     * @brief Set the cumulative translation offset for the source mesh.
     * This offset is applied to vertex coordinates at dispatch time.
     */
    void setSourceTranslation(double tx, double ty, double tz) {
        m_srcTranslation = {tx, ty, tz};
    }

    /**
     * @brief Set the cumulative translation offset for the cut mesh.
     */
    void setCutTranslation(double tx, double ty, double tz) {
        m_cutTranslation = {tx, ty, tz};
    }

    std::array<double,3> getSourceTranslation() const { return m_srcTranslation; }
    std::array<double,3> getCutTranslation()    const { return m_cutTranslation; }

    const MeshModel& getSourceObj() const { return m_srcObj; }
    const MeshModel& getCutObj()    const { return m_cutObj; }

    /**
     * @brief Execute the specified boolean operation.
     * @return true on success.
     */
    bool execute(BoolOpType opType) {
        if (m_context == MC_NULL_HANDLE) {
            statusMessage = "MCUT context not initialized.";
            return false;
        }
        if (!m_srcObj.isValid() || !m_cutObj.isValid()) {
            statusMessage = "Source or cut mesh not loaded.";
            return false;
        }

        // Release previous results
        mcReleaseConnectedComponents(m_context, 0, nullptr);
        resultMeshes.clear();
        hasResult = false;

        // Build dispatch flags
        McFlags flags = MC_DISPATCH_VERTEX_ARRAY_DOUBLE | MC_DISPATCH_ENFORCE_GENERAL_POSITION;
        flags |= getFilterFlags(opType);

        log("Dispatching MCUT (" + std::string(BoolOpNames[(int)opType]) + ")...");

        // Apply translation offsets to vertex arrays (copy to avoid modifying originals)
        std::vector<double> srcVerts = m_srcObj.data.vertices;
        if (m_srcTranslation[0] != 0.0 || m_srcTranslation[1] != 0.0 || m_srcTranslation[2] != 0.0) {
            for (size_t v = 0; v < srcVerts.size(); v += 3) {
                srcVerts[v+0] += m_srcTranslation[0];
                srcVerts[v+1] += m_srcTranslation[1];
                srcVerts[v+2] += m_srcTranslation[2];
            }
        }
        std::vector<double> cutVerts = m_cutObj.data.vertices;
        if (m_cutTranslation[0] != 0.0 || m_cutTranslation[1] != 0.0 || m_cutTranslation[2] != 0.0) {
            for (size_t v = 0; v < cutVerts.size(); v += 3) {
                cutVerts[v+0] += m_cutTranslation[0];
                cutVerts[v+1] += m_cutTranslation[1];
                cutVerts[v+2] += m_cutTranslation[2];
            }
        }

        auto t0 = std::chrono::high_resolution_clock::now();

        McResult err = mcDispatch(
            m_context,
            flags,
            srcVerts.data(),
            m_srcObj.data.indices.data(),
            m_srcObj.data.sizes.data(),
            (uint32_t)(srcVerts.size() / 3),
            (uint32_t)m_srcObj.data.sizes.size(),
            cutVerts.data(),
            m_cutObj.data.indices.data(),
            m_cutObj.data.sizes.data(),
            (uint32_t)(cutVerts.size() / 3),
            (uint32_t)m_cutObj.data.sizes.size());

        auto t1 = std::chrono::high_resolution_clock::now();
        lastDispatchTimeMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

        if (err != MC_NO_ERROR) {
            statusMessage = "mcDispatch failed (code " + std::to_string((int)err) + ")";
            log(statusMessage);
            return false;
        }

        // Query all connected components
        uint32_t numCC = 0;
        err = mcGetConnectedComponents(m_context, MC_CONNECTED_COMPONENT_TYPE_ALL, 0, nullptr, &numCC);
        if (err != MC_NO_ERROR || numCC == 0) {
            statusMessage = "No connected components produced.";
            log(statusMessage);
            return false;
        }

        std::vector<McConnectedComponent> ccs(numCC);
        mcGetConnectedComponents(m_context, MC_CONNECTED_COMPONENT_TYPE_ALL,
            numCC, ccs.data(), nullptr);

        log("Found " + std::to_string(numCC) + " connected component(s).");

        // Extract each CC into a RenderMesh
        for (uint32_t i = 0; i < numCC; ++i) {
            auto rm = extractCC(ccs[i], i);
            if (rm) resultMeshes.push_back(std::move(rm));
        }

        hasResult = !resultMeshes.empty();
        statusMessage = "Done in " + std::to_string((int)lastDispatchTimeMs) + " ms. "
                      + std::to_string(resultMeshes.size()) + " mesh(es) produced.";
        log(statusMessage);
        return hasResult;
    }

    // ---- Accessors ----
    bool isContextValid() const { return m_context != MC_NULL_HANDLE; }

private:
    McContext m_context = MC_NULL_HANDLE;
    MeshModel   m_srcObj;
    MeshModel   m_cutObj;
    std::array<double,3> m_srcTranslation = {0.0, 0.0, 0.0};
    std::array<double,3> m_cutTranslation = {0.0, 0.0, 0.0};

    // Predefined colors for result components
    static constexpr std::array<std::array<float,3>, 8> kColors = {{
        {0.2f, 0.6f, 1.0f},  // blue
        {1.0f, 0.4f, 0.2f},  // orange
        {0.2f, 0.9f, 0.4f},  // green
        {0.9f, 0.9f, 0.2f},  // yellow
        {0.8f, 0.2f, 0.8f},  // purple
        {0.2f, 0.9f, 0.9f},  // cyan
        {0.9f, 0.2f, 0.4f},  // red
        {0.6f, 0.6f, 0.6f},  // grey
    }};

    static McFlags getFilterFlags(BoolOpType op) {
        switch (op) {
            case BoolOpType::UNION:
                // A ∪ B: fragments above cut-mesh (sealed outside)
                return MC_DISPATCH_FILTER_FRAGMENT_SEALING_OUTSIDE
                     | MC_DISPATCH_FILTER_FRAGMENT_LOCATION_ABOVE;
            case BoolOpType::INTERSECTION:
                // A ∩ B: fragments below cut-mesh (sealed inside)
                return MC_DISPATCH_FILTER_FRAGMENT_SEALING_INSIDE
                     | MC_DISPATCH_FILTER_FRAGMENT_LOCATION_BELOW;
            case BoolOpType::A_NOT_B:
                // A − B: fragments above cut-mesh (sealed inside)
                return MC_DISPATCH_FILTER_FRAGMENT_SEALING_INSIDE
                     | MC_DISPATCH_FILTER_FRAGMENT_LOCATION_ABOVE;
            case BoolOpType::B_NOT_A:
                // B − A: fragments below cut-mesh (sealed outside)
                return MC_DISPATCH_FILTER_FRAGMENT_SEALING_OUTSIDE
                     | MC_DISPATCH_FILTER_FRAGMENT_LOCATION_BELOW;
            case BoolOpType::ALL:
            default:
                // All fragments, patches, seams and input meshes
                return MC_DISPATCH_FILTER_ALL;
        }
    }

    std::unique_ptr<RenderMesh> extractCC(McConnectedComponent cc, uint32_t idx) {
        // --- Vertices ---
        McSize numBytes = 0;
        McResult err = mcGetConnectedComponentData(
            m_context, cc, MC_CONNECTED_COMPONENT_DATA_VERTEX_DOUBLE, 0, nullptr, &numBytes);
        if (err != MC_NO_ERROR || numBytes == 0) return nullptr;

        uint32_t numVerts = (uint32_t)(numBytes / (sizeof(double) * 3));
        std::vector<double> verts(numVerts * 3);
        mcGetConnectedComponentData(m_context, cc,
            MC_CONNECTED_COMPONENT_DATA_VERTEX_DOUBLE, numBytes, verts.data(), nullptr);

        // --- Triangulated faces ---
        numBytes = 0;
        err = mcGetConnectedComponentData(
            m_context, cc, MC_CONNECTED_COMPONENT_DATA_FACE_TRIANGULATION, 0, nullptr, &numBytes);
        if (err != MC_NO_ERROR || numBytes == 0) return nullptr;

        uint32_t numTriIdx = (uint32_t)(numBytes / sizeof(uint32_t));
        std::vector<uint32_t> triIdx(numTriIdx);
        mcGetConnectedComponentData(m_context, cc,
            MC_CONNECTED_COMPONENT_DATA_FACE_TRIANGULATION, numBytes, triIdx.data(), nullptr);

        // --- Build RenderMesh ---
        auto rm = std::make_unique<RenderMesh>();
        rm->label = "CC_" + std::to_string(idx);

        rm->vertices.resize(numVerts);
        for (uint32_t v = 0; v < numVerts; ++v) {
            rm->vertices[v].position = {
                (float)verts[v*3+0],
                (float)verts[v*3+1],
                (float)verts[v*3+2]
            };
        }
        rm->indices = triIdx;

        // Query CC type for label and color
        McConnectedComponentType ccType = (McConnectedComponentType)0;
        numBytes = sizeof(McConnectedComponentType);
        mcGetConnectedComponentData(m_context, cc,
            MC_CONNECTED_COMPONENT_DATA_TYPE, numBytes, &ccType, nullptr);

        std::string typeStr;
        switch (ccType) {
            case MC_CONNECTED_COMPONENT_TYPE_FRAGMENT: typeStr = "Fragment"; break;
            case MC_CONNECTED_COMPONENT_TYPE_PATCH:    typeStr = "Patch";    break;
            case MC_CONNECTED_COMPONENT_TYPE_SEAM:     typeStr = "Seam";     break;
            case MC_CONNECTED_COMPONENT_TYPE_INPUT:    typeStr = "Input";    break;
            default:                                   typeStr = "Unknown";  break;
        }
        rm->label = typeStr + "_" + std::to_string(idx);

        // Assign color based on type
        if (ccType == MC_CONNECTED_COMPONENT_TYPE_FRAGMENT) {
            // Bright colors for fragments (the actual boolean result)
            auto& c = kColors[idx % kColors.size()];
            rm->color = { c[0], c[1], c[2] };
            rm->alpha = 1.0f;
        } else if (ccType == MC_CONNECTED_COMPONENT_TYPE_PATCH) {
            rm->color = { 0.9f, 0.8f, 0.1f };  // yellow for patches
            rm->alpha = 0.85f;
        } else if (ccType == MC_CONNECTED_COMPONENT_TYPE_SEAM) {
            rm->color = { 1.0f, 0.0f, 0.5f };  // pink for seams
            rm->alpha = 0.7f;
            rm->showWire = true;
        } else if (ccType == MC_CONNECTED_COMPONENT_TYPE_INPUT) {
            rm->color = { 0.6f, 0.6f, 0.6f };  // grey for input copies
            rm->alpha = 0.4f;
        } else {
            auto& c = kColors[idx % kColors.size()];
            rm->color = { c[0], c[1], c[2] };
            rm->alpha = 0.9f;
        }

        rm->upload();
        return rm;
    }

    void log(const std::string& msg) {
        if (logCallback) logCallback(msg);
        else std::cout << "[MCUT] " << msg << "\n";
    }

    static void MCAPI_PTR debugCallback(
        McDebugSource source, McDebugType type, McUint32 id,
        McDebugSeverity severity, size_t length, const char* message, const void* userParam)
    {
        (void)source; (void)type; (void)id; (void)length;
        auto* self = (BooleanOpManager*)userParam;
        if (severity == MC_DEBUG_SEVERITY_HIGH || severity == MC_DEBUG_SEVERITY_MEDIUM) {
            std::string msg = "[MCUT DEBUG] " + std::string(message);
            if (self) self->log(msg);
            else std::cerr << msg << "\n";
        }
    }
};


