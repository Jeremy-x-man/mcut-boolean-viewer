#pragma once
#include <memory>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <iostream>
#include <unordered_map>
#include <cstring>
#include "RenderMesh.h"

/**
 * @brief Simple OBJ mesh loader that produces flat-shaded RenderMesh.
 *
 * Supports triangulated and quad faces. Returns raw vertex/index arrays
 * suitable for both rendering (RenderMesh) and MCUT dispatch.
 */
struct ObjData {
    std::vector<double>   vertices;   ///< flat array: x,y,z per vertex
    std::vector<uint32_t> faceIndices;///< flat array of vertex indices
    std::vector<uint32_t> faceSizes;  ///< number of vertices per face
    bool valid = false;
};

inline ObjData loadOBJ(const std::string& path) {
    ObjData data;
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "[ObjLoader] Cannot open: " << path << "\n";
        return data;
    }

    std::vector<glm::vec3> positions;
    std::string line;

    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string token;
        ss >> token;

        if (token == "v") {
            float x, y, z;
            ss >> x >> y >> z;
            positions.push_back({x, y, z});
            data.vertices.push_back((double)x);
            data.vertices.push_back((double)y);
            data.vertices.push_back((double)z);
        } else if (token == "f") {
            std::vector<uint32_t> faceVerts;
            std::string vtok;
            while (ss >> vtok) {
                // Handle v, v/vt, v/vt/vn, v//vn
                int vi = 0;
                sscanf(vtok.c_str(), "%d", &vi);
                if (vi < 0) vi = (int)positions.size() + vi + 1;
                faceVerts.push_back((uint32_t)(vi - 1));
            }
            if (faceVerts.size() >= 3) {
                data.faceSizes.push_back((uint32_t)faceVerts.size());
                for (auto idx : faceVerts) data.faceIndices.push_back(idx);
            }
        }
    }

    data.valid = !positions.empty() && !data.faceSizes.empty();
    return data;
}

/**
 * @brief Convert ObjData to a RenderMesh (triangulated, with flat normals).
 */
inline std::unique_ptr<RenderMesh> objDataToRenderMesh(const ObjData& obj, const std::string& label = "") {
    auto mesh = std::make_unique<RenderMesh>();
    mesh->label = label;

    // Copy positions as vertices
    size_t numVerts = obj.vertices.size() / 3;
    mesh->vertices.resize(numVerts);
    for (size_t i = 0; i < numVerts; ++i) {
        mesh->vertices[i].position = {
            (float)obj.vertices[i*3+0],
            (float)obj.vertices[i*3+1],
            (float)obj.vertices[i*3+2]
        };
        mesh->vertices[i].normal = glm::vec3(0.0f);
    }

    // Triangulate faces (fan triangulation)
    size_t faceOffset = 0;
    for (size_t f = 0; f < obj.faceSizes.size(); ++f) {
        uint32_t fsize = obj.faceSizes[f];
        uint32_t v0 = obj.faceIndices[faceOffset];
        for (uint32_t k = 1; k + 1 < fsize; ++k) {
            mesh->indices.push_back(v0);
            mesh->indices.push_back(obj.faceIndices[faceOffset + k]);
            mesh->indices.push_back(obj.faceIndices[faceOffset + k + 1]);
        }
        faceOffset += fsize;
    }

    mesh->upload();
    return mesh;
}
