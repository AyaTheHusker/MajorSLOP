#version 450

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec4 fragColor;

layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D fontTex;

void main() {
    // For background quads: fragUV = (0,0), alpha = 1, tex sample = white → color pass-through
    // For text quads: sample font atlas alpha, tint with color
    float texAlpha = texture(fontTex, fragUV).a;
    outColor = vec4(fragColor.rgb, fragColor.a * texAlpha);
}
