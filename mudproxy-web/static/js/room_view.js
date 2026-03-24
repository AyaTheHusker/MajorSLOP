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

        // Start parallax loop
        this.parallax.depthScale = this.settings.depthScale;
        this.parallax.cameraMode = this.settings.cameraMode;
        this.parallax.cameraIntensity = this.settings.cameraIntensity;
        this.parallax.cameraSpeed = this.settings.cameraSpeed;
        this.parallax.isometric = this.settings.isometric;
        this.parallax.steady = this.settings.steady;
        this.parallax.overscan = this.settings.overscan;
        this.parallax.edgeFadeStart = this.settings.edgeFadeStart;
        this.parallax.edgeFadeEnd = this.settings.edgeFadeEnd;
        this.parallax.depthContrast = this.settings.depthContrast;
        this.parallax.start();
    }

    _loadSettings() {
        const defaults = {
            depth3d: true,
            depthScale: 0.15,
            cameraMode: 3,
            cameraIntensity: 0.03,
            cameraSpeed: 0.4,
            isometric: 0.0,
            steady: 0.5,
            overscan: 0.05,
            edgeFadeStart: 0.05,
            edgeFadeEnd: 0.0,
            depthContrast: 1.0,
            showMonsters: true,
            showItems: true,
            showConsole: true,
            showScanlines: false,
            showWarpZoom: false,
            scanlineThickness: 2,
            npcLocation: 'floating',    // above, below, floating
            lootLocation: 'floating',
            npcLocked: false,
            lootLocked: false,
            npcThumbScale: '100%',
            lootThumbScale: '100%',
            dmgTextScale: '100%',
        };

        try {
            const saved = JSON.parse(localStorage.getItem('roomViewSettings'));
            if (saved) return { ...defaults, ...saved };
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
            case 'edgeFadeStart': p.edgeFadeStart = value; break;
            case 'edgeFadeEnd': p.edgeFadeEnd = value; break;
            case 'depthContrast': p.depthContrast = value; break;
            case 'showMonsters':
                document.getElementById('npc-thumbs').style.display = value ? '' : 'none';
                break;
            case 'showItems':
                document.getElementById('item-thumbs').style.display = value ? '' : 'none';
                break;
            case 'showConsole':
                document.getElementById('side-panel').style.display = value ? '' : 'none';
                break;
            case 'showScanlines':
                document.getElementById('scanline-overlay').style.display = value ? '' : 'none';
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

        // Use the "most floating" mode — if either is floating, panel is floating
        // If both docked, use npc location for panel position
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
        const scales = { '50%': 0.5, '75%': 0.75, '100%': 1.0, '125%': 1.25, '150%': 1.5, '200%': 2.0 };
        const mult = scales[pct] || 1.0;
        const baseSize = containerId === 'npc-thumbs' ? 67 : 32;
        const size = Math.round(baseSize * mult);
        const container = document.getElementById(containerId);
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
            await this.parallax.loadImage(`/api/asset/${encodeURIComponent(roomImageKey)}`);
        }

        // Load depth map
        if (depthKey && this.settings.depth3d) {
            await this.parallax.loadDepth(`/api/asset/${encodeURIComponent(depthKey)}`);
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
