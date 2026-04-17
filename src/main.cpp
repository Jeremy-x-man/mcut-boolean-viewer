/**
 * @file main.cpp
 * @brief MCUT Boolean Operation Viewer
 *
 * An interactive 3D visualization tool for previewing mesh boolean operations
 * powered by the MCUT library, rendered with OpenGL 3.3 and ImGui.
 *
 * Features:
 *  - Load OBJ meshes as Source Mesh (A) and Cut Mesh (B)
 *  - Execute Union / Intersection / Difference (A-B) / Difference (B-A) / All Fragments
 *  - Real-time 3D preview with arcball camera (drag to rotate, scroll to zoom)
 *  - Toggle visibility and wireframe overlay per mesh
 *  - Color-coded result connected components with type labels
 *  - Export result connected components to OBJ
 *  - Log panel showing MCUT debug output
 */

// ---- OpenGL / Window ----
#include <glad/gl.h>
#include <GLFW/glfw3.h>

// ---- GLM ----
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

// ---- ImGui ----
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

// ---- Project headers ----
#include "RenderMesh.h"
#include "ObjLoader.h"
#include "BooleanOp.h"
#include "Camera.h"
#include "Shader.h"

// ---- STL ----
#include <string>
#include <vector>
#include <memory>
#include <deque>
#include <algorithm>
#include <filesystem>
#include <cstring>
#include <fstream>
#include <sstream>
#include <iomanip>

// ============================================================
//  Global state
// ============================================================
static Camera g_camera;
static bool   g_mouseLeftDown   = false;
static bool   g_mouseMiddleDown = false;
static double g_lastMouseX = 0, g_lastMouseY = 0;
static int    g_viewportW = 1280, g_viewportH = 720;

// Mesh storage
static std::unique_ptr<RenderMesh> g_srcMesh;
static std::unique_ptr<RenderMesh> g_cutMesh;

// Boolean manager
static BooleanOpManager g_boolMgr;

// Log
static std::deque<std::string> g_log;
static void addLog(const std::string& msg) {
    g_log.push_back(msg);
    if (g_log.size() > 300) g_log.pop_front();
}

// UI state
static char  g_srcPath[512] = "assets/meshes/cube.obj";
static char  g_cutPath[512] = "assets/meshes/torus.obj";
static int   g_selectedOp   = 0;  // BoolOpType index
static bool  g_showSrcMesh  = true;
static bool  g_showCutMesh  = true;
static bool  g_showResult   = true;
static bool  g_srcWireframe = false;
static bool  g_cutWireframe = false;
static float g_srcAlpha     = 0.45f;
static float g_cutAlpha     = 0.45f;
static bool  g_showGrid     = true;
static bool  g_autoFit      = true;
static bool  g_autoHideInput = false;  // hide input meshes after running boolean
static char  g_exportDir[512] = "exports";
static bool  g_exportSuccess  = false;
static float g_exportMsgTimer = 0.0f;

// Shader
static Shader g_shader;

// ============================================================
//  Inline GLSL sources (embedded to avoid file path issues)
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

void main()
{
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

void main()
{
    if (wireframe) {
        FragColor = vec4(objectColor * 0.25, alpha * 0.7);
        return;
    }
    // Two-sided Phong shading
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
//  Grid rendering
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
//  Render a RenderMesh
// ============================================================
static void drawMesh(const RenderMesh& mesh,
                     const glm::mat4& view, const glm::mat4& proj,
                     const glm::vec3& viewPos,
                     bool wireOverlay = false)
{
    if (!mesh.visible || mesh.VAO == 0 || mesh.indices.empty()) return;

    g_shader.use();
    glm::mat4 model(1.0f);
    glm::mat3 normalMat = glm::mat3(glm::transpose(glm::inverse(model)));

    g_shader.setMat4("model", model);
    g_shader.setMat4("view", view);
    g_shader.setMat4("projection", proj);
    g_shader.setMat3("normalMatrix", normalMat);
    g_shader.setVec3("lightPos", glm::vec3(10.0f, 20.0f, 10.0f));
    g_shader.setVec3("viewPos", viewPos);
    g_shader.setVec3("objectColor", mesh.color);
    g_shader.setFloat("alpha", mesh.alpha);
    g_shader.setBool("wireframe", false);

    // Solid pass
    mesh.draw();

    // Wireframe overlay
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
//  Load mesh helpers
// ============================================================
static bool loadSrcMesh(const std::string& path) {
    ObjData obj = loadOBJ(path);
    if (!obj.valid) { addLog("ERROR: Cannot load source mesh: " + path); return false; }
    g_boolMgr.setSourceMesh(obj);
    g_srcMesh = objDataToRenderMesh(obj, "Source (A)");
    g_srcMesh->color = {0.25f, 0.55f, 1.0f};
    g_srcMesh->alpha = g_srcAlpha;
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
    addLog("Loaded B: " + path
         + " (" + std::to_string(obj.vertices.size()/3) + " verts, "
         + std::to_string(obj.faceSizes.size()) + " faces)");
    return true;
}

static void fitCameraToScene() {
    glm::vec3 minBB(1e9f), maxBB(-1e9f);
    auto expand = [&](const RenderMesh* m) {
        if (m && m->visible) {
            minBB = glm::min(minBB, m->bbMin);
            maxBB = glm::max(maxBB, m->bbMax);
        }
    };
    expand(g_srcMesh.get());
    expand(g_cutMesh.get());
    for (auto& rm : g_boolMgr.resultMeshes) expand(rm.get());
    if (minBB.x < 1e8f) g_camera.fitBoundingBox(minBB, maxBB);
}

// ============================================================
//  Export result CCs to OBJ
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
        f << "# " << rm->vertices.size() << " vertices, "
          << rm->indices.size()/3 << " triangles\n";
        for (const auto& v : rm->vertices)
            f << std::fixed << std::setprecision(6)
              << "v " << v.position.x << " " << v.position.y << " " << v.position.z << "\n";
        for (size_t t = 0; t + 2 < rm->indices.size(); t += 3)
            f << "f " << rm->indices[t]+1 << " "
              << rm->indices[t+1]+1 << " " << rm->indices[t+2]+1 << "\n";
        ++count;
    }
    addLog("Exported " + std::to_string(count) + " mesh(es) to: " + std::string(g_exportDir) + "/");
    g_exportSuccess = true;
    g_exportMsgTimer = 3.0f;
}

// ============================================================
//  GLFW callbacks
// ============================================================
static void framebufferSizeCallback(GLFWwindow*, int w, int h) {
    g_viewportW = w; g_viewportH = h;
    glViewport(0, 0, w, h);
    g_camera.setAspectRatio((float)w / std::max(h, 1));
}

static void mouseButtonCallback(GLFWwindow*, int button, int action, int /*mods*/) {
    if (ImGui::GetIO().WantCaptureMouse) return;
    if (button == GLFW_MOUSE_BUTTON_LEFT)
        g_mouseLeftDown = (action == GLFW_PRESS);
    if (button == GLFW_MOUSE_BUTTON_MIDDLE)
        g_mouseMiddleDown = (action == GLFW_PRESS);
}

static void cursorPosCallback(GLFWwindow*, double x, double y) {
    if (ImGui::GetIO().WantCaptureMouse) { g_lastMouseX = x; g_lastMouseY = y; return; }
    double dx = x - g_lastMouseX;
    double dy = y - g_lastMouseY;
    if (g_mouseLeftDown)   g_camera.orbit((float)dx, (float)dy);
    if (g_mouseMiddleDown) g_camera.pan((float)dx, (float)dy);
    g_lastMouseX = x; g_lastMouseY = y;
}

static void scrollCallback(GLFWwindow*, double /*xoff*/, double yoff) {
    if (ImGui::GetIO().WantCaptureMouse) return;
    g_camera.zoom((float)yoff);
}

// ============================================================
//  ImGui UI
// ============================================================
static void renderUI(float dt) {
    // ---- Left panel: Controls ----
    ImGui::SetNextWindowPos({0, 0}, ImGuiCond_Always);
    ImGui::SetNextWindowSize({310, (float)g_viewportH}, ImGuiCond_Always);
    ImGui::Begin("MCUT Boolean Viewer", nullptr,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

    // ---- Section: Input Meshes ----
    ImGui::SeparatorText("Input Meshes");

    // Source mesh
    ImGui::TextColored({0.4f, 0.7f, 1.0f, 1.0f}, "A  Source Mesh");
    ImGui::SetNextItemWidth(200);
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
        ImGui::SetNextItemWidth(70);
        if (ImGui::SliderFloat("##alphasrc", &g_srcAlpha, 0.0f, 1.0f, "a=%.2f"))
            g_srcMesh->alpha = g_srcAlpha;
        ImGui::SameLine();
        float col[3] = {g_srcMesh->color.r, g_srcMesh->color.g, g_srcMesh->color.b};
        if (ImGui::ColorEdit3("##colsrc", col, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel))
            g_srcMesh->color = {col[0], col[1], col[2]};
        ImGui::TextDisabled("   %zu verts  %zu tris",
            g_srcMesh->vertices.size(), g_srcMesh->indices.size()/3);
    }

    ImGui::Spacing();

    // Cut mesh
    ImGui::TextColored({1.0f, 0.6f, 0.2f, 1.0f}, "B  Cut Mesh");
    ImGui::SetNextItemWidth(200);
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
        ImGui::SetNextItemWidth(70);
        if (ImGui::SliderFloat("##alphacut", &g_cutAlpha, 0.0f, 1.0f, "a=%.2f"))
            g_cutMesh->alpha = g_cutAlpha;
        ImGui::SameLine();
        float col[3] = {g_cutMesh->color.r, g_cutMesh->color.g, g_cutMesh->color.b};
        if (ImGui::ColorEdit3("##colcut", col, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel))
            g_cutMesh->color = {col[0], col[1], col[2]};
        ImGui::TextDisabled("   %zu verts  %zu tris",
            g_cutMesh->vertices.size(), g_cutMesh->indices.size()/3);
    }

    // ---- Section: Boolean Operation ----
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
        bool ok = g_boolMgr.execute((BoolOpType)g_selectedOp);
        if (ok) {
            if (g_autoHideInput) {
                g_showSrcMesh = false;
                g_showCutMesh = false;
            }
            if (g_autoFit) fitCameraToScene();
        }
    }
    ImGui::PopStyleColor(3);
    if (!canRun) ImGui::EndDisabled();

    // Status message
    if (!g_boolMgr.statusMessage.empty()) {
        bool isErr = g_boolMgr.statusMessage.find("ERROR") != std::string::npos
                  || g_boolMgr.statusMessage.find("failed") != std::string::npos;
        if (isErr)
            ImGui::TextColored({1.0f, 0.3f, 0.3f, 1.0f}, "%s", g_boolMgr.statusMessage.c_str());
        else
            ImGui::TextColored({0.4f, 1.0f, 0.4f, 1.0f}, "%s", g_boolMgr.statusMessage.c_str());
    }

    // ---- Section: Results ----
    if (g_boolMgr.hasResult) {
        ImGui::Spacing();
        ImGui::SeparatorText("Results");

        ImGui::Text("Time: %.2f ms", g_boolMgr.lastDispatchTimeMs);
        ImGui::SameLine();
        ImGui::TextDisabled(" | %zu component(s)", g_boolMgr.resultMeshes.size());

        ImGui::Checkbox("Show Results##res", &g_showResult);
        ImGui::SameLine();
        if (ImGui::Button("Fit##res")) fitCameraToScene();
        ImGui::SameLine();
        if (ImGui::Button("All On")) {
            for (auto& rm : g_boolMgr.resultMeshes) rm->visible = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("All Off")) {
            for (auto& rm : g_boolMgr.resultMeshes) rm->visible = false;
        }

        float listH = std::min((float)g_boolMgr.resultMeshes.size() * 52.0f + 8.0f, 220.0f);
        ImGui::BeginChild("CCList", {0, listH}, true);
        for (size_t i = 0; i < g_boolMgr.resultMeshes.size(); ++i) {
            auto& rm = g_boolMgr.resultMeshes[i];
            ImGui::PushID((int)i);

            // Color swatch
            float col[3] = {rm->color.r, rm->color.g, rm->color.b};
            if (ImGui::ColorEdit3("##col", col, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel))
                rm->color = {col[0], col[1], col[2]};
            ImGui::SameLine();

            // Visibility checkbox with label
            ImGui::Checkbox(rm->label.c_str(), &rm->visible);
            ImGui::SameLine();

            // Wireframe
            ImGui::Checkbox("W##w", &rm->showWire);
            ImGui::SameLine();

            // Alpha slider
            ImGui::SetNextItemWidth(55);
            ImGui::SliderFloat("##a", &rm->alpha, 0.0f, 1.0f, "%.2f");

            // Mesh stats
            ImGui::TextDisabled("   %zu v  %zu t",
                rm->vertices.size(), rm->indices.size()/3);
            ImGui::PopID();
        }
        ImGui::EndChild();

        // Export
        ImGui::Spacing();
        ImGui::SetNextItemWidth(160);
        ImGui::InputText("##expdir", g_exportDir, sizeof(g_exportDir));
        ImGui::SameLine();
        if (ImGui::Button("Export OBJ")) {
            exportResults();
        }
        if (g_exportMsgTimer > 0.0f) {
            g_exportMsgTimer -= dt;
            ImGui::TextColored({0.3f, 1.0f, 0.5f, 1.0f}, "Exported!");
        }
    }

    // ---- Section: Presets ----
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

    // ---- Section: View ----
    ImGui::Spacing();
    ImGui::SeparatorText("View");
    ImGui::Checkbox("Grid", &g_showGrid);
    ImGui::SameLine();
    ImGui::Checkbox("Auto-Fit", &g_autoFit);
    ImGui::SameLine();
    if (ImGui::Button("Fit")) fitCameraToScene();
    ImGui::SameLine();
    if (ImGui::Button("Reset Cam")) g_camera.reset();

    // ---- Section: Log ----
    ImGui::Spacing();
    ImGui::SeparatorText("Log");
    float logH = ImGui::GetContentRegionAvail().y - 4;
    if (logH < 60) logH = 60;
    ImGui::BeginChild("LogArea", {0, logH}, true,
        ImGuiWindowFlags_HorizontalScrollbar);
    for (auto& line : g_log) {
        bool isErr = line.find("ERROR") != std::string::npos;
        bool isOk  = line.find("Loaded") != std::string::npos
                  || line.find("Exported") != std::string::npos
                  || line.find("initialized") != std::string::npos;
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

    // ---- Help overlay (top-right) ----
    ImGui::SetNextWindowPos({(float)g_viewportW - 8, 8}, ImGuiCond_Always, {1, 0});
    ImGui::SetNextWindowBgAlpha(0.50f);
    ImGui::Begin("##help", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove);
    ImGui::TextColored({0.8f, 0.8f, 0.8f, 1.0f}, "Mouse Controls:");
    ImGui::Text("  Left drag   : Orbit");
    ImGui::Text("  Middle drag : Pan");
    ImGui::Text("  Scroll      : Zoom");
    ImGui::End();
}

// ============================================================
//  Main
// ============================================================
int main(int argc, char** argv) {
    // ---- GLFW init ----
    if (!glfwInit()) {
        fprintf(stderr, "Failed to init GLFW\n");
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_SAMPLES, 4);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    GLFWwindow* window = glfwCreateWindow(
        g_viewportW, g_viewportH, "MCUT Boolean Viewer", nullptr, nullptr);
    if (!window) {
        fprintf(stderr, "Failed to create GLFW window\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    // ---- GLAD ----
    if (!gladLoadGL(glfwGetProcAddress)) {
        fprintf(stderr, "Failed to init GLAD\n");
        return 1;
    }

    // ---- Callbacks ----
    glfwSetFramebufferSizeCallback(window, framebufferSizeCallback);
    glfwSetMouseButtonCallback(window, mouseButtonCallback);
    glfwSetCursorPosCallback(window, cursorPosCallback);
    glfwSetScrollCallback(window, scrollCallback);

    // ---- ImGui ----
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
    style.Colors[ImGuiCol_WindowBg]     = ImVec4(0.09f, 0.09f, 0.11f, 0.96f);
    style.Colors[ImGuiCol_FrameBg]      = ImVec4(0.16f, 0.16f, 0.20f, 1.00f);
    style.Colors[ImGuiCol_Header]       = ImVec4(0.20f, 0.40f, 0.70f, 0.60f);
    style.Colors[ImGuiCol_HeaderHovered]= ImVec4(0.25f, 0.50f, 0.85f, 0.80f);
    style.Colors[ImGuiCol_CheckMark]    = ImVec4(0.40f, 0.80f, 1.00f, 1.00f);
    style.Colors[ImGuiCol_SliderGrab]   = ImVec4(0.30f, 0.65f, 1.00f, 1.00f);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    // ---- OpenGL state ----
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_MULTISAMPLE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // ---- Shader ----
    if (!g_shader.loadFromSource(kVertSrc, kFragSrc)) {
        fprintf(stderr, "Shader compile failed\n");
        return 1;
    }

    // ---- Grid ----
    buildGrid(8.0f, 32);

    // ---- Camera ----
    g_camera.setAspectRatio((float)g_viewportW / g_viewportH);

    // ---- MCUT init (must be after OpenGL context creation) ----
    g_boolMgr.logCallback = addLog;
    if (!g_boolMgr.init()) {
        addLog("ERROR: Failed to initialize MCUT context!");
    } else {
        addLog("MCUT context initialized successfully.");
    }
    addLog("MCUT Boolean Viewer  |  Drag to orbit  |  Scroll to zoom");

    // ---- Default meshes ----
    if (loadSrcMesh(g_srcPath) && loadCutMesh(g_cutPath)) {
        fitCameraToScene();
    }

    // ============================================================
    //  Main loop
    // ============================================================
    double lastTime = glfwGetTime();
    while (!glfwWindowShouldClose(window)) {
        double now = glfwGetTime();
        float dt = (float)(now - lastTime);
        lastTime = now;

        glfwPollEvents();

        // Sync visibility flags
        if (g_srcMesh) { g_srcMesh->visible = g_showSrcMesh; g_srcMesh->showWire = g_srcWireframe; }
        if (g_cutMesh) { g_cutMesh->visible = g_showCutMesh; g_cutMesh->showWire = g_cutWireframe; }

        // ---- Render ----
        glViewport(0, 0, g_viewportW, g_viewportH);
        glClearColor(0.12f, 0.12f, 0.14f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glm::mat4 view    = g_camera.getViewMatrix();
        glm::mat4 proj    = g_camera.getProjectionMatrix();
        glm::vec3 viewPos = g_camera.getPosition();

        if (g_showGrid) drawGrid(view, proj, viewPos);

        // Draw source mesh (semi-transparent)
        if (g_srcMesh && g_showSrcMesh)
            drawMesh(*g_srcMesh, view, proj, viewPos, g_srcWireframe);

        // Draw cut mesh (semi-transparent)
        if (g_cutMesh && g_showCutMesh)
            drawMesh(*g_cutMesh, view, proj, viewPos, g_cutWireframe);

        // Draw result meshes
        if (g_showResult) {
            for (auto& rm : g_boolMgr.resultMeshes)
                drawMesh(*rm, view, proj, viewPos, rm->showWire);
        }

        // ---- ImGui ----
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        renderUI(dt);

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    // ---- Cleanup ----
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
