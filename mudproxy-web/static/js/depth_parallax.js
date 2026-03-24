// ── WebGL 2 Depth Parallax Renderer ──
// Port of depth_parallax.comp Vulkan compute shader to WebGL fragment shader

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
uniform float u_edgeFadeStart;
uniform float u_edgeFadeEnd;
uniform float u_depthContrast;
uniform vec2 u_mouse;       // normalized mouse offset for interactive mode
uniform float u_mouseActive; // 1.0 if mouse is controlling, 0.0 for auto

vec2 mirrorRepeat(vec2 uv) {
    vec2 m = mod(uv, 2.0);
    return mix(m, 2.0 - m, step(1.0, m));
}

// Camera modes — compute (offsetX, offsetY) based on time
vec2 cameraOffset(float mode, float t, float intensity) {
    float s = t;
    if (mode < 0.5) {
        // Carousel
        return vec2(sin(s) * intensity, cos(s) * intensity * 0.3);
    } else if (mode < 1.5) {
        // Horizontal
        return vec2(sin(s) * intensity, 0.0);
    } else if (mode < 2.5) {
        // Vertical
        return vec2(0.0, sin(s) * intensity);
    } else if (mode < 3.5) {
        // Circle
        return vec2(sin(s) * intensity, cos(s) * intensity);
    } else if (mode < 4.5) {
        // Zoom (depth pulse)
        return vec2(0.0, 0.0); // handled via depthScale modulation
    } else if (mode < 5.5) {
        // Dolly
        return vec2(0.0, sin(s) * intensity * 0.5);
    } else if (mode < 6.5) {
        // Orbital
        return vec2(sin(s) * intensity, sin(s * 0.7) * intensity * 0.5);
    } else {
        // Explore
        return vec2(sin(s * 0.3) * intensity, cos(s * 0.5) * intensity * 0.4);
    }
}

vec2 depthFlowParallax(vec2 uv, vec2 offset, float height, float isoAmt, float steadyAmt, out float hitDepth) {
    // Two-stage ray march
    float numCoarse = 60.0;
    float numFine = 40.0;

    vec2 totalOffset = offset * height;
    vec2 stepCoarse = totalOffset / numCoarse;

    vec2 currentUV = uv;
    float currentDepth = 0.0;
    float prevDepth = 0.0;

    // Coarse pass
    for (float i = 0.0; i < 60.0; i++) {
        float sampledDepth = texture(u_depth, mirrorRepeat(currentUV)).r;
        sampledDepth = pow(sampledDepth, max(u_depthContrast, 0.1));

        if (currentDepth >= sampledDepth) {
            // Refine
            vec2 prevUV = currentUV + stepCoarse;
            float stepFine = 1.0 / numFine;
            for (float j = 0.0; j < 40.0; j++) {
                float t = j * stepFine;
                vec2 midUV = mix(prevUV, currentUV, t);
                float midSample = texture(u_depth, mirrorRepeat(midUV)).r;
                midSample = pow(midSample, max(u_depthContrast, 0.1));
                float midDepth = mix(prevDepth, currentDepth, t);
                if (midDepth >= midSample) {
                    hitDepth = midSample;
                    // Steady: pin to depth layer
                    vec2 result = midUV;
                    if (steadyAmt > 0.0) {
                        result -= offset * midSample * steadyAmt;
                    }
                    return result;
                }
            }
            hitDepth = sampledDepth;
            return currentUV;
        }

        prevDepth = currentDepth;
        currentUV -= stepCoarse;
        currentDepth += 1.0 / numCoarse;
    }

    hitDepth = 0.0;
    return currentUV;
}

void main() {
    vec2 uv = v_uv;

    // Overscan zoom
    if (u_overscan > 0.0) {
        float scale = 1.0 + u_overscan;
        uv = (uv - 0.5) / scale + 0.5;
    }

    // Camera offset
    vec2 offset;
    if (u_mouseActive > 0.5) {
        offset = u_mouse * u_cameraIntensity;
    } else {
        offset = cameraOffset(u_cameraMode, u_time * u_cameraSpeed, u_cameraIntensity);
    }

    // Depth parallax
    float hitDepth;
    vec2 parallaxUV = depthFlowParallax(uv, offset, u_depthScale, u_isometric, u_steady, hitDepth);

    // Sample color
    vec4 color = texture(u_color, mirrorRepeat(parallaxUV));

    // Edge fade
    vec2 edgeDist = min(parallaxUV, 1.0 - parallaxUV);
    float edgeFade = smoothstep(u_edgeFadeEnd, u_edgeFadeStart, min(edgeDist.x, edgeDist.y));
    color.rgb *= edgeFade;

    fragColor = color;
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
        this.mouseX = 0;
        this.mouseY = 0;
        this.mouseActive = false;
        this.hasDepth = false;

        // Parallax settings
        this.depthScale = 0.15;
        this.cameraMode = 0; // carousel
        this.cameraIntensity = 0.03;
        this.cameraSpeed = 0.4;
        this.isometric = 0.0;
        this.steady = 0.5;
        this.overscan = 0.05;
        this.edgeFadeStart = 0.05;
        this.edgeFadeEnd = 0.0;
        this.depthContrast = 1.0;

        this._init();
        this._setupMouse();
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
            'u_edgeFadeStart', 'u_edgeFadeEnd', 'u_depthContrast',
            'u_mouse', 'u_mouseActive',
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
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.MIRRORED_REPEAT);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.MIRRORED_REPEAT);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
        // 1x1 black placeholder
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array([0,0,0,255]));
        return tex;
    }

    _setupMouse() {
        this.canvas.addEventListener('mousemove', (e) => {
            const rect = this.canvas.getBoundingClientRect();
            this.mouseX = ((e.clientX - rect.left) / rect.width - 0.5) * 2;
            this.mouseY = ((e.clientY - rect.top) / rect.height - 0.5) * -2;
            this.mouseActive = true;
        });
        this.canvas.addEventListener('mouseleave', () => {
            this.mouseActive = false;
        });
    }

    loadImage(url) {
        return new Promise((resolve) => {
            const img = new Image();
            img.crossOrigin = 'anonymous';
            img.onload = () => {
                const gl = this.gl;
                gl.bindTexture(gl.TEXTURE_2D, this.colorTex);
                gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, img);
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
                gl.bindTexture(gl.TEXTURE_2D, this.depthTex);
                gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, img);
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

    _render() {
        if (!this.animating || !this.gl) return;

        const gl = this.gl;
        const w = this.canvas.clientWidth;
        const h = this.canvas.clientHeight;

        if (this.canvas.width !== w || this.canvas.height !== h) {
            this.canvas.width = w;
            this.canvas.height = h;
            gl.viewport(0, 0, w, h);
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
        gl.uniform1f(this.uniforms.u_edgeFadeStart, this.edgeFadeStart);
        gl.uniform1f(this.uniforms.u_edgeFadeEnd, this.edgeFadeEnd);
        gl.uniform1f(this.uniforms.u_depthContrast, this.depthContrast);
        gl.uniform2f(this.uniforms.u_mouse, this.mouseX, this.mouseY);
        gl.uniform1f(this.uniforms.u_mouseActive, this.mouseActive ? 1.0 : 0.0);

        gl.activeTexture(gl.TEXTURE0);
        gl.bindTexture(gl.TEXTURE_2D, this.colorTex);
        gl.activeTexture(gl.TEXTURE1);
        gl.bindTexture(gl.TEXTURE_2D, this.depthTex);

        gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);

        requestAnimationFrame(() => this._render());
    }
}
