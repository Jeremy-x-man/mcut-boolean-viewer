#pragma once
#include <glad/gl.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <string>
#include <fstream>
#include <sstream>
#include <iostream>

/**
 * @brief Minimal GLSL shader program wrapper.
 */
class Shader {
public:
    GLuint ID = 0;

    Shader() = default;

    bool loadFromFiles(const std::string& vertPath, const std::string& fragPath) {
        std::string vertCode = readFile(vertPath);
        std::string fragCode = readFile(fragPath);
        if (vertCode.empty() || fragCode.empty()) return false;
        return compile(vertCode.c_str(), fragCode.c_str());
    }

    bool loadFromSource(const char* vertSrc, const char* fragSrc) {
        return compile(vertSrc, fragSrc);
    }

    void use() const { glUseProgram(ID); }

    void setMat4(const std::string& name, const glm::mat4& mat) const {
        glUniformMatrix4fv(glGetUniformLocation(ID, name.c_str()), 1, GL_FALSE, glm::value_ptr(mat));
    }

    void setMat3(const std::string& name, const glm::mat3& mat) const {
        glUniformMatrix3fv(glGetUniformLocation(ID, name.c_str()), 1, GL_FALSE, glm::value_ptr(mat));
    }

    void setVec3(const std::string& name, const glm::vec3& v) const {
        glUniform3fv(glGetUniformLocation(ID, name.c_str()), 1, glm::value_ptr(v));
    }

    void setFloat(const std::string& name, float v) const {
        glUniform1f(glGetUniformLocation(ID, name.c_str()), v);
    }

    void setBool(const std::string& name, bool v) const {
        glUniform1i(glGetUniformLocation(ID, name.c_str()), (int)v);
    }

    ~Shader() {
        if (ID) glDeleteProgram(ID);
    }

private:
    static std::string readFile(const std::string& path) {
        std::ifstream f(path);
        if (!f.is_open()) {
            std::cerr << "[Shader] Cannot open: " << path << "\n";
            return {};
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    bool compile(const char* vertSrc, const char* fragSrc) {
        GLuint vert = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vert, 1, &vertSrc, nullptr);
        glCompileShader(vert);
        if (!checkCompile(vert, "VERTEX")) return false;

        GLuint frag = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(frag, 1, &fragSrc, nullptr);
        glCompileShader(frag);
        if (!checkCompile(frag, "FRAGMENT")) { glDeleteShader(vert); return false; }

        ID = glCreateProgram();
        glAttachShader(ID, vert);
        glAttachShader(ID, frag);
        glLinkProgram(ID);
        glDeleteShader(vert);
        glDeleteShader(frag);
        return checkLink(ID);
    }

    static bool checkCompile(GLuint shader, const std::string& type) {
        GLint ok; glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[1024];
            glGetShaderInfoLog(shader, 1024, nullptr, log);
            std::cerr << "[Shader] " << type << " compile error:\n" << log << "\n";
            return false;
        }
        return true;
    }

    static bool checkLink(GLuint prog) {
        GLint ok; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[1024];
            glGetProgramInfoLog(prog, 1024, nullptr, log);
            std::cerr << "[Shader] Link error:\n" << log << "\n";
            return false;
        }
        return true;
    }
};
