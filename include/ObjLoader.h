#pragma once
#include <memory>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <iostream>
#include <unordered_map>
#include <cstring>
#include <cstdio>
#include "RenderMesh.h"

#define TINYOBJLOADER_IMPLEMENTATION
#include "tinyobj/tiny_obj_loader.h"

// Cross-platform sscanf: MSVC provides sscanf_s as the "safe" variant.
// We alias it here so the rest of the code uses a single name.
#ifdef _MSC_VER
#  define MCUT_SSCANF sscanf_s
#else
#  define MCUT_SSCANF sscanf
#endif

/**
 * @brief Simple OBJ mesh loader that produces flat-shaded RenderMesh.
 *
 * Supports triangulated and quad faces. Returns raw vertex/index arrays
 * suitable for both rendering (RenderMesh) and MCUT dispatch.
 */
struct MeshData {
    std::vector<double>   vertices;     ///< flat array: x,y,z per vertex
    std::vector<uint32_t> indices;      ///< flat array of vertex indices
    std::vector<uint32_t> sizes;        ///< number of vertices per face

    bool isValid() const {
		return !vertices.empty() && !indices.empty() && !sizes.empty();
    }
};

// mesh section for multi-material meshes, stores index/vertex ranges for each section
struct MeshSection {
    /** The offset of this section's indices in the MeshData's faceIndices */
    uint32_t baseIndex;
    /** The number of faces in this section. */
    uint32_t numFaces;
};

// store mesh data and sections (for multi-material meshes)
struct MeshModel {
    bool isValid() const {
        return data.isValid();
    }
    uint32_t numFaces() const {
        return (uint32_t)data.sizes.size();
	}
    uint32_t numVertices() const {
        return (uint32_t)(data.vertices.size() / 3);
	}
    MeshData data;
    std::vector<MeshSection> sections;

    std::vector<MeshData GetTransformedVertices(std::array<double, 3> transform) const {
        if (sections.size() > 1) {
            std::vector<double> outVerts(data.vertices);
            if (transform[0] != 0.0 || transform[1] != 0.0 || transform[2] != 0.0) {
                for (size_t v = 0; v < outVerts.size(); v += 3) {
                    outVerts[v + 0] += transform[0];
                    outVerts[v + 1] += transform[1];
                    outVerts[v + 2] += transform[2];
                }
            }
			return { std::move(outVerts), data.indices, data.sizes };
        }
        else {
            std::vector<double> outVerts(data.vertices);
            if (transform[0] != 0.0 || transform[1] != 0.0 || transform[2] != 0.0) {
                for (size_t v = 0; v < outVerts.size(); v += 3) {
                    outVerts[v + 0] += transform[0];
                    outVerts[v + 1] += transform[1];
                    outVerts[v + 2] += transform[2];
                }
            }
        }
	}
};

inline bool loadOBJModel(const std::string& path, MeshModel& mesh) {
    // 1. 定义存储模型数据的容器
    tinyobj::attrib_t attrib;               // 存储顶点、法线、纹理坐标等属性
    std::vector<tinyobj::shape_t> shapes;   // 存储模型的形状（网格）
    std::vector<tinyobj::material_t> materials; // 存储材质信息
    std::string err;                        // 用于错误信息

    // 2. 加载 OBJ 文件
    // 参数：文件路径, 属性容器, 形状容器, 材质容器, 警告信息, 错误信息, 是否 triangulate
    bool ret = tinyobj::LoadObj (&attrib, &shapes, &materials, &err, path.c_str());
    if (!err.empty()) {
        if (err.find("ERROR:") != std::string::npos || err.find("Error:") != std::string::npos)
        {
            std::cerr << "加载错误: " << err << std::endl;
            return false;// 加载失败，退出程序
        }
        else
        {
            std::cerr << "加载告警: " << err << std::endl;
        }
    }

    if (!ret) {
        std::cerr << "无法加载 OBJ 文件" << std::endl;
        return false;
    }

    // 3. 遍历并处理加载的模型数据
    std::cout << "成功加载 " << shapes.size() << " 个形状。" << std::endl;

    mesh.data.vertices.resize(attrib.vertices.size());
    for (size_t s = 0; s < attrib.vertices.size(); s++) {
        mesh.data.vertices[s] = (double)attrib.vertices[s];
    }

    // Loop over shapes
    mesh.sections.resize(shapes.size());
    for (size_t s = 0; s < shapes.size(); s++) {
        // Loop over faces(polygon)
        mesh.sections[s].baseIndex = (uint32_t)mesh.data.indices.size();
        mesh.sections[s].numFaces = (uint32_t)shapes[s].mesh.indices.size() / 3;

        for (size_t i = 0; i < shapes[s].mesh.indices.size(); i++) {
            mesh.data.indices.push_back((uint32_t)shapes[s].mesh.indices[i].vertex_index);
        }
        for (size_t f = 0; f < shapes[s].mesh.num_face_vertices.size(); f++) {
            mesh.data.sizes.push_back((uint32_t)shapes[s].mesh.num_face_vertices[f]);
        }
    }
    return true;
}

inline bool loadOBJ(const std::string& path, MeshModel& Mesh) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "[ObjLoader] Cannot open: " << path << "\n";
        return false;
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
            Mesh.data.vertices.push_back((double)x);
            Mesh.data.vertices.push_back((double)y);
            Mesh.data.vertices.push_back((double)z);
        } else if (token == "f") {
            std::vector<uint32_t> faceVerts;
            std::string vtok;
            while (ss >> vtok) {
                // Handle v, v/vt, v/vt/vn, v//vn
                int vi = 0;
                MCUT_SSCANF(vtok.c_str(), "%d", &vi);
                if (vi < 0) vi = (int)positions.size() + vi + 1;
                faceVerts.push_back((uint32_t)(vi - 1));
            }
            if (faceVerts.size() >= 3) {
                Mesh.data.sizes.push_back((uint32_t)faceVerts.size());
                for (auto idx : faceVerts) Mesh.data.indices.push_back(idx);
            }
        }
    }
    return Mesh.isValid();
}

inline bool loadOFF(const std::string& path, MeshModel& Mesh) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "[ObjLoader] Cannot open: " << path << "\n";
        return false;
    }
    auto next_line = [&](std::ifstream& f, std::string& s) -> bool {
        while (getline(f, s)) {
            if (s.length() > 1 && s[0] != '#') {
                return true;
            }
        }
        return false;
        };
    // file header
    std::string header;
    if (!next_line(file, header)) {
        printf("error: .off file header not found\n");
        return false;
    }
    if (header != "OFF") {
        printf("error: unrecognised .off file header\n");
        return false;
    }
    // #vertices, #faces, #edges
    std::string info;
    if (!next_line(file, info)) {
        printf("error: .off element count not found\n");
        return false;
    }
    std::istringstream info_stream;
    info_stream.str(info);

    int nvertices;
    int nfaces;
    int nedges;
    info_stream >> nvertices >> nfaces >> nedges;

    //
    // vertices
    //
    std::vector<glm::vec3> positions;
    positions.resize(nvertices);
    for (int i = 0; i < nvertices; ++i) {
        if (!next_line(file, info)) {
            printf("error: .off vertex not found\n");
            return false;
        }
        std::istringstream vtx_line_stream(info);

        double x;
        double y;
        double z;
        vtx_line_stream >> x >> y >> z;
        positions.push_back({ x, y, z });
        Mesh.data.vertices.push_back((double)x);
        Mesh.data.vertices.push_back((double)y);
        Mesh.data.vertices.push_back((double)z);
    }
    // faces
    for (auto i = 0; i < nfaces; ++i) {
        if (!next_line(file, info)) {
            printf("error: .off file face not found\n");
            return false;
        }
        std::istringstream face_line_stream(info);
        int n; // number of vertices in face
        int index;
        face_line_stream >> n;

        if (n < 3) {
            printf("error: invalid polygon vertex count in file (%d)\n", n);
            return false;
        }
        for (int j = 0; j < n; ++j) {
            face_line_stream >> index;
            Mesh.data.indices.push_back(index);
        }
        Mesh.data.sizes.push_back(n);
    }
    return Mesh.isValid();
}

inline bool loadMesh(const std::string& path, MeshModel& Mesh) {
    if (path.find(".obj") != std::string::npos || path.find(".OBJ") != std::string::npos) {
        return loadOBJModel(path, Mesh);
    }
    else if (path.find(".off") != std::string::npos || path.find(".OFF") != std::string::npos) {
        return loadOFF(path, Mesh);
    }
    else {
        std::cerr << "[ObjLoader] Unsupported file format: " << path << "\n";
        return false;
    }
}

/**
 * @brief Convert MeshData to a RenderMesh (triangulated, with flat normals).
 */
inline std::unique_ptr<RenderMesh> meshModelToRenderMesh(const MeshData& data, const std::string& label = "") {
    auto mesh = std::make_unique<RenderMesh>();
    mesh->label = label;

    // Copy positions as vertices
    size_t numVerts = data.vertices.size() / 3;
    mesh->vertices.resize(numVerts);
    for (size_t i = 0; i < numVerts; ++i) {
        mesh->vertices[i].position = {
            (float)data.vertices[i*3+0],
            (float)data.vertices[i*3+1],
            (float)data.vertices[i*3+2]
        };
        mesh->vertices[i].normal = glm::vec3(0.0f);
    }

    // Triangulate faces (fan triangulation)
    size_t faceOffset = 0;
    for (size_t f = 0; f < data.sizes.size(); ++f) {
        uint32_t fsize = data.sizes[f];
        uint32_t v0 = data.indices[faceOffset];
        for (uint32_t k = 1; k + 1 < fsize; ++k) {
            mesh->indices.push_back(v0);
            mesh->indices.push_back(data.indices[faceOffset + k]);
            mesh->indices.push_back(data.indices[faceOffset + k + 1]);
        }
        faceOffset += fsize;
    }

    mesh->upload();
    return mesh;
}
