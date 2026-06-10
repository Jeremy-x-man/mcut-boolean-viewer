#pragma once
// =============================================================================
// SlicerRenderer.h  —  OpenGL renderer for slicer output
// =============================================================================
// Provides two rendering modes:
//   1. Layer-by-layer 2D preview (FBO → ImGui::Image thumbnail per layer)
//   2. 3D slice path visualization (colored line segments in main viewport)
//
// Color coding:
//   Contour (raw intersection)  : orange
//   Perimeter shell 0 (outer)   : white
//   Perimeter shell 1+ (inner)  : cyan
//   Solid fill                  : yellow
//   Infill                      : green
//   Support                     : magenta / purple
//   Skirt                       : light blue
//   Raft                        : brown / tan
//   Travel moves                : gray (thin, optional)
// =============================================================================

#include "SlicerEngine.h"
#include "GcodeExporter.h"
#include "Shader.h"
#include <glad/gl.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <vector>
#include <string>

// ---------------------------------------------------------------------------
// GPU line buffer (VAO/VBO for colored line segments)
// ---------------------------------------------------------------------------
struct LineBuffer {
    GLuint vao = 0, vbo = 0;
    int    count = 0;

    struct Vertex { glm::vec3 pos; glm::vec3 col; };

    void upload(const std::vector<Vertex>& verts) {
        if (verts.empty()) { count = 0; return; }
        if (!vao) { glGenVertexArrays(1, &vao); glGenBuffers(1, &vbo); }
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(Vertex), verts.data(), GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, col));
        glBindVertexArray(0);
        count = (int)verts.size();
    }

    void draw(GLenum mode = GL_LINES) const {
        if (!vao || count == 0) return;
        glBindVertexArray(vao);
        glDrawArrays(mode, 0, count);
        glBindVertexArray(0);
    }

    void destroy() {
        if (vao) { glDeleteVertexArrays(1, &vao); glDeleteBuffers(1, &vbo); vao = vbo = 0; count = 0; }
    }
};

// ---------------------------------------------------------------------------
// Per-layer FBO thumbnail
// ---------------------------------------------------------------------------
struct LayerThumb {
    GLuint fbo = 0, colorTex = 0, depthRbo = 0;
    int    w = 256, h = 256;
    bool   dirty = true;

    void init(int width = 256, int height = 256) {
        w = width; h = height;
        if (!fbo) {
            glGenFramebuffers(1, &fbo);
            glGenTextures(1, &colorTex);
            glGenRenderbuffers(1, &depthRbo);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glBindTexture(GL_TEXTURE_2D, colorTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex, 0);
        glBindRenderbuffer(GL_RENDERBUFFER, depthRbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthRbo);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        dirty = true;
    }

    void destroy() {
        if (fbo) { glDeleteFramebuffers(1, &fbo); glDeleteTextures(1, &colorTex);
                   glDeleteRenderbuffers(1, &depthRbo); fbo = colorTex = depthRbo = 0; }
    }
};

// ---------------------------------------------------------------------------
// SlicerRenderer
// ---------------------------------------------------------------------------
class SlicerRenderer {
public:
    // ---- Color palette ----
    static constexpr glm::vec3 COL_OUTER_SHELL    = {1.0f, 1.0f, 1.0f};
    static constexpr glm::vec3 COL_INNER_SHELL    = {0.4f, 0.9f, 1.0f};
    static constexpr glm::vec3 COL_SOLID_FILL     = {1.0f, 0.9f, 0.2f};
    static constexpr glm::vec3 COL_INFILL         = {0.3f, 0.9f, 0.3f};
    static constexpr glm::vec3 COL_TRAVEL         = {0.4f, 0.4f, 0.4f};
    static constexpr glm::vec3 COL_CONTOUR        = {1.0f, 0.5f, 0.1f};
    static constexpr glm::vec3 COL_SUPPORT        = {0.85f, 0.3f, 0.85f};  // magenta
    static constexpr glm::vec3 COL_SKIRT          = {0.3f, 0.7f, 1.0f};    // light blue
    static constexpr glm::vec3 COL_RAFT           = {0.8f, 0.6f, 0.3f};    // tan/brown
    static constexpr glm::vec3 COL_BRIDGE         = {0.2f, 0.8f, 0.9f};    // teal (bridge fill)
    static constexpr glm::vec3 COL_SUPPORT_IFACE  = {1.0f, 0.5f, 0.8f};    // pink (support interface)
    static constexpr glm::vec3 COL_PRIME_TOWER    = {1.0f, 0.8f, 0.0f};    // gold (prime tower / purge)
    static constexpr glm::vec3 COL_T0             = {0.9f, 0.9f, 0.9f};    // T0 extruder (light gray)
    static constexpr glm::vec3 COL_T1             = {0.3f, 0.6f, 1.0f};    // T1 extruder (blue)

    // ---- Visibility toggles ----
    bool showTravel        = false;
    bool showShells        = true;
    bool showInfill        = true;
    bool showSolid         = true;
    bool showContours      = true;
    bool showSupport       = true;
    bool showSkirt         = true;
    bool showRaft          = true;
    bool showBridge        = true;
    bool showSupportIface  = true;
    bool showPrimeTower    = true;   // dual extruder prime tower

    // Current layer range to display in 3D view
    int  displayLayerMin = 0;
    int  displayLayerMax = 9999;

    // ---- Initialization ----
    bool init() {
        const char* vert = R"(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aCol;
uniform mat4 uMVP;
out vec3 vCol;
void main() { gl_Position = uMVP * vec4(aPos,1.0); vCol = aCol; }
)";
        const char* frag = R"(
#version 330 core
in vec3 vCol;
out vec4 FragColor;
void main() { FragColor = vec4(vCol, 1.0); }
)";
        m_shader = compileShader(vert, frag);
        return m_shader != 0;
    }

    // ---- Build GPU buffers from SliceResult ----
    void buildFromResult(const SliceResult& result) {
        m_result = &result;
        m_layerThumbs.clear();
        m_layerBuffers.clear();
        m_fullBuffer.destroy();
        m_gcodeBuffer.destroy();

        if (!result.success) return;

        m_layerBuffers.resize(result.layers.size());
        for (size_t li = 0; li < result.layers.size(); ++li)
            buildLayerBuffer(result.layers[li], m_layerBuffers[li]);

        buildFullBuffer(result);

        m_layerThumbs.resize(result.layers.size());
        for (auto& t : m_layerThumbs) t.init(256, 256);
    }

    // ---- Build from G-code moves (with semantic pathType) ----
    void buildFromGcode(const std::vector<GcodeExporter::GcodeMove>& moves) {
        m_gcodeBuffer.destroy();
        std::vector<LineBuffer::Vertex> verts;
        verts.reserve(moves.size() * 2);
        for (auto& mv : moves) {
            if (mv.isTravel && !showTravel) continue;
            glm::vec3 col = colorForPathType(mv.pathType, mv.isTravel);
            verts.push_back({mv.from, col});
            verts.push_back({mv.to,   col});
        }
        m_gcodeBuffer.upload(verts);
    }

    // ---- Render 3D view ----
    void render3D(const glm::mat4& vp) {
        if (!m_shader) return;
        glUseProgram(m_shader);
        glUniformMatrix4fv(glGetUniformLocation(m_shader, "uMVP"), 1, GL_FALSE, glm::value_ptr(vp));
        glLineWidth(1.5f);
        int lo = std::max(0, displayLayerMin);
        int hi = std::min((int)m_layerBuffers.size()-1, displayLayerMax);
        for (int i = lo; i <= hi; ++i)
            m_layerBuffers[i].draw(GL_LINES);
        glLineWidth(1.0f);
        glUseProgram(0);
    }

    void render3D(const glm::mat4& view, const glm::mat4& proj, int layerMin, int layerMax) {
        glm::mat4 vp = proj * view;
        int savedMin = displayLayerMin, savedMax = displayLayerMax;
        displayLayerMin = layerMin; displayLayerMax = layerMax;
        render3D(vp);
        displayLayerMin = savedMin; displayLayerMax = savedMax;
    }

    // ---- Render G-code paths in 3D ----
    void renderGcode3D(const glm::mat4& view, const glm::mat4& proj,
                       int /*layerMin*/ = 0, int /*layerMax*/ = 9999) {
        if (!m_shader || m_gcodeBuffer.count == 0) return;
        glm::mat4 vp = proj * view;
        glUseProgram(m_shader);
        glUniformMatrix4fv(glGetUniformLocation(m_shader, "uMVP"), 1, GL_FALSE, glm::value_ptr(vp));
        glLineWidth(1.2f);
        m_gcodeBuffer.draw(GL_LINES);
        glLineWidth(1.0f);
        glUseProgram(0);
    }

    // ---- Render layer thumbnail to FBO ----
    GLuint renderLayerThumb(int layerIdx) {
        if (layerIdx < 0 || layerIdx >= (int)m_layerThumbs.size()) return 0;
        auto& thumb = m_layerThumbs[layerIdx];
        if (!thumb.dirty) return thumb.colorTex;

        const SliceLayer& layer = m_result->layers[layerIdx];

        glBindFramebuffer(GL_FRAMEBUFFER, thumb.fbo);
        glViewport(0, 0, thumb.w, thumb.h);
        glClearColor(0.12f, 0.12f, 0.14f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // Compute orthographic projection fitting the layer
        Vec2 bbMin = layer.bbMin, bbMax = layer.bbMax;
        // For raft layers, use the raft bounding box
        if (layer.isRaftLayer) {
            bbMin = layer.bbMin; bbMax = layer.bbMax;
        }
        float cx = (bbMin.x + bbMax.x) * 0.5f;
        float cy = (bbMin.y + bbMax.y) * 0.5f;
        float hw = (bbMax.x - bbMin.x) * 0.6f + 1.0f;
        float hh = (bbMax.y - bbMin.y) * 0.6f + 1.0f;
        float s  = std::max(hw, hh);
        glm::mat4 proj = glm::ortho(cx-s, cx+s, cy-s, cy+s, -1.0f, 1.0f);

        glUseProgram(m_shader);
        glUniformMatrix4fv(glGetUniformLocation(m_shader, "uMVP"), 1, GL_FALSE, glm::value_ptr(proj));
        glLineWidth(1.5f);
        m_layerBuffers[layerIdx].draw(GL_LINES);
        glLineWidth(1.0f);
        glUseProgram(0);

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        thumb.dirty = false;
        return thumb.colorTex;
    }

    void markAllDirty() {
        for (auto& t : m_layerThumbs) t.dirty = true;
    }

    void destroy() {
        for (auto& b : m_layerBuffers) b.destroy();
        for (auto& t : m_layerThumbs)  t.destroy();
        m_fullBuffer.destroy();
        m_gcodeBuffer.destroy();
        if (m_shader) { glDeleteProgram(m_shader); m_shader = 0; }
    }

    int layerCount() const { return (int)m_layerBuffers.size(); }

    // ---- Color legend for UI ----
    // Returns (r,g,b) for a given feature type name
    static glm::vec3 colorForFeature(const char* name) {
        std::string s(name);
        if (s == "shell_outer")    return COL_OUTER_SHELL;
        if (s == "shell_inner")    return COL_INNER_SHELL;
        if (s == "solid")          return COL_SOLID_FILL;
        if (s == "infill")         return COL_INFILL;
        if (s == "support")        return COL_SUPPORT;
        if (s == "support_iface")  return COL_SUPPORT_IFACE;
        if (s == "bridge")         return COL_BRIDGE;
        if (s == "skirt")          return COL_SKIRT;
        if (s == "raft")           return COL_RAFT;
        if (s == "travel")         return COL_TRAVEL;
        if (s == "contour")        return COL_CONTOUR;
        return {1,1,1};
    }

private:
    GLuint                   m_shader = 0;
    const SliceResult*       m_result = nullptr;
    std::vector<LineBuffer>  m_layerBuffers;
    std::vector<LayerThumb>  m_layerThumbs;
    LineBuffer               m_fullBuffer;
    LineBuffer               m_gcodeBuffer;

    // Map pathType integer to display color
    // 0=travel, 1=outer_shell, 2=infill, 3=solid, 4=support,
    // 5=skirt, 6=raft, 7=bridge, 8=support_interface, 9=inner_shell
    glm::vec3 colorForPathType(int pathType, bool isTravel) const {
        if (isTravel) return COL_TRAVEL;
        switch (pathType) {
            case 1: return COL_OUTER_SHELL;
            case 2: return COL_INFILL;
            case 3: return COL_SOLID_FILL;
            case 4: return COL_SUPPORT;
            case 5: return COL_SKIRT;
            case 6: return COL_RAFT;
            case 7: return COL_BRIDGE;
            case 8: return COL_SUPPORT_IFACE;
            case 9: return COL_INNER_SHELL;
            case 10: return COL_PRIME_TOWER;  // prime tower / purge
            default: return COL_OUTER_SHELL;
        }
    }

    // ---- Build GPU line buffer for one layer ----
    void buildLayerBuffer(const SliceLayer& layer, LineBuffer& buf) {
        std::vector<LineBuffer::Vertex> verts;
        verts.reserve(512);

        auto addLoop = [&](const Loop2& loop, glm::vec3 col) {
            int n = (int)loop.size();
            for (int i = 0; i < n; ++i) {
                Vec2 a = loop[i], b = loop[(i+1)%n];
                verts.push_back({{a.x, a.y, layer.z}, col});
                verts.push_back({{b.x, b.y, layer.z}, col});
            }
        };
        auto addPath = [&](const Path2& path, glm::vec3 col) {
            for (size_t i = 0; i + 1 < path.size(); ++i) {
                Vec2 a = path[i], b = path[i+1];
                verts.push_back({{a.x, a.y, layer.z}, col});
                verts.push_back({{b.x, b.y, layer.z}, col});
            }
        };

        // Raft layer: only raft paths and border
        if (layer.isRaftLayer) {
            if (showRaft) {
                for (auto& path : layer.raftPaths)  addPath(path, COL_RAFT);
                for (auto& loop : layer.skirtLoops) addLoop(loop, COL_RAFT * 1.2f);
            }
            buf.upload(verts);
            return;
        }

        // Contours
        if (showContours)
            for (auto& loop : layer.contours) addLoop(loop, COL_CONTOUR);

        // Skirt
        if (showSkirt)
            for (auto& loop : layer.skirtLoops) addLoop(loop, COL_SKIRT);

        // Support
        if (showSupport)
            for (auto& path : layer.supportPaths) addPath(path, COL_SUPPORT);

        // Support interface
        if (showSupportIface)
            for (auto& path : layer.supportInterfacePaths) addPath(path, COL_SUPPORT_IFACE);

        // Shells
        if (showShells) {
            for (int s = 0; s < (int)layer.shells.size(); ++s) {
                glm::vec3 col = (s == 0) ? COL_OUTER_SHELL : COL_INNER_SHELL;
                for (auto& loop : layer.shells[s]) addLoop(loop, col);
            }
        }

        // Solid fill
        if (showSolid)
            for (auto& path : layer.solidPaths) addPath(path, COL_SOLID_FILL);

        // Bridge fill
        if (showBridge)
            for (auto& path : layer.bridgePaths) addPath(path, COL_BRIDGE);

        // Infill
        if (showInfill)
            for (auto& path : layer.infillPaths) addPath(path, COL_INFILL);

        // Prime tower (dual extruder)
        if (showPrimeTower)
            for (auto& path : layer.primeTowerPaths) addPath(path, COL_PRIME_TOWER);

        buf.upload(verts);
    }

    // ---- Build combined 3D buffer ----
    void buildFullBuffer(const SliceResult& result) {
        std::vector<LineBuffer::Vertex> verts;
        verts.reserve(result.layers.size() * 512);
        for (auto& layer : result.layers) {
            auto addLoop = [&](const Loop2& loop, glm::vec3 col) {
                int n = (int)loop.size();
                for (int i = 0; i < n; ++i) {
                    Vec2 a = loop[i], b = loop[(i+1)%n];
                    verts.push_back({{a.x, a.y, layer.z}, col});
                    verts.push_back({{b.x, b.y, layer.z}, col});
                }
            };
            if (layer.isRaftLayer) {
                if (showRaft)
                    for (auto& loop : layer.skirtLoops) addLoop(loop, COL_RAFT);
                continue;
            }
            if (showShells) {
                for (int s = 0; s < (int)layer.shells.size(); ++s) {
                    glm::vec3 col = (s == 0) ? COL_OUTER_SHELL : COL_INNER_SHELL;
                    for (auto& loop : layer.shells[s]) addLoop(loop, col);
                }
            }
            if (showSkirt)
                for (auto& loop : layer.skirtLoops) addLoop(loop, COL_SKIRT);
        }
        m_fullBuffer.upload(verts);
    }

    // ---- Compile inline shader ----
    static GLuint compileShader(const char* vert, const char* frag) {
        auto compile = [](GLenum type, const char* src) -> GLuint {
            GLuint s = glCreateShader(type);
            glShaderSource(s, 1, &src, nullptr);
            glCompileShader(s);
            GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
            if (!ok) { glDeleteShader(s); return 0; }
            return s;
        };
        GLuint vs = compile(GL_VERTEX_SHADER, vert);
        GLuint fs = compile(GL_FRAGMENT_SHADER, frag);
        if (!vs || !fs) return 0;
        GLuint prog = glCreateProgram();
        glAttachShader(prog, vs); glAttachShader(prog, fs);
        glLinkProgram(prog);
        glDeleteShader(vs); glDeleteShader(fs);
        GLint ok; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) { glDeleteProgram(prog); return 0; }
        return prog;
    }
};
