#version 450

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec4 fragColor;

layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D fontTex;

layout(push_constant) uniform PushConstants {
    float time;
    float speed;
    float turbulence;
    float complexity;     /* 1..8 octaves */
    float hue_shift;      /* 0..360 degrees */
    float saturation;     /* 0..2 */
    float brightness;     /* 0..2 */
    float alpha;          /* 0..1 overall opacity */
    float res_x;          /* viewport width */
    float res_y;          /* viewport height */
    float sobel_str;      /* 0..2 sobel/emboss strength */
    float sobel_angle;    /* 0..360 light direction */
    float edge_glow;      /* 0..2 edge glow intensity */
    float sharpen_str;    /* 0..2 sharpen strength */
    float beat_hue;       /* beat-reactive hue offset */
    float beat_bright;    /* beat-reactive brightness boost */
    float beat_zoom;      /* beat-reactive zoom pulse */
    float material_type;  /* 0=glossy,1=wet,2=metallic,3=matte */
    float material_str;   /* 0..2 material strength */
    float _pad0;
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

/* Domain-warped FBM: deep, organic, 3D-looking cloud patterns.
 * Each warp feeds FBM output back as coordinate offset, creating
 * turbulent folding structures instead of flat scrolling layers. */
float plasma_field(vec2 puv, float t, int octaves, float turb) {
    vec2 p = puv * (2.5 + turb);

    /* First FBM layer — base shape */
    float f1 = fbm(p + vec2(t * 0.15, t * 0.12), octaves);
    float f2 = fbm(p + vec2(t * -0.1, t * 0.18) + 5.2, octaves);

    /* Domain warp: distort coords with first layer's output */
    vec2 warp1 = vec2(f1, f2) * (2.0 + turb * 2.0);
    float f3 = fbm(p + warp1 + vec2(t * 0.08, -t * 0.06), octaves);
    float f4 = fbm(p + warp1 + vec2(-t * 0.12, t * 0.09) + 8.1, octaves);

    /* Second domain warp — deeper folding */
    vec2 warp2 = vec2(f3, f4) * (1.5 + turb);
    float result = fbm(p + warp2 + vec2(t * 0.04, t * 0.03), octaves);

    /* Mix in some of the intermediate layers for richness */
    result = result * 0.6 + f3 * 0.25 + f1 * 0.15;

    return result;
}

/* Fast single-octave noise for edge detection (much cheaper than full FBM) */
float plasma_fast(vec2 puv, float t, float turb) {
    vec2 p = puv * (2.5 + turb);
    float f1 = noise(p + vec2(t * 0.15, t * 0.12));
    float f2 = noise(p + vec2(t * -0.1, t * 0.18) + 5.2);
    vec2 warp = vec2(f1, f2) * (2.0 + turb * 2.0);
    return noise(p + warp + vec2(t * 0.04, t * 0.03));
}

void main() {
    vec2 uv = gl_FragCoord.xy / vec2(pc.res_x, pc.res_y);
    float aspect = pc.res_x / pc.res_y;
    float zoom = pc.beat_zoom;
    vec2 puv = vec2(uv.x * aspect, uv.y) * (1.0 + zoom);

    float t = pc.time * pc.speed;
    int octaves = clamp(int(pc.complexity), 1, 8);

    float plasma = plasma_field(puv, t, octaves, pc.turbulence);

    /* Map to color via HSV with beat-reactive hue and brightness */
    float hue = mod(plasma * 360.0 + pc.hue_shift + pc.beat_hue, 360.0);
    float sat = clamp(pc.saturation, 0.0, 1.0);
    float val = clamp(plasma * 0.5 + 0.25, 0.0, 1.0) * (pc.brightness + pc.beat_bright);

    vec3 color = hsv2rgb(hue, sat, val);

    /* ---- Sharpen (on full plasma) ---- */
    if (pc.sharpen_str > 0.01) {
        /* Use wider spacing (8px) for visible effect */
        vec2 px = 8.0 / vec2(pc.res_x, pc.res_y);
        vec2 apx = vec2(px.x * aspect, px.y);
        float pC = plasma;
        float pL = plasma_fast(puv - vec2(apx.x, 0.0), t, pc.turbulence);
        float pR = plasma_fast(puv + vec2(apx.x, 0.0), t, pc.turbulence);
        float pU = plasma_fast(puv - vec2(0.0, apx.y), t, pc.turbulence);
        float pD = plasma_fast(puv + vec2(0.0, apx.y), t, pc.turbulence);
        float blur = (pL + pR + pU + pD) * 0.25;
        float sharp_p = pC + (pC - blur) * pc.sharpen_str * 4.0;
        sharp_p = clamp(sharp_p, 0.0, 1.0);
        float sh = mod(sharp_p * 360.0 + pc.hue_shift + pc.beat_hue, 360.0);
        float sv = clamp(sharp_p * 0.5 + 0.25, 0.0, 1.0) * (pc.brightness + pc.beat_bright);
        color = hsv2rgb(sh, sat, sv);
    }

    /* ---- Sobel / Emboss / Edge Glow / Material ---- */
    if (pc.sobel_str > 0.01 || pc.edge_glow > 0.01) {
        /* Sample fast noise at 6px spacing for visible gradients */
        vec2 px = 6.0 / vec2(pc.res_x, pc.res_y);
        vec2 apx = vec2(px.x * aspect, px.y);

        float tl = plasma_fast(puv + vec2(-apx.x, -apx.y), t, pc.turbulence);
        float tc = plasma_fast(puv + vec2(  0.0,  -apx.y), t, pc.turbulence);
        float tr = plasma_fast(puv + vec2( apx.x, -apx.y), t, pc.turbulence);
        float ml = plasma_fast(puv + vec2(-apx.x,   0.0),  t, pc.turbulence);
        float mr = plasma_fast(puv + vec2( apx.x,   0.0),  t, pc.turbulence);
        float bl = plasma_fast(puv + vec2(-apx.x,  apx.y), t, pc.turbulence);
        float bc = plasma_fast(puv + vec2(  0.0,   apx.y), t, pc.turbulence);
        float br = plasma_fast(puv + vec2( apx.x,  apx.y), t, pc.turbulence);

        float sx = (-tl - 2.0*ml - bl) + (tr + 2.0*mr + br);
        float sy = (-tl - 2.0*tc - tr) + (bl + 2.0*bc + br);
        float edgeMag = length(vec2(sx, sy));

        /* Emboss lighting */
        if (pc.sobel_str > 0.01) {
            float rad = radians(pc.sobel_angle);
            vec3 lightDir = normalize(vec3(cos(rad), sin(rad), 0.7));
            vec3 normal = normalize(vec3(-sx * 8.0, -sy * 8.0, 1.0));
            vec3 viewDir = vec3(0.0, 0.0, 1.0);

            float NdotL = dot(normal, lightDir);
            float diffuse = max(NdotL * 0.5 + 0.5, 0.0);

            vec3 halfVec = normalize(lightDir + viewDir);
            float fresnel = pow(1.0 - max(dot(normal, viewDir), 0.0), 3.0);

            /* Material properties */
            int mat = int(pc.material_type);
            float mstr = pc.material_str;
            float spec_power = 40.0;
            float spec_int = 0.5;
            float diff_int = 0.8;
            vec3 spec_color = vec3(1.0, 0.98, 0.95);
            float fresnel_int = 0.0;

            if (mat == 0) {
                /* Glossy: sharp specular, moderate diffuse */
                spec_power = 60.0;
                spec_int = 0.6 * mstr;
                diff_int = 0.85;
            } else if (mat == 1) {
                /* Wet: high specular, strong Fresnel, tight highlight */
                spec_power = 120.0;
                spec_int = 0.8 * mstr;
                diff_int = 0.7;
                fresnel_int = 0.4 * mstr;
                spec_color = mix(vec3(1.0), color, 0.2); /* slightly colored reflection */
            } else if (mat == 2) {
                /* Metallic: specular takes base color, hard highlights */
                spec_power = 80.0;
                spec_int = 0.7 * mstr;
                diff_int = 0.6;
                spec_color = color * 1.5; /* colored specular */
                fresnel_int = 0.3 * mstr;
            } else if (mat == 3) {
                /* Matte: soft diffuse, almost no specular */
                spec_power = 8.0;
                spec_int = 0.05 * mstr;
                diff_int = 0.95;
            }

            float spec = pow(max(dot(normal, halfVec), 0.0), spec_power);

            vec3 litColor = color * (diffuse * diff_int + (1.0 - diff_int));
            litColor += spec_color * spec * spec_int;
            litColor += color * fresnel * fresnel_int;

            /* Edge depth */
            litColor *= 1.0 - edgeMag * 0.2 * pc.sobel_str;

            color = mix(color, litColor, pc.sobel_str);
        }

        /* Edge glow */
        if (pc.edge_glow > 0.01) {
            float edge = smoothstep(0.02, 0.3, edgeMag);
            vec3 glowColor = hsv2rgb(mod(hue + 60.0, 360.0), sat * 0.7, 1.2);
            color += glowColor * edge * pc.edge_glow * 0.8;
        }
    }

    /* Subtle vignette */
    float vign = 1.0 - length(uv - 0.5) * 0.5;
    color *= vign;

    outColor = vec4(color, pc.alpha);
}
