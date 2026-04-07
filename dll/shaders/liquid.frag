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
    /* ---- Base text rendering (normal or liquid) ---- */
    if (pc.fx_mode < 0.5) {
        /* Normal mode — standard alpha-textured text */
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

            outColor.rgb = mix(smoke_color * smoke_alpha, outColor.rgb, outColor.a);
            outColor.a = max(outColor.a, smoke_alpha);
        }
    }

    /* ---- Dark outline for bg-highlighted text (recap etc.) ---- */
    /* alpha=0.75 signals text-on-bg: sample neighbors to create dark stroke,
     * making text readable regardless of hue shift or background color.
     * Restore alpha to 1.0 after applying outline so blending works normally. */
    if (has_bg_text) {
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

    /* ---- Sobel/Sharp/Plastic pass ---- */
    /* Applied after recoloring, before scanlines.
     * Computes Sobel edge normals from the font texture, applies:
     *   1. Unsharp mask sharpening
     *   2. Directional lighting using Sobel-derived surface normals
     *   3. Specular highlights for glossy plastic look */
    if (pc.fx_sobel > 0.5 && outColor.a > 0.01) {
        vec2 texSize = vec2(textureSize(fontTex, 0));
        vec2 tx = 1.0 / texSize;

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

        /* --- 1. Unsharp mask sharpening --- */
        /* Blur = average of 3x3 neighborhood */
        float blur = (tl + tc + tr + ml + mc + mr + bl + bc + brr) / 9.0;
        /* Sharpen: original + (original - blur) * strength */
        float sharp = mc + (mc - blur) * 2.5;
        sharp = clamp(sharp, 0.0, 1.0);
        /* Blend sharpened alpha back into output */
        float origAlpha = outColor.a;
        float sharpAlpha = origAlpha * (sharp / max(mc, 0.01));
        sharpAlpha = clamp(sharpAlpha, 0.0, 1.0);
        outColor.a = mix(origAlpha, sharpAlpha, 0.6);

        /* Also sharpen color intensity at edges */
        outColor.rgb *= 1.0 + edgeMag * 0.8;

        /* --- 2. Surface normal from Sobel gradient --- */
        vec3 normal = normalize(vec3(-sx * 6.0, -sy * 6.0, 1.0));

        /* --- 3. Directional plastic lighting --- */
        /* Light from upper-left at ~45 degrees — classic emboss angle */
        vec3 lightDir = normalize(vec3(-0.5, -0.6, 0.8));
        vec3 viewDir = vec3(0.0, 0.0, 1.0);

        /* Diffuse wrap lighting for soft plastic feel */
        float NdotL = dot(normal, lightDir);
        float diffuse = max(NdotL * 0.5 + 0.5, 0.0); /* half-lambert wrap */

        /* Specular — tight highlight for glossy plastic */
        vec3 halfVec = normalize(lightDir + viewDir);
        float spec = pow(max(dot(normal, halfVec), 0.0), 80.0);

        /* Rim/edge highlight from the opposing side */
        float rim = pow(1.0 - max(dot(normal, viewDir), 0.0), 2.5);

        /* Combine: modulate existing color with lighting */
        vec3 litColor = outColor.rgb * (diffuse * 0.85 + 0.15); /* ambient floor */
        litColor += vec3(1.0, 0.98, 0.95) * spec * 0.7;        /* white specular */
        litColor += outColor.rgb * rim * 0.25;                   /* colored rim */

        /* Edge darkening for embossed depth */
        litColor *= 1.0 - edgeMag * 0.15;

        /* Subtle edge outline for crispness */
        if (edgeMag > 0.2) {
            litColor = mix(litColor, litColor * 1.3, smoothstep(0.2, 0.6, edgeMag) * 0.3);
        }

        outColor.rgb = litColor;
    }

    /* ---- Color/Brightness Post-Process (before scanlines) ---- */
    /* Skip post-process for bg-highlighted text (recap) so it stays readable */
    if (outColor.a > 0.01 && !has_bg_text) {
        /* Brightness: shift RGB */
        outColor.rgb += pc.pp_brightness;

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
        float scanline = mod(gl_FragCoord.y, 2.0);
        float scan_intensity = (scanline < 1.0) ? 1.0 : 0.55;
        outColor.rgb *= scan_intensity;

        if (scanline < 1.0) {
            float luminance = dot(outColor.rgb, vec3(0.299, 0.587, 0.114));
            outColor.rgb += outColor.rgb * luminance * 0.15;
        }

        vec2 screenUV = gl_FragCoord.xy / vec2(textureSize(fontTex, 0));
        vec2 vign = screenUV * (1.0 - screenUV);
        float vignette = clamp(pow(vign.x * vign.y * 15.0, 0.25), 0.0, 1.0);
        outColor.rgb *= mix(0.85, 1.0, vignette);
    }
}
