// ── Exp Meter — Synced with MegaMUD's Player Statistics ──
// Reads exp/hr, duration, exp made directly from MegaMUD via DLL.
// Reset button triggers MegaMUD's own reset (BM_CLICK on ctrl 1721).
// Also tracks kills locally and renders a rate graph.

class ExpMeter {
    constructor() {
        this._visible = false;
        this._sessionStart = Date.now();
        this._totalXP = 0;
        this._kills = 0;
        this._xpHistory = [];       // [{ts: Date.now(), amount}]
        this._rateSnapshots = [];   // [{ts, rate}] sampled every 30s
        this._lastSnapshotTs = 0;
        this._updateTimer = null;
        // MegaMUD sync data
        this._megaExpRate = '';
        this._megaDuration = '';
        this._megaExpMade = '';
        this._megaExpNeeded = '';
        this._megaLevelIn = '';
        this._build();
        this._restoreState();
    }

    _build() {
        const el = document.createElement('div');
        el.id = 'exp-meter';
        el.className = 'exp-meter-panel';
        el.style.display = 'none';

        el.innerHTML = `
            <div class="exp-meter-header">
                <span class="exp-meter-title">EXP Meter</span>
                <span class="exp-meter-btn exp-meter-reset" title="Reset MegaMUD exp meter">&#x21BB;</span>
                <span class="exp-meter-btn exp-meter-close" title="Close">&times;</span>
            </div>
            <div class="exp-meter-body">
                <div class="exp-meter-stats">
                    <div class="exp-meter-row">
                        <span class="exp-meter-label">Exp Rate</span>
                        <span class="exp-meter-value em-highlight" id="em-rate">—</span>
                    </div>
                    <div class="exp-meter-row">
                        <span class="exp-meter-label">Exp Made</span>
                        <span class="exp-meter-value" id="em-total-xp">—</span>
                    </div>
                    <div class="exp-meter-row">
                        <span class="exp-meter-label">Duration</span>
                        <span class="exp-meter-value" id="em-time">—</span>
                    </div>
                    <div class="exp-meter-row">
                        <span class="exp-meter-label">Level In</span>
                        <span class="exp-meter-value" id="em-level-in">—</span>
                    </div>
                    <div class="exp-meter-row">
                        <span class="exp-meter-label">Kills</span>
                        <span class="exp-meter-value" id="em-kills">0</span>
                    </div>
                    <div class="exp-meter-row">
                        <span class="exp-meter-label">Avg/Kill</span>
                        <span class="exp-meter-value" id="em-avg">—</span>
                    </div>
                </div>
                <div class="exp-meter-graph-wrap">
                    <canvas id="em-graph" width="260" height="90"></canvas>
                </div>
            </div>
        `;

        document.body.appendChild(el);
        this._el = el;
        this._graphCanvas = el.querySelector('#em-graph');
        this._graphCtx = this._graphCanvas.getContext('2d');

        // Reset = call MegaMUD's reset via API + reset local
        el.querySelector('.exp-meter-reset').addEventListener('click', () => this.reset());
        el.querySelector('.exp-meter-close').addEventListener('click', () => this.toggle(false));

        this._makeDraggable();
    }

    // ── Public API ──

    toggle(show) {
        if (show === undefined) show = !this._visible;
        this._visible = show;
        this._el.style.display = show ? '' : 'none';
        if (show) {
            this._startUpdates();
            this._renderStats();
            this._renderGraph();
        } else {
            this._stopUpdates();
        }
        this._saveState();
    }

    get isVisible() { return this._visible; }

    addXP(amount) {
        this._totalXP += amount;
        this._xpHistory.push({ ts: Date.now(), amount });
        const now = Date.now();
        if (now - this._lastSnapshotTs > 30000) {
            this._rateSnapshots.push({ ts: now, rate: this._calcRate() });
            if (this._rateSnapshots.length > 120) this._rateSnapshots.shift();
            this._lastSnapshotTs = now;
        }
        if (this._visible) {
            this._renderStats();
            this._renderGraph();
        }
    }

    addKill() {
        this._kills++;
        if (this._visible) this._renderStats();
    }

    reset() {
        // Reset MegaMUD's exp meter via BM_CLICK
        fetch('/api/mem/exp-reset', { method: 'POST' });
        // Reset local tracking
        this._sessionStart = Date.now();
        this._totalXP = 0;
        this._kills = 0;
        this._xpHistory = [];
        this._rateSnapshots = [];
        this._lastSnapshotTs = 0;
        if (this._visible) {
            this._renderStats();
            this._renderGraph();
        }
    }

    // Called from app.js when mem_state arrives with exp_meter data
    updateFromMega(data) {
        if (!data) return;
        this._megaExpRate = data.exp_rate || '';
        this._megaDuration = data.duration || '';
        this._megaExpMade = data.exp_made || '';
        this._megaExpNeeded = data.exp_needed || '';
        this._megaLevelIn = data.level_in || '';
        if (this._visible) this._renderStats();
    }

    // ── Rate Calculation (local fallback) ──

    _calcRate() {
        const elapsed = (Date.now() - this._sessionStart) / 1000;
        if (elapsed < 1) return 0;
        return Math.round(this._totalXP / elapsed * 3600);
    }

    _calcRecentRate(windowMs = 300000) {
        const now = Date.now();
        const cutoff = now - windowMs;
        let recent = 0;
        for (let i = this._xpHistory.length - 1; i >= 0; i--) {
            if (this._xpHistory[i].ts < cutoff) break;
            recent += this._xpHistory[i].amount;
        }
        const actualWindow = Math.min(windowMs, now - this._sessionStart);
        if (actualWindow < 1000) return 0;
        return Math.round(recent / actualWindow * 3600000);
    }

    // ── Stats Rendering ──

    _renderStats() {
        const rate = this._el.querySelector('#em-rate');
        const total = this._el.querySelector('#em-total-xp');
        const time = this._el.querySelector('#em-time');
        const levelIn = this._el.querySelector('#em-level-in');
        const kills = this._el.querySelector('#em-kills');
        const avg = this._el.querySelector('#em-avg');

        // Use MegaMUD data if available, otherwise local
        rate.textContent = this._megaExpRate || `${this._calcRate().toLocaleString()}/hr`;
        total.textContent = this._megaExpMade || this._totalXP.toLocaleString();
        time.textContent = this._megaDuration || this._fmtElapsed();
        levelIn.textContent = this._megaLevelIn || '—';
        kills.textContent = this._kills.toLocaleString();
        avg.textContent = this._kills > 0
            ? Math.round(this._totalXP / this._kills).toLocaleString()
            : '—';
    }

    _fmtElapsed() {
        const elapsed = Math.floor((Date.now() - this._sessionStart) / 1000);
        const hrs = Math.floor(elapsed / 3600);
        const mins = Math.floor((elapsed % 3600) / 60);
        const secs = elapsed % 60;
        return hrs > 0
            ? `${hrs}:${mins.toString().padStart(2, '0')}:${secs.toString().padStart(2, '0')}`
            : `${mins}:${secs.toString().padStart(2, '0')}`;
    }

    // ── Graph Rendering ──

    _renderGraph() {
        const ctx = this._graphCtx;
        const W = 260, H = 90;
        ctx.clearRect(0, 0, W, H);

        const cs = getComputedStyle(document.documentElement);
        const accent = cs.getPropertyValue('--accent').trim() || '#4444cc';
        const dim = cs.getPropertyValue('--text-dim').trim() || '#6666aa';
        const accentRgb = this._hexToRgb(accent);

        const bucketMs = 30000;
        const now = Date.now();
        const startTs = this._sessionStart;
        const elapsed = now - startTs;
        if (elapsed < 5000 || this._xpHistory.length < 1) {
            ctx.font = '11px "Segoe UI", sans-serif';
            ctx.textAlign = 'center';
            ctx.fillStyle = dim;
            ctx.fillText('Collecting data...', W / 2, H / 2);
            return;
        }

        const maxBuckets = 40;
        const totalBuckets = Math.min(maxBuckets, Math.max(2, Math.ceil(elapsed / bucketMs)));
        const actualBucketMs = elapsed / totalBuckets;
        const buckets = new Array(totalBuckets).fill(0);

        for (const entry of this._xpHistory) {
            const idx = Math.min(totalBuckets - 1,
                Math.floor((entry.ts - startTs) / actualBucketMs));
            buckets[idx] += entry.amount;
        }

        const rates = buckets.map(xp => Math.round(xp / (actualBucketMs / 3600000)));
        const maxRate = Math.max(1, ...rates);

        ctx.strokeStyle = `rgba(${accentRgb.r}, ${accentRgb.g}, ${accentRgb.b}, 0.1)`;
        ctx.lineWidth = 0.5;
        for (let i = 1; i < 4; i++) {
            const y = H - (H * i / 4);
            ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(W, y); ctx.stroke();
        }

        const grad = ctx.createLinearGradient(0, 0, 0, H);
        grad.addColorStop(0, `rgba(${accentRgb.r}, ${accentRgb.g}, ${accentRgb.b}, 0.35)`);
        grad.addColorStop(0.7, `rgba(${accentRgb.r}, ${accentRgb.g}, ${accentRgb.b}, 0.08)`);
        grad.addColorStop(1, `rgba(${accentRgb.r}, ${accentRgb.g}, ${accentRgb.b}, 0.0)`);

        const padding = 4;
        const graphW = W - padding * 2;
        const graphH = H - 14;

        const points = rates.map((r, i) => ({
            x: padding + (i / (totalBuckets - 1 || 1)) * graphW,
            y: 4 + graphH - (r / maxRate) * graphH,
        }));

        ctx.beginPath();
        ctx.moveTo(points[0].x, 4 + graphH);
        for (const p of points) ctx.lineTo(p.x, p.y);
        ctx.lineTo(points[points.length - 1].x, 4 + graphH);
        ctx.closePath();
        ctx.fillStyle = grad;
        ctx.fill();

        ctx.beginPath();
        ctx.moveTo(points[0].x, points[0].y);
        for (let i = 1; i < points.length; i++) {
            const prev = points[i - 1];
            const cur = points[i];
            const cpx = (prev.x + cur.x) / 2;
            ctx.quadraticCurveTo(prev.x + (cpx - prev.x) * 0.8, prev.y, cpx, (prev.y + cur.y) / 2);
            ctx.quadraticCurveTo(cur.x - (cur.x - cpx) * 0.8, cur.y, cur.x, cur.y);
        }
        ctx.strokeStyle = accent;
        ctx.lineWidth = 2;
        ctx.shadowColor = `rgba(${accentRgb.r}, ${accentRgb.g}, ${accentRgb.b}, 0.5)`;
        ctx.shadowBlur = 6;
        ctx.stroke();
        ctx.shadowBlur = 0;

        for (let i = 0; i < points.length; i++) {
            if (rates[i] > 0) {
                ctx.beginPath();
                ctx.arc(points[i].x, points[i].y, 2, 0, Math.PI * 2);
                ctx.fillStyle = accent;
                ctx.fill();
            }
        }

        if (points.length > 0) {
            const last = points[points.length - 1];
            ctx.beginPath();
            ctx.arc(last.x, last.y, 4, 0, Math.PI * 2);
            ctx.fillStyle = '#fff';
            ctx.shadowColor = `rgba(${accentRgb.r}, ${accentRgb.g}, ${accentRgb.b}, 0.8)`;
            ctx.shadowBlur = 8;
            ctx.fill();
            ctx.shadowBlur = 0;
        }

        ctx.font = '9px "Segoe UI", sans-serif';
        ctx.textAlign = 'right';
        ctx.fillStyle = dim;
        ctx.fillText(this._fmtRate(maxRate), W - 2, 12);
        ctx.fillText('0', W - 2, 4 + graphH);

        ctx.font = '9px "Segoe UI", sans-serif';
        ctx.textAlign = 'center';
        ctx.fillStyle = dim;
        const elapsedMin = Math.round(elapsed / 60000);
        ctx.fillText(elapsedMin > 0 ? `${elapsedMin}m ago → now` : 'just started', W / 2, H - 1);
    }

    _fmtRate(rate) {
        if (rate >= 1000000) return (rate / 1000000).toFixed(1) + 'M';
        if (rate >= 1000) return (rate / 1000).toFixed(0) + 'K';
        return rate.toString();
    }

    _hexToRgb(hex) {
        hex = hex.replace('#', '');
        if (hex.length === 3) hex = hex[0]+hex[0]+hex[1]+hex[1]+hex[2]+hex[2];
        return {
            r: parseInt(hex.substr(0,2), 16),
            g: parseInt(hex.substr(2,2), 16),
            b: parseInt(hex.substr(4,2), 16)
        };
    }

    // ── Auto-update timer ──

    _startUpdates() {
        this._stopUpdates();
        this._updateTimer = setInterval(() => {
            this._renderStats();
            if (Date.now() - (this._lastGraphRender || 0) > 30000) {
                this._renderGraph();
                this._lastGraphRender = Date.now();
            }
        }, 1000);
    }

    _stopUpdates() {
        if (this._updateTimer) {
            clearInterval(this._updateTimer);
            this._updateTimer = null;
        }
    }

    // ── Draggable ──

    _makeDraggable() {
        let dragging = false, offsetX = 0, offsetY = 0;
        const header = this._el.querySelector('.exp-meter-header');
        header.addEventListener('mousedown', (e) => {
            if (e.target.classList.contains('exp-meter-btn')) return;
            dragging = true;
            const rect = this._el.getBoundingClientRect();
            offsetX = e.clientX - rect.left;
            offsetY = e.clientY - rect.top;
            e.preventDefault();
        });
        document.addEventListener('mousemove', (e) => {
            if (!dragging) return;
            let newLeft = e.clientX - offsetX;
            let newTop = e.clientY - offsetY;
            const rect = this._el.getBoundingClientRect();
            newTop = Math.max(0, Math.min(newTop, window.innerHeight - 32));
            newLeft = Math.max(-rect.width + 80, Math.min(newLeft, window.innerWidth - 80));
            this._el.style.left = newLeft + 'px';
            this._el.style.top = newTop + 'px';
            this._el.style.right = 'auto';
            this._el.style.bottom = 'auto';
        });
        document.addEventListener('mouseup', () => {
            if (dragging) {
                dragging = false;
                this._saveState();
            }
        });
    }

    // ── Persistence ──

    _saveState() {
        try {
            const rect = this._el.getBoundingClientRect();
            localStorage.setItem('expMeterState', JSON.stringify({
                visible: this._visible,
                left: rect.left,
                top: rect.top,
            }));
        } catch {}
    }

    _restoreState() {
        try {
            const saved = JSON.parse(localStorage.getItem('expMeterState') || '{}');
            if (saved.visible) {
                this._visible = true;
                this._el.style.display = '';
                this._startUpdates();
            }
            if (saved.left != null) {
                this._el.style.left = saved.left + 'px';
                this._el.style.top = saved.top + 'px';
                this._el.style.right = 'auto';
                this._el.style.bottom = 'auto';
            }
        } catch {}
    }
}
