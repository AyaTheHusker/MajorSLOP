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
    float pp_brightness; /* -1..1, default 0 */
    float pp_contrast;   /* 0..2, default 1 */
    float pp_hue;        /* -180..180 degrees shift */
    float pp_saturation; /* 0..2, default 1 */
    float fx_smoky;      /* 0 = off, 1 = smoky letters */
    float smoke_decay;   /* how fast smoke fades (0.5-5.0) */
    float smoke_depth;   /* smoke opacity/strength (0.0-2.0) */
    float smoke_zoom;    /* noise scale (0.5-4.0) */
    float smoke_hue;     /* 0..360 degrees */
    float smoke_sat;     /* 0..2 */
    float smoke_val;     /* 0..2 brightness */
    float char_w_px;     /* cell width in pixels */
    float char_h_px;     /* cell height in pixels */
    float shadow_opacity; /* 0 = no shadow, >0 = shadow alpha */
    float shadow_blur;    /* 0..4 gaussian blur radius */
    float is_pixel_font;  /* 1.0 = bitmap font */
    float fx_rain;        /* 0 = off, 1 = raindrop ripple warp */
    float rain_size;      /* drop radius in NDC (0.02 - 0.15) */
    float rain_speed;     /* animation speed multiplier (0.5 - 3.0) */
    float rain_freq;      /* drop density (1.0 - 8.0) */
    float rain_warp;      /* displacement strength (0.005 - 0.05) */
    float fx_crawl;       /* 0 = off, 1 = SpaceWarZ perspective crawl */
} pc;

/* ---- FBM Noise (hash-based, no texture needed) ---- */

/* Simple 2D hash -> pseudo-random float in [0,1] */
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

    /* ---- Diagonal Waves (with organic variation) ---- */
    if (pc.fx_waves > 0.5) {
        float seed = inPos.x * 7.13 + inPos.y * 3.37;
        /* Add slow-moving noise to break the repetitive pattern */
        float n1 = noise(inPos * 3.0 + vec2(pc.time * 0.3, pc.time * 0.2));
        float n2 = noise(inPos * 5.0 + vec2(pc.time * 0.5 + 7.0, pc.time * 0.4 + 13.0));
        float vary = (n1 - 0.5) * 0.4 + (n2 - 0.5) * 0.2;
        float angle = sin(seed * 4.7 + pc.time * 2.0 + vary * 3.0) * 0.015;
        pos.x += sin(seed * 5.3 + pc.time * 2.5 + n1 * 2.0) * 0.002;
        pos.y += cos(seed * 3.1 + pc.time * 1.7 + n2 * 2.0) * 0.003;
        /* Noise-modulated amplitude — waves pulse organically */
        float amp = 1.0 + vary * 0.5;
        float cx = inPos.x;
        float cy = inPos.y;
        pos.x += (pos.y - cy) * sin(angle) * amp;
        pos.y -= (pos.x - cx) * sin(angle) * amp;
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

    /* ---- Raindrop Ripple Warp ---- */
    if (pc.fx_rain > 0.5) {
        float warp_x = 0.0, warp_y = 0.0;
        int max_drops = 12;
        float drop_life = 2.5 / pc.rain_speed;
        float stagger = drop_life / (pc.rain_freq * 1.5);

        for (int i = 0; i < max_drops; i++) {
            /* Each drop slot cycles through epochs — staggered birth times */
            float slot_offset = float(i) * stagger;
            float epoch = floor((pc.time - slot_offset) / drop_life);
            float birth = epoch * drop_life + slot_offset;
            float age = pc.time - birth;
            if (age < 0.0 || age > drop_life) continue;

            /* Drop position: random within text area NDC [-0.85, 0.85] */
            vec2 seed = vec2(epoch * 13.37 + float(i) * 7.13,
                             epoch * 23.71 + float(i) * 31.17);
            float dx = hash(seed) * 1.7 - 0.85;
            float dy = hash(seed + vec2(57.29, 83.41)) * 1.7 - 0.85;

            /* Distance from vertex to drop center */
            float dist = length(inPos - vec2(dx, dy));

            /* Expanding ripple wavefront */
            float ripple_radius = age * pc.rain_speed * 0.35;
            float ring_dist = abs(dist - ripple_radius);

            /* Multiple concentric rings (2-3 visible waves) */
            float wavelength = pc.rain_size * 0.4;
            float phase = (dist - ripple_radius) / max(wavelength, 0.01);
            float ripple = cos(phase * 6.2832) * exp(-ring_dist * ring_dist / (wavelength * wavelength * 2.0));

            /* Fade with age — energy dissipates as ripple expands */
            float fade = exp(-age * pc.rain_speed * 1.0);
            /* Energy spreads over growing circumference */
            fade *= 1.0 / (1.0 + ripple_radius * 3.0);
            /* Only show ripple within a reasonable radius */
            fade *= smoothstep(ripple_radius + pc.rain_size * 2.0, ripple_radius - pc.rain_size, dist);

            /* Central splash bulge — short-lived impact crater */
            float splash_r = pc.rain_size * 0.6;
            float splash = exp(-dist * dist / (splash_r * splash_r))
                         * exp(-age * pc.rain_speed * 5.0);

            /* Radial displacement direction */
            vec2 dir = (dist > 0.001) ? normalize(inPos - vec2(dx, dy)) : vec2(0.0);

            /* Accumulate displacement: ripple waves + central splash */
            float displacement = (ripple * fade + splash * 0.4) * pc.rain_warp;
            warp_x += dir.x * displacement;
            warp_y += dir.y * displacement;
        }

        pos.x += warp_x;
        pos.y += warp_y;
    }

    /* ---- SpaceWarZ Crawl: gradual perspective bend ---- */
    /* Unified tilt for both X and Y so aspect ratio stays natural.
     * NO alpha fade — the fragment shader treats alpha values in
     * [0.59, 0.71] as shadow signals, so fading text alpha makes
     * it get misinterpreted as shadow quads and vanish when drop
     * shadow is off. Perspective shrink alone sells the distance. */
    if (pc.fx_crawl > 0.5) {
        /* Vulkan NDC: y=-1 top, y=+1 bottom.
         * Normalize to [0,1] with 0=bottom, 1=top. */
        float u = clamp((1.0 - pos.y) * 0.5, 0.0, 1.0);

        /* Smooth easing — lower rows barely move, upper rows bend hard */
        float tilt = pow(u, 2.2);

        float depth    = 4.5;
        float vanish_y = -0.98;
        float w = 1.0 / (1.0 + tilt * depth);

        /* Shrink X toward vanishing point at screen center */
        pos.x = pos.x * w;

        /* Pull Y toward horizon at the same rate — preserves aspect ratio */
        pos.y = mix(pos.y, vanish_y, 1.0 - w);
    }

    gl_Position = pos;
    fragUV = inUV;
    fragColor = vec4(inColor, inAlpha);
}
