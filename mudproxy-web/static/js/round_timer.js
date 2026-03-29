// ── Round Timer — Persistent orbital rhythm tracker ──
// The server has a global ~4s round clock that never stops.
// Combat events give us observable proof of where the tick is.
// The orbit keeps spinning forever once calibrated — combat just re-syncs it.
// Colors come from the theme's orbit palette via getThemeOrbit().
// "Cast to Time" feature: spam a spell to force-detect round boundaries.

class RoundTimer {
    constructor(sendCommand) {
        this._sendCommand = sendCommand || null;
        this._visible = false;
        this._synced = false;
        this._inCombat = false;
        this._roundNum = 0;
        this._lastTickTs = 0;
        this._roundPeriod = parseInt(localStorage.getItem('roundTimerDefaultPeriod') || '5000');
        this._recentIntervals = [];
        this._maxIntervals = 10;
        this._spellUsed = false;
        this._animId = null;
        this._trail = [];
        this._maxTrail = 28;
        this._ripples = [];
        this._deltaText = '';
        this._deltaFade = 0;
        this._scale = 1.0;
        this._recentDeltas = [];     // absolute deltas for lag detection
        this._maxDeltas = 12;

        // Cast to Time state
        this._timingSpell = localStorage.getItem('roundTimerSpell') || '';
        this._timingActive = false;
        this._timingInterval = null;
        this._timingRoundsNeeded = 6;
        this._timingRoundsGot = 0;
        this._timingAlreadyCount = 0;  // consecutive "You have already" before cast

        this._build();
        this._restoreState();
    }

    _build() {
        const el = document.createElement('div');
        el.id = 'round-timer';
        el.className = 'round-timer-widget';
        el.style.display = 'none';
        const canvas = document.createElement('canvas');
        canvas.width = 180;
        canvas.height = 200;
        el.appendChild(canvas);
        this._canvas = canvas;
        this._ctx = canvas.getContext('2d');
        document.body.appendChild(el);
        this._el = el;
        this._makeDraggable();
        this._buildContextMenu();
        this._buildSpellPopup();

        // Right-click → context menu
        el.addEventListener('contextmenu', (e) => {
            e.preventDefault();
            e.stopPropagation();
            this._showContextMenu(e.clientX, e.clientY);
        });

        // Double-click → cast to time
        el.addEventListener('dblclick', (e) => {
            e.preventDefault();
            this._startTiming();
        });
    }

    // ── Context Menu ──
    _buildContextMenu() {
        const menu = document.createElement('div');
        menu.className = 'rt-context-menu';
        menu.style.display = 'none';
        menu.innerHTML = `
            <div class="rt-menu-item rt-has-sub">Scale
                <div class="rt-submenu">
                    <div class="rt-menu-item" data-scale="0.5">0.5x</div>
                    <div class="rt-menu-item rt-active" data-scale="1">1x</div>
                    <div class="rt-menu-item" data-scale="2">2x</div>
                </div>
            </div>
            <div class="rt-menu-item rt-has-sub">Round Period
                <div class="rt-submenu">
                    <div class="rt-menu-item" data-period="4000">4.0s</div>
                    <div class="rt-menu-item" data-period="4500">4.5s</div>
                    <div class="rt-menu-item rt-active" data-period="5000">5.0s (default)</div>
                    <div class="rt-menu-item" data-period="5500">5.5s</div>
                    <div class="rt-menu-item" data-period="6000">6.0s</div>
                </div>
            </div>
            <div class="rt-menu-divider"></div>
            <div class="rt-menu-item" data-action="set-spell">Set Timing Spell</div>
            <div class="rt-menu-item" data-action="cast-to-time">Cast to Time</div>
        `;
        document.body.appendChild(menu);
        this._ctxMenu = menu;

        // Scale items
        menu.querySelectorAll('[data-scale]').forEach(item => {
            item.addEventListener('click', (e) => {
                e.stopPropagation();
                this._setScale(parseFloat(item.dataset.scale));
                this._hideContextMenu();
            });
        });

        // Period items
        menu.querySelectorAll('[data-period]').forEach(item => {
            item.addEventListener('click', (e) => {
                e.stopPropagation();
                const ms = parseInt(item.dataset.period);
                this._roundPeriod = ms;
                localStorage.setItem('roundTimerDefaultPeriod', ms);
                // Reset measured intervals so it uses new default
                this._recentIntervals = [];
                this._hideContextMenu();
            });
        });

        // Action items
        menu.querySelector('[data-action="set-spell"]').addEventListener('click', (e) => {
            e.stopPropagation();
            this._hideContextMenu();
            this._showSpellPopup();
        });
        menu.querySelector('[data-action="cast-to-time"]').addEventListener('click', (e) => {
            e.stopPropagation();
            this._hideContextMenu();
            this._startTiming();
        });

        // Close on click outside
        document.addEventListener('mousedown', (e) => {
            if (!menu.contains(e.target)) this._hideContextMenu();
        });
    }

    _showContextMenu(x, y) {
        const menu = this._ctxMenu;
        // Update active scale
        menu.querySelectorAll('[data-scale]').forEach(item => {
            item.classList.toggle('rt-active', parseFloat(item.dataset.scale) === this._scale);
        });
        // Update active period (closest match)
        const curPeriod = parseInt(localStorage.getItem('roundTimerDefaultPeriod') || '5000');
        menu.querySelectorAll('[data-period]').forEach(item => {
            item.classList.toggle('rt-active', parseInt(item.dataset.period) === curPeriod);
        });
        // Update cast to time label
        const castItem = menu.querySelector('[data-action="cast-to-time"]');
        if (this._timingActive) {
            castItem.textContent = 'Stop Timing';
        } else {
            castItem.textContent = this._timingSpell ? 'Cast to Time' : 'Cast to Time (set spell first)';
        }
        menu.style.display = 'block';
        // Position near cursor, keep on screen
        const mw = menu.offsetWidth, mh = menu.offsetHeight;
        menu.style.left = Math.min(x, window.innerWidth - mw - 4) + 'px';
        menu.style.top = Math.min(y, window.innerHeight - mh - 4) + 'px';
    }

    _hideContextMenu() {
        this._ctxMenu.style.display = 'none';
    }

    // ── Spell Popup ──
    _buildSpellPopup() {
        const pop = document.createElement('div');
        pop.className = 'rt-spell-popup';
        pop.style.display = 'none';
        pop.innerHTML = `
            <div class="rt-spell-title">Set Timing Spell</div>
            <div class="rt-spell-desc">Enter the spell command to spam for round detection.<br>Example: cast or c ligh or sing etc.</div>
            <input type="text" class="rt-spell-input" maxlength="30" placeholder="e.g. c ligh" spellcheck="false">
            <div class="rt-spell-buttons">
                <button class="rt-spell-btn rt-spell-ok">OK</button>
                <button class="rt-spell-btn rt-spell-cancel">Cancel</button>
                <button class="rt-spell-btn rt-spell-clear">Clear</button>
            </div>
        `;
        document.body.appendChild(pop);
        this._spellPopup = pop;
        this._spellInput = pop.querySelector('.rt-spell-input');

        pop.querySelector('.rt-spell-ok').addEventListener('click', () => {
            this._timingSpell = this._spellInput.value.trim();
            localStorage.setItem('roundTimerSpell', this._timingSpell);
            pop.style.display = 'none';
        });
        pop.querySelector('.rt-spell-cancel').addEventListener('click', () => {
            pop.style.display = 'none';
        });
        pop.querySelector('.rt-spell-clear').addEventListener('click', () => {
            this._timingSpell = '';
            this._spellInput.value = '';
            localStorage.setItem('roundTimerSpell', '');
            pop.style.display = 'none';
        });
        this._spellInput.addEventListener('keydown', (e) => {
            if (e.key === 'Enter') pop.querySelector('.rt-spell-ok').click();
            if (e.key === 'Escape') pop.querySelector('.rt-spell-cancel').click();
        });
    }

    _showSpellPopup() {
        this._spellInput.value = this._timingSpell;
        this._spellPopup.style.display = 'block';
        // Center it
        const pop = this._spellPopup;
        pop.style.left = Math.max(10, (window.innerWidth - 300) / 2) + 'px';
        pop.style.top = Math.max(10, (window.innerHeight - 200) / 2) + 'px';
        setTimeout(() => this._spellInput.focus(), 50);
    }

    // ── Scale ──
    _setScale(s) {
        this._scale = s;
        const w = Math.round(180 * s), h = Math.round(200 * s);
        this._canvas.width = w;
        this._canvas.height = h;
        this._el.style.width = w + 'px';
        this._el.style.height = h + 'px';
        this._saveState();
    }

    // ── Cast to Time system ──
    _startTiming() {
        if (this._timingActive) { this._stopTiming(); return; }
        if (!this._timingSpell) { this._showSpellPopup(); return; }
        if (!this._sendCommand) return;
        this._timingActive = true;
        this._timingRoundsGot = 0;
        this._timingAlreadyCount = 0;
        this._sendSpell();
        // Spam every 500ms
        this._timingInterval = setInterval(() => this._sendSpell(), 500);
    }

    _stopTiming() {
        this._timingActive = false;
        if (this._timingInterval) { clearInterval(this._timingInterval); this._timingInterval = null; }
    }

    _sendSpell() {
        if (!this._sendCommand || !this._timingSpell) { this._stopTiming(); return; }
        this._sendCommand('inject', { text: this._timingSpell });
    }

    // Called when server says "You cast" / "You sing" / "You attempt to cast"
    // Only feeds round timer when Cast to Time is active
    onSpellCast(data) {
        if (this._timingActive) {
            // Only count if preceded by 2+ "You have already" — confirms we were spamming
            if (this._timingAlreadyCount >= 2) {
                this.onRoundTick({ round: this._roundNum + 1 });
                this._timingRoundsGot++;
                if (this._timingRoundsGot >= this._timingRoundsNeeded) {
                    this._stopTiming();
                }
            }
            this._timingAlreadyCount = 0;
        }
    }

    // Called on "You have already cast"
    onSpellBlocked() {
        this._spellUsed = true;
        if (this._timingActive) {
            this._timingAlreadyCount++;
        }
    }

    toggle(show) {
        if (show === undefined) show = !this._visible;
        this._visible = show;
        this._el.style.display = show ? '' : 'none';
        if (show) this._startLoop(); else this._stopLoop();
        this._saveState();
    }
    get isVisible() { return this._visible; }

    onRoundTick(data) {
        const now = performance.now();
        const prev = this._lastTickTs;
        this._roundNum = data.round || (this._roundNum + 1);
        this._inCombat = true;

        if (this._synced && prev > 0 && this._roundPeriod > 0) {
            const elapsed = now - prev;
            const cycles = Math.round(elapsed / this._roundPeriod);
            const predicted = prev + cycles * this._roundPeriod;
            const delta = Math.round(now - predicted);
            this._deltaText = (delta >= 0 ? '+' : '') + delta + 'ms';
            this._deltaFade = 1.0;

            // Lag spike detection — only during continuous combat (ticks within ~2 rounds)
            // If there's been a long gap (buff break, room change, etc), just resync
            const absDelta = Math.abs(delta);
            const continuous = elapsed < this._roundPeriod * 2.5;
            if (continuous && this._recentDeltas.length >= 5) {
                const avgDelta = this._recentDeltas.reduce((a, b) => a + b, 0) / this._recentDeltas.length;
                if (absDelta > Math.max(avgDelta * 4, 800)) {
                    this._deltaText += ' LAG';
                    this._spellUsed = false;
                    this._ripples.push({ born: now, color: '#ff4444' });
                    if (this._visible && !this._animId) this._startLoop();
                    return;
                }
            }
            // Track deltas for baseline (only from continuous ticks)
            if (continuous) {
                this._recentDeltas.push(absDelta);
                if (this._recentDeltas.length > this._maxDeltas) this._recentDeltas.shift();
            }
        }

        if (prev > 0) {
            const interval = now - prev;
            let single = interval;
            if (interval > 6000 && this._roundPeriod > 0) {
                const rounds = Math.round(interval / this._roundPeriod);
                if (rounds > 0) single = interval / rounds;
            }
            if (single > 2000 && single < 8000) {
                this._recentIntervals.push(single);
                if (this._recentIntervals.length > this._maxIntervals) this._recentIntervals.shift();
                let wS = 0, vS = 0;
                for (let i = 0; i < this._recentIntervals.length; i++) {
                    const w = 1 + i; wS += w; vS += this._recentIntervals[i] * w;
                }
                this._roundPeriod = vS / wS;
            }
        }

        this._lastTickTs = now;
        // Sync on first tick if we have a default period, otherwise after 2+ ticks
        if (this._roundPeriod > 0) this._synced = true;
        this._spellUsed = false;

        const orbit = (typeof getThemeOrbit === 'function') ? getThemeOrbit() : {};
        this._ripples.push({ born: now, color: orbit.ripple || '#6677cc' });

        if (this._visible && !this._animId) this._startLoop();
    }

    onCombatEnd() {
        this._inCombat = false;
        this._spellUsed = false;
    }

    _startLoop() {
        if (this._animId) return;
        const loop = () => { this._render(); this._animId = requestAnimationFrame(loop); };
        this._animId = requestAnimationFrame(loop);
    }
    _stopLoop() {
        if (this._animId) { cancelAnimationFrame(this._animId); this._animId = null; }
    }

    _hex(hex) {
        hex = hex.replace('#', '');
        if (hex.length === 3) hex = hex[0]+hex[0]+hex[1]+hex[1]+hex[2]+hex[2];
        return [parseInt(hex.substr(0,2),16), parseInt(hex.substr(2,2),16), parseInt(hex.substr(4,2),16)];
    }

    _outlineText(ctx, text, x, y, fillStyle) {
        ctx.save();
        ctx.fillStyle = fillStyle;
        ctx.strokeStyle = 'rgba(0,0,0,0.95)';
        ctx.lineWidth = 3.5;
        ctx.lineJoin = 'round';
        ctx.miterLimit = 2;
        ctx.strokeText(text, x, y);
        ctx.shadowColor = 'rgba(0,0,0,0.8)';
        ctx.shadowBlur = 6;
        ctx.shadowOffsetX = 0;
        ctx.shadowOffsetY = 1;
        ctx.fillText(text, x, y);
        ctx.restore();
    }

    _outlineGradText(ctx, text, x, y, gradFill) {
        ctx.save();
        ctx.fillStyle = gradFill;
        ctx.strokeStyle = 'rgba(0,0,0,0.95)';
        ctx.lineWidth = 4;
        ctx.lineJoin = 'round';
        ctx.miterLimit = 2;
        ctx.strokeText(text, x, y);
        ctx.shadowColor = 'rgba(0,0,0,0.8)';
        ctx.shadowBlur = 8;
        ctx.shadowOffsetY = 1;
        ctx.fillText(text, x, y);
        ctx.restore();
    }

    _render() {
        const ctx = this._ctx;
        const s = this._scale;
        const W = Math.round(180 * s), H = Math.round(200 * s);
        const cx = W / 2, cy = Math.round(90 * s);
        const R = Math.round(72 * s), arcW = Math.round(10 * s);
        const now = performance.now();
        const period = this._roundPeriod;
        ctx.clearRect(0, 0, W, H);

        const orbit = (typeof getThemeOrbit === 'function') ? getThemeOrbit() : {};
        const ballColor = orbit.ball || '#c0c8e0';
        const trailColor = orbit.trail || '#4455aa';
        const rippleColor = orbit.ripple || '#6677cc';
        const textColors = orbit.text || ['#8899cc', '#c0c8e0'];
        const ballRgb = this._hex(ballColor);
        const trailRgb = this._hex(trailColor);
        const rippleRgb = this._hex(rippleColor);
        const dimA = this._synced ? 1.0 : 0.3;
        const arcRgb = this._inCombat ? ballRgb : trailRgb;

        // ── Background circle ──
        ctx.beginPath();
        ctx.arc(cx, cy, R + 5 * s, 0, Math.PI * 2);
        ctx.fillStyle = 'rgba(0, 0, 0, 0.55)';
        ctx.fill();

        // ── Outer ring track ──
        ctx.beginPath();
        ctx.arc(cx, cy, R, 0, Math.PI * 2);
        ctx.strokeStyle = `rgba(${arcRgb.join(',')}, ${0.12 * dimA})`;
        ctx.lineWidth = arcW;
        ctx.stroke();

        // ── Tick marks ──
        const ticks = Math.max(2, Math.round(period / 1000));
        for (let i = 0; i < ticks; i++) {
            const a = -Math.PI / 2 + (i / ticks) * Math.PI * 2;
            const r1 = R - arcW / 2 - 1 * s, r2 = R + arcW / 2 + 1 * s;
            ctx.beginPath();
            ctx.moveTo(cx + Math.cos(a) * r1, cy + Math.sin(a) * r1);
            ctx.lineTo(cx + Math.cos(a) * r2, cy + Math.sin(a) * r2);
            ctx.strokeStyle = i === 0
                ? `rgba(255,255,255,${0.7 * dimA})`
                : `rgba(${arcRgb.join(',')},${0.25 * dimA})`;
            ctx.lineWidth = i === 0 ? 2.5 * s : 1 * s;
            ctx.stroke();
        }

        // ── Progress arc + ball + trail ──
        if (this._synced && this._lastTickTs > 0) {
            const elapsed = now - this._lastTickTs;
            const phase = (elapsed % period) / period;
            const startA = -Math.PI / 2;
            const endA = startA + phase * Math.PI * 2;

            // Safety gradient arc: green (safe) → yellow → red (danger near next round)
            const steps = 48;
            for (let i = 0; i < steps; i++) {
                const t0 = i / steps, t1 = (i + 1) / steps;
                if (t1 > phase) break;
                const a0 = startA + t0 * Math.PI * 2;
                const a1 = startA + t1 * Math.PI * 2 + 0.02; // tiny overlap
                // Green → yellow at 67%, yellow → red last 33%
                let r, g, b;
                if (t1 < 0.5) {
                    // green to yellow
                    const p = t1 / 0.5;
                    r = Math.round(40 + 200 * p); g = Math.round(220 - 40 * p); b = 40;
                } else if (t1 < 0.67) {
                    // yellow
                    const p = (t1 - 0.5) / 0.17;
                    r = Math.round(240 + 15 * p); g = Math.round(180 - 80 * p); b = 30;
                } else {
                    // red zone — brighter red closer to apex
                    const p = (t1 - 0.67) / 0.33;
                    r = Math.round(255); g = Math.round(100 - 80 * p); b = Math.round(30 - 20 * p);
                }
                const alpha = (0.5 + t1 * 0.45) * dimA;
                ctx.beginPath();
                ctx.arc(cx, cy, R, a0, a1);
                ctx.strokeStyle = `rgba(${r},${g},${b},${alpha})`;
                ctx.lineWidth = arcW;
                ctx.lineCap = 'butt';
                ctx.stroke();
            }

            // Leading glow in phase color
            const glowR = phase < 0.67 ? (phase < 0.5 ? 180 : 240) : 255;
            const glowG = phase < 0.67 ? (phase < 0.5 ? 220 : 140) : Math.round(60 * (1 - phase));
            const glowB = phase < 0.67 ? 40 : 10;
            ctx.save();
            ctx.shadowColor = `rgba(${glowR},${glowG},${glowB},${0.7 * dimA})`;
            ctx.shadowBlur = (14 + phase * 8) * s;
            ctx.beginPath();
            ctx.arc(cx, cy, R, endA - 0.12, endA);
            ctx.strokeStyle = `rgba(${glowR},${glowG},${glowB},${0.95 * dimA})`;
            ctx.lineWidth = arcW + 2 * s;
            ctx.lineCap = 'round';
            ctx.stroke();
            ctx.restore();

            const bx = cx + Math.cos(endA) * R;
            const by = cy + Math.sin(endA) * R;

            // ── Comet trail ──
            this._trail.push({ x: bx, y: by, born: now });
            if (this._trail.length > this._maxTrail) this._trail.shift();
            for (let i = 0; i < this._trail.length; i++) {
                const t = this._trail[i];
                const age = (now - t.born) / 420;
                if (age > 1) continue;
                const a = (1 - age) * (i / this._trail.length) * 0.65 * dimA;
                const sz = 4 * s * (1 - age * 0.5);
                ctx.beginPath();
                ctx.arc(t.x, t.y, sz, 0, Math.PI * 2);
                ctx.fillStyle = `rgba(${trailRgb.join(',')},${a})`;
                ctx.fill();
            }
            while (this._trail.length > 0 && (now - this._trail[0].born) > 500) this._trail.shift();

            // ── Ball — colored by phase safety ──
            ctx.save();
            ctx.shadowColor = `rgba(${glowR},${glowG},${glowB},${0.9 * dimA})`;
            ctx.shadowBlur = 12 * s;
            ctx.beginPath();
            ctx.arc(bx, by, 5.5 * s, 0, Math.PI * 2);
            ctx.fillStyle = `rgb(${glowR},${glowG},${glowB})`;
            ctx.fill();
            ctx.restore();

            // ── Elapsed text ──
            const elSec = ((elapsed % period) / 1000).toFixed(1);
            ctx.font = `${Math.round(11 * s)}px "Segoe UI", sans-serif`;
            ctx.textAlign = 'center';
            ctx.textBaseline = 'alphabetic';
            this._outlineText(ctx, `${elSec}s / ${(period / 1000).toFixed(1)}s`, cx, cy + 26 * s,
                getComputedStyle(document.documentElement).getPropertyValue('--text-dim').trim() || '#6666aa');
        }

        // ── Ripple shockwaves ──
        for (let i = this._ripples.length - 1; i >= 0; i--) {
            const rip = this._ripples[i];
            const age = (now - rip.born) / 1200;
            if (age > 1) { this._ripples.splice(i, 1); continue; }
            const rgb = this._hex(rip.color);
            const radius = (R + 8 * s + age * 40 * s);
            const alpha = (1 - age) * 0.6;
            ctx.save();
            ctx.beginPath();
            ctx.arc(cx, cy, radius, 0, Math.PI * 2);
            ctx.strokeStyle = `rgba(${rgb.join(',')},${alpha})`;
            ctx.lineWidth = 2.5 * s * (1 - age);
            ctx.shadowColor = `rgba(${rgb.join(',')},${alpha * 0.8})`;
            ctx.shadowBlur = 15 * s * (1 - age);
            ctx.stroke();
            ctx.restore();
        }

        // ── Center: "ROUND" with gradient pulse ──
        ctx.textBaseline = 'middle';
        ctx.textAlign = 'center';

        if (this._synced) {
            // "ROUND" text — flash bright green on each tick, fade to dull grey
            const tickAge = this._lastTickTs > 0 ? (now - this._lastTickTs) / 1000 : 99;
            const flashT = Math.max(0, 1 - tickAge / 1.5); // 1.5s fade
            const roundFontSize = Math.round(18 * s);
            ctx.font = `bold ${roundFontSize}px "Segoe UI", sans-serif`;

            if (flashT > 0) {
                // Bright green flash → dull grey
                const gr = Math.round(80 + 175 * flashT);
                const gg = Math.round(90 + 165 * flashT);
                const gb = Math.round(100 + 50 * flashT);
                const glowA = flashT * 0.8;
                ctx.save();
                ctx.shadowColor = `rgba(60,255,80,${glowA})`;
                ctx.shadowBlur = 16 * s * flashT;
                this._outlineText(ctx, 'ROUND', cx, cy - 2 * s, `rgb(${gr},${gg},${gb})`);
                ctx.restore();
            } else {
                this._outlineText(ctx, 'ROUND', cx, cy - 2 * s, 'rgba(140,140,160,0.7)');
            }

            // Crossed swords when in combat, flanking "ROUND"
            if (this._inCombat) {
                const rw = ctx.measureText('ROUND').width;
                const swordSize = Math.round(13 * s);
                ctx.font = `${swordSize}px "Segoe UI", sans-serif`;
                const swordX = rw / 2 + 8 * s;
                this._outlineText(ctx, '\u2694', cx - swordX, cy - 2 * s, `rgba(${ballRgb.join(',')},0.8)`);
                this._outlineText(ctx, '\u2694', cx + swordX, cy - 2 * s, `rgba(${ballRgb.join(',')},0.8)`);
            }

            if (this._timingActive) {
                ctx.font = `bold ${Math.round(11 * s)}px "Segoe UI", sans-serif`;
                const blink = Math.sin(now / 200) > 0 ? 1 : 0.4;
                this._outlineText(ctx, 'TIMING...', cx, cy + 16 * s,
                    `rgba(255,180,60,${blink})`);
            } else if (!this._inCombat) {
                ctx.font = `${Math.round(8 * s)}px "Segoe UI", sans-serif`;
                this._outlineText(ctx, 'FREEWHEEL', cx, cy - 16 * s,
                    `rgba(${trailRgb.join(',')},0.5)`);
            }

            if (this._spellUsed && this._inCombat) {
                ctx.font = `bold ${Math.round(11 * s)}px "Segoe UI", sans-serif`;
                this._outlineText(ctx, 'ALREADY CAST', cx, cy + 38 * s, '#ff6655');
            }
        } else {
            ctx.font = `bold ${Math.round(13 * s)}px "Segoe UI", sans-serif`;
            this._outlineText(ctx, 'WAITING', cx, cy - 4 * s,
                getComputedStyle(document.documentElement).getPropertyValue('--text-dim').trim() || '#6666aa');
            ctx.font = `bold ${Math.round(11 * s)}px "Segoe UI", sans-serif`;
            this._outlineText(ctx, 'FOR SYNC', cx, cy + 12 * s,
                getComputedStyle(document.documentElement).getPropertyValue('--text-dim').trim() || '#6666aa');
        }

        // ── Delta readout inside circle, just under the top (round hit point) ──
        if (this._deltaFade > 0 && this._deltaText) {
            const isLate = this._deltaText.startsWith('+');
            const col = isLate ? 'rgba(255,180,60,' + this._deltaFade + ')' : 'rgba(60,220,120,' + this._deltaFade + ')';
            ctx.font = `bold ${Math.round(9 * s)}px "Segoe UI", sans-serif`;
            ctx.textAlign = 'center';
            ctx.textBaseline = 'top';
            this._outlineText(ctx, this._deltaText, cx, cy - R + 6 * s, col);
            this._deltaFade = Math.max(0, this._deltaFade - 0.005);
        }
    }

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

    _saveState() {
        try {
            const r = this._el.getBoundingClientRect();
            localStorage.setItem('roundTimerState', JSON.stringify({
                visible: this._visible, left: r.left, top: r.top, scale: this._scale
            }));
        } catch {}
    }

    _restoreState() {
        try {
            const s = JSON.parse(localStorage.getItem('roundTimerState') || '{}');
            if (s.visible) { this._visible = true; this._el.style.display = ''; this._startLoop(); }
            if (s.left != null) {
                this._el.style.left = s.left + 'px'; this._el.style.top = s.top + 'px';
                this._el.style.right = 'auto'; this._el.style.bottom = 'auto';
            }
            if (s.scale && s.scale !== 1) this._setScale(s.scale);
        } catch {}
    }
}
