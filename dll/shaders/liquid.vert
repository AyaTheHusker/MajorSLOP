#version 450

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec3 inColor;
layout(location = 3) in float inAlpha;

layout(location = 0) out vec2 fragUV;
layout(location = 1) out vec4 fragColor;

layout(push_constant) uniform PushConstants {
    float time;
    float fx_mode;       /* 0 = normal, 1 = liquid letters */
    float fx_waves;      /* 0 = off, 1 = diagonal waves */
    float fx_scanlines;  /* 0 = off, 1 = CRT scanlines (fragment only) */
    float fx_fbm;        /* 0 = off, 1 = FBM noise warp */
    float fx_sobel;      /* 0 = off, 1 = sobel/sharp (fragment only) */
} pc;

/* ---- FBM Noise (hash-based, no texture needed) ---- */

/* Simple 2D hash → pseudo-random float in [0,1] */
float hash(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

/* Value noise with smooth interpolation */
float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    /* Quintic Hermite for smoother derivatives */
    vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);

    float a = hash(i);
    float b = hash(i + vec2(1.0, 0.0));
    float c = hash(i + vec2(0.0, 1.0));
    float d = hash(i + vec2(1.0, 1.0));

    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

/* Fractal Brownian Motion — 4 octaves for rich organic detail */
float fbm(vec2 p) {
    float value = 0.0;
    float amplitude = 0.5;
    float frequency = 1.0;
    /* Rotation matrix to break grid alignment between octaves */
    mat2 rot = mat2(0.8, 0.6, -0.6, 0.8);
    for (int i = 0; i < 4; i++) {
        value += amplitude * noise(p * frequency);
        p = rot * p;
        frequency *= 2.0;
        amplitude *= 0.5;
    }
    return value;
}

void main() {
    vec4 pos = vec4(inPos, 0.0, 1.0);

    /* ---- Diagonal Waves ---- */
    if (pc.fx_waves > 0.5) {
        float seed = inPos.x * 7.13 + inPos.y * 3.37;
        float angle = sin(seed * 4.7 + pc.time * 2.0) * 0.015;
        pos.x += sin(seed * 5.3 + pc.time * 2.5) * 0.002;
        pos.y += cos(seed * 3.1 + pc.time * 1.7) * 0.003;
        float cx = inPos.x;
        float cy = inPos.y;
        pos.x += (pos.y - cy) * sin(angle);
        pos.y -= (pos.x - cx) * sin(angle);
    }

    /* ---- FBM Noise Warp (fluid currents flowing between letters) ---- */
    if (pc.fx_fbm > 0.5) {
        /* Large-scale fluid flow — low frequency for broad sweeping currents */
        vec2 np = inPos * 1.5; /* large eddies */
        float t = pc.time * 0.4;

        /* Multi-scale curl noise for divergence-free fluid motion */
        float eps = 0.02;

        /* Large eddies — slow, sweeping */
        float n0 = fbm(np + vec2(t * 0.3, t * 0.2));
        float curl1_x = fbm(np + vec2(0, eps) + vec2(t * 0.3, t * 0.2)) - n0;
        float curl1_y = -(fbm(np + vec2(eps, 0) + vec2(t * 0.3, t * 0.2)) - n0);

        /* Medium eddies — faster, more detail */
        vec2 mp = inPos * 4.0;
        float m0 = fbm(mp + vec2(t * 0.7 + 7.3, t * 0.5 + 13.7));
        float curl2_x = fbm(mp + vec2(0, eps) + vec2(t * 0.7 + 7.3, t * 0.5 + 13.7)) - m0;
        float curl2_y = -(fbm(mp + vec2(eps, 0) + vec2(t * 0.7 + 7.3, t * 0.5 + 13.7)) - m0);

        /* Small turbulence — fast ripples */
        vec2 sp = inPos * 8.0;
        float s0 = fbm(sp + vec2(t * 1.2 + 23.1, t * 0.9 + 41.3));
        float curl3_x = fbm(sp + vec2(0, eps) + vec2(t * 1.2 + 23.1, t * 0.9 + 41.3)) - s0;
        float curl3_y = -(fbm(sp + vec2(eps, 0) + vec2(t * 1.2 + 23.1, t * 0.9 + 41.3)) - s0);

        /* Blend scales: large dominant, medium adds detail, small adds turbulence */
        float wx = curl1_x * 0.6 + curl2_x * 0.3 + curl3_x * 0.1;
        float wy = curl1_y * 0.6 + curl2_y * 0.3 + curl3_y * 0.1;

        /* Time-varying vortex centers for unpredictable swirling */
        float vx = sin(t * 0.7) * 0.3;
        float vy = cos(t * 0.5) * 0.3;
        float dist = length(inPos - vec2(vx, vy));
        float vortex = exp(-dist * 2.0) * 0.3;
        wx += -sin(atan(inPos.y - vy, inPos.x - vx)) * vortex;
        wy +=  cos(atan(inPos.y - vy, inPos.x - vx)) * vortex;

        /* Strong amplitude for visible fluid motion */
        float amp = 0.025;
        pos.x += wx * amp;
        pos.y += wy * amp;
    }

    gl_Position = pos;
    fragUV = inUV;
    fragColor = vec4(inColor, inAlpha);
}
