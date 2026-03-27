// ── WebGL 2 Depth Parallax Renderer ──
// Ported from Scurry's vk_depthcarousel.frag (DepthFlow-style two-stage ray march)

const VERT_SHADER = `#version 300 es
in vec2 a_pos;
out vec2 v_uv;
void main() {
    v_uv = a_pos * 0.5 + 0.5;
    gl_Position = vec4(a_pos, 0.0, 1.0);
}`;

const FRAG_SHADER = `#version 300 es
precision highp float;

in vec2 v_uv;
out vec4 fragColor;

uniform sampler2D u_color;
uniform sampler2D u_depth;
uniform float u_time;
uniform vec2 u_resolution;
uniform float u_depthScale;
uniform float u_cameraMode;
uniform float u_cameraIntensity;
uniform float u_cameraSpeed;
uniform float u_isometric;
uniform float u_steady;
uniform float u_overscan;
uniform float u_panSpeed;
uniform float u_panAmountX;
uniform float u_panAmountY;
uniform float u_depthContrast;
uniform float u_vignetteAmount;
uniform float u_vignetteFeather;

// ============================================================================
// Mirror wrapping (DepthFlow technique)
// Triangle wave maps any UV coordinate to [0,1] with mirrored repeat.
// Prevents accordion/tiling artifacts when parallax pushes UVs out of bounds.
// ============================================================================
float triangleWave(float x) {
    return abs(mod(x, 2.0) - 1.0);
}

vec2 mirrorUV(vec2 uv) {
    return vec2(triangleWave(uv.x), triangleWave(uv.y));
}

// Sample color with clamped UVs (no mirroring — preserves text direction)
vec3 sampleColor(vec2 uv) {
    return texture(u_color, clamp(uv, 0.0, 1.0)).rgb;
}

// Sample depth with contrast and clamped UVs
float sampleDepth(vec2 uv) {
    float d = texture(u_depth, clamp(uv, 0.0, 1.0)).r;
    return pow(d, max(u_depthContrast, 0.1));
}

// ============================================================================
// DepthFlow-style two-stage ray march with isometric + steady camera
//
// Stage 1 (FORWARD): Coarse steps to quickly find where the ray enters the
//   depth surface. Overshoots past the intersection.
// Stage 2 (BACKWARD): Fine steps to refine the exact intersection point.
//
// depth map: 1 = foreground (close), 0 = background (far)
// surface height = depthScale * depthValue
// Ray walks from camera origin toward the image plane.
//
// Isometric: spreads ray origins across screen (parallel rays) to flatten
//   perspective, reducing edge artifacts by design.
// Steady: pins a depth layer so it doesn't move, reducing parallax at that depth.
// ============================================================================
vec2 depthFlowParallax(vec2 uv, vec2 offset, float height,
                        float isoAmount, float steadyAmount, out float hitDepth) {
    float probe   = 1.0 / 80.0;   // coarse forward step
    float quality = 1.0 / 400.0;   // fine backward step

    // The guaranteed safe distance before hitting any surface
    float safe = 1.0 - height;

    // Isometric: spread ray origins across screen (parallel rays)
    vec2 screenPos = (uv - 0.5) * 2.0;
    vec2 isoSpread = screenPos * isoAmount * height;

    // Ray origin with isometric spread
    vec3 origin = vec3(uv + offset + isoSpread, 0.0);

    // Steady pivot: pin a depth layer so it doesn't move
    vec2 steadyCorrection = offset * (1.0 / max(1.0 - steadyAmount, 0.01));
    vec3 target = vec3(uv + steadyCorrection * steadyAmount, 1.0);

    float walk = 0.0;
    float depthVal = 0.0;
    vec2 hitUV = uv;

    // === Stage 1: Forward coarse march ===
    for (int i = 0; i < 120; i++) {
        if (walk > 1.0) break;
        walk += probe;

        vec3 point = mix(origin, target, mix(safe, 1.0, walk));
        hitUV = point.xy;

        depthVal = sampleDepth(hitUV);

        float surface = height * depthVal;
        float ceiling = 1.0 - point.z;

        // Ray has entered the surface
        if (ceiling < surface) break;
    }

    // === Stage 2: Backward fine refinement ===
    for (int i = 0; i < 80; i++) {
        walk -= quality;

        vec3 point = mix(origin, target, mix(safe, 1.0, walk));
        hitUV = point.xy;

        depthVal = sampleDepth(hitUV);

        float surface = height * depthVal;
        float ceiling = 1.0 - point.z;

        // Ray has exited the surface — we found the boundary
        if (ceiling >= surface) break;
    }

    hitDepth = depthVal;
    return hitUV;
}

// ============================================================================
// Camera mode animation — DepthFlow-inspired camera presets
// ============================================================================
vec2 computeCameraOffset(float t, float intensity, float speed, float mode) {
    float phase = t * speed;

    if (mode < 0.5) {
        // Carousel — gentle circular sway
        return vec2(cos(phase) * intensity * 0.5, sin(phase) * intensity * 0.3);
    } else if (mode < 1.5) {
        // Horizontal
        return vec2(sin(phase) * intensity * 0.5, 0.0);
    } else if (mode < 2.5) {
        // Vertical
        return vec2(0.0, sin(phase) * intensity * 0.5);
    } else if (mode < 3.5) {
        // Circle
        return vec2(cos(phase) * intensity * 0.5, sin(phase) * intensity * 0.5);
    } else if (mode < 4.5) {
        // Zoom — offset is zero, height modulated in main
        return vec2(0.0);
    } else if (mode < 5.5) {
        // Dolly — offset is zero, isometric modulated in main
        return vec2(0.0);
    } else if (mode < 6.5) {
        // Orbital — lateral offset + isometric oscillation
        return vec2(sin(phase) * intensity * 0.5, 0.0);
    } else {
        // Explore — slow multi-axis drift
        return vec2(sin(phase * 0.3) * intensity * 0.4, cos(phase * 0.5) * intensity * 0.3);
    }
}

void main() {
    vec2 uv = v_uv;

    // === Overscan: zoom into center, extra pixels at edges for panning ===
    float scale = 1.0 / (1.0 + u_overscan * 2.0);

    // Pan offset within overscan margin using circular loop
    float panT = u_time * u_panSpeed;
    vec2 panOffset = vec2(sin(panT), cos(panT)) * vec2(u_panAmountX, u_panAmountY) * u_overscan;

    // Apply overscan crop + pan
    uv = (uv - 0.5) * scale + 0.5 + panOffset;

    float effectiveDepthScale = u_depthScale;
    float effectiveIso = u_isometric;
    float camPhase = u_time * u_cameraSpeed;

    // Zoom mode: modulate depth scale
    if (u_cameraMode > 3.5 && u_cameraMode < 4.5) {
        effectiveDepthScale += sin(camPhase) * u_cameraIntensity * 0.5 * u_depthScale;
    }
    // Dolly mode: modulate isometric
    if (u_cameraMode > 4.5 && u_cameraMode < 5.5) {
        effectiveIso += sin(camPhase) * u_cameraIntensity * 0.5;
        effectiveIso = clamp(effectiveIso, 0.0, 1.0);
    }
    // Orbital mode: oscillate isometric alongside lateral offset
    if (u_cameraMode > 5.5 && u_cameraMode < 6.5) {
        effectiveIso += cos(camPhase) * u_cameraIntensity * 0.3;
        effectiveIso = clamp(effectiveIso, 0.0, 1.0);
    }

    // Camera offset from mode animation (passed directly like DepthFlow)
    vec2 cameraOffset = computeCameraOffset(u_time, u_cameraIntensity, u_cameraSpeed, u_cameraMode);

    // === Two-stage parallax ray march (DepthFlow style) ===
    float hitDepth = 0.0;
    vec2 finalUV = depthFlowParallax(uv, cameraOffset, effectiveDepthScale,
                                      effectiveIso, u_steady, hitDepth);

    // === Sample color (mirror wrapping handles edges, like DepthFlow) ===
    vec3 color = sampleColor(finalUV);

    // === Vignette (edges/corners only, off by default) ===
    if (u_vignetteAmount > 0.001) {
        vec2 away = v_uv * (1.0 - v_uv);
        float linear = 10.0 * (away.x * away.y);
        float vig = clamp(pow(linear, u_vignetteFeather), 0.0, 1.0);
        color *= mix(1.0, vig, u_vignetteAmount);
    }

    fragColor = vec4(color, 1.0);
}`;

class DepthParallax {
    constructor(canvas) {
        this.canvas = canvas;
        this.gl = canvas.getContext('webgl2', { antialias: false, alpha: false });
        if (!this.gl) {
            console.warn('WebGL 2 not available');
            return;
        }

        this.program = null;
        this.colorTex = null;
        this.depthTex = null;
        this.uniforms = {};
        this.animating = false;
        this.startTime = performance.now() / 1000;
        this.hasDepth = false;
        this.imageAspect = 3 / 2; // default 3:2 until image loads
        this.fillMode = 'fill'; // 'fit' = letterbox, 'fill' = cover (no black bars)

        // Parallax settings (defaults match DepthFlow)
        this.depthScale = 0.20;
        this.cameraMode = 0; // carousel
        this.cameraIntensity = 0.20;
        this.cameraSpeed = 0.4;
        this.isometric = 0.0;
        this.steady = 0.3;
        this.overscan = 0.06;
        this.panSpeed = 0.05;
        this.panAmountX = 0.3;
        this.panAmountY = 0.2;
        this.depthContrast = 1.0;
        this.vignetteAmount = 0.0;
        this.vignetteFeather = 0.5;

        this._init();
    }

    _init() {
        const gl = this.gl;

        // Compile shaders
        const vs = this._compileShader(gl.VERTEX_SHADER, VERT_SHADER);
        const fs = this._compileShader(gl.FRAGMENT_SHADER, FRAG_SHADER);
        this.program = gl.createProgram();
        gl.attachShader(this.program, vs);
        gl.attachShader(this.program, fs);
        gl.linkProgram(this.program);

        if (!gl.getProgramParameter(this.program, gl.LINK_STATUS)) {
            console.error('Shader link error:', gl.getProgramInfoLog(this.program));
            return;
        }

        // Fullscreen quad
        const verts = new Float32Array([-1,-1, 1,-1, -1,1, 1,1]);
        const vbo = gl.createBuffer();
        gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
        gl.bufferData(gl.ARRAY_BUFFER, verts, gl.STATIC_DRAW);

        const aPos = gl.getAttribLocation(this.program, 'a_pos');
        gl.enableVertexAttribArray(aPos);
        gl.vertexAttribPointer(aPos, 2, gl.FLOAT, false, 0, 0);

        // Cache uniform locations
        gl.useProgram(this.program);
        for (const name of [
            'u_color', 'u_depth', 'u_time', 'u_resolution',
            'u_depthScale', 'u_cameraMode', 'u_cameraIntensity', 'u_cameraSpeed',
            'u_isometric', 'u_steady', 'u_overscan',
            'u_panSpeed', 'u_panAmountX', 'u_panAmountY',
            'u_depthContrast',
            'u_vignetteAmount', 'u_vignetteFeather',
        ]) {
            this.uniforms[name] = gl.getUniformLocation(this.program, name);
        }

        // Texture units
        gl.uniform1i(this.uniforms.u_color, 0);
        gl.uniform1i(this.uniforms.u_depth, 1);

        // Create placeholder textures
        this.colorTex = this._createTexture();
        this.depthTex = this._createTexture();
    }

    _compileShader(type, src) {
        const gl = this.gl;
        const shader = gl.createShader(type);
        gl.shaderSource(shader, src);
        gl.compileShader(shader);
        if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
            console.error('Shader error:', gl.getShaderInfoLog(shader));
        }
        return shader;
    }

    _createTexture() {
        const gl = this.gl;
        const tex = gl.createTexture();
        gl.bindTexture(gl.TEXTURE_2D, tex);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
        // 1x1 black placeholder
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array([0,0,0,255]));
        return tex;
    }

    loadImage(url) {
        return new Promise((resolve) => {
            const img = new Image();
            img.crossOrigin = 'anonymous';
            img.onload = () => {
                const gl = this.gl;
                this.imageAspect = img.naturalWidth / img.naturalHeight;
                gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, true);
                gl.bindTexture(gl.TEXTURE_2D, this.colorTex);
                gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, img);
                gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
                this.hasDepth = false;
                resolve(true);
            };
            img.onerror = () => resolve(false);
            img.src = url;
        });
    }

    loadDepth(url) {
        return new Promise((resolve) => {
            const img = new Image();
            img.crossOrigin = 'anonymous';
            img.onload = () => {
                const gl = this.gl;
                gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, true);
                gl.bindTexture(gl.TEXTURE_2D, this.depthTex);
                gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, img);
                gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
                this.hasDepth = true;
                resolve(true);
            };
            img.onerror = () => resolve(false);
            img.src = url;
        });
    }

    start() {
        if (this.animating) return;
        this.animating = true;
        this._render();
    }

    stop() {
        this.animating = false;
    }

    clearDepth() {
        if (!this.gl) return;
        const gl = this.gl;
        gl.bindTexture(gl.TEXTURE_2D, this.depthTex);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array([0,0,0,255]));
        this.hasDepth = false;
    }

    _render() {
        if (!this.animating || !this.gl) return;

        const gl = this.gl;
        const parentW = window.innerWidth;
        const parentH = window.innerHeight;

        // Scale canvas to image aspect ratio
        const aspect = this.imageAspect || (3 / 2);
        let w, h;
        if (this.fillMode === 'fill') {
            // Cover: scale up to fill viewport, crop overflow (no black bars)
            if (parentW / parentH > aspect) {
                w = parentW;
                h = Math.round(w / aspect);
            } else {
                h = parentH;
                w = Math.round(h * aspect);
            }
        } else {
            // Fit: letterbox/pillarbox (no crop, black bars)
            if (parentW / parentH > aspect) {
                h = parentH;
                w = Math.round(h * aspect);
            } else {
                w = parentW;
                h = Math.round(w / aspect);
            }
        }

        // Size and center the canvas
        if (this.canvas.style.width !== w + 'px' || this.canvas.style.height !== h + 'px') {
            this.canvas.style.width = w + 'px';
            this.canvas.style.height = h + 'px';
            this.canvas.style.position = 'absolute';
            this.canvas.style.left = Math.round((parentW - w) / 2) + 'px';
            this.canvas.style.top = Math.round((parentH - h) / 2) + 'px';
        }

        // Set backing buffer to match CSS size for sharp rendering
        const dpr = Math.min(window.devicePixelRatio || 1, 2);
        const bufW = Math.round(w * dpr);
        const bufH = Math.round(h * dpr);
        if (this.canvas.width !== bufW || this.canvas.height !== bufH) {
            this.canvas.width = bufW;
            this.canvas.height = bufH;
            gl.viewport(0, 0, bufW, bufH);
        }

        gl.useProgram(this.program);

        const t = performance.now() / 1000 - this.startTime;
        gl.uniform1f(this.uniforms.u_time, t);
        gl.uniform2f(this.uniforms.u_resolution, w, h);
        gl.uniform1f(this.uniforms.u_depthScale, this.hasDepth ? this.depthScale : 0.0);
        gl.uniform1f(this.uniforms.u_cameraMode, this.cameraMode);
        gl.uniform1f(this.uniforms.u_cameraIntensity, this.cameraIntensity);
        gl.uniform1f(this.uniforms.u_cameraSpeed, this.cameraSpeed);
        gl.uniform1f(this.uniforms.u_isometric, this.isometric);
        gl.uniform1f(this.uniforms.u_steady, this.steady);
        gl.uniform1f(this.uniforms.u_overscan, this.overscan);
        gl.uniform1f(this.uniforms.u_panSpeed, this.panSpeed);
        gl.uniform1f(this.uniforms.u_panAmountX, this.panAmountX);
        gl.uniform1f(this.uniforms.u_panAmountY, this.panAmountY);
        gl.uniform1f(this.uniforms.u_depthContrast, this.depthContrast);
        gl.uniform1f(this.uniforms.u_vignetteAmount, this.vignetteAmount);
        gl.uniform1f(this.uniforms.u_vignetteFeather, this.vignetteFeather);

        gl.activeTexture(gl.TEXTURE0);
        gl.bindTexture(gl.TEXTURE_2D, this.colorTex);
        gl.activeTexture(gl.TEXTURE1);
        gl.bindTexture(gl.TEXTURE_2D, this.depthTex);

        gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);

        requestAnimationFrame(() => this._render());
    }
}
