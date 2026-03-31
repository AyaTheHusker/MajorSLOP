// ── ANSI Gradient Engine ──
// Post-processes ANSI terminal data to apply smooth color gradients
// within runs of same-colored text. Uses 24-bit ANSI (ESC[38;2;R;G;Bm).
// Schemes named after MajorMUD legends.
// Each palette remaps ANSI colors to thematic starting hues so colors
// remain distinct but are shifted into the palette's aesthetic.

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

    // Generate bright variant from a dark color (boost toward 255)
    static _bright(rgb) {
        return rgb.map(c => Math.min(255, Math.round(c + (255 - c) * 0.45)));
    }

    // Build full colorMap (30-37 + 90-97) from just the 8 base colors
    static _buildMap(base) {
        const m = {};
        m[30] = base.black;   m[90] = AnsiGradient._bright(base.black);
        m[31] = base.red;     m[91] = AnsiGradient._bright(base.red);
        m[32] = base.green;   m[92] = AnsiGradient._bright(base.green);
        m[33] = base.yellow;  m[93] = AnsiGradient._bright(base.yellow);
        m[34] = base.blue;    m[94] = AnsiGradient._bright(base.blue);
        m[35] = base.magenta; m[95] = AnsiGradient._bright(base.magenta);
        m[36] = base.cyan;    m[96] = AnsiGradient._bright(base.cyan);
        m[37] = base.white;   m[97] = AnsiGradient._bright(base.white);
        return m;
    }

    // Gradient function signature: fn(r, g, b, charInWord, wordIndex)
    // r,g,b are the palette-remapped starting color (not raw ANSI)
    static SCHEMES = {
        none: { name: 'None', fn: null },
        newhaven: {
            name: 'Newhaven',
            desc: 'Subtle gradient within warm natural tones',
            colorMap: AnsiGradient._buildMap({
                black:   [20, 15, 10],
                red:     [190, 75, 55],
                green:   [85, 170, 70],
                yellow:  [210, 180, 90],
                blue:    [70, 90, 170],
                magenta: [175, 85, 140],
                cyan:    [80, 175, 165],
                white:   [200, 195, 180],
            }),
            fn: (r, g, b, pos) => {
                const wave = Math.sin(pos * 0.15) * 0.18;
                return [
                    Math.round(Math.min(255, Math.max(0, r + r * wave))),
                    Math.round(Math.min(255, Math.max(0, g + g * wave * 0.8))),
                    Math.round(Math.min(255, Math.max(0, b + b * wave * 0.6))),
                ];
            }
        },
        ice_bitch: {
            name: 'Ice Bitch',
            desc: 'Cold blue-cyan shimmer',
            colorMap: AnsiGradient._buildMap({
                black:   [20, 30, 50],
                red:     [180, 120, 155],
                green:   [100, 190, 175],
                yellow:  [180, 195, 155],
                blue:    [50, 90, 210],
                magenta: [145, 115, 200],
                cyan:    [80, 185, 220],
                white:   [200, 215, 235],
            }),
            fn: (r, g, b, pos) => {
                const t = (Math.sin(pos * 0.12) + 1) / 2;
                const lum = (r + g + b) / 3;
                const minL = Math.max(50, lum * 0.4);
                return [
                    Math.round(Math.min(255, Math.max(minL * 0.4, r * 0.7 + 30 * t))),
                    Math.round(Math.min(255, Math.max(minL * 0.7, g * 0.7 + 50 * t + 30 * (1 - t)))),
                    Math.round(Math.min(255, Math.max(minL, b * 0.7 + 80 * t + 40 * (1 - t)))),
                ];
            }
        },
        she_dragon: {
            name: 'She Dragon',
            desc: 'Fiery red-orange-gold bands',
            colorMap: AnsiGradient._buildMap({
                black:   [45, 15, 5],
                red:     [220, 45, 15],
                green:   [200, 155, 35],
                yellow:  [250, 195, 45],
                blue:    [155, 35, 70],
                magenta: [225, 75, 55],
                cyan:    [200, 165, 90],
                white:   [240, 215, 175],
            }),
            fn: (r, g, b, pos) => {
                const t = (Math.sin(pos * 0.18) + 1) / 2;
                const lum = (r + g + b) / 3;
                const minL = Math.max(40, lum * 0.35);
                return [
                    Math.round(Math.min(255, Math.max(minL, r * 0.8 + 60 * t))),
                    Math.round(Math.min(255, Math.max(minL * 0.3, g * 0.6 + 50 * t))),
                    Math.round(Math.min(255, Math.max(8, b * 0.3 + 30 * t))),
                ];
            }
        },
        mad_wizard: {
            name: 'Mad Wizard',
            desc: 'Neon-saturated electric pulse',
            colorMap: AnsiGradient._buildMap({
                black:   [50, 30, 70],
                red:     [255, 30, 70],
                green:   [30, 255, 90],
                yellow:  [255, 250, 30],
                blue:    [30, 70, 255],
                magenta: [255, 30, 210],
                cyan:    [30, 250, 255],
                white:   [225, 225, 255],
            }),
            fn: (r, g, b, pos) => {
                const t = (Math.sin(pos * 0.2) + 1) / 2;
                const t2 = (Math.sin(pos * 0.13 + 2) + 1) / 2;
                const max = Math.max(r, g, b, 1);
                const boost = 1.3;
                return [
                    Math.round(Math.min(255, Math.max(25, (r / max) * 255 * boost * (0.6 + 0.4 * t)))),
                    Math.round(Math.min(255, Math.max(25, (g / max) * 255 * boost * (0.6 + 0.4 * t2)))),
                    Math.round(Math.min(255, Math.max(25, (b / max) * 255 * boost * (0.6 + 0.4 * (1 - t))))),
                ];
            }
        },
        giovanni: {
            name: 'Giovanni',
            desc: 'Prismatic full-spectrum rainbow',
            // Giovanni: each ANSI color starts its rainbow from its natural hue position
            // plus per-word golden angle shifts
            hueMap: {
                30: 0,   31: 0,    32: 120,  33: 50,   34: 240,  35: 300,  36: 180,  37: 30,
                90: 0,   91: 10,   92: 130,  93: 55,   94: 250,  95: 310,  96: 190,  97: 40,
            },
            perWordHue: true,
            fn: (r, g, b, pos, wordIdx, hueOffset) => {
                // hueOffset comes from the ANSI color's natural hue position
                const hueBase = ((hueOffset || 0) + wordIdx * 137.5) % 360;
                const hue = (hueBase + pos * 18) % 360;
                const lum = Math.max(80, (r + g + b) / 3);
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
            colorMap: AnsiGradient._buildMap({
                black:   [15, 8, 25],
                red:     [135, 25, 45],
                green:   [40, 110, 75],
                yellow:  [140, 115, 45],
                blue:    [35, 25, 135],
                magenta: [125, 35, 155],
                cyan:    [50, 100, 125],
                white:   [175, 165, 195],
            }),
            fn: (r, g, b, pos) => {
                const t = (Math.sin(pos * 0.14) + 1) / 2;
                const lum = (r + g + b) / 3;
                const minL = Math.max(30, lum * 0.3);
                return [
                    Math.round(Math.min(255, Math.max(minL * 0.5, r * 0.7 + 40 * t))),
                    Math.round(Math.min(255, Math.max(minL * 0.2, g * 0.5 + 20 * t))),
                    Math.round(Math.min(255, Math.max(minL * 0.8, b * 0.7 + 60 * t))),
                ];
            }
        },
        slime_beast: {
            name: 'Slime Beast',
            desc: 'Sickly green necrotic pulse',
            colorMap: AnsiGradient._buildMap({
                black:   [25, 35, 15],
                red:     [155, 55, 35],
                green:   [50, 195, 45],
                yellow:  [175, 185, 35],
                blue:    [50, 55, 120],
                magenta: [130, 55, 110],
                cyan:    [50, 165, 125],
                white:   [185, 205, 165],
            }),
            fn: (r, g, b, pos) => {
                const t = (Math.sin(pos * 0.16) + 1) / 2;
                const lum = (r + g + b) / 3;
                const minL = Math.max(35, lum * 0.35);
                return [
                    Math.round(Math.min(255, Math.max(minL * 0.4, r * 0.6 + 40 * t))),
                    Math.round(Math.min(255, Math.max(minL, g * 0.7 + 60 * t))),
                    Math.round(Math.min(255, Math.max(minL * 0.2, b * 0.3 + 30 * (1 - t)))),
                ];
            }
        },
        ozrinom: {
            name: 'Ozrinom',
            desc: 'Deep crimson-to-black vampiric',
            colorMap: AnsiGradient._buildMap({
                black:   [35, 8, 12],
                red:     [180, 18, 28],
                green:   [115, 45, 30],
                yellow:  [155, 135, 105],
                blue:    [95, 18, 75],
                magenta: [165, 35, 55],
                cyan:    [120, 85, 80],
                white:   [195, 165, 160],
            }),
            fn: (r, g, b, pos) => {
                const t = (Math.sin(pos * 0.13) + 1) / 2;
                const lum = (r + g + b) / 3;
                const minL = Math.max(30, lum * 0.25);
                return [
                    Math.round(Math.min(255, Math.max(minL, r * 0.8 + 50 * t))),
                    Math.round(Math.min(255, Math.max(8, g * 0.4 + 15 * t))),
                    Math.round(Math.min(255, Math.max(10, b * 0.5 + 20 * (1 - t)))),
                ];
            }
        },
    };

    constructor() {
        this._scheme = localStorage.getItem('ansiGradientScheme') || 'none';
        this._shadowEnabled = localStorage.getItem('ansiGradientShadow') === '1';
        this._wordPos = 0;    // character position within current word
        this._wordIndex = 0;  // which word we're on (for per-word hue shifts)
    }

    get scheme() { return this._scheme; }
    set scheme(s) {
        this._scheme = s;
        localStorage.setItem('ansiGradientScheme', s);
    }

    get shadowEnabled() { return this._shadowEnabled; }
    set shadowEnabled(v) {
        this._shadowEnabled = !!v;
        localStorage.setItem('ansiGradientShadow', v ? '1' : '0');
    }

    // Process an ANSI data string, applying gradient to colored text runs
    process(data) {
        if (this._scheme === 'none' || !AnsiGradient.SCHEMES[this._scheme]?.fn) {
            return data;
        }
        const scheme = AnsiGradient.SCHEMES[this._scheme];
        return this._applyGradient(data, scheme);
    }

    // Blend gradient color into white/default text at reduced intensity
    _tintWhite(gr, gg, gb) {
        const blend = 0.35;
        return [
            Math.round(255 * (1 - blend) + gr * blend),
            Math.round(255 * (1 - blend) + gg * blend),
            Math.round(255 * (1 - blend) + gb * blend),
        ];
    }

    _applyGradient(data, scheme) {
        const fn = scheme.fn;
        const colorMap = scheme.colorMap || null;
        const hueMap = scheme.hueMap || null;
        const parts = data.split(/(\x1b\[[0-9;]*[A-Za-z])/);
        let currentFg = null; // current foreground ANSI code (30-37, 90-97), null = default/white
        let currentBg = '';
        let otherSGR = '';
        let result = '';

        for (const part of parts) {
            if (!part) continue;

            if (part.startsWith('\x1b[')) {
                if (!part.endsWith('m')) {
                    result += part;
                    continue;
                }
                const codes = part.slice(2, -1).split(';').map(Number);
                let isFgSet = false;
                for (const c of codes) {
                    if (c === 0) {
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
                        result += part;
                        currentFg = -1;
                        isFgSet = false;
                        break;
                    }
                }
                if (!isFgSet && currentFg === null) {
                    result += part;
                } else if (currentFg === -1) {
                    // custom color — already emitted
                }
                continue;
            }

            // Text chunk — apply gradient
            if (currentFg === -1) {
                result += part;
                continue;
            }

            const isWhite = currentFg === null;
            // Look up palette-remapped color, fall back to raw ANSI base
            let baseRGB;
            if (isWhite) {
                // White/default: use palette's white mapping or default grey
                baseRGB = colorMap ? (colorMap[37] || [204, 204, 204]) : [204, 204, 204];
            } else if (colorMap && colorMap[currentFg]) {
                baseRGB = colorMap[currentFg];
            } else {
                baseRGB = AnsiGradient.ANSI_BASE[currentFg] || null;
            }

            // For Giovanni, get the hue offset for this ANSI color
            const hueOffset = hueMap ? (hueMap[currentFg] || hueMap[isWhite ? 37 : currentFg] || 0) : 0;

            if (baseRGB) {
                const [br, bg, bb] = baseRGB;
                for (let i = 0; i < part.length; i++) {
                    const ch = part[i];
                    if (ch === '\r' || ch === '\n' || ch === '\t') {
                        result += ch;
                        if (ch === '\n') {
                            this._wordPos = 0;
                            this._wordIndex = 0;
                        }
                        continue;
                    }
                    if (ch === ' ') {
                        result += ch;
                        if (this._wordPos > 0) {
                            this._wordIndex++;
                            this._wordPos = 0;
                        }
                        continue;
                    }
                    let [gr, gg, gb] = fn(br, bg, bb, this._wordPos, this._wordIndex, hueOffset);
                    if (isWhite) {
                        [gr, gg, gb] = this._tintWhite(gr, gg, gb);
                    }
                    result += `\x1b[38;2;${gr};${gg};${gb}m${otherSGR}${currentBg}${ch}`;
                    this._wordPos++;
                }
            } else {
                result += part;
            }
        }
        return result;
    }
}
