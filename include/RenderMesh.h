#pragma once
#include <glad/gl.h>
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <array>

/**
 * @brief Represents a GPU-resident triangle mesh for OpenGL rendering.
 *
 * Stores interleaved vertex data (position + normal) and uploads to VBO/VAO.
 * Supports both solid (Phong shading) and wireframe overlay rendering.
 */
struct RenderMesh {
    struct Vertex {
        glm::vec3 position;
        glm::vec3 normal;
    };

    std::vector<Vertex>   vertices;
    std::vector<uint32_t> indices;

    GLuint VAO = 0, VBO = 0, EBO = 0;

    // Bounding box
    glm::vec3 bbMin{  1e9f,  1e9f,  1e9f };
    glm::vec3 bbMax{ -1e9f, -1e9f, -1e9f };

    // Display properties
    glm::vec3 color    { 0.7f, 0.7f, 0.7f };
    float     alpha    { 1.0f };
    bool      visible  { true };
    bool      showWire { false };
    std::string label;

    RenderMesh() = default;
    ~RenderMesh() { freeGPU(); }

    // Non-copyable, movable
    RenderMesh(const RenderMesh&) = delete;
    RenderMesh& operator=(const RenderMesh&) = delete;
    RenderMesh(RenderMesh&& o) noexcept
        : vertices(std::move(o.vertices)), indices(std::move(o.indices))
        , VAO(o.VAO), VBO(o.VBO), EBO(o.EBO)
        , bbMin(o.bbMin), bbMax(o.bbMax)
        , color(o.color), alpha(o.alpha), visible(o.visible), showWire(o.showWire)
        , label(std::move(o.label))
    {
        o.VAO = o.VBO = o.EBO = 0;
    }

    /** Upload geometry to GPU. Must be called after filling vertices/indices. */
    void upload() {
        freeGPU();
        computeBoundingBox();
        computeFlatNormals();

        glGenVertexArrays(1, &VAO);
        glGenBuffers(1, &VBO);
        glGenBuffers(1, &EBO);

        glBindVertexArray(VAO);

        glBindBuffer(GL_ARRAY_BUFFER, VBO);
        glBufferData(GL_ARRAY_BUFFER,
            vertices.size() * sizeof(Vertex),
            vertices.data(), GL_STATIC_DRAW);

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
            indices.size() * sizeof(uint32_t),
            indices.data(), GL_STATIC_DRAW);

        // Position
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
            (void*)offsetof(Vertex, position));
        glEnableVertexAttribArray(0);

        // Normal
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
            (void*)offsetof(Vertex, normal));
        glEnableVertexAttribArray(1);

        glBindVertexArray(0);
    }

    void draw() const {
        if (!visible || VAO == 0 || indices.empty()) return;
        glBindVertexArray(VAO);
        glDrawElements(GL_TRIANGLES, (GLsizei)indices.size(), GL_UNSIGNED_INT, nullptr);
        glBindVertexArray(0);
    }

    void freeGPU() {
        if (VAO) { glDeleteVertexArrays(1, &VAO); VAO = 0; }
        if (VBO) { glDeleteBuffers(1, &VBO); VBO = 0; }
        if (EBO) { glDeleteBuffers(1, &EBO); EBO = 0; }
    }

    /** Compute flat normals from triangle faces (per-vertex, averaged). */
    void computeFlatNormals() {
        // Reset normals
        for (auto& v : vertices) v.normal = glm::vec3(0.0f);

        for (size_t i = 0; i + 2 < indices.size(); i += 3) {
            uint32_t i0 = indices[i], i1 = indices[i+1], i2 = indices[i+2];
            glm::vec3 e1 = vertices[i1].position - vertices[i0].position;
            glm::vec3 e2 = vertices[i2].position - vertices[i0].position;
            glm::vec3 n  = glm::cross(e1, e2);
            vertices[i0].normal += n;
            vertices[i1].normal += n;
            vertices[i2].normal += n;
        }
        for (auto& v : vertices) {
            float len = glm::length(v.normal);
            if (len > 1e-8f) v.normal /= len;
        }
    }

    void computeBoundingBox() {
        bbMin = glm::vec3( 1e9f);
        bbMax = glm::vec3(-1e9f);
        for (const auto& v : vertices) {
            bbMin = glm::min(bbMin, v.position);
            bbMax = glm::max(bbMax, v.position);
        }
    }
};
