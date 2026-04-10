#version 450

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec4 fragColor;

layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D fontTex;

/* Push constant for time-based animation */
layout(push_constant) uniform PushConstants {
    float time;
    float fx_mode;       /* 0 = normal, 1 = liquid letters */
    float fx_waves;      /* 0 = off, 1 = diagonal waves (vertex shader) */
    float fx_scanlines;  /* 0 = off, 1 = CRT scanlines */
    float fx_fbm;        /* 0 = off, 1 = FBM currents (vertex shader) */
    float fx_sobel;      /* 0 = off, 1 = sobel/sharp/plastic */
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
    float is_pixel_font;  /* 1.0 = bitmap font, skip text sharpening */
    float fx_rain;        /* 0 = off, 1 = raindrop warp (vertex only) */
    float rain_size;      /* vertex only */
    float rain_speed;     /* vertex only */
    float rain_freq;      /* vertex only */
    float rain_warp;      /* vertex only */
} pc;

/* --- Helpers --- */

vec3 palette(float t) {
    vec3 a = vec3(0.5, 0.5, 0.5);
    vec3 b = vec3(0.5, 0.5, 0.5);
    vec3 c = vec3(1.0, 1.0, 1.0);
    vec3 d = vec3(0.00, 0.33, 0.67);
    return a + b * cos(6.28318 * (c * t + d));
}

void main() {
    /* ---- Color emoji passthrough (icon bar) ---- */
    if (fragColor.r < -0.5) {
        vec4 tex = texture(fontTex, fragUV);
        outColor = vec4(tex.rgb, tex.a * fragColor.a);
        return;
    }

    /* ---- Base text rendering (normal or liquid) ---- */
    if (pc.fx_mode < 0.5 || pc.is_pixel_font > 0.5) {
        /* Normal mode (or pixel font — skip liquid to avoid neighbor-sampling artifacts) */
        float texAlpha = texture(fontTex, fragUV).a;
        outColor = vec4(fragColor.rgb, fragColor.a * texAlpha);
    } else {
        /* ---- Liquid Letters Mode ---- */
        float d = texture(fontTex, fragUV).a;
        if (d < 0.01) {
            outColor = vec4(fragColor.rgb, 0.0);
        } else {
            vec2 texSize = vec2(textureSize(fontTex, 0));
            vec2 texel = 1.0 / texSize;
            float dx = texture(fontTex, fragUV + vec2(texel.x, 0)).a
                     - texture(fontTex, fragUV - vec2(texel.x, 0)).a;
            float dy = texture(fontTex, fragUV + vec2(0, texel.y)).a
                     - texture(fontTex, fragUV - vec2(0, texel.y)).a;

            vec3 normal = normalize(vec3(-dx * 4.0, -dy * 4.0, 1.0));
            float noise = sin(fragUV.x * 40.0 + pc.time * 2.0) *
                          cos(fragUV.y * 40.0 + pc.time * 1.5) * 0.15;
            normal.x += noise;
            normal.y += noise * 0.7;
            normal = normalize(normal);

            vec3 lightPos1 = vec3(0.5 + 0.4 * cos(pc.time * 0.8),
                                  0.5 + 0.4 * sin(pc.time * 0.8), 0.6);
            vec3 lightPos2 = vec3(0.5 + 0.3 * cos(pc.time * 1.2 + 2.0),
                                  0.5 + 0.3 * sin(pc.time * 1.2 + 2.0), 0.5);

            vec3 fragPos = vec3(fragUV, 0.0);
            vec3 viewDir = vec3(0.0, 0.0, 1.0);

            vec3 lightDir1 = normalize(lightPos1 - fragPos);
            float diff1 = max(dot(normal, lightDir1), 0.0);
            vec3 halfDir1 = normalize(lightDir1 + viewDir);
            float spec1 = pow(max(dot(normal, halfDir1), 0.0), 64.0);

            vec3 lightDir2 = normalize(lightPos2 - fragPos);
            float diff2 = max(dot(normal, lightDir2), 0.0);
            vec3 halfDir2 = normalize(lightDir2 + viewDir);
            float spec2 = pow(max(dot(normal, halfDir2), 0.0), 48.0);

            float iridescence = dot(normal, viewDir);
            vec3 baseColor = fragColor.rgb * 0.6 + palette(iridescence + pc.time * 0.1) * 0.4;

            float fresnel = pow(1.0 - max(dot(normal, viewDir), 0.0), 3.0);

            vec3 ambient = baseColor * 0.15;
            vec3 diffuse = baseColor * (diff1 * 0.6 + diff2 * 0.4);
            vec3 specular = vec3(1.0, 0.95, 0.9) * spec1 * 0.8
                          + vec3(0.8, 0.9, 1.0) * spec2 * 0.6;
            vec3 rim = baseColor * fresnel * 0.4;

            vec3 finalColor = ambient + diffuse + specular + rim;
            finalColor += (spec1 + spec2) * 0.15;

            float edge = smoothstep(0.0, 0.5, d);
            finalColor *= edge;

            float alpha = fragColor.a * smoothstep(0.05, 0.3, d);
            outColor = vec4(finalColor, alpha);
        }
    }

    /* ---- Drop Shadow pass ---- */
    /* Shadow quads have alpha=0.65 signal. Apply gaussian blur to font alpha
     * for soft shadow edges, then output shadow color at shadow opacity. */
    bool is_shadow = (fragColor.a > 0.59 && fragColor.a < 0.71);
    if (is_shadow) {
        float shadowAlpha;
        if (pc.is_pixel_font > 0.5) {
            /* Sharp shadow for bitmap fonts — no neighbor sampling to avoid
             * artifact lines at atlas cell boundaries */
            shadowAlpha = texture(fontTex, fragUV).a;
        } else {
            vec2 texSize = vec2(textureSize(fontTex, 0));
            vec2 tx = 1.0 / texSize;
            float blurR = max(pc.shadow_blur, 0.1);
            /* 5x5 gaussian blur on font alpha for soft edges */
            float totalW = 0.0;
            shadowAlpha = 0.0;
            for (int dy = -2; dy <= 2; dy++) {
                for (int dx = -2; dx <= 2; dx++) {
                    float w = exp(-float(dx*dx + dy*dy) / (blurR * blurR * 2.0));
                    shadowAlpha += texture(fontTex, fragUV + vec2(float(dx), float(dy)) * tx * blurR).a * w;
                    totalW += w;
                }
            }
            shadowAlpha /= totalW;
        }
        outColor = vec4(fragColor.rgb, shadowAlpha * pc.shadow_opacity);
        /* Shadow quads skip all subsequent passes */
        return;
    }

    /* ---- Smoky Letters pass ---- */
    /* Screen-space smoke field that flows continuously across the terminal.
     * Uses gl_FragCoord for noise (no quad boundary artifacts).
     * Letter proximity from font texture controls local intensity.
     * Smoke creeps around letters, never covers them.
     * Skip smoke entirely for text with background (recap etc: alpha=0.75) */
    bool has_bg_text = (fragColor.a > 0.7 && fragColor.a < 0.8);
    if (pc.fx_smoky > 0.5 && !has_bg_text) {
        float center_a = texture(fontTex, fragUV).a;
        float t = pc.time;
        bool is_smoke_quad = (fragColor.a < 0.01);
        float cw = max(pc.char_w_px, 4.0);
        float ch = max(pc.char_h_px, 8.0);

        /* Screen-space position within column (continuous, no cell boundaries) */
        float col_x = mod(gl_FragCoord.x, cw) / cw;  /* 0-1 within column */
        /* Absolute screen Y in cell units (continuous across all cells) */
        float screen_row = gl_FragCoord.y / ch;

        float smoke_alpha = 0.0;

        if (is_smoke_quad) {
            /* ---- Wisp rising from letter below ---- */
            float rise_cells = fragColor.g;   /* how many cells above the source letter */
            float col_seed = fragColor.b;     /* unique per-column seed */

            /* Source letter's screen row (continuous, derived from this fragment's position) */
            float source_row = screen_row + rise_cells;
            /* Continuous rise height from source (0 = at letter, increases upward) */
            float h = source_row - screen_row;
            float h_norm = h * pc.smoke_zoom * 0.5;

            /* Generate 3 wisps per column with different seeds for organic look */
            for (int w = 0; w < 3; w++) {
                float seed = col_seed * 37.0 + float(w) * 13.7;
                float wisp_x = 0.5 + float(w - 1) * 0.15; /* offset each wisp */

                /* Brownian horizontal displacement — integrated sine waves.
                 * Uses screen_row (continuous) so wisps flow smoothly across cells */
                float sr = screen_row * pc.smoke_zoom * 0.5;
                float dx = cos(sr * 2.3 + seed + t * 0.6) * 0.12
                         + cos(sr * 5.7 + seed * 1.7 + t * 0.35) * 0.07
                         + cos(sr * 11.3 + seed * 2.9 + t * 0.8) * 0.04
                         + cos(sr * 23.0 + seed * 4.1 + t * 1.1) * 0.02;
                /* Displacement grows with height (wisp wanders more as it rises) */
                dx *= (1.0 + h * 0.4);

                /* Distance from this wisp's centerline */
                float dist = abs(col_x - wisp_x - dx);

                /* Gaussian cross-section — thin tendril */
                float sharpness = 30.0 + float(w) * 15.0; /* inner wisps thinner */
                float wisp = exp(-dist * dist * sharpness);

                /* Vertical fade with height — decay controls how fast */
                float fade = exp(-h * pc.smoke_decay * 0.4);

                /* Add subtle internal turbulence (wisp density variation) */
                float turb = 0.6 + 0.4 * sin(sr * 8.0 + seed * 3.0 + t * 1.5)
                                       * sin(sr * 13.0 - seed * 2.0 + t * 0.9);

                smoke_alpha += wisp * fade * turb * pc.smoke_depth;
            }
            smoke_alpha = clamp(smoke_alpha, 0.0, 0.85);

        } else {
            /* ---- Letter cell: smoke in empty space around glyph ---- */
            /* The glyph IS the smoke source. Empty space (low center_a) in
             * this cell should show wisp bases, matching the wisps in smoke
             * quads directly above. This eliminates the black gap between
             * glyph body and rising smoke. */
            float empty = 1.0 - smoothstep(0.02, 0.25, center_a);
            if (empty > 0.01) {
                float col_idx = floor(gl_FragCoord.x / cw);
                float col_seed = fract(col_idx * 0.2851 + 0.067);
                float cell_y = mod(gl_FragCoord.y, ch) / ch; /* 0=top, 1=bottom */
                /* Stronger at top of cell (closer to smoke quads above) */
                float top_fade = 1.0 - cell_y * 0.6;
                float sr = screen_row * pc.smoke_zoom * 0.5;
                for (int w = 0; w < 3; w++) {
                    float seed = col_seed * 37.0 + float(w) * 13.7;
                    float wisp_x = 0.5 + float(w - 1) * 0.15;
                    float dx = cos(sr * 2.3 + seed + t * 0.6) * 0.12
                             + cos(sr * 5.7 + seed * 1.7 + t * 0.35) * 0.07;
                    dx *= 0.3;
                    float dist = abs(col_x - wisp_x - dx);
                    float wisp = exp(-dist * dist * 35.0);
                    smoke_alpha += wisp * empty * top_fade * pc.smoke_depth * 0.4;
                }
            }
            smoke_alpha = clamp(smoke_alpha, 0.0, 0.85);
        }

        if (smoke_alpha > 0.003) {
            /* HSV → RGB */
            float sh = mod(pc.smoke_hue, 360.0) / 60.0;
            int shi = int(floor(sh));
            float sf = sh - float(shi);
            float sv = clamp(pc.smoke_val, 0.0, 2.0);
            float ss = clamp(pc.smoke_sat, 0.0, 1.0);
            float sp2 = sv * (1.0 - ss);
            float sq2 = sv * (1.0 - ss * sf);
            float st2 = sv * (1.0 - ss * (1.0 - sf));
            vec3 smoke_color;
            if (shi == 0)      smoke_color = vec3(sv, st2, sp2);
            else if (shi == 1) smoke_color = vec3(sq2, sv, sp2);
            else if (shi == 2) smoke_color = vec3(sp2, sv, st2);
            else if (shi == 3) smoke_color = vec3(sp2, sq2, sv);
            else if (shi == 4) smoke_color = vec3(st2, sp2, sv);
            else               smoke_color = vec3(sv, sp2, sq2);

            /* Composite smoke behind existing content.
             * Use straight (non-premultiplied) smoke_color so hardware
             * alpha blending (SRC_ALPHA, 1-SRC_ALPHA) doesn't square it. */
            outColor.rgb = mix(smoke_color, outColor.rgb, outColor.a);
            outColor.a = max(outColor.a, smoke_alpha);
        }
    }

    /* ---- Dark outline for bg-highlighted text (recap etc.) ---- */
    /* alpha=0.75 signals text-on-bg: sample neighbors to create dark stroke,
     * making text readable regardless of hue shift or background color.
     * Restore alpha to 1.0 after applying outline so blending works normally. */
    if (has_bg_text && pc.is_pixel_font < 0.5) {
        float texAlpha = texture(fontTex, fragUV).a;
        vec2 texSize = vec2(textureSize(fontTex, 0));
        vec2 tx = 1.5 / texSize;  /* 1.5 texel spread for outline */
        /* Sample 8 neighbors for dilated glyph mask */
        float nb = 0.0;
        nb = max(nb, texture(fontTex, fragUV + vec2(-tx.x, 0.0)).a);
        nb = max(nb, texture(fontTex, fragUV + vec2( tx.x, 0.0)).a);
        nb = max(nb, texture(fontTex, fragUV + vec2(0.0, -tx.y)).a);
        nb = max(nb, texture(fontTex, fragUV + vec2(0.0,  tx.y)).a);
        nb = max(nb, texture(fontTex, fragUV + vec2(-tx.x, -tx.y)).a);
        nb = max(nb, texture(fontTex, fragUV + vec2( tx.x, -tx.y)).a);
        nb = max(nb, texture(fontTex, fragUV + vec2(-tx.x,  tx.y)).a);
        nb = max(nb, texture(fontTex, fragUV + vec2( tx.x,  tx.y)).a);
        /* Outline: where neighbors have glyph but this pixel doesn't */
        float outline = smoothstep(0.1, 0.5, nb) * (1.0 - smoothstep(0.05, 0.3, texAlpha));
        /* Composite: dark outline behind, then text color on top */
        if (outline > 0.01) {
            outColor.rgb = mix(outColor.rgb, vec3(0.0), outline * 0.9);
            outColor.a = max(outColor.a, outline * 0.85);
        }
        /* Fix alpha back to 1 for proper text compositing */
        if (texAlpha > 0.1) outColor.a = texAlpha;
    }

    /* Text anti-aliasing: handled by GPU texture filtering (LINEAR).
     * No custom sharpening — it caused artifact lines on bitmap fonts. */

    /* ---- Sobel/Sharp/Plastic pass ---- */
    /* Applied after recoloring, before scanlines.
     * Computes Sobel edge normals from the font texture, applies:
     *   1. Unsharp mask sharpening (all text)
     *   2. Directional lighting using Sobel-derived surface normals (skip recap bg)
     *   3. Specular highlights for glossy plastic look (skip recap bg)
     * Recap bg text gets sharpening only — no lighting to avoid gradient banding. */
    if (pc.fx_sobel > 0.5 && outColor.a > 0.01 && pc.is_pixel_font < 0.5) {
        vec2 texSize = vec2(textureSize(fontTex, 0));
        /* Adaptive sampling: scale to ~1 screen pixel instead of 1 texel.
         * At high DPI (4K), atlas texels map to multiple screen pixels,
         * so 1-texel sampling creates blocky/pixelated lighting. Use
         * fractional texel offsets for smooth gradients at any resolution.
         * glyph_texels = atlas_size/16, screen_px = char_w/h_px.
         * ratio = texels_per_glyph / screen_px_per_glyph, clamped >= 0.5 */
        vec2 glyph_tex = texSize / 16.0;
        vec2 screen_px = max(vec2(pc.char_w_px, pc.char_h_px), vec2(4.0));
        vec2 scale = clamp(glyph_tex / screen_px, vec2(0.4), vec2(1.5));
        vec2 tx = scale / texSize;

        /* 3x3 Sobel kernel on font texture alpha */
        float tl = texture(fontTex, fragUV + vec2(-tx.x, -tx.y)).a;
        float tc = texture(fontTex, fragUV + vec2(  0.0, -tx.y)).a;
        float tr = texture(fontTex, fragUV + vec2( tx.x, -tx.y)).a;
        float ml = texture(fontTex, fragUV + vec2(-tx.x,   0.0)).a;
        float mc = texture(fontTex, fragUV).a;
        float mr = texture(fontTex, fragUV + vec2( tx.x,   0.0)).a;
        float bl = texture(fontTex, fragUV + vec2(-tx.x,  tx.y)).a;
        float bc = texture(fontTex, fragUV + vec2(  0.0,  tx.y)).a;
        float brr= texture(fontTex, fragUV + vec2( tx.x,  tx.y)).a;

        /* Sobel X and Y gradients */
        float sx = (-tl - 2.0*ml - bl) + (tr + 2.0*mr + brr);
        float sy = (-tl - 2.0*tc - tr) + (bl + 2.0*bc + brr);
        float edgeMag = length(vec2(sx, sy));

        /* --- 1. Unsharp mask sharpening (applies to ALL text including recap) --- */
        float blur = (tl + tc + tr + ml + mc + mr + bl + bc + brr) / 9.0;
        float sharp = mc + (mc - blur) * 2.5;
        sharp = clamp(sharp, 0.0, 1.0);
        float origAlpha = outColor.a;
        float sharpAlpha = origAlpha * (sharp / max(mc, 0.01));
        sharpAlpha = clamp(sharpAlpha, 0.0, 1.0);
        outColor.a = mix(origAlpha, sharpAlpha, 0.6);

        /* --- 2-3. Plastic lighting — only where there's actual glyph data.
         * Skip on empty bg fill (mc near 0) to avoid gradient banding on recap edges. */
        if (mc > 0.15) {
            /* Sharpen color intensity at edges */
            outColor.rgb *= 1.0 + edgeMag * 0.8;

            /* Surface normal from Sobel gradient — scale with sampling distance
             * so lighting is resolution-independent */
            float nscale = 4.0 / max(scale.x, 0.4);
            vec3 normal = normalize(vec3(-sx * nscale, -sy * nscale, 1.0));

            /* Directional plastic lighting */
            vec3 lightDir = normalize(vec3(-0.5, -0.6, 0.8));
            vec3 viewDir = vec3(0.0, 0.0, 1.0);

            float NdotL = dot(normal, lightDir);
            float diffuse = max(NdotL * 0.5 + 0.5, 0.0);

            vec3 halfVec = normalize(lightDir + viewDir);
            float spec = pow(max(dot(normal, halfVec), 0.0), 80.0);

            float rim = pow(1.0 - max(dot(normal, viewDir), 0.0), 2.5);

            vec3 litColor = outColor.rgb * (diffuse * 0.85 + 0.15);
            litColor += vec3(1.0, 0.98, 0.95) * spec * 0.7;
            litColor += outColor.rgb * rim * 0.25;

            litColor *= 1.0 - edgeMag * 0.15;

            if (edgeMag > 0.2) {
                litColor = mix(litColor, litColor * 1.3, smoothstep(0.2, 0.6, edgeMag) * 0.3);
            }

            outColor.rgb = litColor;
        }
    }

    /* ---- Color/Brightness Post-Process (before scanlines) ---- */
    /* Skip post-process for bg-highlighted text (recap) so it stays readable */
    if (outColor.a > 0.01 && !has_bg_text) {
        /* Brightness: multiplicative scale preserves dim/bright ratio.
         * Map pp_brightness -1..+1 to multiplier 0.25..2.0 (0 = 1.0 = no change) */
        float bri_mul = (pc.pp_brightness >= 0.0)
            ? (1.0 + pc.pp_brightness)            /* 0→1.0, +1→2.0 */
            : (1.0 + pc.pp_brightness * 0.75);    /* -1→0.25, 0→1.0 */
        outColor.rgb *= bri_mul;

        /* Contrast: scale around 0.5 midpoint */
        outColor.rgb = (outColor.rgb - 0.5) * pc.pp_contrast + 0.5;

        /* RGB → HSV for hue/saturation */
        if (pc.pp_hue != 0.0 || pc.pp_saturation != 1.0) {
            vec3 c = outColor.rgb;
            float cmax = max(c.r, max(c.g, c.b));
            float cmin = min(c.r, min(c.g, c.b));
            float delta = cmax - cmin;
            float h = 0.0, s = 0.0, v = cmax;
            if (delta > 0.0001) {
                s = delta / cmax;
                if (c.r >= cmax) h = (c.g - c.b) / delta;
                else if (c.g >= cmax) h = 2.0 + (c.b - c.r) / delta;
                else h = 4.0 + (c.r - c.g) / delta;
                h *= 60.0;
                if (h < 0.0) h += 360.0;
            }
            /* Apply hue shift and saturation */
            h = mod(h + pc.pp_hue, 360.0);
            s = clamp(s * pc.pp_saturation, 0.0, 1.0);
            /* HSV → RGB */
            float hh = h / 60.0;
            int hi = int(floor(hh));
            float f = hh - float(hi);
            float p = v * (1.0 - s);
            float q = v * (1.0 - s * f);
            float tt = v * (1.0 - s * (1.0 - f));
            if (hi == 0)      outColor.rgb = vec3(v, tt, p);
            else if (hi == 1) outColor.rgb = vec3(q, v, p);
            else if (hi == 2) outColor.rgb = vec3(p, v, tt);
            else if (hi == 3) outColor.rgb = vec3(p, q, v);
            else if (hi == 4) outColor.rgb = vec3(tt, p, v);
            else              outColor.rgb = vec3(v, p, q);
        }

        outColor.rgb = clamp(outColor.rgb, 0.0, 1.0);
    }

    /* ---- CRT Scanlines (applied last) ---- */
    if (pc.fx_scanlines > 0.5 && outColor.a > 0.01) {
        /* Scale scanline width with resolution: 1px at 1080p (~16px cells),
         * wider at 4K (~32px cells) so they remain visible */
        float scan_width = max(1.0, floor(pc.char_h_px / 16.0));
        float scanline = mod(gl_FragCoord.y, scan_width + 1.0);
        if (scanline < scan_width)
            outColor.rgb = vec3(0.0);
    }
}
