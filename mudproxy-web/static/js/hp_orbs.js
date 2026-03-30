// ── HP & Mana Orbs — Floating draggable widget ──
// Two glowing orbs showing HP (green) and Mana (blue) with liquid fill.
// Follows the same pattern as RoundTimer: draggable, hideable, rescalable.

class HpOrbs {
    constructor() {
        this._visible = true;
        this._scale = 1.0;
        this._hp = 0; this._maxHp = 1;
        this._mana = 0; this._maxMana = 1;
        this._animId = null;
        this._waveOffset = 0;
        // Options (persisted)
        this._manaColor = 'blue_water';    // blue_water | white_glow | neon_plasma
        this._hpColor = 'solid_green';     // solid_green | green_yellow_red | smooth_shift
        this._showLabels = true;           // HP/MA labels above orbs
        this._showTotals = true;           // numbers inside orbs
        this._layout = 'horizontal';       // horizontal | vertical
        this._build();
        this._restoreState();
    }

    _build() {
        const el = document.createElement('div');
        el.id = 'hp-orbs';
        el.className = 'hp-orbs-widget';
        const canvas = document.createElement('canvas');
        canvas.width = 200;
        canvas.height = 110;
        el.appendChild(canvas);
        this._canvas = canvas;
        this._ctx = canvas.getContext('2d');
        document.body.appendChild(el);
        this._el = el;
        this._makeDraggable();
        this._buildContextMenu();

        el.addEventListener('contextmenu', (e) => {
            e.preventDefault();
            e.stopPropagation();
            this._showContextMenu(e.clientX, e.clientY);
        });

        if (this._visible) this._startLoop();
    }

    // ── Canvas sizing based on layout ──
    _updateCanvasSize() {
        if (this._layout === 'vertical') {
            this._canvas.width = Math.round(110 * this._scale);
            this._canvas.height = Math.round(200 * this._scale);
        } else {
            this._canvas.width = Math.round(200 * this._scale);
            this._canvas.height = Math.round(110 * this._scale);
        }
    }

    // ── Context Menu ──
    _buildContextMenu() {
        const menu = document.createElement('div');
        menu.className = 'orb-context-menu';
        menu.style.display = 'none';
        menu.innerHTML = `
            <div class="orb-menu-item orb-has-sub">Mana Color
                <div class="orb-submenu">
                    <div class="orb-menu-item" data-mana="blue_water">Blue Water</div>
                    <div class="orb-menu-item" data-mana="white_glow">White Glow</div>
                    <div class="orb-menu-item" data-mana="neon_plasma">Neon Fractal Plasma</div>
                </div>
            </div>
            <div class="orb-menu-item orb-has-sub">Health Color
                <div class="orb-submenu">
                    <div class="orb-menu-item" data-hp="solid_green">Solid Green</div>
                    <div class="orb-menu-item" data-hp="green_yellow_red">Green-Yellow-Red</div>
                    <div class="orb-menu-item" data-hp="smooth_shift">Smooth HP Shift</div>
                </div>
            </div>
            <div class="orb-menu-item orb-has-sub">Layout
                <div class="orb-submenu">
                    <div class="orb-menu-item" data-layout="horizontal">Horizontal</div>
                    <div class="orb-menu-item" data-layout="vertical">Vertical</div>
                </div>
            </div>
            <div class="orb-menu-item orb-has-sub">Scale
                <div class="orb-submenu">
                    <div class="orb-menu-item" data-scale="0.5">Small</div>
                    <div class="orb-menu-item" data-scale="0.75">Medium</div>
                    <div class="orb-menu-item" data-scale="1">Normal</div>
                    <div class="orb-menu-item" data-scale="1.5">Large</div>
                    <div class="orb-menu-item" data-scale="2">XL</div>
                </div>
            </div>
            <div class="orb-menu-sep"></div>
            <div class="orb-menu-item" data-action="toggle-labels">Show/Hide HP/MA Labels</div>
            <div class="orb-menu-item" data-action="toggle-totals">Show/Hide Totals</div>
            <div class="orb-menu-sep"></div>
            <div class="orb-menu-item" data-action="hide">Hide Orbs</div>
        `;
        document.body.appendChild(menu);
        this._menu = menu;

        menu.querySelectorAll('[data-scale]').forEach(item => {
            item.addEventListener('click', () => {
                this._setScale(parseFloat(item.dataset.scale));
                menu.style.display = 'none';
            });
        });
        menu.querySelectorAll('[data-mana]').forEach(item => {
            item.addEventListener('click', () => {
                this._manaColor = item.dataset.mana;
                this._saveState();
                menu.style.display = 'none';
            });
        });
        menu.querySelectorAll('[data-hp]').forEach(item => {
            item.addEventListener('click', () => {
                this._hpColor = item.dataset.hp;
                this._saveState();
                menu.style.display = 'none';
            });
        });
        menu.querySelectorAll('[data-layout]').forEach(item => {
            item.addEventListener('click', () => {
                this._layout = item.dataset.layout;
                this._updateCanvasSize();
                this._saveState();
                menu.style.display = 'none';
            });
        });
        menu.querySelector('[data-action="toggle-labels"]').addEventListener('click', () => {
            this._showLabels = !this._showLabels;
            this._saveState();
            menu.style.display = 'none';
        });
        menu.querySelector('[data-action="toggle-totals"]').addEventListener('click', () => {
            this._showTotals = !this._showTotals;
            this._saveState();
            menu.style.display = 'none';
        });
        menu.querySelector('[data-action="hide"]').addEventListener('click', () => {
            this.toggle(false);
            menu.style.display = 'none';
        });

        document.addEventListener('click', () => { menu.style.display = 'none'; });
    }

    _showContextMenu(x, y) {
        // Update checkmarks
        this._menu.querySelectorAll('[data-mana]').forEach(el => {
            el.classList.toggle('orb-active', el.dataset.mana === this._manaColor);
        });
        this._menu.querySelectorAll('[data-hp]').forEach(el => {
            el.classList.toggle('orb-active', el.dataset.hp === this._hpColor);
        });
        this._menu.querySelectorAll('[data-layout]').forEach(el => {
            el.classList.toggle('orb-active', el.dataset.layout === this._layout);
        });
        this._menu.querySelectorAll('[data-scale]').forEach(el => {
            el.classList.toggle('orb-active', parseFloat(el.dataset.scale) === this._scale);
        });
        const toggleLabels = this._menu.querySelector('[data-action="toggle-labels"]');
        toggleLabels.textContent = (this._showLabels ? '✓ ' : '  ') + 'HP/MA Labels';
        const toggleTotals = this._menu.querySelector('[data-action="toggle-totals"]');
        toggleTotals.textContent = (this._showTotals ? '✓ ' : '  ') + 'HP/MA Totals';

        this._menu.style.display = 'block';
        const r = this._menu.getBoundingClientRect();
        if (x + r.width > window.innerWidth) x = window.innerWidth - r.width - 4;
        if (y + r.height > window.innerHeight) y = window.innerHeight - r.height - 4;
        this._menu.style.left = x + 'px';
        this._menu.style.top = y + 'px';
    }

    _setScale(s) {
        this._scale = s;
        this._updateCanvasSize();
        this._saveState();
    }

    // ── Toggle Visibility ──
    toggle(show) {
        if (show === undefined) show = !this._visible;
        this._visible = show;
        this._el.style.display = show ? '' : 'none';
        if (show) this._startLoop(); else this._stopLoop();
        this._saveState();
    }

    // ── Update Data ──
    update(hp, maxHp, mana, maxMana) {
        this._hp = hp || 0;
        this._maxHp = maxHp || 1;
        this._mana = mana || 0;
        this._maxMana = maxMana || 1;
    }

    // ── Animation Loop ──
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

    // ── HP Color helpers ──
    _getHpColors(pct) {
        switch (this._hpColor) {
            case 'green_yellow_red': {
                // 3-stop: green > yellow > red
                let r, g, b;
                if (pct > 0.5) {
                    const t = (pct - 0.5) * 2; // 1 at full, 0 at half
                    r = Math.round(34 + (1 - t) * 200);
                    g = Math.round(204);
                    b = Math.round(68 * t);
                } else {
                    const t = pct * 2; // 0 at empty, 1 at half
                    r = Math.round(220);
                    g = Math.round(50 + t * 154);
                    b = Math.round(30);
                }
                const color = `rgb(${r},${g},${b})`;
                const dark = `rgb(${r >> 1},${g >> 1},${b >> 1})`;
                const glow = `rgb(${Math.min(255, r + 50)},${Math.min(255, g + 50)},${Math.min(255, b + 50)})`;
                return { color, dark, glow, label: `rgb(${Math.min(255, r + 30)},${Math.min(255, g + 30)},${Math.min(255, b + 30)})`, sub: `rgba(${r},${g},${b},0.7)` };
            }
            case 'smooth_shift': {
                // Smooth HSL shift: green(120) at full → red(0) at empty
                const hue = pct * 120;
                const color = `hsl(${hue}, 80%, 45%)`;
                const dark = `hsl(${hue}, 60%, 20%)`;
                const glow = `hsl(${hue}, 90%, 65%)`;
                return { color, dark, glow, label: `hsl(${hue}, 85%, 65%)`, sub: `hsl(${hue}, 70%, 55%)` };
            }
            default: // solid_green
                return { color: '#22cc44', dark: '#115522', glow: '#44ff66', label: '#88ff88', sub: '#aaffaa' };
        }
    }

    // ── Mana Color helpers ──
    _getManaColors() {
        switch (this._manaColor) {
            case 'white_glow':
                return { color: '#ccccee', dark: '#555566', glow: '#ffffff', label: '#ddddf8', sub: '#ccccee' };
            case 'neon_plasma':
                return null; // special render path
            default: // blue_water
                return { color: '#2266ee', dark: '#112255', glow: '#44aaff', label: '#88bbff', sub: '#aaddff' };
        }
    }

    // ── Render ──
    _render() {
        const ctx = this._ctx;
        const s = this._scale;
        const w = this._canvas.width;
        const h = this._canvas.height;
        ctx.clearRect(0, 0, w, h);

        this._waveOffset += 0.03;

        const orbR = 38 * s;
        const gap = 16 * s;

        let cx1, cy1, cx2, cy2;
        if (this._layout === 'vertical') {
            cx1 = w / 2;
            cy1 = orbR + 14 * s;
            cx2 = w / 2;
            cy2 = h - orbR - 14 * s;
        } else {
            cx1 = w / 2 - orbR - gap / 2;
            cy1 = h / 2 + 2 * s;
            cx2 = w / 2 + orbR + gap / 2;
            cy2 = cy1;
        }

        // Shadow for all text
        ctx.shadowColor = 'rgba(0,0,0,0.9)';
        ctx.shadowBlur = 6 * s;
        ctx.shadowOffsetX = 0;
        ctx.shadowOffsetY = 2 * s;

        // Draw HP orb
        const hpPct = this._maxHp > 0 ? this._hp / this._maxHp : 0;
        const hpC = this._getHpColors(hpPct);
        this._drawOrb(ctx, cx1, cy1, orbR, hpPct, hpC.color, hpC.dark, hpC.glow, s);
        if (this._showTotals) {
            ctx.fillStyle = '#fff';
            ctx.font = `bold ${Math.round(12 * s)}px monospace`;
            ctx.textAlign = 'center';
            ctx.textBaseline = 'middle';
            ctx.fillText(`${this._hp}`, cx1, cy1 - 4 * s);
            ctx.font = `${Math.round(9 * s)}px monospace`;
            ctx.fillStyle = hpC.sub;
            ctx.fillText(`/ ${this._maxHp}`, cx1, cy1 + 11 * s);
        }
        if (this._showLabels) {
            ctx.font = `bold ${Math.round(11 * s)}px monospace`;
            ctx.fillStyle = hpC.label;
            ctx.textAlign = 'center';
            ctx.fillText('HP', cx1, cy1 - orbR - 7 * s);
        }

        // Draw Mana orb
        const manaPct = this._maxMana > 0 ? this._mana / this._maxMana : 0;
        const manaC = this._getManaColors();
        if (manaC) {
            this._drawOrb(ctx, cx2, cy2, orbR, manaPct, manaC.color, manaC.dark, manaC.glow, s);
        } else {
            // Neon plasma
            this._drawPlasmaOrb(ctx, cx2, cy2, orbR, manaPct, s);
        }
        if (this._showTotals) {
            ctx.fillStyle = '#fff';
            ctx.font = `bold ${Math.round(12 * s)}px monospace`;
            ctx.textAlign = 'center';
            ctx.textBaseline = 'middle';
            ctx.fillText(`${this._mana}`, cx2, cy2 - 4 * s);
            ctx.font = `${Math.round(9 * s)}px monospace`;
            ctx.fillStyle = manaC ? manaC.sub : '#ccaaff';
            ctx.fillText(`/ ${this._maxMana}`, cx2, cy2 + 11 * s);
        }
        if (this._showLabels) {
            ctx.font = `bold ${Math.round(11 * s)}px monospace`;
            ctx.fillStyle = manaC ? manaC.label : '#cc88ff';
            ctx.textAlign = 'center';
            ctx.fillText('MA', cx2, cy2 - orbR - 7 * s);
        }

        // Reset shadow
        ctx.shadowColor = 'transparent';
        ctx.shadowBlur = 0;
        ctx.shadowOffsetY = 0;
    }

    _drawOrb(ctx, cx, cy, r, pct, color, darkColor, glowColor, s) {
        // Background circle
        ctx.save();
        ctx.beginPath();
        ctx.arc(cx, cy, r, 0, Math.PI * 2);
        ctx.closePath();
        ctx.clip();

        // Dark fill
        const bg = ctx.createRadialGradient(cx - r * 0.3, cy - r * 0.3, 0, cx, cy, r);
        bg.addColorStop(0, '#1a1a2e');
        bg.addColorStop(1, '#050510');
        ctx.fillStyle = bg;
        ctx.fillRect(cx - r, cy - r, r * 2, r * 2);

        // Liquid fill from bottom
        const fillY = cy + r - (pct * r * 2);
        // Wavy top edge
        ctx.beginPath();
        ctx.moveTo(cx - r, cy + r);
        for (let x = cx - r; x <= cx + r; x += 2) {
            const norm = (x - (cx - r)) / (r * 2);
            const wave = Math.sin(norm * Math.PI * 3 + this._waveOffset) * 3 * s;
            const wave2 = Math.sin(norm * Math.PI * 2 - this._waveOffset * 0.7) * 2 * s;
            ctx.lineTo(x, fillY + wave + wave2);
        }
        ctx.lineTo(cx + r, cy + r);
        ctx.closePath();

        const fillGrad = ctx.createLinearGradient(cx, fillY, cx, cy + r);
        fillGrad.addColorStop(0, color);
        fillGrad.addColorStop(0.5, darkColor);
        fillGrad.addColorStop(1, color);
        ctx.fillStyle = fillGrad;
        ctx.fill();

        ctx.restore();

        // Outer ring
        ctx.beginPath();
        ctx.arc(cx, cy, r, 0, Math.PI * 2);
        ctx.strokeStyle = color;
        ctx.lineWidth = 2 * s;
        ctx.stroke();

        // Glass highlight
        ctx.beginPath();
        ctx.arc(cx - r * 0.2, cy - r * 0.3, r * 0.4, 0, Math.PI * 2);
        const hl = ctx.createRadialGradient(cx - r * 0.2, cy - r * 0.3, 0, cx - r * 0.2, cy - r * 0.3, r * 0.4);
        hl.addColorStop(0, 'rgba(255,255,255,0.2)');
        hl.addColorStop(1, 'rgba(255,255,255,0)');
        ctx.fillStyle = hl;
        ctx.fill();

        // Glow when pct is high
        if (pct > 0.8) {
            ctx.beginPath();
            ctx.arc(cx, cy, r + 4 * s, 0, Math.PI * 2);
            ctx.strokeStyle = glowColor + '40';
            ctx.lineWidth = 3 * s;
            ctx.stroke();
        }

        // Danger pulse when low
        if (pct < 0.25 && pct > 0) {
            const pulse = Math.sin(Date.now() / 200) * 0.3 + 0.3;
            ctx.beginPath();
            ctx.arc(cx, cy, r + 2 * s, 0, Math.PI * 2);
            ctx.strokeStyle = `rgba(255,50,50,${pulse})`;
            ctx.lineWidth = 2 * s;
            ctx.stroke();
        }
    }

    // ── Neon Fractal Plasma orb ──
    _drawPlasmaOrb(ctx, cx, cy, r, pct, s) {
        ctx.save();
        ctx.beginPath();
        ctx.arc(cx, cy, r, 0, Math.PI * 2);
        ctx.closePath();
        ctx.clip();

        // Dark background
        const bg = ctx.createRadialGradient(cx - r * 0.3, cy - r * 0.3, 0, cx, cy, r);
        bg.addColorStop(0, '#0a0a1e');
        bg.addColorStop(1, '#020208');
        ctx.fillStyle = bg;
        ctx.fillRect(cx - r, cy - r, r * 2, r * 2);

        // Animated fractal plasma fill
        const fillY = cy + r - (pct * r * 2);
        const t = this._waveOffset;
        const step = 3;
        for (let py = Math.floor(fillY); py <= cy + r; py += step) {
            for (let px = cx - r; px <= cx + r; px += step) {
                // Check if pixel is inside circle
                const dx = px - cx, dy = py - cy;
                if (dx * dx + dy * dy > r * r) continue;

                // Plasma function — overlapping sine waves
                const v1 = Math.sin((px * 0.04 + t * 1.3));
                const v2 = Math.sin((py * 0.05 - t * 0.9));
                const v3 = Math.sin((px * 0.03 + py * 0.04 + t * 1.1));
                const v4 = Math.sin(Math.sqrt(dx * dx + dy * dy) * 0.08 + t * 1.5);
                const v = (v1 + v2 + v3 + v4) / 4;

                const hue = (v * 60 + t * 30 + 270) % 360; // cycle through purple/cyan/magenta
                const lit = 45 + v * 20;
                ctx.fillStyle = `hsl(${hue}, 95%, ${lit}%)`;
                ctx.fillRect(px, py, step, step);
            }
        }

        // Wavy edge at fill line to match other orbs
        ctx.beginPath();
        ctx.moveTo(cx - r, fillY - 4 * s);
        for (let x = cx - r; x <= cx + r; x += 2) {
            const norm = (x - (cx - r)) / (r * 2);
            const wave = Math.sin(norm * Math.PI * 3 + t) * 3 * s;
            ctx.lineTo(x, fillY + wave);
        }
        ctx.lineTo(cx + r, fillY - 4 * s);
        ctx.lineTo(cx + r, cy - r - 1);
        ctx.lineTo(cx - r, cy - r - 1);
        ctx.closePath();
        ctx.fillStyle = '#020208';
        ctx.fill();

        ctx.restore();

        // Neon outer ring — animated color
        const ringHue = (t * 40 + 270) % 360;
        ctx.beginPath();
        ctx.arc(cx, cy, r, 0, Math.PI * 2);
        ctx.strokeStyle = `hsl(${ringHue}, 90%, 55%)`;
        ctx.lineWidth = 2 * s;
        ctx.stroke();

        // Glow
        ctx.beginPath();
        ctx.arc(cx, cy, r + 3 * s, 0, Math.PI * 2);
        ctx.strokeStyle = `hsla(${ringHue}, 90%, 60%, 0.3)`;
        ctx.lineWidth = 4 * s;
        ctx.stroke();

        // Glass highlight
        ctx.beginPath();
        ctx.arc(cx - r * 0.2, cy - r * 0.3, r * 0.4, 0, Math.PI * 2);
        const hl = ctx.createRadialGradient(cx - r * 0.2, cy - r * 0.3, 0, cx - r * 0.2, cy - r * 0.3, r * 0.4);
        hl.addColorStop(0, 'rgba(255,255,255,0.15)');
        hl.addColorStop(1, 'rgba(255,255,255,0)');
        ctx.fillStyle = hl;
        ctx.fill();
    }

    // ── Drag ──
    _makeDraggable() {
        let d = false, ox = 0, oy = 0;
        this._el.addEventListener('mousedown', (e) => {
            if (e.button !== 0) return;
            d = true; const r = this._el.getBoundingClientRect();
            ox = e.clientX - r.left; oy = e.clientY - r.top; e.preventDefault();
        });
        document.addEventListener('mousemove', (e) => {
            if (!d) return;
            let nl = e.clientX - ox, nt = e.clientY - oy;
            const r = this._el.getBoundingClientRect();
            nt = Math.max(0, Math.min(nt, window.innerHeight - 32));
            nl = Math.max(-r.width + 40, Math.min(nl, window.innerWidth - 40));
            this._el.style.left = nl + 'px'; this._el.style.top = nt + 'px';
            this._el.style.right = 'auto'; this._el.style.bottom = 'auto';
        });
        document.addEventListener('mouseup', () => { if (d) { d = false; this._saveState(); } });
    }

    // ── Persistence ──
    _saveState() {
        try {
            const r = this._el.getBoundingClientRect();
            localStorage.setItem('hpOrbsState', JSON.stringify({
                visible: this._visible, left: r.left, top: r.top, scale: this._scale,
                manaColor: this._manaColor, hpColor: this._hpColor,
                showLabels: this._showLabels, showTotals: this._showTotals,
                layout: this._layout
            }));
        } catch {}
    }

    _restoreState() {
        try {
            const s = JSON.parse(localStorage.getItem('hpOrbsState') || '{}');
            if (s.visible === false) { this._visible = false; this._el.style.display = 'none'; this._stopLoop(); }
            if (s.left != null) {
                this._el.style.left = s.left + 'px'; this._el.style.top = s.top + 'px';
                this._el.style.right = 'auto'; this._el.style.bottom = 'auto';
            }
            if (s.manaColor) this._manaColor = s.manaColor;
            if (s.hpColor) this._hpColor = s.hpColor;
            if (s.showLabels === false) this._showLabels = false;
            if (s.showTotals === false) this._showTotals = false;
            if (s.layout) this._layout = s.layout;
            if (s.scale && s.scale !== 1) this._setScale(s.scale);
            this._updateCanvasSize();
        } catch {}
    }
}
