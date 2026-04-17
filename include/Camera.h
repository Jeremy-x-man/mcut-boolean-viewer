#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

/**
 * @brief Arcball-style orbit camera for 3D mesh preview.
 *
 * Supports mouse-drag rotation, scroll-wheel zoom, and middle-button pan.
 * All state is maintained internally; call update() each frame.
 */
class Camera {
public:
    Camera()
        : m_target(0.0f, 0.0f, 0.0f)
        , m_distance(5.0f)
        , m_yaw(-90.0f)
        , m_pitch(30.0f)
        , m_fov(45.0f)
        , m_aspectRatio(1.0f)
        , m_nearPlane(0.01f)
        , m_farPlane(500.0f)
    {
        updatePosition();
    }

    // ---- Accessors ----
    glm::mat4 getViewMatrix() const {
        return glm::lookAt(m_position, m_target, glm::vec3(0, 1, 0));
    }

    glm::mat4 getProjectionMatrix() const {
        return glm::perspective(glm::radians(m_fov), m_aspectRatio, m_nearPlane, m_farPlane);
    }

    glm::vec3 getPosition() const { return m_position; }
    float     getDistance()  const { return m_distance; }

    void setAspectRatio(float ratio) { m_aspectRatio = ratio; }

    // ---- Interaction ----
    void orbit(float dx, float dy) {
        m_yaw   += dx * 0.4f;
        m_pitch += dy * 0.4f;
        m_pitch  = glm::clamp(m_pitch, -89.0f, 89.0f);
        updatePosition();
    }

    void pan(float dx, float dy) {
        glm::vec3 forward = glm::normalize(m_target - m_position);
        glm::vec3 right   = glm::normalize(glm::cross(forward, glm::vec3(0, 1, 0)));
        glm::vec3 up      = glm::normalize(glm::cross(right, forward));
        float panSpeed    = m_distance * 0.001f;
        m_target -= right * dx * panSpeed;
        m_target += up    * dy * panSpeed;
        updatePosition();
    }

    void zoom(float delta) {
        m_distance -= delta * m_distance * 0.1f;
        m_distance  = glm::clamp(m_distance, 0.1f, 1000.0f);
        updatePosition();
    }

    void reset() {
        m_target   = glm::vec3(0.0f);
        m_distance = 5.0f;
        m_yaw      = -90.0f;
        m_pitch    = 30.0f;
        updatePosition();
    }

    void fitBoundingBox(const glm::vec3& minBB, const glm::vec3& maxBB) {
        m_target   = (minBB + maxBB) * 0.5f;
        float diag = glm::length(maxBB - minBB);
        m_distance = diag * 1.5f;
        if (m_distance < 0.1f) m_distance = 5.0f;
        updatePosition();
    }

private:
    void updatePosition() {
        float yawR   = glm::radians(m_yaw);
        float pitchR = glm::radians(m_pitch);
        m_position = m_target + m_distance * glm::vec3(
            cos(pitchR) * cos(yawR),
            sin(pitchR),
            cos(pitchR) * sin(yawR));
    }

    glm::vec3 m_position;
    glm::vec3 m_target;
    float m_distance;
    float m_yaw;
    float m_pitch;
    float m_fov;
    float m_aspectRatio;
    float m_nearPlane;
    float m_farPlane;
};
