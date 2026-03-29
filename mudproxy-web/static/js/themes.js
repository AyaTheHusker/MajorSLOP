// ── UI Color Themes ──
// Each theme overrides CSS custom properties on :root via data-theme attribute.
// All panels use frosted glass (backdrop-filter) + gradient decorations.
// Each theme also defines orbit colors for the round timer and ripple FX.

const UI_THEMES = {
    greylord: {
        name: 'Grey Lord',
        desc: 'Default stone grey',
        orbit: { ball: '#c0c8e0', trail: '#4455aa', ripple: '#6677cc', text: ['#8899cc','#c0c8e0'] },
        vars: {
            '--bg': '#0a0a12',
            '--bg-panel': 'rgba(16, 16, 28, 0.55)',
            '--bg-header': 'rgba(18, 18, 42, 0.5)',
            '--bg-glass': 'rgba(16, 16, 28, 0.6)',
            '--bg-glass-border': 'rgba(80, 80, 140, 0.3)',
            '--bg-input': 'rgba(10, 10, 20, 0.9)',
            '--border': '#1a1a3a',
            '--text': '#c8c8e0',
            '--text-dim': '#6666aa',
            '--accent': '#4444cc',
            '--header-grad': 'linear-gradient(135deg, rgba(18,18,42,0.5) 0%, rgba(26,26,58,0.4) 100%)',
            '--panel-grad': 'linear-gradient(180deg, rgba(16,16,28,0.55) 0%, rgba(10,10,20,0.5) 100%)',
            '--panel-border': '1px solid rgba(80, 80, 140, 0.3)',
            '--menu-bg': 'rgba(12, 12, 28, 0.95)',
            '--topbar-bg': 'rgba(10, 10, 20, 0.95)',
        },
    },
    blackfort: {
        name: 'Black Fort',
        desc: 'Obsidian darkness',
        orbit: { ball: '#ccccdd', trail: '#443366', ripple: '#554477', text: ['#554477','#aaaacc'] },
        vars: {
            '--bg': '#030306',
            '--bg-panel': 'rgba(4, 4, 10, 0.7)',
            '--bg-header': 'rgba(8, 8, 16, 0.6)',
            '--bg-glass': 'rgba(6, 6, 14, 0.65)',
            '--bg-glass-border': 'rgba(40, 40, 60, 0.3)',
            '--bg-input': 'rgba(2, 2, 6, 0.95)',
            '--border': '#0e0e1a',
            '--text': '#999ab0',
            '--text-dim': '#444466',
            '--accent': '#333366',
            '--header-grad': 'linear-gradient(135deg, rgba(6,6,14,0.6) 0%, rgba(10,10,20,0.5) 100%)',
            '--panel-grad': 'linear-gradient(180deg, rgba(4,4,10,0.7) 0%, rgba(2,2,6,0.6) 100%)',
            '--panel-border': '1px solid rgba(40, 40, 60, 0.25)',
            '--menu-bg': 'rgba(4, 4, 10, 0.97)',
            '--topbar-bg': 'rgba(3, 3, 8, 0.97)',
        },
    },
    khazarad: {
        name: 'Khazarad',
        desc: 'Ancient bronze',
        orbit: { ball: '#ffd080', trail: '#885522', ripple: '#cc9944', text: ['#886633','#ffd080'] },
        vars: {
            '--bg': '#0c0a06',
            '--bg-panel': 'rgba(24, 18, 10, 0.6)',
            '--bg-header': 'rgba(36, 28, 16, 0.55)',
            '--bg-glass': 'rgba(28, 22, 12, 0.6)',
            '--bg-glass-border': 'rgba(140, 100, 50, 0.3)',
            '--bg-input': 'rgba(14, 10, 6, 0.9)',
            '--border': '#2a1e10',
            '--text': '#d4c4a0',
            '--text-dim': '#8a7a56',
            '--accent': '#aa8844',
            '--header-grad': 'linear-gradient(135deg, rgba(50,36,16,0.55) 0%, rgba(30,22,10,0.5) 100%)',
            '--panel-grad': 'linear-gradient(180deg, rgba(28,20,10,0.6) 0%, rgba(16,12,6,0.55) 100%)',
            '--panel-border': '1px solid rgba(140, 100, 50, 0.25)',
            '--menu-bg': 'rgba(18, 14, 8, 0.96)',
            '--topbar-bg': 'rgba(14, 10, 6, 0.96)',
        },
    },
    silvermere: {
        name: 'Silvermere',
        desc: 'Gleaming silver',
        orbit: { ball: '#ffffff', trail: '#7788bb', ripple: '#aabbdd', text: ['#7788bb','#e0e0f0'] },
        vars: {
            '--bg': '#0e0e12',
            '--bg-panel': 'rgba(28, 28, 36, 0.55)',
            '--bg-header': 'rgba(40, 40, 52, 0.5)',
            '--bg-glass': 'rgba(32, 32, 42, 0.55)',
            '--bg-glass-border': 'rgba(140, 140, 170, 0.3)',
            '--bg-input': 'rgba(16, 16, 22, 0.9)',
            '--border': '#2a2a38',
            '--text': '#d8d8e8',
            '--text-dim': '#8888aa',
            '--accent': '#7788bb',
            '--header-grad': 'linear-gradient(135deg, rgba(50,50,66,0.5) 0%, rgba(36,36,48,0.45) 100%)',
            '--panel-grad': 'linear-gradient(180deg, rgba(32,32,42,0.55) 0%, rgba(22,22,30,0.5) 100%)',
            '--panel-border': '1px solid rgba(140, 140, 170, 0.25)',
            '--menu-bg': 'rgba(22, 22, 30, 0.96)',
            '--topbar-bg': 'rgba(16, 16, 22, 0.96)',
        },
    },
    annora: {
        name: 'Annora',
        desc: 'Radiant white',
        orbit: { ball: '#ffffee', trail: '#ccaa66', ripple: '#eeddaa', text: ['#ccaa66','#ffffff'] },
        vars: {
            '--bg': '#141418',
            '--bg-panel': 'rgba(40, 40, 50, 0.5)',
            '--bg-header': 'rgba(55, 55, 70, 0.45)',
            '--bg-glass': 'rgba(46, 46, 58, 0.5)',
            '--bg-glass-border': 'rgba(180, 180, 200, 0.3)',
            '--bg-input': 'rgba(26, 26, 34, 0.9)',
            '--border': '#363644',
            '--text': '#eeeef4',
            '--text-dim': '#aaaacc',
            '--accent': '#99aadd',
            '--header-grad': 'linear-gradient(135deg, rgba(66,66,82,0.45) 0%, rgba(50,50,64,0.4) 100%)',
            '--panel-grad': 'linear-gradient(180deg, rgba(46,46,58,0.5) 0%, rgba(34,34,44,0.45) 100%)',
            '--panel-border': '1px solid rgba(180, 180, 200, 0.2)',
            '--menu-bg': 'rgba(30, 30, 40, 0.96)',
            '--topbar-bg': 'rgba(24, 24, 32, 0.96)',
        },
    },
    jorah: {
        name: 'Jorah',
        desc: 'Royal blue',
        orbit: { ball: '#88ccff', trail: '#2244aa', ripple: '#4488ee', text: ['#2255cc','#88ccff'] },
        vars: {
            '--bg': '#060810',
            '--bg-panel': 'rgba(10, 16, 36, 0.6)',
            '--bg-header': 'rgba(14, 22, 50, 0.55)',
            '--bg-glass': 'rgba(12, 18, 40, 0.6)',
            '--bg-glass-border': 'rgba(60, 80, 160, 0.35)',
            '--bg-input': 'rgba(6, 10, 24, 0.9)',
            '--border': '#0e1830',
            '--text': '#b0c0e8',
            '--text-dim': '#5566aa',
            '--accent': '#3366cc',
            '--header-grad': 'linear-gradient(135deg, rgba(16,28,66,0.55) 0%, rgba(10,18,44,0.5) 100%)',
            '--panel-grad': 'linear-gradient(180deg, rgba(10,18,42,0.6) 0%, rgba(6,12,28,0.55) 100%)',
            '--panel-border': '1px solid rgba(60, 80, 160, 0.3)',
            '--menu-bg': 'rgba(8, 12, 28, 0.96)',
            '--topbar-bg': 'rgba(6, 10, 22, 0.96)',
        },
    },
    putakwa: {
        name: 'Putakwa',
        desc: 'Deep sea green',
        orbit: { ball: '#66ffcc', trail: '#116655', ripple: '#33ccaa', text: ['#118866','#66ffcc'] },
        vars: {
            '--bg': '#040c0a',
            '--bg-panel': 'rgba(8, 22, 18, 0.6)',
            '--bg-header': 'rgba(12, 30, 26, 0.55)',
            '--bg-glass': 'rgba(10, 26, 22, 0.6)',
            '--bg-glass-border': 'rgba(40, 140, 110, 0.3)',
            '--bg-input': 'rgba(4, 12, 10, 0.9)',
            '--border': '#0c2820',
            '--text': '#a0d8c8',
            '--text-dim': '#448866',
            '--accent': '#22aa88',
            '--header-grad': 'linear-gradient(135deg, rgba(14,36,30,0.55) 0%, rgba(8,24,20,0.5) 100%)',
            '--panel-grad': 'linear-gradient(180deg, rgba(8,24,20,0.6) 0%, rgba(4,14,12,0.55) 100%)',
            '--panel-border': '1px solid rgba(40, 140, 110, 0.25)',
            '--menu-bg': 'rgba(6, 16, 14, 0.96)',
            '--topbar-bg': 'rgba(4, 12, 10, 0.96)',
        },
    },
    void: {
        name: 'Void',
        desc: 'Vibrant purple',
        orbit: { ball: '#dd88ff', trail: '#6622aa', ripple: '#aa44ee', text: ['#7733cc','#dd88ff'] },
        vars: {
            '--bg': '#0a0410',
            '--bg-panel': 'rgba(20, 8, 32, 0.6)',
            '--bg-header': 'rgba(30, 12, 48, 0.55)',
            '--bg-glass': 'rgba(24, 10, 38, 0.6)',
            '--bg-glass-border': 'rgba(120, 50, 180, 0.35)',
            '--bg-input': 'rgba(10, 4, 18, 0.9)',
            '--border': '#1e0c30',
            '--text': '#d0b0e8',
            '--text-dim': '#7744aa',
            '--accent': '#8844cc',
            '--header-grad': 'linear-gradient(135deg, rgba(36,14,58,0.55) 0%, rgba(22,8,36,0.5) 100%)',
            '--panel-grad': 'linear-gradient(180deg, rgba(22,10,36,0.6) 0%, rgba(14,6,22,0.55) 100%)',
            '--panel-border': '1px solid rgba(120, 50, 180, 0.3)',
            '--menu-bg': 'rgba(14, 6, 24, 0.96)',
            '--topbar-bg': 'rgba(10, 4, 18, 0.96)',
        },
    },
    ozzrinom: {
        name: 'Ozzrinom',
        desc: 'Blood crimson',
        orbit: { ball: '#ff6666', trail: '#881122', ripple: '#cc3344', text: ['#aa2233','#ff8888'] },
        vars: {
            '--bg': '#0c0406',
            '--bg-panel': 'rgba(28, 8, 12, 0.6)',
            '--bg-header': 'rgba(40, 12, 18, 0.55)',
            '--bg-glass': 'rgba(32, 10, 14, 0.6)',
            '--bg-glass-border': 'rgba(160, 40, 50, 0.35)',
            '--bg-input': 'rgba(14, 4, 6, 0.9)',
            '--border': '#2a0c10',
            '--text': '#e0b0b0',
            '--text-dim': '#884444',
            '--accent': '#cc3344',
            '--header-grad': 'linear-gradient(135deg, rgba(50,14,20,0.55) 0%, rgba(32,10,14,0.5) 100%)',
            '--panel-grad': 'linear-gradient(180deg, rgba(30,10,14,0.6) 0%, rgba(18,6,8,0.55) 100%)',
            '--panel-border': '1px solid rgba(160, 40, 50, 0.3)',
            '--menu-bg': 'rgba(16, 6, 8, 0.96)',
            '--topbar-bg': 'rgba(12, 4, 6, 0.96)',
        },
    },
    phoenix: {
        name: 'Phoenix',
        desc: 'Fire gradients',
        orbit: { ball: '#ffcc44', trail: '#cc4400', ripple: '#ff8822', text: ['#cc5500','#ffdd66'] },
        vars: {
            '--bg': '#0c0804',
            '--bg-panel': 'rgba(26, 14, 6, 0.6)',
            '--bg-header': 'rgba(40, 20, 8, 0.55)',
            '--bg-glass': 'rgba(30, 16, 8, 0.6)',
            '--bg-glass-border': 'rgba(180, 80, 20, 0.35)',
            '--bg-input': 'rgba(14, 8, 4, 0.9)',
            '--border': '#2a1808',
            '--text': '#e8d0a8',
            '--text-dim': '#886640',
            '--accent': '#dd6622',
            '--header-grad': 'linear-gradient(135deg, rgba(60,24,8,0.55) 0%, rgba(40,16,4,0.45) 50%, rgba(50,20,6,0.5) 100%)',
            '--panel-grad': 'linear-gradient(180deg, rgba(30,14,6,0.6) 0%, rgba(20,8,4,0.5) 50%, rgba(26,12,4,0.55) 100%)',
            '--panel-border': '1px solid rgba(180, 80, 20, 0.3)',
            '--menu-bg': 'rgba(16, 8, 4, 0.96)',
            '--topbar-bg': 'rgba(14, 8, 4, 0.96)',
        },
    },
    madwizard: {
        name: 'Mad Wizard',
        desc: 'Trippy neon',
        orbit: { ball: '#00ffee', trail: '#ff00cc', ripple: '#88ffff', text: ['#ff44cc','#00ffee'] },
        vars: {
            '--bg': '#06060c',
            '--bg-panel': 'rgba(12, 8, 24, 0.55)',
            '--bg-header': 'rgba(18, 10, 36, 0.5)',
            '--bg-glass': 'rgba(14, 10, 28, 0.55)',
            '--bg-glass-border': 'rgba(120, 220, 255, 0.25)',
            '--bg-input': 'rgba(8, 4, 16, 0.9)',
            '--border': '#1a1030',
            '--text': '#e0f0ff',
            '--text-dim': '#66aacc',
            '--accent': '#00eeff',
            '--header-grad': 'linear-gradient(135deg, rgba(40,10,60,0.5) 0%, rgba(10,20,50,0.45) 50%, rgba(20,40,40,0.5) 100%)',
            '--panel-grad': 'linear-gradient(135deg, rgba(20,6,40,0.55) 0%, rgba(6,14,30,0.5) 50%, rgba(10,24,24,0.55) 100%)',
            '--panel-border': '1px solid rgba(120, 220, 255, 0.2)',
            '--menu-bg': 'rgba(10, 6, 20, 0.96)',
            '--topbar-bg': 'rgba(8, 4, 16, 0.96)',
        },
    },
    tasloi: {
        name: 'Tasloi',
        desc: 'Green & brown camo',
        vines: true,
        orbit: { ball: '#88ff44', trail: '#664422', ripple: '#55cc22', text: ['#448822','#aaff66'] },
        vars: {
            '--bg': '#060a04',
            '--bg-panel': 'rgba(14, 22, 10, 0.55)',
            '--bg-header': 'rgba(20, 30, 14, 0.5)',
            '--bg-glass': 'rgba(16, 26, 12, 0.55)',
            '--bg-glass-border': 'rgba(80, 140, 50, 0.3)',
            '--bg-input': 'rgba(8, 12, 6, 0.9)',
            '--border': '#1a2a10',
            '--text': '#c0d8a0',
            '--text-dim': '#668844',
            '--accent': '#44aa22',
            '--header-grad': 'linear-gradient(135deg, rgba(24,36,16,0.55) 0%, rgba(20,28,12,0.5) 100%)',
            '--panel-grad': 'linear-gradient(180deg, rgba(16,26,12,0.55) 0%, rgba(12,18,8,0.5) 100%)',
            '--panel-border': '1px solid rgba(80, 140, 50, 0.25)',
            '--menu-bg': 'rgba(10, 16, 8, 0.96)',
            '--topbar-bg': 'rgba(8, 12, 6, 0.96)',
        },
    },
    frostborn: {
        name: 'Frostborn',
        desc: 'Frozen tundra',
        orbit: { ball: '#ccf0ff', trail: '#2277bb', ripple: '#66ccee', text: ['#3399cc','#ccf0ff'] },
        vars: {
            '--bg': '#060a0e',
            '--bg-panel': 'rgba(12, 20, 30, 0.55)',
            '--bg-header': 'rgba(16, 28, 42, 0.5)',
            '--bg-glass': 'rgba(14, 24, 36, 0.55)',
            '--bg-glass-border': 'rgba(100, 170, 220, 0.3)',
            '--bg-input': 'rgba(6, 12, 18, 0.9)',
            '--border': '#102030',
            '--text': '#c8e0f0',
            '--text-dim': '#5588aa',
            '--accent': '#44aadd',
            '--header-grad': 'linear-gradient(135deg, rgba(18,32,50,0.5) 0%, rgba(12,24,38,0.45) 100%)',
            '--panel-grad': 'linear-gradient(180deg, rgba(14,26,40,0.55) 0%, rgba(8,16,26,0.5) 100%)',
            '--panel-border': '1px solid rgba(100, 170, 220, 0.25)',
            '--menu-bg': 'rgba(8, 14, 22, 0.96)',
            '--topbar-bg': 'rgba(6, 12, 18, 0.96)',
        },
    },
    sandstorm: {
        name: 'Sandstorm',
        desc: 'Desert dunes',
        orbit: { ball: '#ffe088', trail: '#996622', ripple: '#ddaa44', text: ['#aa8833','#ffe088'] },
        vars: {
            '--bg': '#0c0a06',
            '--bg-panel': 'rgba(26, 20, 12, 0.55)',
            '--bg-header': 'rgba(36, 28, 16, 0.5)',
            '--bg-glass': 'rgba(30, 24, 14, 0.55)',
            '--bg-glass-border': 'rgba(180, 140, 70, 0.3)',
            '--bg-input': 'rgba(14, 10, 6, 0.9)',
            '--border': '#2a2010',
            '--text': '#e0d4b8',
            '--text-dim': '#8a7a56',
            '--accent': '#ccaa44',
            '--header-grad': 'linear-gradient(135deg, rgba(44,34,18,0.5) 0%, rgba(32,24,12,0.45) 100%)',
            '--panel-grad': 'linear-gradient(180deg, rgba(28,22,12,0.55) 0%, rgba(18,14,8,0.5) 100%)',
            '--panel-border': '1px solid rgba(180, 140, 70, 0.25)',
            '--menu-bg': 'rgba(16, 12, 8, 0.96)',
            '--topbar-bg': 'rgba(14, 10, 6, 0.96)',
        },
    },
    crystal: {
        name: 'Crystal Cavern',
        desc: 'Gem-lit depths',
        orbit: { ball: '#cc88ff', trail: '#4466cc', ripple: '#aa66ee', text: ['#7744cc','#ddaaff'] },
        vars: {
            '--bg': '#08060c',
            '--bg-panel': 'rgba(16, 12, 26, 0.55)',
            '--bg-header': 'rgba(22, 16, 36, 0.5)',
            '--bg-glass': 'rgba(18, 14, 30, 0.55)',
            '--bg-glass-border': 'rgba(120, 80, 200, 0.3)',
            '--bg-input': 'rgba(10, 6, 16, 0.9)',
            '--border': '#1a1228',
            '--text': '#d0c0e8',
            '--text-dim': '#7766aa',
            '--accent': '#9966ee',
            '--header-grad': 'linear-gradient(135deg, rgba(28,18,46,0.5) 0%, rgba(16,30,36,0.45) 50%, rgba(26,16,40,0.5) 100%)',
            '--panel-grad': 'linear-gradient(135deg, rgba(18,12,30,0.55) 0%, rgba(12,20,26,0.5) 50%, rgba(16,10,28,0.55) 100%)',
            '--panel-border': '1px solid rgba(120, 80, 200, 0.25)',
            '--menu-bg': 'rgba(12, 8, 20, 0.96)',
            '--topbar-bg': 'rgba(10, 6, 16, 0.96)',
        },
    },
    afroman: {
        name: 'Afroman',
        desc: 'Red, white & blue',
        orbit: { ball: '#ffffff', trail: '#cc2244', ripple: '#4466dd', text: ['#cc2244','#4466dd','#ffffff'] },
        vars: {
            '--bg': '#060608',
            '--bg-panel': 'rgba(12, 12, 24, 0.55)',
            '--bg-header': 'rgba(20, 14, 32, 0.5)',
            '--bg-glass': 'rgba(14, 12, 28, 0.55)',
            '--bg-glass-border': 'rgba(120, 80, 160, 0.3)',
            '--bg-input': 'rgba(6, 6, 14, 0.9)',
            '--border': '#141428',
            '--text': '#e8e0f0',
            '--text-dim': '#8877aa',
            '--accent': '#cc2244',
            '--header-grad': 'linear-gradient(135deg, rgba(50,10,20,0.5) 0%, rgba(240,240,255,0.06) 50%, rgba(14,14,60,0.5) 100%)',
            '--panel-grad': 'linear-gradient(135deg, rgba(40,8,16,0.5) 0%, rgba(200,200,220,0.06) 50%, rgba(10,10,50,0.5) 100%)',
            '--panel-border': '1px solid rgba(200, 60, 80, 0.25)',
            '--menu-bg': 'rgba(8, 8, 18, 0.96)',
            '--topbar-bg': 'rgba(6, 6, 14, 0.96)',
        },
    },
    swamp: {
        name: 'Bog Lord',
        desc: 'Murky swamplands',
        orbit: { ball: '#bbcc44', trail: '#445522', ripple: '#88aa33', text: ['#556622','#ccdd66'] },
        vars: {
            '--bg': '#060806',
            '--bg-panel': 'rgba(14, 18, 10, 0.6)',
            '--bg-header': 'rgba(18, 24, 14, 0.55)',
            '--bg-glass': 'rgba(16, 20, 12, 0.6)',
            '--bg-glass-border': 'rgba(90, 110, 50, 0.3)',
            '--bg-input': 'rgba(8, 10, 6, 0.9)',
            '--border': '#182010',
            '--text': '#b0b890',
            '--text-dim': '#606840',
            '--accent': '#778833',
            '--header-grad': 'linear-gradient(135deg, rgba(22,28,16,0.55) 0%, rgba(16,20,10,0.5) 100%)',
            '--panel-grad': 'linear-gradient(180deg, rgba(16,20,12,0.6) 0%, rgba(10,14,8,0.55) 100%)',
            '--panel-border': '1px solid rgba(90, 110, 50, 0.25)',
            '--menu-bg': 'rgba(10, 14, 8, 0.96)',
            '--topbar-bg': 'rgba(8, 10, 6, 0.96)',
        },
    },
};

// ── Tasloi Vine Overlay ──
// Procedural animated vines with red glow pulses and spider webs in corners.

class TasloiVines {
    constructor() {
        this._canvas = null;
        this._ctx = null;
        this._animId = null;
        this._vines = [];
        this._webs = [];
        this._built = false;
    }

    show() {
        if (!this._built) this._build();
        this._canvas.style.display = '';
        this._generateVines();
        this._generateWebs();
        this._startLoop();
    }

    hide() {
        if (this._canvas) this._canvas.style.display = 'none';
        this._stopLoop();
    }

    _build() {
        const c = document.createElement('canvas');
        c.id = 'tasloi-vines';
        c.style.cssText = 'position:fixed;inset:0;pointer-events:none;z-index:9998;';
        document.body.appendChild(c);
        this._canvas = c;
        this._ctx = c.getContext('2d');
        this._resize();
        window.addEventListener('resize', () => this._resize());
        this._built = true;
    }

    _resize() {
        if (!this._canvas) return;
        this._canvas.width = window.innerWidth;
        this._canvas.height = window.innerHeight;
        this._generateVines();
        this._generateWebs();
    }

    _generateVines() {
        const W = this._canvas.width, H = this._canvas.height;
        this._vines = [];
        // Generate vine paths from edges/corners
        const sources = [
            { x: 0, y: H * 0.2, dx: 1, dy: 0.3 },
            { x: 0, y: H * 0.6, dx: 1, dy: -0.2 },
            { x: W, y: H * 0.3, dx: -1, dy: 0.4 },
            { x: W, y: H * 0.7, dx: -1, dy: -0.3 },
            { x: W * 0.3, y: 0, dx: 0.2, dy: 1 },
            { x: W * 0.7, y: 0, dx: -0.3, dy: 1 },
            { x: W * 0.2, y: H, dx: 0.4, dy: -1 },
            { x: W * 0.8, y: H, dx: -0.2, dy: -1 },
        ];
        for (const src of sources) {
            const points = [{ x: src.x, y: src.y }];
            let x = src.x, y = src.y;
            let dx = src.dx, dy = src.dy;
            const len = 80 + Math.random() * 120;
            const segs = 8 + Math.floor(Math.random() * 6);
            const segLen = len / segs;
            for (let i = 0; i < segs; i++) {
                dx += (Math.random() - 0.5) * 0.6;
                dy += (Math.random() - 0.5) * 0.6;
                const mag = Math.sqrt(dx * dx + dy * dy) || 1;
                dx /= mag; dy /= mag;
                x += dx * segLen;
                y += dy * segLen;
                points.push({ x, y });
            }
            this._vines.push({
                points,
                thickness: 1.5 + Math.random() * 2,
                phase: Math.random() * Math.PI * 2,
                speed: 0.3 + Math.random() * 0.5,
            });
        }
    }

    _generateWebs() {
        const W = this._canvas.width, H = this._canvas.height;
        this._webs = [
            { cx: 0, cy: 0, r: 60 + Math.random() * 40, startAngle: 0, endAngle: Math.PI / 2 },
            { cx: W, cy: 0, r: 50 + Math.random() * 35, startAngle: Math.PI / 2, endAngle: Math.PI },
            { cx: W, cy: H, r: 55 + Math.random() * 30, startAngle: Math.PI, endAngle: Math.PI * 1.5 },
            { cx: 0, cy: H, r: 45 + Math.random() * 35, startAngle: Math.PI * 1.5, endAngle: Math.PI * 2 },
        ];
    }

    _startLoop() {
        if (this._animId) return;
        const loop = () => {
            this._render();
            this._animId = requestAnimationFrame(loop);
        };
        this._animId = requestAnimationFrame(loop);
    }

    _stopLoop() {
        if (this._animId) { cancelAnimationFrame(this._animId); this._animId = null; }
    }

    _render() {
        const ctx = this._ctx;
        const W = this._canvas.width, H = this._canvas.height;
        const now = performance.now() / 1000;
        ctx.clearRect(0, 0, W, H);

        // ── Draw spider webs ──
        for (const web of this._webs) {
            const strands = 8;
            const rings = 5;
            ctx.save();
            ctx.globalAlpha = 0.12 + 0.03 * Math.sin(now * 0.5);
            ctx.strokeStyle = '#aabbaa';
            ctx.lineWidth = 0.5;
            // Radial strands
            for (let i = 0; i < strands; i++) {
                const angle = web.startAngle + (i / strands) * (web.endAngle - web.startAngle);
                ctx.beginPath();
                ctx.moveTo(web.cx, web.cy);
                ctx.lineTo(web.cx + Math.cos(angle) * web.r, web.cy + Math.sin(angle) * web.r);
                ctx.stroke();
            }
            // Concentric arcs
            for (let r = 1; r <= rings; r++) {
                const radius = (r / rings) * web.r;
                ctx.beginPath();
                for (let i = 0; i <= strands; i++) {
                    const angle = web.startAngle + (i / strands) * (web.endAngle - web.startAngle);
                    const wobble = Math.sin(angle * 3 + now * 0.3) * 2;
                    const x = web.cx + Math.cos(angle) * (radius + wobble);
                    const y = web.cy + Math.sin(angle) * (radius + wobble);
                    if (i === 0) ctx.moveTo(x, y);
                    else ctx.lineTo(x, y);
                }
                ctx.stroke();
            }
            ctx.restore();
        }

        // ── Draw vines with red glow pulse ──
        for (const vine of this._vines) {
            const pts = vine.points;
            if (pts.length < 2) continue;

            // Draw the vine stem
            ctx.save();
            ctx.lineWidth = vine.thickness;
            ctx.lineCap = 'round';
            ctx.lineJoin = 'round';

            // Base vine color (dark green-brown)
            ctx.strokeStyle = 'rgba(30, 55, 20, 0.3)';
            ctx.beginPath();
            ctx.moveTo(pts[0].x, pts[0].y);
            for (let i = 1; i < pts.length; i++) {
                const prev = pts[i - 1], cur = pts[i];
                const cpx = (prev.x + cur.x) / 2;
                const cpy = (prev.y + cur.y) / 2;
                ctx.quadraticCurveTo(prev.x, prev.y, cpx, cpy);
            }
            ctx.stroke();

            // Red glow pulse traveling along vine
            const pulsePos = ((now * vine.speed + vine.phase) % 1.0);
            const pulseIdx = pulsePos * (pts.length - 1);
            const pi = Math.floor(pulseIdx);
            const pf = pulseIdx - pi;
            if (pi < pts.length - 1) {
                const px = pts[pi].x + (pts[pi + 1].x - pts[pi].x) * pf;
                const py = pts[pi].y + (pts[pi + 1].y - pts[pi].y) * pf;
                const grad = ctx.createRadialGradient(px, py, 0, px, py, 30);
                grad.addColorStop(0, 'rgba(200, 40, 20, 0.25)');
                grad.addColorStop(0.5, 'rgba(180, 30, 15, 0.08)');
                grad.addColorStop(1, 'rgba(160, 20, 10, 0.0)');
                ctx.fillStyle = grad;
                ctx.fillRect(px - 30, py - 30, 60, 60);
            }

            // Small leaves/thorns along vine
            for (let i = 1; i < pts.length - 1; i += 2) {
                const p = pts[i];
                const next = pts[Math.min(i + 1, pts.length - 1)];
                const angle = Math.atan2(next.y - p.y, next.x - p.x) + Math.PI / 2;
                const leafSize = 3 + Math.random() * 3;
                const side = (i % 4 < 2) ? 1 : -1;
                ctx.fillStyle = `rgba(40, 80, 25, ${0.2 + 0.05 * Math.sin(now + i)})`;
                ctx.beginPath();
                ctx.ellipse(
                    p.x + Math.cos(angle) * side * 4,
                    p.y + Math.sin(angle) * side * 4,
                    leafSize, leafSize * 0.4,
                    angle + side * 0.5, 0, Math.PI * 2
                );
                ctx.fill();
            }
            ctx.restore();
        }
    }
}

const _tasloiVines = new TasloiVines();

function applyTheme(themeId) {
    const theme = UI_THEMES[themeId];
    if (!theme) return;
    const root = document.documentElement;
    for (const [prop, val] of Object.entries(theme.vars)) {
        root.style.setProperty(prop, val);
    }
    root.dataset.theme = themeId;
    localStorage.setItem('uiTheme', themeId);

    // Tasloi: animated vine overlay (replaces old static camo)
    let camoEl = document.getElementById('camo-overlay');
    if (camoEl) { camoEl.style.display = 'none'; }

    if (theme.vines) {
        _tasloiVines.show();
    } else {
        _tasloiVines.hide();
    }
}

// Helper: get current theme's orbit colors for round timer
function getThemeOrbit() {
    const id = localStorage.getItem('uiTheme') || 'greylord';
    const t = UI_THEMES[id];
    return t && t.orbit ? t.orbit : { ball: '#c0c8e0', trail: '#4455aa', ripple: '#6677cc', text: ['#8899cc','#c0c8e0'] };
}

// Apply saved theme on load
(function() {
    const saved = localStorage.getItem('uiTheme') || 'greylord';
    applyTheme(saved);
})();
