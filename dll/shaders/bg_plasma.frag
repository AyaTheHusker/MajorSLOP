#version 450

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec4 fragColor;

layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D fontTex;

layout(push_constant) uniform PushConstants {
    float time;
    float speed;
    float turbulence;
    float complexity;   /* 1..8 octaves */
    float hue_shift;    /* 0..360 degrees */
    float saturation;   /* 0..2 */
    float brightness;   /* 0..2 */
    float alpha;        /* 0..1 overall opacity */
    float res_x;        /* viewport width */
    float res_y;        /* viewport height */
} pc;

/* --- Gradient noise (no grid artifacts) --- */

vec2 hash2(vec2 p) {
    p = vec2(dot(p, vec2(127.1, 311.7)),
             dot(p, vec2(269.5, 183.3)));
    return -1.0 + 2.0 * fract(sin(p) * 43758.5453123);
}

float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    /* Quintic Hermite — C2 continuous, no grid seams */
    vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);

    float a = dot(hash2(i + vec2(0.0, 0.0)), f - vec2(0.0, 0.0));
    float b = dot(hash2(i + vec2(1.0, 0.0)), f - vec2(1.0, 0.0));
    float c = dot(hash2(i + vec2(0.0, 1.0)), f - vec2(0.0, 1.0));
    float d = dot(hash2(i + vec2(1.0, 1.0)), f - vec2(1.0, 1.0));

    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y) * 0.5 + 0.5;
}

/* FBM with variable octaves */
float fbm(vec2 p, int octaves) {
    float value = 0.0;
    float amplitude = 0.5;
    float frequency = 1.0;
    mat2 rot = mat2(0.8, 0.6, -0.6, 0.8);
    for (int i = 0; i < octaves; i++) {
        value += amplitude * noise(p * frequency);
        p = rot * p;
        frequency *= 2.0;
        amplitude *= 0.5;
    }
    return value;
}

/* HSV to RGB */
vec3 hsv2rgb(float h, float s, float v) {
    h = mod(h, 360.0) / 60.0;
    int hi = int(floor(h));
    float f = h - float(hi);
    float p = v * (1.0 - s);
    float q = v * (1.0 - s * f);
    float t = v * (1.0 - s * (1.0 - f));
    if (hi == 0)      return vec3(v, t, p);
    else if (hi == 1) return vec3(q, v, p);
    else if (hi == 2) return vec3(p, v, t);
    else if (hi == 3) return vec3(p, q, v);
    else if (hi == 4) return vec3(t, p, v);
    else              return vec3(v, p, q);
}

void main() {
    vec2 uv = gl_FragCoord.xy / vec2(pc.res_x, pc.res_y);
    float aspect = pc.res_x / pc.res_y;
    vec2 puv = vec2(uv.x * aspect, uv.y); /* aspect-corrected for plasma */

    float t = pc.time * pc.speed;
    int octaves = clamp(int(pc.complexity), 1, 8);

    /* Multiple overlapping plasma waves (aspect-corrected) */
    vec2 p1 = puv * 3.0 + vec2(t * 0.3, t * 0.2);
    vec2 p2 = puv * 5.0 + vec2(-t * 0.4, t * 0.15);
    vec2 p3 = puv * 2.0 + vec2(t * 0.1, -t * 0.25);

    float n1 = fbm(p1 * (1.0 + pc.turbulence), octaves);
    float n2 = fbm(p2 * (1.0 + pc.turbulence * 0.7), octaves);
    float n3 = fbm(p3 * (1.0 + pc.turbulence * 1.3), octaves);

    /* Classic plasma: sum of sine waves modulated by noise */
    float plasma = 0.0;
    plasma += sin(puv.x * 10.0 + t * 1.2 + n1 * 6.28 * pc.turbulence);
    plasma += sin(puv.y * 8.0 - t * 0.9 + n2 * 6.28 * pc.turbulence);
    plasma += sin((puv.x + puv.y) * 6.0 + t * 0.7 + n3 * 4.0);
    plasma += sin(length(puv - vec2(aspect * 0.5, 0.5)) * 12.0 - t * 1.5);

    /* Normalize to 0..1 */
    plasma = plasma * 0.125 + 0.5;

    /* Add FBM detail layer */
    float detail = fbm(puv * 4.0 + vec2(t * 0.5), octaves);
    plasma = mix(plasma, detail, 0.3);

    /* Map to color via HSV */
    float hue = mod(plasma * 360.0 + pc.hue_shift, 360.0);
    float sat = clamp(pc.saturation, 0.0, 1.0);
    float val = clamp(plasma * 0.4 + 0.3, 0.0, 1.0) * pc.brightness;

    vec3 color = hsv2rgb(hue, sat, val);

    /* Subtle edge darkening for depth */
    float vign = 1.0 - length(uv - 0.5) * 0.5;
    color *= vign;

    outColor = vec4(color, pc.alpha);
}
