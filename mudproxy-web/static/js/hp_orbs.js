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

    // ── Context Menu ──
    _buildContextMenu() {
        const menu = document.createElement('div');
        menu.className = 'orb-context-menu';
        menu.style.display = 'none';
        menu.innerHTML = `
            <div class="orb-menu-item orb-has-sub">Scale
                <div class="orb-submenu">
                    <div class="orb-menu-item" data-scale="0.5">Small</div>
                    <div class="orb-menu-item" data-scale="0.75">Medium</div>
                    <div class="orb-menu-item" data-scale="1">Normal</div>
                    <div class="orb-menu-item" data-scale="1.5">Large</div>
                    <div class="orb-menu-item" data-scale="2">XL</div>
                </div>
            </div>
            <div class="orb-menu-item" data-action="hide">Hide</div>
        `;
        document.body.appendChild(menu);
        this._menu = menu;

        menu.querySelectorAll('[data-scale]').forEach(item => {
            item.addEventListener('click', () => {
                this._setScale(parseFloat(item.dataset.scale));
                menu.style.display = 'none';
            });
        });
        menu.querySelector('[data-action="hide"]').addEventListener('click', () => {
            this.toggle(false);
            menu.style.display = 'none';
        });

        document.addEventListener('click', () => { menu.style.display = 'none'; });
    }

    _showContextMenu(x, y) {
        this._menu.style.display = 'block';
        const r = this._menu.getBoundingClientRect();
        if (x + r.width > window.innerWidth) x = window.innerWidth - r.width - 4;
        if (y + r.height > window.innerHeight) y = window.innerHeight - r.height - 4;
        this._menu.style.left = x + 'px';
        this._menu.style.top = y + 'px';
    }

    _setScale(s) {
        this._scale = s;
        this._canvas.width = Math.round(200 * s);
        this._canvas.height = Math.round(110 * s);
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
        const cx1 = w / 2 - orbR - gap / 2;
        const cx2 = w / 2 + orbR + gap / 2;
        const cy = h / 2 + 2 * s;

        // Shadow for all text
        ctx.shadowColor = 'rgba(0,0,0,0.9)';
        ctx.shadowBlur = 6 * s;
        ctx.shadowOffsetX = 0;
        ctx.shadowOffsetY = 2 * s;

        // Draw HP orb
        const hpPct = this._maxHp > 0 ? this._hp / this._maxHp : 0;
        this._drawOrb(ctx, cx1, cy, orbR, hpPct, '#22cc44', '#115522', '#44ff66', s);
        // HP text
        ctx.fillStyle = '#fff';
        ctx.font = `bold ${Math.round(12 * s)}px monospace`;
        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';
        ctx.fillText(`${this._hp}`, cx1, cy - 4 * s);
        ctx.font = `${Math.round(9 * s)}px monospace`;
        ctx.fillStyle = '#aaffaa';
        ctx.fillText(`/ ${this._maxHp}`, cx1, cy + 11 * s);
        // Label
        ctx.font = `bold ${Math.round(11 * s)}px monospace`;
        ctx.fillStyle = '#88ff88';
        ctx.fillText('HP', cx1, cy - orbR - 7 * s);

        // Draw Mana orb
        const manaPct = this._maxMana > 0 ? this._mana / this._maxMana : 0;
        this._drawOrb(ctx, cx2, cy, orbR, manaPct, '#2266ee', '#112255', '#44aaff', s);
        // Mana text
        ctx.fillStyle = '#fff';
        ctx.font = `bold ${Math.round(12 * s)}px monospace`;
        ctx.textAlign = 'center';
        ctx.fillText(`${this._mana}`, cx2, cy - 4 * s);
        ctx.font = `${Math.round(9 * s)}px monospace`;
        ctx.fillStyle = '#aaddff';
        ctx.fillText(`/ ${this._maxMana}`, cx2, cy + 11 * s);
        // Label
        ctx.font = `bold ${Math.round(11 * s)}px monospace`;
        ctx.fillStyle = '#88bbff';
        ctx.fillText('MA', cx2, cy - orbR - 7 * s);

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
                visible: this._visible, left: r.left, top: r.top, scale: this._scale
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
            if (s.scale && s.scale !== 1) this._setScale(s.scale);
        } catch {}
    }
}
