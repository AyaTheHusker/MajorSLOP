#version 450

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec4 fragColor;

layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D fontTex;

void main() {
    vec4 tex = texture(fontTex, fragUV);
    if (fragColor.r < -0.5) {
        // Color emoji mode: use full texture RGBA, modulate alpha by vertex alpha
        outColor = vec4(tex.rgb, tex.a * fragColor.a);
    } else {
        // Normal mode: font atlas alpha mask tinted by vertex color
        outColor = vec4(fragColor.rgb, fragColor.a * tex.a);
    }
}
