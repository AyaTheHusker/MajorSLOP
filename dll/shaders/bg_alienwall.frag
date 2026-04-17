#version 450

/* Alien Wall — fractal bioluminescent organic surface.
 *
 * Single unified height field: everything derives from ONE warped domain.
 * Fractal sub-cells live in parent-cell-local coords (locked, not swimming).
 * Looks like flesh pushing through a membrane from underneath. */

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec4 fragColor;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D fontTex;

layout(push_constant) uniform PushConstants {
    float time;
    float speed;
    float cell_scale;
    float warp_strength;
    float hue_shift;
    float saturation;
    float brightness;
    float alpha;
    float res_x;
    float res_y;
    float vein_strength;
    float pulse_speed;
    float glow_intensity;
    float depth_layers;     /* 1..4 fractal depth */
    float beat_hue;
    float beat_bright;
    float beat_zoom;
    float roughness;
    float membrane_mix;
    float tonemap_mode;
    float tonemap_exposure;
    float wet_glossy;
    float fluid_speed;
    float specular_tight;
} pc;

vec2 hash2(vec2 p) {
    p = vec2(dot(p, vec2(127.1, 311.7)),
             dot(p, vec2(269.5, 183.3)));
    return fract(sin(p) * 43758.5453123);
}

float hash1(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
}

float gnoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = dot(hash2(i) * 2.0 - 1.0, f);
    float b = dot(hash2(i + vec2(1, 0)) * 2.0 - 1.0, f - vec2(1, 0));
    float c = dot(hash2(i + vec2(0, 1)) * 2.0 - 1.0, f - vec2(0, 1));
    float d = dot(hash2(i + vec2(1, 1)) * 2.0 - 1.0, f - vec2(1, 1));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y) * 0.5 + 0.5;
}

float fbm(vec2 p, int oct) {
    float v = 0.0, a = 0.5;
    mat2 rot = mat2(0.8, 0.6, -0.6, 0.8);
    for (int i = 0; i < oct; i++) {
        v += a * gnoise(p);
        p = rot * p * 2.0;
        a *= 0.5;
    }
    return v;
}

vec3 hsv2rgb(float h, float s, float v) {
    h = mod(h, 360.0) / 60.0;
    int hi = int(floor(h));
    float f = h - float(hi);
    float pp = v * (1.0 - s);
    float q = v * (1.0 - s * f);
    float t = v * (1.0 - s * (1.0 - f));
    if (hi == 0)      return vec3(v, t, pp);
    else if (hi == 1) return vec3(q, v, pp);
    else if (hi == 2) return vec3(pp, v, t);
    else if (hi == 3) return vec3(pp, q, v);
    else if (hi == 4) return vec3(t, pp, v);
    else              return vec3(v, pp, q);
}

/* Worley returning (d1, d2, nearest_cell_center).
 * Cell centers are STATIC — they don't animate independently.
 * All motion comes from the domain warp applied BEFORE this call,
 * so every element moves as one unified surface. */
struct WCell {
    float d1;
    float d2;
    vec2  center;   /* world-space center of nearest cell */
    vec2  id;       /* grid ID for hashing */
};

WCell worley_cell(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    WCell c;
    c.d1 = 8.0; c.d2 = 8.0;
    c.center = vec2(0.0); c.id = vec2(0.0);
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            vec2 nb = vec2(float(x), float(y));
            vec2 pt = hash2(i + nb);
            /* Static cell positions — NO sin(time) animation here.
             * The organic motion is entirely from the FBM domain warp. */
            vec2 diff = nb + pt - f;
            float dist = dot(diff, diff);
            if (dist < c.d1) {
                c.d2 = c.d1;
                c.d1 = dist;
                c.center = i + nb + pt;
                c.id = i + nb;
            } else if (dist < c.d2) {
                c.d2 = dist;
            }
        }
    }
    c.d1 = sqrt(c.d1);
    c.d2 = sqrt(c.d2);
    return c;
}

/* Height field for the unified surface.
 * Primary cells define the large bulges pushing through the wall.
 * Sub-cells are evaluated in parent-cell-local coords so they're
 * spatially locked to their parent — no independent swimming.
 * Returns a single height value that all effects derive from. */
float surface_height(vec2 p, int levels) {
    WCell outer = worley_cell(p);

    /* Primary: inverted distance = bulge height (close to center = tall) */
    float h = 1.0 - outer.d1;

    if (levels < 2) return h;

    /* Sub-cells: transform into parent-cell-local coordinates.
     * local = (p - center) is where we are inside this cell.
     * Scale it up to create sub-cell structure WITHIN the parent. */
    vec2 local = p - outer.center;

    /* Unique rotation per parent cell so children look different */
    float ang = hash1(outer.id) * 6.2831;
    float ca = cos(ang), sa = sin(ang);
    local = mat2(ca, sa, -sa, ca) * local;

    /* Sub-cell Worley at higher frequency, in local space */
    vec2 sub_p = local * 3.2 + hash2(outer.id) * 100.0;
    WCell sub = worley_cell(sub_p);

    /* Sub-cell height, masked by parent interior (only visible inside bulges) */
    float interior = smoothstep(0.6, 0.2, outer.d1);
    float sub_h = (1.0 - sub.d1) * 0.35 * interior;
    h += sub_h;

    if (levels < 3) return h;

    /* Third level: sub-sub-cells inside each sub-cell */
    vec2 local2 = sub_p - sub.center;
    float ang2 = hash1(sub.id) * 6.2831;
    float ca2 = cos(ang2), sa2 = sin(ang2);
    local2 = mat2(ca2, sa2, -sa2, ca2) * local2;

    vec2 sub2_p = local2 * 3.0 + hash2(sub.id) * 100.0;
    WCell sub2 = worley_cell(sub2_p);

    float interior2 = interior * smoothstep(0.6, 0.2, sub.d1);
    float sub2_h = (1.0 - sub2.d1) * 0.15 * interior2;
    h += sub2_h;

    if (levels < 4) return h;

    /* Fourth level */
    vec2 local3 = sub2_p - sub2.center;
    float ang3 = hash1(sub2.id) * 6.2831;
    float ca3 = cos(ang3), sa3 = sin(ang3);
    local3 = mat2(ca3, sa3, -sa3, ca3) * local3;

    vec2 sub3_p = local3 * 2.8 + hash2(sub2.id) * 100.0;
    WCell sub3 = worley_cell(sub3_p);

    float interior3 = interior2 * smoothstep(0.6, 0.2, sub2.d1);
    h += (1.0 - sub3.d1) * 0.07 * interior3;

    return h;
}

/* Central-difference normal from the unified height field */
vec3 surface_normal(vec2 p, int levels, float eps) {
    float h0 = surface_height(p, levels);
    float hx = surface_height(p + vec2(eps, 0.0), levels);
    float hy = surface_height(p + vec2(0.0, eps), levels);
    return normalize(vec3(h0 - hx, h0 - hy, eps * 3.0));
}

/* Vein pattern from the outer cells + sub-cells */
float surface_veins(vec2 p, int levels) {
    WCell outer = worley_cell(p);
    float edge = outer.d2 - outer.d1;
    float v = smoothstep(0.0, 0.10, edge) * (1.0 - smoothstep(0.10, 0.30, edge));

    if (levels >= 2) {
        vec2 local = p - outer.center;
        float ang = hash1(outer.id) * 6.2831;
        local = mat2(cos(ang), sin(ang), -sin(ang), cos(ang)) * local;
        vec2 sub_p = local * 3.2 + hash2(outer.id) * 100.0;
        WCell sub = worley_cell(sub_p);
        float sub_edge = sub.d2 - sub.d1;
        float sub_v = smoothstep(0.0, 0.10, sub_edge) * (1.0 - smoothstep(0.10, 0.30, sub_edge));
        float interior = smoothstep(0.6, 0.2, outer.d1);
        v = max(v, sub_v * 0.6 * interior);
    }
    return v;
}

void main() {
    vec2 uv = gl_FragCoord.xy / vec2(pc.res_x, pc.res_y);
    float aspect = pc.res_x / pc.res_y;
    float t = pc.time * pc.speed;
    float zoom = 1.0 + pc.beat_zoom;

    /* === UNIFIED DOMAIN — everything derives from this single p === */

    vec2 p = vec2(uv.x * aspect, uv.y) * pc.cell_scale * zoom;

    /* Fluid flow: slow undulation of the entire surface */
    float ft = pc.time * pc.fluid_speed;
    p += vec2(
        sin(p.y * 0.8 + ft * 0.7) * cos(p.x * 0.6 + ft * 0.5),
        cos(p.x * 0.9 + ft * 0.6) * sin(p.y * 0.7 + ft * 0.8)
    ) * 0.15 * pc.fluid_speed;

    /* Domain warp: organic distortion that moves the ENTIRE surface.
     * This is the "pushing through from underneath" motion.
     * All features inherit this warp — nothing moves independently. */
    float warp = pc.warp_strength;
    vec2 w1 = vec2(fbm(p + vec2(t * 0.1, t * 0.13), 4),
                   fbm(p + vec2(t * -0.08, t * 0.11) + 4.3, 4));
    p += w1 * warp;

    vec2 w2 = vec2(fbm(p + vec2(t * 0.05, -t * 0.07) + 7.1, 3),
                   fbm(p + vec2(-t * 0.06, t * 0.04) + 11.5, 3));
    p += w2 * warp * 0.5;

    /* === ALL sampling uses this same warped p === */

    int levels = clamp(int(pc.depth_layers), 1, 4);

    /* Unified height field */
    float h = surface_height(p, levels);

    /* Outer cell for color/edge info */
    WCell outer = worley_cell(p);

    /* Veins */
    float veins = surface_veins(p, levels) * pc.vein_strength;

    /* Surface normal from the same height field */
    float eps = 0.06 / pc.cell_scale;
    vec3 N = surface_normal(p, levels, eps);

    /* Pulsing: slow throb tied to the height field, not independent */
    float pulse = 0.5 + 0.5 * sin(t * pc.pulse_speed + h * 4.0);
    pulse = mix(1.0, pulse, 0.25);

    /* Membrane: same warped p at larger scale — no independent warp */
    float membrane = 0.0;
    if (pc.membrane_mix > 0.01) {
        WCell mem = worley_cell(p * 0.35);
        membrane = mem.d1;
    }

    /* === COLORING — all derived from the unified height field === */

    float hue = mod(h * 80.0 + outer.d1 * 60.0 + hash1(outer.id) * 30.0 +
                    pc.hue_shift + pc.beat_hue, 360.0);
    float sat = clamp(pc.saturation, 0.0, 1.0);
    float val = clamp(h * 0.5 + 0.1, 0.0, 1.0) * pc.brightness * pulse
                + pc.beat_bright * 0.3;

    vec3 color = hsv2rgb(hue, sat, val);

    /* Vein highlights */
    vec3 vein_color = hsv2rgb(mod(hue + 30.0, 360.0), sat * 0.6, 1.2);
    color += vein_color * veins * 0.8;

    /* Bioluminescent glow at the peaks of bulges */
    float glow = smoothstep(0.7, 1.0, h);
    glow *= glow;
    vec3 glow_color = hsv2rgb(mod(hue - 40.0 + pulse * 20.0, 360.0), sat * 0.4, 1.5);
    color += glow_color * glow * pc.glow_intensity * pulse;

    /* === WET / GLOSSY — uses the same unified normal === */

    float wetness = pc.wet_glossy;
    float smooth_fac = 1.0 - pc.roughness;

    if (wetness > 0.01) {
        vec3 light_pos = normalize(vec3(
            sin(t * 0.3) * 0.6,
            cos(t * 0.4) * 0.4,
            0.8
        ));
        vec3 view_dir = vec3(0.0, 0.0, 1.0);
        vec3 half_dir = normalize(light_pos + view_dir);
        float NdotH = max(dot(N, half_dir), 0.0);
        float spec_power = mix(8.0, 128.0, pc.specular_tight);
        float spec = pow(NdotH, spec_power);

        vec3 light2 = normalize(vec3(
            cos(t * 0.25 + 2.0) * 0.5,
            sin(t * 0.35 + 1.5) * 0.5,
            0.7
        ));
        float spec2 = pow(max(dot(normalize(light2 + view_dir), N), 0.0), spec_power * 0.7);

        vec3 spec_col = hsv2rgb(mod(hue + 60.0, 360.0), sat * 0.2, 2.0);
        color += spec_col * (spec * 1.2 + spec2 * 0.5) * wetness * smooth_fac;

        /* Fresnel */
        float fresnel = pow(1.0 - max(dot(N, view_dir), 0.0), 3.0);
        vec3 fresnel_col = hsv2rgb(mod(hue + 90.0, 360.0), sat * 0.3, 1.8);
        color += fresnel_col * fresnel * wetness * 0.4 * smooth_fac;
    }

    /* Caustics — from the same warped p */
    if (wetness > 0.3) {
        float caust = 0.0;
        for (int i = 0; i < 3; i++) {
            float s = 1.0 + float(i) * 0.8;
            WCell cw = worley_cell(p * 0.5 * s);
            float ridge = cw.d2 - cw.d1;
            caust += pow(max(1.0 - ridge * 3.0, 0.0), 3.0) / s;
        }
        caust *= 0.5;
        vec3 caust_col = hsv2rgb(mod(hue + 120.0, 360.0), sat * 0.15, 1.0);
        color += caust_col * caust * (wetness - 0.3) * 0.6 * pulse;
    }

    /* Microtexture — from the same warped p */
    float micro = gnoise(p * 8.0);
    float micro2 = gnoise(p * 16.0);
    color *= 1.0 - (mix(micro, micro2, 0.5) - 0.5) * pc.roughness * 0.5;

    /* Wet darkening in crevices */
    float crevice = smoothstep(0.3, 0.7, h);
    color *= mix(1.0, 0.7 + 0.3 * crevice, wetness * 0.5 * smooth_fac);

    /* Glossy sheen — from the same warped p */
    if (wetness > 0.01) {
        float sheen = fbm(p * 2.0, 3);
        sheen = smoothstep(0.3, 0.7, sheen);
        color += vec3(sheen * 0.08) * wetness * smooth_fac;
    }

    /* Membrane — same p, different scale */
    if (pc.membrane_mix > 0.01) {
        float m = 1.0 - smoothstep(0.0, 0.5, membrane);
        vec3 mem_col = hsv2rgb(mod(hue + 180.0, 360.0), sat * 0.3, 0.3);
        color = mix(color, color + mem_col * m, pc.membrane_mix);
    }

    /* Edge darkening from outer cell boundaries */
    float edge = outer.d2 - outer.d1;
    float edge_dark = smoothstep(0.0, 0.08, edge);
    color *= 0.6 + 0.4 * edge_dark;

    /* Vignette */
    float vign = 1.0 - length(uv - 0.5) * 0.4;
    color *= vign;

    /* Tonemapping */
    if (pc.tonemap_mode > 0.5) {
        color *= pc.tonemap_exposure;
        int tm = int(pc.tonemap_mode + 0.5);
        if (tm == 1) {
            mat3 aces_in = mat3(0.59719, 0.07600, 0.02840,
                                0.35458, 0.90834, 0.13383,
                                0.04823, 0.01566, 0.83777);
            mat3 aces_out = mat3(1.60475, -0.10208, -0.00327,
                                -0.53108,  1.10813, -0.07276,
                                -0.07367, -0.00605,  1.07602);
            vec3 v = aces_in * color;
            vec3 a = v * (v + 0.0245786) - 0.000090537;
            vec3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
            color = aces_out * (a / b);
        } else if (tm == 2) {
            color = color / (color + 1.0);
        } else if (tm == 3) {
            float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
            vec3 x = color;
            color = ((x*(A*x+C*B)+D*E)/(x*(A*x+B)+D*F))-E/F;
            float W = 11.2;
            float wn = ((W*(A*W+C*B)+D*E)/(W*(A*W+B)+D*F))-E/F;
            color /= wn;
        } else if (tm == 4) {
            color = max(color, vec3(1e-6));
            color = log2(color) * 0.18 + 0.5;
            color = clamp(color, 0.0, 1.0);
            color = color * color * (3.0 - 2.0 * color);
        }
        color = clamp(color, 0.0, 1.0);
    }

    outColor = vec4(color, pc.alpha);
}
