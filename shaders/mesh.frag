#version 330 core
in vec3 FragPos;
in vec3 Normal;

uniform vec3 lightPos;
uniform vec3 viewPos;
uniform vec3 objectColor;
uniform float alpha;
uniform bool wireframe;

out vec4 FragColor;

void main()
{
    if (wireframe) {
        FragColor = vec4(objectColor, alpha);
        return;
    }

    // Ambient
    float ambientStrength = 0.25;
    vec3 ambient = ambientStrength * vec3(1.0, 1.0, 1.0);

    // Diffuse
    vec3 norm = normalize(Normal);
    vec3 lightDir = normalize(lightPos - FragPos);
    float diff = max(dot(norm, lightDir), 0.0);
    // Two-sided lighting
    float diffBack = max(dot(-norm, lightDir), 0.0);
    float diffFinal = max(diff, diffBack * 0.5);
    vec3 diffuse = diffFinal * vec3(1.0, 1.0, 1.0);

    // Specular
    float specularStrength = 0.4;
    vec3 viewDir = normalize(viewPos - FragPos);
    vec3 reflectDir = reflect(-lightDir, norm);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), 32.0);
    vec3 specular = specularStrength * spec * vec3(1.0, 1.0, 1.0);

    vec3 result = (ambient + diffuse + specular) * objectColor;
    FragColor = vec4(result, alpha);
}
