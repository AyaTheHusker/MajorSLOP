#version 450

/* Alien Wall — fractal bioluminescent organic surface.
 *
 * Unified domain: all motion from FBM warp + fluid flow.
 * Fractal sub-cells in parent-local coords.
 * Cell styles: Voronoi, Hex, Hive, Geodesic, Mixed.
 * Individual cell migration + per-cell outward bulge variance. */

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
    float cell_move_speed;  /* 0..2 individual cell crawl rate */
    float bulge_strength;   /* 0..2 outward push variance per cell */
    float cell_style;       /* 0=Voronoi 1=Hex 2=Hive 3=Geodesic 4=Mixed */
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

/* Cell center placement based on geometry style.
 * grid_id = integer cell coordinate, style = cell_style push constant.
 * Returns position in [0,1]^2 within the grid cell. */
vec2 cell_point(vec2 grid_id, float style) {
    vec2 rnd = hash2(grid_id);
    int s = int(style + 0.5);

    if (s == 0) {
        /* Voronoi: fully random — classic organic irregular cells */
        return rnd;
    }

    float row_parity = mod(floor(grid_id.y), 2.0);

    if (s == 1) {
        /* Hex: honeycomb lattice with organic jitter */
        vec2 hex = vec2(0.5 + row_parity * 0.5, 0.5);
        return mix(hex, rnd, 0.12);
    }
    if (s == 2) {
        /* Hive: elongated hex (taller cells) */
        vec2 hex = vec2(0.5 + row_parity * 0.5, 0.5);
        return mix(hex, rnd, 0.10);
    }
    if (s == 3) {
        /* Geodesic: triangular lattice */
        vec2 tri = vec2(0.333 + row_parity * 0.333, 0.5);
        return mix(tri, rnd, 0.12);
    }

    /* Mixed: per-cell random choice between voronoi and hex */
    float choice = hash1(grid_id * 17.3 + 91.7);
    vec2 hex = vec2(0.5 + row_parity * 0.5, 0.5);
    if (choice < 0.4)      return rnd;                        /* voronoi */
    else if (choice < 0.7) return mix(hex, rnd, 0.12);        /* hex */
    else if (choice < 0.85) {
        vec2 tri = vec2(0.333 + row_parity * 0.333, 0.5);
        return mix(tri, rnd, 0.12);                            /* geodesic */
    }
    return mix(hex, rnd, 0.10);                                /* hive */
}

/* Per-cell individual drift — cells slowly crawl around like living organisms.
 * Each cell has its own random direction and phase. */
vec2 cell_drift(vec2 grid_id, float t) {
    if (pc.cell_move_speed < 0.01) return vec2(0.0);
    vec2 dir = hash2(grid_id + 73.1) * 2.0 - 1.0;
    float phase = hash1(grid_id + 31.7) * 6.2831;
    float phase2 = hash1(grid_id + 57.3) * 6.2831;
    /* Two sine waves for figure-8-ish organic crawl */
    return vec2(
        dir.x * sin(t * pc.cell_move_speed * 0.7 + phase),
        dir.y * sin(t * pc.cell_move_speed * 0.9 + phase2)
    ) * 0.18;
}

struct WCell {
    float d1;
    float d2;
    vec2  center;
    vec2  id;
};

WCell worley_cell(vec2 p, float style, float t) {
    /* For hive style, pre-stretch y to create elongated cells */
    vec2 sp = p;
    if (int(style + 0.5) == 2) sp.y *= 0.65;

    vec2 i = floor(sp);
    vec2 f = fract(sp);
    WCell c;
    c.d1 = 8.0; c.d2 = 8.0;
    c.center = vec2(0.0); c.id = vec2(0.0);

    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            vec2 nb = vec2(float(x), float(y));
            vec2 gid = i + nb;
            vec2 pt = cell_point(gid, style);
            pt += cell_drift(gid, t);
            vec2 diff = nb + pt - f;
            float dist = dot(diff, diff);
            if (dist < c.d1) {
                c.d2 = c.d1;
                c.d1 = dist;
                /* Un-stretch center for hive */
                if (int(style + 0.5) == 2) {
                    c.center = vec2((gid.x + pt.x), (gid.y + pt.y) / 0.65);
                } else {
                    c.center = gid + pt;
                }
                c.id = gid;
            } else if (dist < c.d2) {
                c.d2 = dist;
            }
        }
    }
    c.d1 = sqrt(c.d1);
    c.d2 = sqrt(c.d2);
    return c;
}

/* Per-cell outward bulge — some cells push through more than others.
 * Slowly swells and recedes for living feel. */
float cell_bulge(vec2 cell_id, float t) {
    float base = hash1(cell_id + 91.3);
    base = mix(0.4, 1.6, base);
    float swell = sin(t * pc.pulse_speed * 0.3 + hash1(cell_id + 47.1) * 6.2831);
    return base * (1.0 + swell * 0.25);
}

/* Unified height field with fractal sub-cells and per-cell bulge.
 * Bulge adds dome-shaped displacement so cells physically push outward. */
float surface_height(vec2 p, int levels, float style, float t) {
    WCell outer = worley_cell(p, style, t);

    float h = 1.0 - outer.d1;

    /* Per-cell 3D dome: peaks at cell center, falls to zero at edges.
     * Each cell gets random protrusion height — creates real depth variance. */
    float dome = smoothstep(0.8, 0.0, outer.d1);
    float bulge = cell_bulge(outer.id, t);
    h += dome * bulge * pc.bulge_strength * 0.5;

    if (levels < 2) return h;

    /* Sub-cells in parent-local coords */
    vec2 local = p - outer.center;
    float ang = hash1(outer.id) * 6.2831;
    float ca = cos(ang), sa = sin(ang);
    local = mat2(ca, sa, -sa, ca) * local;

    /* Sub-cells use voronoi regardless of outer style for organic variety */
    vec2 sub_p = local * 3.2 + hash2(outer.id) * 100.0;
    WCell sub = worley_cell(sub_p, 0.0, t);

    float interior = smoothstep(0.6, 0.2, outer.d1);
    float sub_h = (1.0 - sub.d1) * 0.35 * interior;
    /* Sub-cell dome pushes against parent cell interior */
    float sub_dome = smoothstep(0.7, 0.0, sub.d1) * interior;
    float sub_bulge = cell_bulge(sub.id + 200.0, t);
    sub_h += sub_dome * sub_bulge * pc.bulge_strength * 0.3;
    h += sub_h;

    if (levels < 3) return h;

    vec2 local2 = sub_p - sub.center;
    float ang2 = hash1(sub.id) * 6.2831;
    local2 = mat2(cos(ang2), sin(ang2), -sin(ang2), cos(ang2)) * local2;
    vec2 sub2_p = local2 * 3.0 + hash2(sub.id) * 100.0;
    WCell sub2 = worley_cell(sub2_p, 0.0, t);

    float interior2 = interior * smoothstep(0.6, 0.2, sub.d1);
    float sub2_h = (1.0 - sub2.d1) * 0.15 * interior2;
    float sub2_dome = smoothstep(0.7, 0.0, sub2.d1) * interior2;
    sub2_h += sub2_dome * cell_bulge(sub2.id + 400.0, t) * pc.bulge_strength * 0.15;
    h += sub2_h;

    if (levels < 4) return h;

    vec2 local3 = sub2_p - sub2.center;
    float ang3 = hash1(sub2.id) * 6.2831;
    local3 = mat2(cos(ang3), sin(ang3), -sin(ang3), cos(ang3)) * local3;
    vec2 sub3_p = local3 * 2.8 + hash2(sub2.id) * 100.0;
    WCell sub3 = worley_cell(sub3_p, 0.0, t);

    float interior3 = interior2 * smoothstep(0.6, 0.2, sub2.d1);
    h += (1.0 - sub3.d1) * 0.07 * interior3;

    return h;
}

vec3 surface_normal(vec2 p, int levels, float style, float t, float eps) {
    float h0 = surface_height(p, levels, style, t);
    float hx = surface_height(p + vec2(eps, 0.0), levels, style, t);
    float hy = surface_height(p + vec2(0.0, eps), levels, style, t);
    return normalize(vec3(h0 - hx, h0 - hy, eps * 3.0));
}

float surface_veins(vec2 p, int levels, float style, float t) {
    WCell outer = worley_cell(p, style, t);
    float edge = outer.d2 - outer.d1;
    float v = smoothstep(0.0, 0.10, edge) * (1.0 - smoothstep(0.10, 0.30, edge));

    if (levels >= 2) {
        vec2 local = p - outer.center;
        float ang = hash1(outer.id) * 6.2831;
        local = mat2(cos(ang), sin(ang), -sin(ang), cos(ang)) * local;
        vec2 sub_p = local * 3.2 + hash2(outer.id) * 100.0;
        WCell sub = worley_cell(sub_p, 0.0, t);
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
    float style = pc.cell_style;

    vec2 p = vec2(uv.x * aspect, uv.y) * pc.cell_scale * zoom;

    /* Fluid flow: whole-surface undulation */
    float ft = pc.time * pc.fluid_speed;
    p += vec2(
        sin(p.y * 0.8 + ft * 0.7) * cos(p.x * 0.6 + ft * 0.5),
        cos(p.x * 0.9 + ft * 0.6) * sin(p.y * 0.7 + ft * 0.8)
    ) * 0.15 * pc.fluid_speed;

    /* Domain warp: organic distortion — the "pushing through" motion */
    float warp = pc.warp_strength;
    vec2 w1 = vec2(fbm(p + vec2(t * 0.1, t * 0.13), 4),
                   fbm(p + vec2(t * -0.08, t * 0.11) + 4.3, 4));
    p += w1 * warp;

    vec2 w2 = vec2(fbm(p + vec2(t * 0.05, -t * 0.07) + 7.1, 3),
                   fbm(p + vec2(-t * 0.06, t * 0.04) + 11.5, 3));
    p += w2 * warp * 0.5;

    int levels = clamp(int(pc.depth_layers), 1, 4);

    float h = surface_height(p, levels, style, t);
    WCell outer = worley_cell(p, style, t);
    float veins = surface_veins(p, levels, style, t) * pc.vein_strength;

    float eps = 0.06 / pc.cell_scale;
    vec3 N = surface_normal(p, levels, style, t, eps);

    float pulse = 0.5 + 0.5 * sin(t * pc.pulse_speed + h * 4.0);
    pulse = mix(1.0, pulse, 0.25);

    float membrane = 0.0;
    if (pc.membrane_mix > 0.01) {
        membrane = hash1(outer.id * 0.37 + 13.7);
    }

    /* Coloring */
    float hue = mod(h * 80.0 + outer.d1 * 60.0 + hash1(outer.id) * 30.0 +
                    pc.hue_shift + pc.beat_hue, 360.0);
    float sat = clamp(pc.saturation, 0.0, 1.0);
    float val = clamp(h * 0.5 + 0.1, 0.0, 1.0) * pc.brightness * pulse
                + pc.beat_bright * 0.3;

    vec3 color = hsv2rgb(hue, sat, val);

    vec3 vein_color = hsv2rgb(mod(hue + 30.0, 360.0), sat * 0.6, 1.2);
    color += vein_color * veins * 0.8;

    float glow = smoothstep(0.7, 1.0, h);
    glow *= glow;
    vec3 glow_color = hsv2rgb(mod(hue - 40.0 + pulse * 20.0, 360.0), sat * 0.4, 1.5);
    color += glow_color * glow * pc.glow_intensity * pulse;

    /* Wet / glossy */
    float wetness = pc.wet_glossy;
    float smooth_fac = 1.0 - pc.roughness;

    if (wetness > 0.01) {
        vec3 light_pos = normalize(vec3(0.4, 0.3, 0.8));
        vec3 view_dir = vec3(0.0, 0.0, 1.0);
        vec3 half_dir = normalize(light_pos + view_dir);
        float spec_power = mix(8.0, 128.0, pc.specular_tight);
        float spec = pow(max(dot(N, half_dir), 0.0), spec_power);

        vec3 light2 = normalize(vec3(-0.3, 0.4, 0.7));
        float spec2 = pow(max(dot(normalize(light2 + view_dir), N), 0.0), spec_power * 0.7);

        vec3 spec_col = hsv2rgb(mod(hue + 60.0, 360.0), sat * 0.2, 2.0);
        color += spec_col * (spec * 1.2 + spec2 * 0.5) * wetness * smooth_fac;

        float fresnel = pow(1.0 - max(dot(N, view_dir), 0.0), 3.0);
        vec3 fresnel_col = hsv2rgb(mod(hue + 90.0, 360.0), sat * 0.3, 1.8);
        color += fresnel_col * fresnel * wetness * 0.4 * smooth_fac;
    }

    if (wetness > 0.3) {
        float edge_prox = outer.d2 - outer.d1;
        float caust = pow(max(1.0 - edge_prox * 3.0, 0.0), 3.0);
        caust += pow(max(1.0 - edge_prox * 5.0, 0.0), 5.0) * 0.5;
        caust += smoothstep(0.15, 0.0, outer.d1) * 0.3;
        caust *= 0.4;
        vec3 caust_col = hsv2rgb(mod(hue + 120.0, 360.0), sat * 0.15, 1.0);
        color += caust_col * caust * (wetness - 0.3) * 0.6 * pulse;
    }

    float micro = gnoise(p * 8.0);
    float micro2 = gnoise(p * 16.0);
    color *= 1.0 - (mix(micro, micro2, 0.5) - 0.5) * pc.roughness * 0.5;

    float crevice = smoothstep(0.3, 0.7, h);
    color *= mix(1.0, 0.7 + 0.3 * crevice, wetness * 0.5 * smooth_fac);

    if (wetness > 0.01) {
        float sheen = smoothstep(0.3, 0.8, h);
        color += vec3(sheen * 0.08) * wetness * smooth_fac;
    }

    if (pc.membrane_mix > 0.01) {
        float m = 1.0 - smoothstep(0.0, 0.5, membrane);
        vec3 mem_col = hsv2rgb(mod(hue + 180.0, 360.0), sat * 0.3, 0.3);
        color = mix(color, color + mem_col * m, pc.membrane_mix);
    }

    float edge = outer.d2 - outer.d1;
    float edge_dark = smoothstep(0.0, 0.08, edge);
    color *= 0.6 + 0.4 * edge_dark;

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
