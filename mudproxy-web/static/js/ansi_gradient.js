// ── ANSI Gradient Engine ──
// Post-processes ANSI terminal data to apply smooth color gradients
// within runs of same-colored text. Uses 24-bit ANSI (ESC[38;2;R;G;Bm).
// Schemes named after MajorMUD legends.

class AnsiGradient {
    // Base ANSI colors → RGB
    static ANSI_BASE = {
        30: [0,0,0],       // black
        31: [204,68,68],    // red
        32: [68,204,68],    // green
        33: [204,170,68],   // yellow
        34: [68,68,204],    // blue
        35: [204,68,204],   // magenta
        36: [68,204,204],   // cyan
        37: [204,204,204],  // white
        90: [102,102,102],  // bright black
        91: [255,102,102],  // bright red
        92: [102,255,102],  // bright green
        93: [255,255,102],  // bright yellow
        94: [102,102,255],  // bright blue
        95: [255,102,255],  // bright magenta
        96: [102,255,255],  // bright cyan
        97: [255,255,255],  // bright white
    };

    static SCHEMES = {
        none: { name: 'None', fn: null },
        newhaven: {
            name: 'Newhaven',
            desc: 'Subtle gradient within natural ANSI tones',
            // Gentle luminance wave — stays close to original color
            fn: (r, g, b, pos) => {
                const wave = Math.sin(pos * 0.15) * 0.15;
                return [
                    Math.round(Math.min(255, Math.max(0, r + r * wave))),
                    Math.round(Math.min(255, Math.max(0, g + g * wave))),
                    Math.round(Math.min(255, Math.max(0, b + b * wave))),
                ];
            }
        },
        ice_bitch: {
            name: 'Ice Bitch',
            desc: 'Cold blue-cyan shimmer',
            fn: (r, g, b, pos) => {
                const t = (Math.sin(pos * 0.12) + 1) / 2;
                // Shift toward blue/cyan, keep luminance
                const lum = (r + g + b) / 3;
                const minL = Math.max(60, lum * 0.5);
                return [
                    Math.round(Math.min(255, Math.max(minL * 0.3, r * 0.3 + 40 * t))),
                    Math.round(Math.min(255, Math.max(minL * 0.7, g * 0.5 + 80 * t + 60 * (1 - t)))),
                    Math.round(Math.min(255, Math.max(minL, b * 0.6 + 160 * t + 80 * (1 - t)))),
                ];
            }
        },
        she_dragon: {
            name: 'She Dragon',
            desc: 'Fiery red-orange-gold bands',
            fn: (r, g, b, pos) => {
                const t = (Math.sin(pos * 0.18) + 1) / 2;
                const lum = (r + g + b) / 3;
                const minL = Math.max(50, lum * 0.4);
                return [
                    Math.round(Math.min(255, Math.max(minL, 180 + 75 * t))),
                    Math.round(Math.min(255, Math.max(minL * 0.3, 40 + 120 * t))),
                    Math.round(Math.min(255, Math.max(10, 10 + 40 * t))),
                ];
            }
        },
        mad_wizard: {
            name: 'Mad Wizard',
            desc: 'Neon-saturated electric pulse',
            fn: (r, g, b, pos) => {
                const t = (Math.sin(pos * 0.2) + 1) / 2;
                const t2 = (Math.sin(pos * 0.13 + 2) + 1) / 2;
                // Pump saturation and shift hue
                const max = Math.max(r, g, b, 1);
                const boost = 1.4;
                return [
                    Math.round(Math.min(255, Math.max(30, (r / max) * 255 * boost * (0.6 + 0.4 * t)))),
                    Math.round(Math.min(255, Math.max(30, (g / max) * 255 * boost * (0.6 + 0.4 * t2)))),
                    Math.round(Math.min(255, Math.max(30, (b / max) * 255 * boost * (0.6 + 0.4 * (1 - t))))),
                ];
            }
        },
        giovanni: {
            name: 'Giovanni',
            desc: 'Prismatic full-spectrum rainbow',
            fn: (r, g, b, pos) => {
                const hue = (pos * 3.6) % 360;
                const lum = Math.max(80, (r + g + b) / 3);
                // HSL-ish rainbow at fixed saturation/luminance
                const c = lum * 0.9;
                const x = c * (1 - Math.abs(((hue / 60) % 2) - 1));
                const m = lum * 0.15;
                let rr, gg, bb;
                if (hue < 60) { rr = c; gg = x; bb = 0; }
                else if (hue < 120) { rr = x; gg = c; bb = 0; }
                else if (hue < 180) { rr = 0; gg = c; bb = x; }
                else if (hue < 240) { rr = 0; gg = x; bb = c; }
                else if (hue < 300) { rr = x; gg = 0; bb = c; }
                else { rr = c; gg = 0; bb = x; }
                return [Math.round(rr + m), Math.round(gg + m), Math.round(bb + m)];
            }
        },
        shadow_master: {
            name: 'Shadow Master',
            desc: 'Dark purple-black eldritch glow',
            fn: (r, g, b, pos) => {
                const t = (Math.sin(pos * 0.14) + 1) / 2;
                const lum = (r + g + b) / 3;
                const minL = Math.max(40, lum * 0.35);
                return [
                    Math.round(Math.min(255, Math.max(minL * 0.6, 60 + 80 * t))),
                    Math.round(Math.min(255, Math.max(minL * 0.2, 15 + 30 * t))),
                    Math.round(Math.min(255, Math.max(minL, 100 + 120 * t))),
                ];
            }
        },
        slime_beast: {
            name: 'Slime Beast',
            desc: 'Sickly green necrotic pulse',
            fn: (r, g, b, pos) => {
                const t = (Math.sin(pos * 0.16) + 1) / 2;
                const lum = (r + g + b) / 3;
                const minL = Math.max(45, lum * 0.4);
                return [
                    Math.round(Math.min(255, Math.max(minL * 0.4, 30 + 60 * t))),
                    Math.round(Math.min(255, Math.max(minL, 120 + 100 * t))),
                    Math.round(Math.min(255, Math.max(minL * 0.2, 10 + 50 * (1 - t)))),
                ];
            }
        },
        ozrinom: {
            name: 'Ozrinom',
            desc: 'Deep crimson-to-black vampiric',
            fn: (r, g, b, pos) => {
                const t = (Math.sin(pos * 0.13) + 1) / 2;
                const lum = (r + g + b) / 3;
                const minL = Math.max(35, lum * 0.3);
                return [
                    Math.round(Math.min(255, Math.max(minL, 140 + 80 * t))),
                    Math.round(Math.min(255, Math.max(10, 10 + 25 * t))),
                    Math.round(Math.min(255, Math.max(15, 15 + 30 * (1 - t)))),
                ];
            }
        },
    };

    constructor() {
        this._scheme = localStorage.getItem('ansiGradientScheme') || 'none';
        this._charPos = 0; // global character position counter for smooth scrolling gradient
    }

    get scheme() { return this._scheme; }
    set scheme(s) {
        this._scheme = s;
        localStorage.setItem('ansiGradientScheme', s);
    }

    // Process an ANSI data string, applying gradient to colored text runs
    process(data) {
        if (this._scheme === 'none' || !AnsiGradient.SCHEMES[this._scheme]?.fn) {
            return data;
        }
        const fn = AnsiGradient.SCHEMES[this._scheme].fn;
        return this._applyGradient(data, fn);
    }

    _applyGradient(data, fn) {
        // Parse ANSI stream: split into escape sequences and text chunks
        // Regex matches ESC[ ... m (SGR sequences) and everything else
        const parts = data.split(/(\x1b\[[0-9;]*m)/);
        let currentFg = null; // current foreground ANSI code (30-37, 90-97)
        let currentBg = '';   // preserve background sequences
        let otherSGR = '';    // preserve bold/underline/etc
        let result = '';

        for (const part of parts) {
            if (!part) continue;

            // Is this an SGR escape sequence?
            if (part.startsWith('\x1b[') && part.endsWith('m')) {
                const codes = part.slice(2, -1).split(';').map(Number);
                let isFgSet = false;
                for (const c of codes) {
                    if (c === 0) {
                        // Reset
                        currentFg = null;
                        currentBg = '';
                        otherSGR = '';
                        isFgSet = false;
                    } else if ((c >= 30 && c <= 37) || (c >= 90 && c <= 97)) {
                        currentFg = c;
                        isFgSet = true;
                    } else if ((c >= 40 && c <= 47) || (c >= 100 && c <= 107)) {
                        currentBg = `\x1b[${c}m`;
                    } else if (c === 1 || c === 2 || c === 3 || c === 4 || c === 7 || c === 9) {
                        otherSGR += `\x1b[${c}m`;
                    } else if (c === 38 || c === 48) {
                        // 256-color or truecolor — pass through unchanged
                        result += part;
                        currentFg = null; // don't gradient custom colors
                        isFgSet = false;
                        break;
                    }
                }
                // If we didn't break early (256/truecolor), don't emit the original SGR
                // We'll emit per-character SGRs instead when there's a gradient fg
                if (!isFgSet && currentFg === null) {
                    result += part;
                }
                continue;
            }

            // Text chunk — apply gradient if we have a foreground color
            if (currentFg !== null && AnsiGradient.ANSI_BASE[currentFg]) {
                const [br, bg, bb] = AnsiGradient.ANSI_BASE[currentFg];
                for (let i = 0; i < part.length; i++) {
                    const ch = part[i];
                    // Don't gradient control chars or whitespace
                    if (ch === '\r' || ch === '\n' || ch === '\t') {
                        result += ch;
                        continue;
                    }
                    if (ch === ' ') {
                        result += ch;
                        this._charPos++;
                        continue;
                    }
                    const [gr, gg, gb] = fn(br, bg, bb, this._charPos);
                    result += `\x1b[38;2;${gr};${gg};${gb}m${otherSGR}${currentBg}${ch}`;
                    this._charPos++;
                }
            } else {
                result += part;
                // Count chars for position tracking even in non-colored text
                for (let i = 0; i < part.length; i++) {
                    if (part[i] !== '\x1b') this._charPos++;
                }
            }
        }
        return result;
    }
}
