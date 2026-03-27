// ── Room View Manager ──
// Manages room image loading, crossfade, depth parallax, and entity thumbnails.
// Designed for a full-viewport room viewer with overlay panels.

class RoomView {
    constructor(canvas) {
        this.canvas = canvas;
        this.parallax = new DepthParallax(canvas);
        this.currentRoomKey = null;
        this.currentDepthKey = null;
        this.loading = false;

        // Settings (persisted to localStorage)
        this.settings = this._loadSettings();

        // Apply all parallax settings
        const p = this.parallax;
        p.fillMode = this.settings.fillMode;
        p.depthScale = this.settings.depthScale;
        p.cameraMode = this.settings.cameraMode;
        p.cameraIntensity = this.settings.cameraIntensity;
        p.cameraSpeed = this.settings.cameraSpeed;
        p.isometric = this.settings.isometric;
        p.steady = this.settings.steady;
        p.overscan = this.settings.overscan;
        p.panSpeed = this.settings.panSpeed;
        p.panAmountX = this.settings.panAmountX;
        p.panAmountY = this.settings.panAmountY;
        p.edgeFadeStart = this.settings.edgeFadeStart;
        p.edgeFadeEnd = this.settings.edgeFadeEnd;
        p.depthContrast = this.settings.depthContrast;
        p.vignetteAmount = this.settings.vignetteAmount;
        p.vignetteFeather = this.settings.vignetteFeather;
        p.start();
    }

    _loadSettings() {
        const defaults = {
            depth3d: true,
            preferHires: true,
            fillMode: 'fill',
            depthScale: 0.20,
            cameraMode: 0,
            cameraIntensity: 0.20,
            cameraSpeed: 0.4,
            isometric: 0.0,
            steady: 0.3,
            overscan: 0.06,
            panSpeed: 0.05,
            panAmountX: 0.3,
            panAmountY: 0.2,
            depthContrast: 1.0,
            vignetteAmount: 0.0,
            vignetteFeather: 0.5,
            showMonsters: true,
            showItems: true,
            showConsole: false,
            showScanlines: false,
            showWarpZoom: false,
            scanlineThickness: 2,
            npcLocation: 'floating',
            lootLocation: 'floating',
            npcLocked: false,
            lootLocked: false,
            npcThumbScale: '100%',
            lootThumbScale: '100%',
            dmgTextScale: '100%',
            proMode: 'silent',
            ambientFilter: true,
        };

        try {
            const saved = JSON.parse(localStorage.getItem('roomViewSettings'));
            if (saved) {
                // Force showConsole off — side panel replaced by pop-out terminal
                delete saved.showConsole;
                return { ...defaults, ...saved };
            }
        } catch {}
        return defaults;
    }

    saveSettings() {
        localStorage.setItem('roomViewSettings', JSON.stringify(this.settings));
    }

    updateSetting(key, value) {
        this.settings[key] = value;
        this.saveSettings();
        this._applySetting(key, value);
    }

    _applySetting(key, value) {
        const p = this.parallax;
        switch (key) {
            case 'depthScale': p.depthScale = value; break;
            case 'cameraMode': p.cameraMode = value; break;
            case 'cameraIntensity': p.cameraIntensity = value; break;
            case 'cameraSpeed': p.cameraSpeed = value; break;
            case 'isometric': p.isometric = value; break;
            case 'steady': p.steady = value; break;
            case 'overscan': p.overscan = value; break;
            case 'panSpeed': p.panSpeed = value; break;
            case 'panAmountX': p.panAmountX = value; break;
            case 'panAmountY': p.panAmountY = value; break;
            case 'depthContrast': p.depthContrast = value; break;
            case 'vignetteAmount': p.vignetteAmount = value; break;
            case 'vignetteFeather': p.vignetteFeather = value; break;
            case 'showMonsters':
                document.getElementById('npc-thumbs').style.display = value ? '' : 'none';
                break;
            case 'showItems':
                document.getElementById('item-thumbs').style.display = value ? '' : 'none';
                break;
            case 'showConsole':
                document.getElementById('side-panel').style.display = value ? 'flex' : 'none';
                break;
            case 'showScanlines':
                document.getElementById('scanline-overlay').style.display = value ? '' : 'none';
                break;
            case 'scanlineThickness': {
                const t = parseInt(value) || 2;
                const gap = Math.max(t + 1, Math.round(t * 1.5));
                document.getElementById('scanline-overlay').style.background =
                    `repeating-linear-gradient(0deg, rgba(0,0,0,0.15) 0px, rgba(0,0,0,0.15) ${t}px, transparent ${t}px, transparent ${t + gap}px)`;
                break;
            }
            case 'depth3d':
                if (!value) {
                    p.clearDepth();
                } else {
                    // Re-load depth for current room
                    if (this.currentDepthKey) {
                        this.parallax.loadDepth(`/api/asset/${encodeURIComponent(this.currentDepthKey)}`);
                    }
                }
                break;
            case 'fillMode':
                p.fillMode = value;
                break;
            case 'preferHires':
                fetch('/api/slop/quality', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ hires: value }),
                }).then(() => {
                    // Force room reload
                    this.currentRoomKey = null;
                    this.currentDepthKey = null;
                    if (typeof sendCommand === 'function') sendCommand('get_state');
                });
                break;
            case 'npcThumbScale':
                this._applyThumbScale('npc-thumbs', value);
                break;
            case 'lootThumbScale':
                this._applyThumbScale('item-thumbs', value);
                break;
            case 'npcLocation':
            case 'lootLocation':
                this._applyEntityPanelMode();
                break;
        }
    }

    _applyEntityPanelMode() {
        const panel = document.getElementById('entity-panel');
        const npcLoc = this.settings.npcLocation || 'floating';
        const lootLoc = this.settings.lootLocation || 'floating';

        panel.classList.remove('dock-above', 'dock-below', 'floating');

        if (npcLoc === 'floating' || lootLoc === 'floating') {
            panel.classList.add('floating');
        } else if (npcLoc === 'above') {
            panel.classList.add('dock-above');
        } else {
            panel.classList.add('dock-below');
        }
    }

    _applyThumbScale(containerId, pct) {
        const scales = { '50%': 0.5, '75%': 0.75, '100%': 1.0, '125%': 1.25, '150%': 1.5,
                         '200%': 2.0, '250%': 2.5, '300%': 3.0, '400%': 4.0 };
        const mult = scales[pct] || 1.0;
        const isItem = containerId === 'item-thumbs';
        const baseSize = isItem ? 64 : 67;
        const container = document.getElementById(containerId);
        const count = container.querySelectorAll('.thumb').length;

        let size;
        if (isItem && count > 4) {
            // Auto-grid: pack into a square-ish block, shrink as count grows
            const fullSize = Math.round(baseSize * mult);
            const maxBox = Math.round(fullSize * 3.5);
            const cols = Math.ceil(Math.sqrt(count));
            const fitSize = Math.floor((maxBox - (cols - 1) * 4) / cols);
            size = Math.max(Math.min(fitSize, fullSize), 20);
            container.style.maxWidth = `${maxBox + 16}px`;
        } else {
            size = Math.round(baseSize * mult);
            container.style.maxWidth = '';
        }

        for (const thumb of container.querySelectorAll('.thumb')) {
            thumb.style.width = `${size}px`;
            thumb.style.height = `${size}px`;
        }
    }

    async loadRoom(roomImageKey, depthKey) {
        if (this.currentRoomKey === roomImageKey && this.currentDepthKey === depthKey) {
            return; // Same room, skip
        }

        this.loading = true;
        this.currentRoomKey = roomImageKey;
        this.currentDepthKey = depthKey;

        // Load color image
        if (roomImageKey) {
            const imgOk = await this.parallax.loadImage(`/api/asset/${encodeURIComponent(roomImageKey)}`);
            console.log(`[RoomView] color image loaded: ${imgOk}, key=${roomImageKey}`);
        }

        // Load depth map
        if (depthKey && this.settings.depth3d) {
            const depthOk = await this.parallax.loadDepth(`/api/asset/${encodeURIComponent(depthKey)}`);
            console.log(`[RoomView] depth map loaded: ${depthOk}, key=${depthKey}, hasDepth=${this.parallax.hasDepth}, depthScale=${this.parallax.depthScale}`);
        } else {
            console.log(`[RoomView] NO depth: depthKey=${depthKey}, depth3d=${this.settings.depth3d}`);
        }

        this.loading = false;
    }

    applyAllSettings() {
        for (const [key, value] of Object.entries(this.settings)) {
            this._applySetting(key, value);
        }
        this._applyEntityPanelMode();
    }
}
