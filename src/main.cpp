/**
 * @file main.cpp
 * @brief MCUT Boolean Operation Viewer
 *
 * Features:
 *  - Load OBJ meshes as Source Mesh (A) and Cut Mesh (B)
 *  - Execute Union / Intersection / Difference (A-B) / Difference (B-A) / All Fragments
 *  - Real-time 3D preview with arcball camera
 *  - Interactive mesh dragging: Alt+Left-drag to move selected mesh
 *  - Right-side panel: each result CC rendered in its own FBO thumbnail
 *  - Snapshot-based Undo/Redo (Ctrl+Z / Ctrl+Y, up to 20 snapshots)
 */

// ---- Platform ----
#ifdef _WIN32
#  define NOMINMAX
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

#include <glad/gl.h>
#include <GLFW/glfw3.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include "RenderMesh.h"
#include "ObjLoader.h"
#include "BooleanOp.h"
#include "Camera.h"
#include "Shader.h"

#include <string>
#include <vector>
#include <deque>
#include <memory>
#include <algorithm>
#include <filesystem>
#include <cstring>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <cmath>

// ============================================================
//  Snapshot for Undo/Redo
// ============================================================
struct CCSnapshot {
    std::string             label;
    glm::vec3               color;
    float                   alpha;
    std::vector<RenderMesh::Vertex> vertices;
    std::vector<uint32_t>   indices;
};

struct AppSnapshot {
    std::string   srcPath, cutPath;
    ObjData       srcObj,  cutObj;
    glm::vec3     srcTranslation{0.0f}, cutTranslation{0.0f};
    int           opType = 0;
    double        dispatchTimeMs = 0.0;
    std::vector<CCSnapshot> ccs;
    bool          hasResult = false;
};

// ============================================================
//  Per-CC FBO thumbnail
// ============================================================
static constexpr int kThumbW = 256;
static constexpr int kThumbH = 256;

struct CCThumb {
    GLuint fbo     = 0;
    GLuint colorTex= 0;
    GLuint depthRbo= 0;
    Camera cam;
    float  rotY    = 0.0f;   // auto-spin angle (degrees)
    bool   focused = false;  // whether user clicked this thumb

    void create() {
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);

        glGenTextures(1, &colorTex);
        glBindTexture(GL_TEXTURE_2D, colorTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kThumbW, kThumbH, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, colorTex, 0);

        glGenRenderbuffers(1, &depthRbo);
        glBindRenderbuffer(GL_RENDERBUFFER, depthRbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, kThumbW, kThumbH);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                  GL_RENDERBUFFER, depthRbo);

        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        cam.setAspectRatio(1.0f);
    }

    void destroy() {
        if (fbo)      { glDeleteFramebuffers(1, &fbo);      fbo = 0; }
        if (colorTex) { glDeleteTextures(1, &colorTex);     colorTex = 0; }
        if (depthRbo) { glDeleteRenderbuffers(1, &depthRbo); depthRbo = 0; }
    }
};

// ============================================================
//  Global state
// ============================================================
static Camera g_camera;
static bool   g_mouseLeftDown   = false;
static bool   g_mouseMiddleDown = false;
static double g_lastMouseX = 0, g_lastMouseY = 0;
static int    g_viewportW = 1400, g_viewportH = 800;

static std::unique_ptr<RenderMesh> g_srcMesh;
static std::unique_ptr<RenderMesh> g_cutMesh;
static glm::vec3 g_srcTranslation{0.0f};
static glm::vec3 g_cutTranslation{0.0f};

static BooleanOpManager g_boolMgr;

// CC thumbnails (one per result CC)
static std::vector<CCThumb> g_thumbs;

// Undo/Redo stacks
static std::deque<AppSnapshot> g_undoStack;   // past states
static std::deque<AppSnapshot> g_redoStack;   // future states
static constexpr int kMaxSnapshots = 20;

// Log
static std::deque<std::string> g_log;
static void addLog(const std::string& msg) {
    g_log.push_back(msg);
    if (g_log.size() > 300) g_log.pop_front();
}

// UI state
static char  g_srcPath[512] = "assets/meshes/cube.obj";
static char  g_cutPath[512] = "assets/meshes/torus.obj";
static int   g_selectedOp   = 0;
static bool  g_showSrcMesh  = true;
static bool  g_showCutMesh  = true;
static bool  g_showResult   = true;
static bool  g_srcWireframe = false;
static bool  g_cutWireframe = false;
static float g_srcAlpha     = 0.45f;
static float g_cutAlpha     = 0.45f;
static bool  g_showGrid     = true;
static bool  g_autoFit      = true;
static bool  g_autoHideInput = false;
static char  g_exportDir[512] = "exports";
static float g_exportMsgTimer = 0.0f;
static bool  g_thumbAutoSpin  = true;  // auto-spin CC thumbnails

// Drag state
static int    g_dragTarget    = 1;
static bool   g_isDragging    = false;
static bool   g_altWasHeld    = false;   // latched at mouse-press time
static glm::vec3 g_dragPlaneNormal{0.0f};
static glm::vec3 g_dragPlanePoint{0.0f};
static glm::vec3 g_dragStartWorld{0.0f};
static bool   g_needsBooleanUpdate = false;

// Shader
static Shader g_shader;

// ============================================================
//  Inline GLSL
// ============================================================
static const char* kVertSrc = R"GLSL(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform mat3 normalMatrix;
out vec3 FragPos;
out vec3 Normal;
void main() {
    FragPos = vec3(model * vec4(aPos, 1.0));
    Normal = normalMatrix * aNormal;
    gl_Position = projection * view * vec4(FragPos, 1.0);
}
)GLSL";

static const char* kFragSrc = R"GLSL(
#version 330 core
in vec3 FragPos;
in vec3 Normal;
uniform vec3  lightPos;
uniform vec3  viewPos;
uniform vec3  objectColor;
uniform float alpha;
uniform bool  wireframe;
out vec4 FragColor;
void main() {
    if (wireframe) { FragColor = vec4(objectColor * 0.25, alpha * 0.7); return; }
    float ambientStr = 0.20;
    vec3 ambient = ambientStr * vec3(1.0);
    vec3 norm = normalize(Normal);
    vec3 lightDir = normalize(lightPos - FragPos);
    float diff  = max(dot( norm, lightDir), 0.0);
    float diffB = max(dot(-norm, lightDir), 0.0) * 0.45;
    vec3 diffuse = max(diff, diffB) * vec3(1.0);
    float specStr = 0.30;
    vec3 viewDir    = normalize(viewPos - FragPos);
    vec3 reflectDir = reflect(-lightDir, norm);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), 48.0);
    vec3 specular = specStr * spec * vec3(1.0);
    vec3 result = (ambient + diffuse + specular) * objectColor;
    FragColor = vec4(result, alpha);
}
)GLSL";

// ============================================================
//  Grid
// ============================================================
static GLuint g_gridVAO = 0, g_gridVBO = 0;
static int    g_gridLineCount = 0;

static void buildGrid(float size = 8.0f, int divs = 32) {
    std::vector<float> verts;
    float step = size * 2.0f / divs;
    for (int i = 0; i <= divs; ++i) {
        float t = -size + i * step;
        verts.insert(verts.end(), {t, 0.0f, -size,  t, 0.0f, size});
        verts.insert(verts.end(), {-size, 0.0f, t,  size, 0.0f, t});
    }
    g_gridLineCount = (int)(verts.size() / 3);
    glGenVertexArrays(1, &g_gridVAO);
    glGenBuffers(1, &g_gridVBO);
    glBindVertexArray(g_gridVAO);
    glBindBuffer(GL_ARRAY_BUFFER, g_gridVBO);
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);
}

static void drawGrid(const glm::mat4& view, const glm::mat4& proj, const glm::vec3& viewPos) {
    if (!g_gridVAO) return;
    g_shader.use();
    g_shader.setMat4("model", glm::mat4(1.0f));
    g_shader.setMat4("view", view);
    g_shader.setMat4("projection", proj);
    g_shader.setMat3("normalMatrix", glm::mat3(1.0f));
    g_shader.setVec3("lightPos", glm::vec3(10, 20, 10));
    g_shader.setVec3("viewPos", viewPos);
    g_shader.setVec3("objectColor", glm::vec3(0.30f));
    g_shader.setFloat("alpha", 0.5f);
    g_shader.setBool("wireframe", true);
    glBindVertexArray(g_gridVAO);
    glDrawArrays(GL_LINES, 0, g_gridLineCount);
    glBindVertexArray(0);
}

// ============================================================
//  Draw mesh helper
// ============================================================
static void drawMesh(const RenderMesh& mesh,
                     const glm::mat4& view, const glm::mat4& proj,
                     const glm::vec3& viewPos,
                     const glm::mat4& modelMatrix,
                     bool wireOverlay = false)
{
    if (!mesh.visible || mesh.VAO == 0 || mesh.indices.empty()) return;
    g_shader.use();
    glm::mat3 normalMat = glm::mat3(glm::transpose(glm::inverse(modelMatrix)));
    g_shader.setMat4("model", modelMatrix);
    g_shader.setMat4("view", view);
    g_shader.setMat4("projection", proj);
    g_shader.setMat3("normalMatrix", normalMat);
    g_shader.setVec3("lightPos", glm::vec3(10.0f, 20.0f, 10.0f));
    g_shader.setVec3("viewPos", viewPos);
    g_shader.setVec3("objectColor", mesh.color);
    g_shader.setFloat("alpha", mesh.alpha);
    g_shader.setBool("wireframe", false);
    mesh.draw();
    if (wireOverlay || mesh.showWire) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        glLineWidth(1.0f);
        g_shader.setBool("wireframe", true);
        g_shader.setVec3("objectColor", glm::vec3(0.05f));
        g_shader.setFloat("alpha", 0.8f);
        mesh.draw();
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }
}

// ============================================================
//  Ray-plane intersection
// ============================================================
static bool rayPlaneIntersect(const glm::vec3& ro, const glm::vec3& rd,
                               const glm::vec3& pn, const glm::vec3& pp,
                               glm::vec3& hit) {
    float denom = glm::dot(pn, rd);
    if (std::abs(denom) < 1e-6f) return false;
    float t = glm::dot(pn, pp - ro) / denom;
    if (t < 0.0f) return false;
    hit = ro + t * rd;
    return true;
}

static glm::vec3 screenToRayDir(double sx, double sy,
                                 const glm::mat4& proj, const glm::mat4& view) {
    float ndcX = (float)(2.0 * sx / g_viewportW - 1.0);
    float ndcY = (float)(1.0 - 2.0 * sy / g_viewportH);
    glm::vec4 clip(ndcX, ndcY, -1.0f, 1.0f);
    glm::vec4 eye = glm::inverse(proj) * clip;
    eye = glm::vec4(eye.x, eye.y, -1.0f, 0.0f);
    return glm::normalize(glm::vec3(glm::inverse(view) * eye));
}

// ============================================================
//  Mesh center helper
// ============================================================
static glm::vec3 meshCenter(const RenderMesh* m, const glm::vec3& trans) {
    if (!m) return glm::vec3(0.0f);
    return (m->bbMin + m->bbMax) * 0.5f + trans;
}

// ============================================================
//  Camera fit
// ============================================================
static void fitCameraToScene() {
    glm::vec3 minBB(1e9f), maxBB(-1e9f);
    auto expand = [&](const RenderMesh* m, const glm::vec3& t) {
        if (m && m->visible) {
            minBB = glm::min(minBB, m->bbMin + t);
            maxBB = glm::max(maxBB, m->bbMax + t);
        }
    };
    expand(g_srcMesh.get(), g_srcTranslation);
    expand(g_cutMesh.get(), g_cutTranslation);
    for (auto& rm : g_boolMgr.resultMeshes)
        expand(rm.get(), glm::vec3(0.0f));
    if (minBB.x < 1e8f) g_camera.fitBoundingBox(minBB, maxBB);
}

// ============================================================
//  Snapshot helpers
// ============================================================
static AppSnapshot captureSnapshot() {
    AppSnapshot snap;
    snap.srcPath        = g_srcPath;
    snap.cutPath        = g_cutPath;
    snap.srcObj         = g_boolMgr.getSourceObj();
    snap.cutObj         = g_boolMgr.getCutObj();
    snap.srcTranslation = g_srcTranslation;
    snap.cutTranslation = g_cutTranslation;
    snap.opType         = g_selectedOp;
    snap.dispatchTimeMs = g_boolMgr.lastDispatchTimeMs;
    snap.hasResult      = g_boolMgr.hasResult;
    for (const auto& rm : g_boolMgr.resultMeshes) {
        CCSnapshot cc;
        cc.label    = rm->label;
        cc.color    = rm->color;
        cc.alpha    = rm->alpha;
        cc.vertices = rm->vertices;
        cc.indices  = rm->indices;
        snap.ccs.push_back(std::move(cc));
    }
    return snap;
}

// Rebuild GPU meshes and thumbnails from a snapshot
static void rebuildThumbsFromCCs();

static void restoreSnapshot(const AppSnapshot& snap) {
    strncpy(g_srcPath, snap.srcPath.c_str(), sizeof(g_srcPath));
    strncpy(g_cutPath, snap.cutPath.c_str(), sizeof(g_cutPath));
    g_srcTranslation = snap.srcTranslation;
    g_cutTranslation = snap.cutTranslation;
    g_selectedOp     = snap.opType;

    // Restore source mesh GPU
    if (snap.srcObj.valid) {
        g_boolMgr.setSourceMesh(snap.srcObj);
        g_srcMesh = objDataToRenderMesh(snap.srcObj, "Source (A)");
        g_srcMesh->color = {0.25f, 0.55f, 1.0f};
        g_srcMesh->alpha = g_srcAlpha;
    }
    // Restore cut mesh GPU
    if (snap.cutObj.valid) {
        g_boolMgr.setCutMesh(snap.cutObj);
        g_cutMesh = objDataToRenderMesh(snap.cutObj, "Cut (B)");
        g_cutMesh->color = {1.0f, 0.45f, 0.15f};
        g_cutMesh->alpha = g_cutAlpha;
    }

    // Restore result CCs
    g_boolMgr.resultMeshes.clear();
    g_boolMgr.hasResult = snap.hasResult;
    g_boolMgr.lastDispatchTimeMs = snap.dispatchTimeMs;
    for (const auto& cc : snap.ccs) {
        auto rm = std::make_unique<RenderMesh>();
        rm->label    = cc.label;
        rm->color    = cc.color;
        rm->alpha    = cc.alpha;
        rm->vertices = cc.vertices;
        rm->indices  = cc.indices;
        rm->upload();
        g_boolMgr.resultMeshes.push_back(std::move(rm));
    }

    rebuildThumbsFromCCs();
    if (g_autoFit) fitCameraToScene();
}

static void pushUndo() {
    g_undoStack.push_back(captureSnapshot());
    if ((int)g_undoStack.size() > kMaxSnapshots)
        g_undoStack.pop_front();
    g_redoStack.clear();
}

static void doUndo() {
    if (g_undoStack.empty()) { addLog("Nothing to undo."); return; }
    g_redoStack.push_back(captureSnapshot());
    AppSnapshot snap = std::move(g_undoStack.back());
    g_undoStack.pop_back();
    restoreSnapshot(snap);
    addLog("Undo → " + std::string(BoolOpNames[snap.opType]));
}

static void doRedo() {
    if (g_redoStack.empty()) { addLog("Nothing to redo."); return; }
    g_undoStack.push_back(captureSnapshot());
    AppSnapshot snap = std::move(g_redoStack.back());
    g_redoStack.pop_back();
    restoreSnapshot(snap);
    addLog("Redo → " + std::string(BoolOpNames[snap.opType]));
}

// ============================================================
//  CC Thumbnails
// ============================================================
static void destroyThumbs() {
    for (auto& t : g_thumbs) t.destroy();
    g_thumbs.clear();
}

static void rebuildThumbsFromCCs() {
    destroyThumbs();
    g_thumbs.resize(g_boolMgr.resultMeshes.size());
    for (size_t i = 0; i < g_boolMgr.resultMeshes.size(); ++i) {
        auto& t  = g_thumbs[i];
        auto& rm = g_boolMgr.resultMeshes[i];
        t.create();
        t.cam.setAspectRatio(1.0f);
        t.cam.fitBoundingBox(rm->bbMin, rm->bbMax);
        t.rotY = 0.0f;
    }
}

// Render all CC thumbnails into their FBOs
static void renderThumbs() {
    if (g_thumbs.empty()) return;

    GLint prevFBO = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);

    for (size_t i = 0; i < g_thumbs.size() && i < g_boolMgr.resultMeshes.size(); ++i) {
        auto& t  = g_thumbs[i];
        auto& rm = g_boolMgr.resultMeshes[i];
        if (!rm || rm->VAO == 0) continue;

        glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
        glViewport(0, 0, kThumbW, kThumbH);
        glClearColor(0.10f, 0.10f, 0.13f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // Apply auto-spin rotation around Y
        glm::vec3 center = (rm->bbMin + rm->bbMax) * 0.5f;
        glm::mat4 model = glm::translate(glm::mat4(1.0f), -center);
        model = glm::rotate(glm::mat4(1.0f),
                            glm::radians(t.rotY), glm::vec3(0,1,0)) * model;
        model = glm::translate(glm::mat4(1.0f), center) * model;

        glm::mat4 view = t.cam.getViewMatrix();
        glm::mat4 proj = t.cam.getProjectionMatrix();
        glm::vec3 vpos = t.cam.getPosition();

        g_shader.use();
        glm::mat3 normalMat = glm::mat3(glm::transpose(glm::inverse(model)));
        g_shader.setMat4("model", model);
        g_shader.setMat4("view", view);
        g_shader.setMat4("projection", proj);
        g_shader.setMat3("normalMatrix", normalMat);
        g_shader.setVec3("lightPos", glm::vec3(10.0f, 20.0f, 10.0f));
        g_shader.setVec3("viewPos", vpos);
        g_shader.setVec3("objectColor", rm->color);
        g_shader.setFloat("alpha", 1.0f);
        g_shader.setBool("wireframe", false);
        rm->draw();

        // Wireframe overlay
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        glLineWidth(0.8f);
        g_shader.setBool("wireframe", true);
        g_shader.setVec3("objectColor", glm::vec3(0.05f));
        g_shader.setFloat("alpha", 0.6f);
        rm->draw();
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFBO);
    glViewport(0, 0, g_viewportW, g_viewportH);
}

// ============================================================
//  Load mesh helpers
// ============================================================
static bool loadSrcMesh(const std::string& path) {
    ObjData obj = loadOBJ(path);
    if (!obj.valid) { addLog("ERROR: Cannot load source mesh: " + path); return false; }
    g_boolMgr.setSourceMesh(obj);
    g_srcMesh = objDataToRenderMesh(obj, "Source (A)");
    g_srcMesh->color = {0.25f, 0.55f, 1.0f};
    g_srcMesh->alpha = g_srcAlpha;
    g_srcTranslation = glm::vec3(0.0f);
    addLog("Loaded A: " + path
         + " (" + std::to_string(obj.vertices.size()/3) + " verts, "
         + std::to_string(obj.faceSizes.size()) + " faces)");
    return true;
}

static bool loadCutMesh(const std::string& path) {
    ObjData obj = loadOBJ(path);
    if (!obj.valid) { addLog("ERROR: Cannot load cut mesh: " + path); return false; }
    g_boolMgr.setCutMesh(obj);
    g_cutMesh = objDataToRenderMesh(obj, "Cut (B)");
    g_cutMesh->color = {1.0f, 0.45f, 0.15f};
    g_cutMesh->alpha = g_cutAlpha;
    g_cutTranslation = glm::vec3(0.0f);
    addLog("Loaded B: " + path
         + " (" + std::to_string(obj.vertices.size()/3) + " verts, "
         + std::to_string(obj.faceSizes.size()) + " faces)");
    return true;
}

// ============================================================
//  Export results
// ============================================================
static void exportResults() {
    if (g_boolMgr.resultMeshes.empty()) return;
    std::filesystem::create_directories(g_exportDir);
    int count = 0;
    for (size_t i = 0; i < g_boolMgr.resultMeshes.size(); ++i) {
        const auto& rm = g_boolMgr.resultMeshes[i];
        if (!rm || rm->vertices.empty()) continue;
        std::string fname = std::string(g_exportDir) + "/" + rm->label + ".obj";
        std::ofstream f(fname);
        if (!f.is_open()) { addLog("ERROR: Cannot write " + fname); continue; }
        f << "# MCUT result: " << rm->label << "\n";
        for (const auto& v : rm->vertices)
            f << std::fixed << std::setprecision(6)
              << "v " << v.position.x << " " << v.position.y << " " << v.position.z << "\n";
        for (size_t t = 0; t + 2 < rm->indices.size(); t += 3)
            f << "f " << rm->indices[t]+1 << " "
              << rm->indices[t+1]+1 << " " << rm->indices[t+2]+1 << "\n";
        ++count;
    }
    addLog("Exported " + std::to_string(count) + " mesh(es) to: " + std::string(g_exportDir) + "/");
    g_exportMsgTimer = 3.0f;
}

// ============================================================
//  Run boolean
// ============================================================
static void runBoolean() {
    if (!g_boolMgr.isContextValid() || !g_srcMesh || !g_cutMesh) return;
    pushUndo();
    g_boolMgr.setSourceTranslation(
        (double)g_srcTranslation.x, (double)g_srcTranslation.y, (double)g_srcTranslation.z);
    g_boolMgr.setCutTranslation(
        (double)g_cutTranslation.x, (double)g_cutTranslation.y, (double)g_cutTranslation.z);
    bool ok = g_boolMgr.execute((BoolOpType)g_selectedOp);
    if (ok) {
        if (g_autoHideInput) { g_showSrcMesh = false; g_showCutMesh = false; }
        rebuildThumbsFromCCs();
        if (g_autoFit) fitCameraToScene();
    }
}

// ============================================================
//  GLFW callbacks
// ============================================================
static void framebufferSizeCallback(GLFWwindow*, int w, int h) {
    g_viewportW = w; g_viewportH = h;
    glViewport(0, 0, w, h);
    g_camera.setAspectRatio((float)w / std::max(h, 1));
}

static void mouseButtonCallback(GLFWwindow* window, int button, int action, int mods) {
    if (ImGui::GetIO().WantCaptureMouse) return;

    // Use mods bitmask (accurate at callback time) instead of polling
    bool altHeld = (mods & GLFW_MOD_ALT) != 0;

    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        if (action == GLFW_PRESS) {
            g_mouseLeftDown = true;
            g_altWasHeld = altHeld;

            if (altHeld && (g_dragTarget == 1 || g_dragTarget == 2)) {
                g_isDragging = true;
                glm::mat4 view = g_camera.getViewMatrix();
                glm::mat4 proj = g_camera.getProjectionMatrix();
                glm::vec3 camPos = g_camera.getPosition();
                glm::vec3 center = (g_dragTarget == 1)
                    ? meshCenter(g_srcMesh.get(), g_srcTranslation)
                    : meshCenter(g_cutMesh.get(), g_cutTranslation);
                g_dragPlanePoint  = center;
                g_dragPlaneNormal = glm::normalize(camPos - center);
                glm::vec3 rayDir = screenToRayDir(g_lastMouseX, g_lastMouseY, proj, view);
                glm::vec3 hit;
                if (rayPlaneIntersect(camPos, rayDir, g_dragPlaneNormal, g_dragPlanePoint, hit))
                    g_dragStartWorld = hit;
                else
                    g_dragStartWorld = center;
            }
        } else {
            g_mouseLeftDown = false;
            g_altWasHeld = false;
            if (g_isDragging) {
                g_isDragging = false;
                g_needsBooleanUpdate = true;
            }
        }
    }
    if (button == GLFW_MOUSE_BUTTON_MIDDLE)
        g_mouseMiddleDown = (action == GLFW_PRESS);
}

static void cursorPosCallback(GLFWwindow* window, double x, double y) {
    if (ImGui::GetIO().WantCaptureMouse) { g_lastMouseX = x; g_lastMouseY = y; return; }
    double dx = x - g_lastMouseX;
    double dy = y - g_lastMouseY;

    if (g_mouseLeftDown && g_isDragging) {
        // Mesh drag — use latched altWasHeld, no re-poll needed
        glm::mat4 view = g_camera.getViewMatrix();
        glm::mat4 proj = g_camera.getProjectionMatrix();
        glm::vec3 camPos = g_camera.getPosition();
        glm::vec3 rayDir = screenToRayDir(x, y, proj, view);
        glm::vec3 hit;
        if (rayPlaneIntersect(camPos, rayDir, g_dragPlaneNormal, g_dragPlanePoint, hit)) {
            glm::vec3 delta = hit - g_dragStartWorld;
            if (g_dragTarget == 1) g_srcTranslation += delta;
            else                   g_cutTranslation += delta;
            g_dragStartWorld = hit;
        }
    } else if (g_mouseLeftDown && !g_isDragging) {
        g_camera.orbit((float)dx, (float)dy);
    }
    if (g_mouseMiddleDown) g_camera.pan((float)dx, (float)dy);

    g_lastMouseX = x;
    g_lastMouseY = y;
}

static void scrollCallback(GLFWwindow* window, double /*xoff*/, double yoff) {
    if (ImGui::GetIO().WantCaptureMouse) return;
    bool altHeld = (glfwGetKey(window, GLFW_KEY_LEFT_ALT)  == GLFW_PRESS ||
                    glfwGetKey(window, GLFW_KEY_RIGHT_ALT) == GLFW_PRESS);
    if (altHeld && (g_dragTarget == 1 || g_dragTarget == 2)) {
        glm::vec3 camPos  = g_camera.getPosition();
        glm::vec3 center  = (g_dragTarget == 1)
            ? meshCenter(g_srcMesh.get(), g_srcTranslation)
            : meshCenter(g_cutMesh.get(), g_cutTranslation);
        glm::vec3 forward = glm::normalize(center - camPos);
        float step = (float)yoff * g_camera.getDistance() * 0.05f;
        if (g_dragTarget == 1) g_srcTranslation += forward * step;
        else                   g_cutTranslation += forward * step;
        g_needsBooleanUpdate = true;
    } else {
        g_camera.zoom((float)yoff);
    }
}

static void keyCallback(GLFWwindow* window, int key, int /*scancode*/, int action, int mods) {
    if (action != GLFW_PRESS) return;
    if (ImGui::GetIO().WantCaptureKeyboard) return;

    bool ctrl = (mods & GLFW_MOD_CONTROL) != 0;
    if (ctrl && key == GLFW_KEY_Z) doUndo();
    if (ctrl && key == GLFW_KEY_Y) doRedo();
}

// ============================================================
//  Right-side preview panel (CC thumbnails)
// ============================================================
static void renderPreviewPanel() {
    // Panel width
    static const float kPanelW = 300.0f;
    ImGui::SetNextWindowPos({(float)g_viewportW - kPanelW, 0}, ImGuiCond_Always);
    ImGui::SetNextWindowSize({kPanelW, (float)g_viewportH}, ImGuiCond_Always);
    ImGui::Begin("Result Preview", nullptr,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

    if (!g_boolMgr.hasResult || g_boolMgr.resultMeshes.empty()) {
        ImGui::TextDisabled("No result yet.");
        ImGui::TextWrapped("Execute a boolean operation to see the result components here.");
        ImGui::End();
        return;
    }

    ImGui::Text("Components: %zu", g_boolMgr.resultMeshes.size());
    ImGui::SameLine();
    ImGui::TextDisabled("(%.1f ms)", g_boolMgr.lastDispatchTimeMs);
    ImGui::Checkbox("Auto-spin", &g_thumbAutoSpin);
    ImGui::Separator();

    // Scrollable area for thumbnails
    ImGui::BeginChild("ThumbScroll", {0, 0}, false, ImGuiWindowFlags_HorizontalScrollbar);

    float availW = ImGui::GetContentRegionAvail().x;
    // Fit thumb size to panel width (at most kThumbW)
    float thumbDisp = std::min(availW - 8.0f, (float)kThumbW);

    for (size_t i = 0; i < g_boolMgr.resultMeshes.size() && i < g_thumbs.size(); ++i) {
        auto& rm = g_boolMgr.resultMeshes[i];
        auto& t  = g_thumbs[i];

        ImGui::PushID((int)i);

        // Color swatch + label
        float col[3] = {rm->color.r, rm->color.g, rm->color.b};
        if (ImGui::ColorEdit3("##col", col,
                ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
            rm->color = {col[0], col[1], col[2]};
        }
        ImGui::SameLine();
        ImGui::TextColored({rm->color.r, rm->color.g, rm->color.b, 1.0f},
                           "%s", rm->label.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("[%zu v]", rm->vertices.size());

        // Thumbnail image (FBO texture)
        if (t.colorTex) {
            ImVec2 uv0(0, 1), uv1(1, 0);  // flip Y for OpenGL convention
            char btnId[32]; snprintf(btnId, sizeof(btnId), "##thumb%zu", i);
            bool clicked = ImGui::ImageButton(
                btnId,
                (ImTextureID)(intptr_t)t.colorTex,
                {thumbDisp, thumbDisp}, uv0, uv1,
                ImVec4(0.05f, 0.05f, 0.08f, 1.0f),  // bg
                ImVec4(1, 1, 1, 1));        // tint

            if (clicked) {
                // Focus main camera on this CC
                g_camera.fitBoundingBox(rm->bbMin, rm->bbMax);
                addLog("Focused on: " + rm->label);
            }

            // Drag to rotate thumb camera
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0)) {
                ImVec2 drag = ImGui::GetMouseDragDelta(0, 0.0f);
                ImGui::ResetMouseDragDelta(0);
                t.cam.orbit(drag.x * 0.5f, drag.y * 0.5f);
            }
        }

        // Visibility / wireframe toggles
        ImGui::Checkbox("Vis##v", &rm->visible);
        ImGui::SameLine();
        ImGui::Checkbox("Wire##w", &rm->showWire);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60);
        ImGui::SliderFloat("##a", &rm->alpha, 0.0f, 1.0f, "a=%.2f");

        // Export single CC
        if (ImGui::SmallButton("Export##exp")) {
            std::filesystem::create_directories(g_exportDir);
            std::string fname = std::string(g_exportDir) + "/" + rm->label + ".obj";
            std::ofstream f(fname);
            if (f.is_open()) {
                for (const auto& v : rm->vertices)
                    f << std::fixed << std::setprecision(6)
                      << "v " << v.position.x << " " << v.position.y << " " << v.position.z << "\n";
                for (size_t t2 = 0; t2 + 2 < rm->indices.size(); t2 += 3)
                    f << "f " << rm->indices[t2]+1 << " "
                      << rm->indices[t2+1]+1 << " " << rm->indices[t2+2]+1 << "\n";
                addLog("Exported: " + fname);
            }
        }

        ImGui::Separator();
        ImGui::PopID();
    }

    ImGui::EndChild();
    ImGui::End();
}

// ============================================================
//  Left-side control panel
// ============================================================
static void renderControlPanel(float dt) {
    static const float kPanelW = 300.0f;
    static const float kPreviewW = 300.0f;  // right panel width
    float mainW = (float)g_viewportW - kPanelW - kPreviewW;
    (void)mainW;

    ImGui::SetNextWindowPos({0, 0}, ImGuiCond_Always);
    ImGui::SetNextWindowSize({kPanelW, (float)g_viewportH}, ImGuiCond_Always);
    ImGui::Begin("Controls", nullptr,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

    // ---- Undo/Redo buttons ----
    bool canUndo = !g_undoStack.empty();
    bool canRedo = !g_redoStack.empty();
    if (!canUndo) ImGui::BeginDisabled();
    if (ImGui::Button(" Undo (Ctrl+Z) ")) doUndo();
    if (!canUndo) ImGui::EndDisabled();
    ImGui::SameLine();
    if (!canRedo) ImGui::BeginDisabled();
    if (ImGui::Button(" Redo (Ctrl+Y) ")) doRedo();
    if (!canRedo) ImGui::EndDisabled();
    ImGui::TextDisabled("History: %zu undo / %zu redo",
        g_undoStack.size(), g_redoStack.size());

    // ---- Input Meshes ----
    ImGui::SeparatorText("Input Meshes");

    ImGui::TextColored({0.4f, 0.7f, 1.0f, 1.0f}, "A  Source Mesh");
    ImGui::SetNextItemWidth(190);
    ImGui::InputText("##src", g_srcPath, sizeof(g_srcPath));
    ImGui::SameLine();
    if (ImGui::Button("Load##src")) {
        loadSrcMesh(g_srcPath);
        if (g_autoFit) fitCameraToScene();
    }
    if (g_srcMesh) {
        ImGui::Checkbox("Vis##src", &g_showSrcMesh);
        ImGui::SameLine();
        ImGui::Checkbox("Wire##src", &g_srcWireframe);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(65);
        if (ImGui::SliderFloat("##alphasrc", &g_srcAlpha, 0.0f, 1.0f, "a=%.2f"))
            g_srcMesh->alpha = g_srcAlpha;
        ImGui::SameLine();
        float col[3] = {g_srcMesh->color.r, g_srcMesh->color.g, g_srcMesh->color.b};
        if (ImGui::ColorEdit3("##colsrc", col, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel))
            g_srcMesh->color = {col[0], col[1], col[2]};
        ImGui::TextDisabled("   %zu verts  %zu tris",
            g_srcMesh->vertices.size(), g_srcMesh->indices.size()/3);
        ImGui::TextDisabled("   pos (%.2f, %.2f, %.2f)",
            g_srcTranslation.x, g_srcTranslation.y, g_srcTranslation.z);
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##posA")) {
            g_srcTranslation = glm::vec3(0.0f);
            g_needsBooleanUpdate = true;
        }
    }

    ImGui::Spacing();
    ImGui::TextColored({1.0f, 0.6f, 0.2f, 1.0f}, "B  Cut Mesh");
    ImGui::SetNextItemWidth(190);
    ImGui::InputText("##cut", g_cutPath, sizeof(g_cutPath));
    ImGui::SameLine();
    if (ImGui::Button("Load##cut")) {
        loadCutMesh(g_cutPath);
        if (g_autoFit) fitCameraToScene();
    }
    if (g_cutMesh) {
        ImGui::Checkbox("Vis##cut", &g_showCutMesh);
        ImGui::SameLine();
        ImGui::Checkbox("Wire##cut", &g_cutWireframe);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(65);
        if (ImGui::SliderFloat("##alphacut", &g_cutAlpha, 0.0f, 1.0f, "a=%.2f"))
            g_cutMesh->alpha = g_cutAlpha;
        ImGui::SameLine();
        float col[3] = {g_cutMesh->color.r, g_cutMesh->color.g, g_cutMesh->color.b};
        if (ImGui::ColorEdit3("##colcut", col, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel))
            g_cutMesh->color = {col[0], col[1], col[2]};
        ImGui::TextDisabled("   %zu verts  %zu tris",
            g_cutMesh->vertices.size(), g_cutMesh->indices.size()/3);
        ImGui::TextDisabled("   pos (%.2f, %.2f, %.2f)",
            g_cutTranslation.x, g_cutTranslation.y, g_cutTranslation.z);
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##posB")) {
            g_cutTranslation = glm::vec3(0.0f);
            g_needsBooleanUpdate = true;
        }
    }

    // ---- Drag Control ----
    ImGui::Spacing();
    ImGui::SeparatorText("Drag Control");
    ImGui::Text("Target:");
    ImGui::SameLine();
    bool selA = (g_dragTarget == 1);
    bool selB = (g_dragTarget == 2);
    if (selA) ImGui::PushStyleColor(ImGuiCol_Button, {0.15f, 0.40f, 0.80f, 1.0f});
    if (ImGui::Button("  A  ##dragA")) g_dragTarget = 1;
    if (selA) ImGui::PopStyleColor();
    ImGui::SameLine();
    if (selB) ImGui::PushStyleColor(ImGuiCol_Button, {0.70f, 0.30f, 0.05f, 1.0f});
    if (ImGui::Button("  B  ##dragB")) g_dragTarget = 2;
    if (selB) ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.9f, 0.7f, 1.0f));
    ImGui::TextWrapped("Alt+Left drag: move mesh  |  Alt+Scroll: depth");
    ImGui::PopStyleColor();
    if (g_isDragging)
        ImGui::TextColored({1.0f, 0.85f, 0.2f, 1.0f}, "Dragging %s ...",
                           g_dragTarget == 1 ? "A" : "B");

    // ---- Boolean Operation ----
    ImGui::Spacing();
    ImGui::SeparatorText("Boolean Operation");
    static const char* opDescs[] = {
        "A Union B        (A | B)",
        "A Intersect B    (A & B)",
        "Difference A - B",
        "Difference B - A",
        "All Fragments",
    };
    for (int i = 0; i < 5; ++i)
        ImGui::RadioButton(opDescs[i], &g_selectedOp, i);

    ImGui::Spacing();
    ImGui::Checkbox("Auto-hide inputs after run", &g_autoHideInput);

    ImGui::Spacing();
    bool canRun = g_boolMgr.isContextValid() && g_srcMesh && g_cutMesh;
    if (!canRun) ImGui::BeginDisabled();
    ImGui::PushStyleColor(ImGuiCol_Button,        {0.15f, 0.50f, 0.85f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.25f, 0.65f, 1.00f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  {0.10f, 0.40f, 0.70f, 1.0f});
    if (ImGui::Button("  Execute Boolean  ", {-1, 32})) {
        addLog("--- Running " + std::string(BoolOpNames[g_selectedOp]) + " ---");
        runBoolean();
    }
    ImGui::PopStyleColor(3);
    if (!canRun) ImGui::EndDisabled();

    if (!g_boolMgr.statusMessage.empty()) {
        bool isErr = g_boolMgr.statusMessage.find("ERROR") != std::string::npos;
        if (isErr)
            ImGui::TextColored({1.0f, 0.3f, 0.3f, 1.0f}, "%s", g_boolMgr.statusMessage.c_str());
        else
            ImGui::TextColored({0.4f, 1.0f, 0.4f, 1.0f}, "%s", g_boolMgr.statusMessage.c_str());
    }

    // ---- Export ----
    if (g_boolMgr.hasResult) {
        ImGui::Spacing();
        ImGui::SeparatorText("Export All");
        ImGui::SetNextItemWidth(160);
        ImGui::InputText("##expdir", g_exportDir, sizeof(g_exportDir));
        ImGui::SameLine();
        if (ImGui::Button("Export OBJ")) exportResults();
        if (g_exportMsgTimer > 0.0f) {
            g_exportMsgTimer -= dt;
            ImGui::TextColored({0.3f, 1.0f, 0.5f, 1.0f}, "Exported!");
        }
    }

    // ---- Presets ----
    ImGui::Spacing();
    ImGui::SeparatorText("Presets");
    auto loadPreset = [&](const char* a, const char* b) {
        strncpy(g_srcPath, a, sizeof(g_srcPath));
        strncpy(g_cutPath, b, sizeof(g_cutPath));
        loadSrcMesh(g_srcPath);
        loadCutMesh(g_cutPath);
        g_showSrcMesh = g_showCutMesh = true;
        g_boolMgr.hasResult = false;
        g_boolMgr.resultMeshes.clear();
        destroyThumbs();
        if (g_autoFit) fitCameraToScene();
    };
    if (ImGui::Button("Cube+Torus"))
        loadPreset("assets/meshes/cube.obj", "assets/meshes/torus.obj");
    ImGui::SameLine();
    if (ImGui::Button("Cube+Plane"))
        loadPreset("assets/meshes/cube.obj", "assets/meshes/plane.obj");
    ImGui::SameLine();
    if (ImGui::Button("Sphere+Cube"))
        loadPreset("assets/meshes/sphere.obj", "assets/meshes/cube.obj");
    if (ImGui::Button("Sphere+Sphere"))
        loadPreset("assets/meshes/sphere.obj", "assets/meshes/sphere_small.obj");
    ImGui::SameLine();
    if (ImGui::Button("Sphere+Cyl"))
        loadPreset("assets/meshes/sphere.obj", "assets/meshes/cylinder.obj");
    ImGui::SameLine();
    if (ImGui::Button("Cube+Cube"))
        loadPreset("assets/meshes/cube.obj", "assets/meshes/cube2.obj");

    // ---- View ----
    ImGui::Spacing();
    ImGui::SeparatorText("View");
    ImGui::Checkbox("Grid", &g_showGrid);
    ImGui::SameLine();
    ImGui::Checkbox("Auto-Fit", &g_autoFit);
    ImGui::SameLine();
    if (ImGui::Button("Fit")) fitCameraToScene();
    ImGui::SameLine();
    if (ImGui::Button("Reset Cam")) g_camera.reset();

    // ---- Log ----
    ImGui::Spacing();
    ImGui::SeparatorText("Log");
    float logH = ImGui::GetContentRegionAvail().y - 4;
    if (logH < 60) logH = 60;
    ImGui::BeginChild("LogArea", {0, logH}, true, ImGuiWindowFlags_HorizontalScrollbar);
    for (auto& line : g_log) {
        bool isErr = line.find("ERROR") != std::string::npos;
        bool isOk  = line.find("Loaded") != std::string::npos
                  || line.find("Exported") != std::string::npos
                  || line.find("initialized") != std::string::npos
                  || line.find("Undo") != std::string::npos
                  || line.find("Redo") != std::string::npos;
        if (isErr)
            ImGui::TextColored({1.0f, 0.4f, 0.4f, 1.0f}, "%s", line.c_str());
        else if (isOk)
            ImGui::TextColored({0.5f, 1.0f, 0.5f, 1.0f}, "%s", line.c_str());
        else
            ImGui::TextUnformatted(line.c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();

    ImGui::End();
}

// ============================================================
//  Main
// ============================================================
int main(int /*argc*/, char** /*argv*/) {
    if (!glfwInit()) { fprintf(stderr, "Failed to init GLFW\n"); return 1; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_SAMPLES, 4);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    GLFWwindow* window = glfwCreateWindow(
        g_viewportW, g_viewportH, "MCUT Boolean Viewer", nullptr, nullptr);
    if (!window) { fprintf(stderr, "Failed to create window\n"); glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    if (!gladLoadGL(glfwGetProcAddress)) { fprintf(stderr, "Failed to init GLAD\n"); return 1; }

    glfwSetFramebufferSizeCallback(window, framebufferSizeCallback);
    glfwSetMouseButtonCallback(window, mouseButtonCallback);
    glfwSetCursorPosCallback(window, cursorPosCallback);
    glfwSetScrollCallback(window, scrollCallback);
    glfwSetKeyCallback(window, keyCallback);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding  = 5.0f;
    style.FrameRounding   = 4.0f;
    style.GrabRounding    = 3.0f;
    style.ScrollbarRounding = 3.0f;
    style.Colors[ImGuiCol_WindowBg]      = ImVec4(0.09f, 0.09f, 0.11f, 0.96f);
    style.Colors[ImGuiCol_FrameBg]       = ImVec4(0.16f, 0.16f, 0.20f, 1.00f);
    style.Colors[ImGuiCol_Header]        = ImVec4(0.20f, 0.40f, 0.70f, 0.60f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.25f, 0.50f, 0.85f, 0.80f);
    style.Colors[ImGuiCol_CheckMark]     = ImVec4(0.40f, 0.80f, 1.00f, 1.00f);
    style.Colors[ImGuiCol_SliderGrab]    = ImVec4(0.30f, 0.65f, 1.00f, 1.00f);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_MULTISAMPLE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    if (!g_shader.loadFromSource(kVertSrc, kFragSrc)) {
        fprintf(stderr, "Shader compile failed\n"); return 1;
    }

    buildGrid(8.0f, 32);
    g_camera.setAspectRatio((float)g_viewportW / g_viewportH);

    g_boolMgr.logCallback = addLog;
    if (!g_boolMgr.init()) {
        addLog("ERROR: Failed to initialize MCUT context!");
    } else {
        addLog("MCUT context initialized.");
    }
    addLog("Ctrl+Z: Undo  |  Ctrl+Y: Redo  |  Alt+drag: move mesh");

    if (loadSrcMesh(g_srcPath) && loadCutMesh(g_cutPath))
        fitCameraToScene();

    // ============================================================
    //  Main loop
    // ============================================================
    double lastTime = glfwGetTime();
    while (!glfwWindowShouldClose(window)) {
        double now = glfwGetTime();
        float dt = (float)(now - lastTime);
        lastTime = now;

        glfwPollEvents();

        // Deferred boolean update (after drag release)
        if (g_needsBooleanUpdate && !g_isDragging) {
            g_needsBooleanUpdate = false;
            if (g_boolMgr.isContextValid() && g_srcMesh && g_cutMesh) {
                addLog("--- Auto-update after drag ---");
                runBoolean();
            }
        }

        // Auto-spin CC thumbnails
        if (g_thumbAutoSpin) {
            for (auto& t : g_thumbs)
                t.rotY += 30.0f * dt;  // 30 deg/s
        }

        // Sync visibility flags
        if (g_srcMesh) { g_srcMesh->visible = g_showSrcMesh; g_srcMesh->showWire = g_srcWireframe; }
        if (g_cutMesh) { g_cutMesh->visible = g_showCutMesh; g_cutMesh->showWire = g_cutWireframe; }

        // ---- Render CC thumbnails into FBOs ----
        renderThumbs();

        // ---- Main viewport ----
        // Compute main viewport rect (between left panel and right panel)
        static const float kLeftW  = 300.0f;
        static const float kRightW = 300.0f;
        int mainX = (int)kLeftW;
        int mainW = g_viewportW - (int)kLeftW - (int)kRightW;
        int mainH = g_viewportH;

        glViewport(mainX, 0, mainW, mainH);
        glScissor(mainX, 0, mainW, mainH);
        glEnable(GL_SCISSOR_TEST);
        glClearColor(0.12f, 0.12f, 0.14f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // Update camera aspect ratio for main viewport
        g_camera.setAspectRatio((float)mainW / std::max(mainH, 1));

        glm::mat4 view    = g_camera.getViewMatrix();
        glm::mat4 proj    = g_camera.getProjectionMatrix();
        glm::vec3 viewPos = g_camera.getPosition();

        if (g_showGrid) drawGrid(view, proj, viewPos);

        glm::mat4 srcModel = glm::translate(glm::mat4(1.0f), g_srcTranslation);
        glm::mat4 cutModel = glm::translate(glm::mat4(1.0f), g_cutTranslation);

        if (g_srcMesh && g_showSrcMesh) {
            float savedAlpha = g_srcMesh->alpha;
            if (g_dragTarget == 1 && g_isDragging)
                g_srcMesh->alpha = std::min(1.0f, savedAlpha + 0.25f);
            drawMesh(*g_srcMesh, view, proj, viewPos, srcModel, g_srcWireframe);
            g_srcMesh->alpha = savedAlpha;
        }
        if (g_cutMesh && g_showCutMesh) {
            float savedAlpha = g_cutMesh->alpha;
            if (g_dragTarget == 2 && g_isDragging)
                g_cutMesh->alpha = std::min(1.0f, savedAlpha + 0.25f);
            drawMesh(*g_cutMesh, view, proj, viewPos, cutModel, g_cutWireframe);
            g_cutMesh->alpha = savedAlpha;
        }
        if (g_showResult) {
            for (auto& rm : g_boolMgr.resultMeshes)
                drawMesh(*rm, view, proj, viewPos, glm::mat4(1.0f), rm->showWire);
        }

        glDisable(GL_SCISSOR_TEST);
        glViewport(0, 0, g_viewportW, g_viewportH);

        // ---- ImGui ----
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        renderControlPanel(dt);
        renderPreviewPanel();

        // Help overlay
        ImGui::SetNextWindowPos({300 + 8.0f, 8.0f}, ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.50f);
        ImGui::Begin("##help", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove);
        ImGui::TextColored({0.9f, 0.9f, 0.9f, 1.0f}, "Mouse:");
        ImGui::Text("  Left drag        : Orbit");
        ImGui::Text("  Middle drag      : Pan");
        ImGui::Text("  Scroll           : Zoom");
        ImGui::Separator();
        ImGui::TextColored({1.0f, 0.85f, 0.3f, 1.0f}, "Mesh Drag (Alt held):");
        ImGui::Text("  Alt+Left drag    : Move mesh A or B");
        ImGui::Text("  Alt+Scroll       : Move along depth");
        ImGui::Separator();
        ImGui::TextColored({0.5f, 1.0f, 0.5f, 1.0f}, "Keyboard:");
        ImGui::Text("  Ctrl+Z / Ctrl+Y  : Undo / Redo");
        ImGui::End();

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    // Cleanup
    destroyThumbs();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    g_srcMesh.reset();
    g_cutMesh.reset();
    if (g_gridVAO) { glDeleteVertexArrays(1, &g_gridVAO); glDeleteBuffers(1, &g_gridVBO); }
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
