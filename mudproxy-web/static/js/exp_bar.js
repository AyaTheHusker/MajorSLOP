// ── EXP Bar — WoW-style 20-segment experience bar at bottom of screen ──

class ExpBar {
    constructor() {
        this._visible = true;
        this._data = null; // {exp, level, needed, total_for_level, percent}
        this._build();
        this._restoreState();
    }

    _build() {
        const bar = document.createElement('div');
        bar.id = 'exp-bar';
        bar.className = 'exp-bar';

        // 20 segments
        this._segments = [];
        for (let i = 0; i < 20; i++) {
            const seg = document.createElement('div');
            seg.className = 'exp-seg';
            bar.appendChild(seg);
            this._segments.push(seg);
        }

        // Info overlay
        const info = document.createElement('div');
        info.className = 'exp-info';
        info.textContent = 'EXP: --';
        bar.appendChild(info);
        this._info = info;

        document.body.appendChild(bar);
        this._el = bar;

        // Hover to show detailed info
        bar.addEventListener('mouseenter', () => this._showDetail(true));
        bar.addEventListener('mouseleave', () => this._showDetail(false));
    }

    update(data) {
        this._data = data;
        if (!this._visible) return;
        this._render();
    }

    _render() {
        if (!this._data) return;
        const { exp, level, needed, total_for_level, percent } = this._data;

        // percent is how far through the level (0-100+)
        const pct = Math.min(percent, 100);
        const filledSegs = Math.floor(pct / 5);       // each seg = 5%
        const partialFill = (pct % 5) / 5;            // partial fill of next seg

        for (let i = 0; i < 20; i++) {
            const seg = this._segments[i];
            seg.classList.remove('exp-seg-full', 'exp-seg-partial', 'exp-seg-overflow');

            if (percent > 100) {
                // Can level up — all segments glow gold
                seg.classList.add('exp-seg-overflow');
            } else if (i < filledSegs) {
                seg.classList.add('exp-seg-full');
            } else if (i === filledSegs && partialFill > 0) {
                seg.classList.add('exp-seg-partial');
                seg.style.setProperty('--partial', `${partialFill * 100}%`);
            } else {
                seg.style.removeProperty('--partial');
            }
        }

        // Info text
        if (percent > 100) {
            this._info.textContent = `Level ${level} \u2014 READY TO TRAIN! (${percent}%)`;
        } else {
            const neededFmt = needed.toLocaleString();
            this._info.textContent = `Level ${level} \u2014 ${percent}% \u2014 ${neededFmt} to next`;
        }
    }

    _showDetail(show) {
        if (!this._data) return;
        if (show) {
            this._info.classList.add('exp-info-visible');
        } else {
            this._info.classList.remove('exp-info-visible');
        }
    }

    toggle(show) {
        if (show === undefined) show = !this._visible;
        this._visible = show;
        this._el.style.display = show ? '' : 'none';
        if (show && this._data) this._render();
        this._saveState();
    }

    get isVisible() { return this._visible; }

    flashGain(amount) {
        // Optimistically bump the bar if we have data
        if (this._data) {
            this._data.exp += amount;
            if (this._data.needed > 0) {
                this._data.needed = Math.max(0, this._data.needed - amount);
            }
            if (this._data.total_for_level > 0) {
                this._data.percent = Math.round((this._data.exp / (this._data.exp + this._data.needed)) * 100);
                if (this._data.needed === 0) this._data.percent = Math.max(this._data.percent, 100);
            }
            this._render();
        }

        // Floating +XP text above the bar
        const floater = document.createElement('div');
        floater.className = 'exp-float-xp';
        floater.textContent = `+${amount.toLocaleString()} EXP`;
        document.body.appendChild(floater);
        // Trigger animation
        requestAnimationFrame(() => floater.classList.add('exp-float-rise'));
        setTimeout(() => floater.remove(), 4000);
    }

    _saveState() {
        try {
            const saved = JSON.parse(localStorage.getItem('expBarState') || '{}');
            saved.visible = this._visible;
            localStorage.setItem('expBarState', JSON.stringify(saved));
        } catch {}
    }

    _restoreState() {
        try {
            const saved = JSON.parse(localStorage.getItem('expBarState') || '{}');
            if (saved.visible === false) {
                this._visible = false;
                this._el.style.display = 'none';
            }
        } catch {}
    }
}
