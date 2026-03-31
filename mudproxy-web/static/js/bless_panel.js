// ── Bless Panel ──
// Floating, draggable panel showing 10 bless slots read from MegaMUD memory,
// each with expandable condition rack for smart recast logic.

class BlessPanel {
    constructor(sendFn) {
        this._send = sendFn;
        this._visible = false;
        this._locked = false;
        this._el = null;
        this._data = null;          // bless_slots from mem_reader
        this._spellCache = {};      // short -> spell data from API
        this._slotExpanded = {};    // slot# -> bool
        this._slotOverrides = {};   // slot# -> {hp_min, hp_max, mp_min, conditions, mode}
        this._build();
        this._initDrag();
        this._restoreState();
        this._loadOverrides();
    }

    get isOpen() { return this._visible; }

    _build() {
        const panel = document.createElement('div');
        panel.id = 'bless-panel';
        panel.className = 'bless-panel';
        panel.style.display = 'none';

        // Header
        const header = document.createElement('div');
        header.className = 'bless-header';

        const title = document.createElement('span');
        title.className = 'bless-title';
        title.textContent = 'Bless Slots';

        const controls = document.createElement('div');
        controls.className = 'bless-controls';

        const refreshBtn = document.createElement('span');
        refreshBtn.className = 'bless-btn';
        refreshBtn.textContent = '\u21BB';
        refreshBtn.title = 'Refresh bless data';
        refreshBtn.addEventListener('click', (e) => {
            e.stopPropagation();
            this._send('get_state', {});
        });

        const lockBtn = document.createElement('span');
        lockBtn.className = 'bless-btn';
        lockBtn.textContent = '\u{1F513}';
        lockBtn.title = 'Lock/unlock position';
        lockBtn.addEventListener('click', (e) => {
            e.stopPropagation();
            this._locked = !this._locked;
            lockBtn.textContent = this._locked ? '\u{1F512}' : '\u{1F513}';
            this._saveState();
        });
        this._lockBtn = lockBtn;

        const closeBtn = document.createElement('span');
        closeBtn.className = 'bless-btn';
        closeBtn.textContent = '\u2715';
        closeBtn.title = 'Close';
        closeBtn.addEventListener('click', (e) => {
            e.stopPropagation();
            this.toggle(false);
        });

        controls.appendChild(refreshBtn);
        controls.appendChild(lockBtn);
        controls.appendChild(closeBtn);
        header.appendChild(title);
        header.appendChild(controls);
        panel.appendChild(header);

        // Settings bar (auto_bless, bless_resting, bless_combat, mana%)
        const settingsBar = document.createElement('div');
        settingsBar.className = 'bless-settings-bar';
        settingsBar.id = 'bless-settings-bar';
        panel.appendChild(settingsBar);

        // Scrollable slot container
        const body = document.createElement('div');
        body.className = 'bless-body';
        body.id = 'bless-body';
        panel.appendChild(body);

        document.body.appendChild(panel);
        this._el = panel;
    }

    toggle(show) {
        if (show === undefined) show = !this._visible;
        this._visible = show;
        this._el.style.display = show ? 'flex' : 'none';
        if (show) this._render();
        this._saveState();
    }

    update(data) {
        this._data = data;
        // Resolve spell names for any new slots
        if (data && data.self_slots) {
            for (const slot of data.self_slots) {
                if (slot.command && !this._spellCache[slot.command]) {
                    this._fetchSpell(slot.command);
                }
            }
        }
        if (data && data.party_slots) {
            for (const slot of data.party_slots) {
                if (slot.command && !this._spellCache[slot.command]) {
                    this._fetchSpell(slot.command);
                }
            }
        }
        if (this._visible) this._render();
    }

    async _fetchSpell(short) {
        if (!short || this._spellCache[short]) return;
        this._spellCache[short] = { _loading: true };
        try {
            const resp = await fetch(`/api/spells/lookup/${encodeURIComponent(short)}`);
            if (resp.ok) {
                this._spellCache[short] = await resp.json();
            } else {
                this._spellCache[short] = { Name: short, _notFound: true };
            }
        } catch {
            this._spellCache[short] = { Name: short, _error: true };
        }
        if (this._visible) this._render();
    }

    _render() {
        if (!this._data) return;

        // Settings bar
        const settingsBar = document.getElementById('bless-settings-bar');
        settingsBar.innerHTML = '';
        const flags = [
            { label: 'Auto', value: this._data.auto_bless, cls: 'bless-flag' },
            { label: 'Rest', value: this._data.bless_resting, cls: 'bless-flag' },
            { label: 'Combat', value: this._data.bless_combat, cls: 'bless-flag' },
        ];
        for (const f of flags) {
            const el = document.createElement('span');
            el.className = f.cls + (f.value ? ' active' : '');
            el.textContent = f.label;
            settingsBar.appendChild(el);
        }
        const manaPct = document.createElement('span');
        manaPct.className = 'bless-mana-pct';
        manaPct.textContent = `Mana: ${this._data.mana_bless_pct}%`;
        settingsBar.appendChild(manaPct);

        // Slot list
        const body = document.getElementById('bless-body');
        body.innerHTML = '';

        // Self bless slots
        const selfLabel = document.createElement('div');
        selfLabel.className = 'bless-section-label';
        selfLabel.textContent = 'Self Bless';
        body.appendChild(selfLabel);

        for (const slot of this._data.self_slots) {
            body.appendChild(this._renderSlot(slot, 'self'));
        }

        // Party bless slots
        const partyLabel = document.createElement('div');
        partyLabel.className = 'bless-section-label';
        partyLabel.textContent = 'Party Bless';
        body.appendChild(partyLabel);

        for (const slot of this._data.party_slots) {
            body.appendChild(this._renderSlot(slot, 'party'));
        }
    }

    _renderSlot(slot, type) {
        const key = `${type}_${slot.slot}`;
        const spell = slot.command ? this._spellCache[slot.command] : null;
        const overrides = this._slotOverrides[key] || {};
        const expanded = this._slotExpanded[key] || false;

        const wrapper = document.createElement('div');
        wrapper.className = 'bless-slot' + (slot.command ? '' : ' empty');

        // Slot header (always visible) — click to expand/collapse
        const header = document.createElement('div');
        header.className = 'bless-slot-header';
        header.addEventListener('click', () => {
            this._slotExpanded[key] = !this._slotExpanded[key];
            this._render();
        });

        // Expand arrow
        const arrow = document.createElement('span');
        arrow.className = 'bless-expand-arrow';
        arrow.textContent = expanded ? '\u25BC' : '\u25B6';
        header.appendChild(arrow);

        // Slot number
        const num = document.createElement('span');
        num.className = 'bless-slot-num';
        num.textContent = `#${slot.slot}`;
        header.appendChild(num);

        // Spell name + command
        const nameEl = document.createElement('span');
        nameEl.className = 'bless-slot-name';
        if (slot.command) {
            const fullName = spell && spell.Name ? spell.Name : '...';
            nameEl.textContent = `${fullName}`;
            const shortEl = document.createElement('span');
            shortEl.className = 'bless-slot-short';
            shortEl.textContent = ` (${slot.command})`;
            nameEl.appendChild(shortEl);
        } else {
            nameEl.textContent = '(empty)';
            nameEl.classList.add('dim');
        }
        header.appendChild(nameEl);

        // Right side info
        const info = document.createElement('span');
        info.className = 'bless-slot-info';
        if (spell && spell.Dur) {
            info.textContent = `${spell.ManaCost}mp \u00B7 ${spell.Dur}t`;
            info.title = `Mana: ${spell.ManaCost}, Duration: ${spell.Dur} ticks`;
        }
        if (type === 'party' && slot.timeout_secs) {
            info.textContent += ` \u00B7 ${slot.timeout_secs}s`;
        }
        // Override indicator
        if (overrides.mode && overrides.mode !== 'default') {
            const badge = document.createElement('span');
            badge.className = 'bless-override-badge';
            badge.textContent = overrides.mode === 'ignore' ? 'IGN' :
                               overrides.mode === 'conditional' ? 'CND' : '';
            if (badge.textContent) info.appendChild(badge);
        }
        header.appendChild(info);

        wrapper.appendChild(header);

        // Expandable condition rack
        if (expanded && slot.command) {
            const rack = this._buildConditionRack(key, slot, spell, overrides);
            wrapper.appendChild(rack);
        }

        return wrapper;
    }

    _buildConditionRack(key, slot, spell, overrides) {
        const rack = document.createElement('div');
        rack.className = 'bless-condition-rack';

        // Mode selector
        const modeRow = document.createElement('div');
        modeRow.className = 'bless-cond-row';
        const modeLabel = document.createElement('span');
        modeLabel.className = 'bless-cond-label';
        modeLabel.textContent = 'Mode:';
        modeRow.appendChild(modeLabel);

        const modes = [
            { value: 'default', label: 'Default', title: 'Use MegaMUD\'s built-in bless logic' },
            { value: 'smart', label: 'Smart', title: 'Proxy tracks duration and recasts proactively' },
            { value: 'conditional', label: 'Conditional', title: 'Only cast when conditions are met' },
            { value: 'ignore', label: 'Ignore', title: 'Never cast this spell' },
        ];
        for (const m of modes) {
            const btn = document.createElement('button');
            btn.className = 'bless-mode-btn' + ((overrides.mode || 'default') === m.value ? ' active' : '');
            btn.textContent = m.label;
            btn.title = m.title;
            btn.addEventListener('click', (e) => {
                e.stopPropagation();
                this._setOverride(key, 'mode', m.value);
            });
            modeRow.appendChild(btn);
        }
        rack.appendChild(modeRow);

        // Only show condition controls for conditional/smart modes
        const mode = overrides.mode || 'default';
        if (mode === 'conditional' || mode === 'smart') {
            // HP threshold
            rack.appendChild(this._buildSlider(key, 'hp_min', 'Cast if HP below', '%',
                overrides.hp_min ?? 100, 0, 100));

            // MP threshold
            rack.appendChild(this._buildSlider(key, 'mp_min', 'Min MP to cast', '%',
                overrides.mp_min ?? 0, 0, 100));

            // Status conditions
            const condRow = document.createElement('div');
            condRow.className = 'bless-cond-row';
            const condLabel = document.createElement('span');
            condLabel.className = 'bless-cond-label';
            condLabel.textContent = 'Only if:';
            condRow.appendChild(condLabel);

            const conditions = [
                { key: 'poisoned', label: 'Poisoned' },
                { key: 'diseased', label: 'Diseased' },
                { key: 'blinded', label: 'Blinded' },
                { key: 'held', label: 'Held' },
                { key: 'confused', label: 'Confused' },
                { key: 'in_combat', label: 'In Combat' },
                { key: 'resting', label: 'Resting' },
            ];
            const condList = overrides.conditions || [];
            for (const c of conditions) {
                const pill = document.createElement('span');
                pill.className = 'bless-cond-pill' + (condList.includes(c.key) ? ' active' : '');
                pill.textContent = c.label;
                pill.addEventListener('click', (e) => {
                    e.stopPropagation();
                    const cur = this._slotOverrides[key]?.conditions || [];
                    if (cur.includes(c.key)) {
                        this._setOverride(key, 'conditions', cur.filter(x => x !== c.key));
                    } else {
                        this._setOverride(key, 'conditions', [...cur, c.key]);
                    }
                });
                condRow.appendChild(pill);
            }
            rack.appendChild(condRow);

            // Priority (recast urgency)
            rack.appendChild(this._buildSlider(key, 'priority', 'Priority', '',
                overrides.priority ?? 5, 1, 10));

            // Recast buffer (seconds before expiry to recast)
            if (mode === 'smart' && spell && spell.Dur) {
                rack.appendChild(this._buildSlider(key, 'recast_buffer', 'Recast before expiry', 's',
                    overrides.recast_buffer ?? 10, 0, 60));
            }
        }

        return rack;
    }

    _buildSlider(key, field, label, unit, value, min, max) {
        const row = document.createElement('div');
        row.className = 'bless-cond-row';

        const lbl = document.createElement('span');
        lbl.className = 'bless-cond-label';
        lbl.textContent = `${label}:`;
        row.appendChild(lbl);

        const slider = document.createElement('input');
        slider.type = 'range';
        slider.className = 'bless-slider';
        slider.min = min;
        slider.max = max;
        slider.value = value;

        const valSpan = document.createElement('span');
        valSpan.className = 'bless-cond-value';
        valSpan.textContent = `${value}${unit}`;

        slider.addEventListener('input', () => {
            valSpan.textContent = `${slider.value}${unit}`;
        });
        slider.addEventListener('change', () => {
            this._setOverride(key, field, parseInt(slider.value));
        });

        row.appendChild(slider);
        row.appendChild(valSpan);
        return row;
    }

    _setOverride(key, field, value) {
        if (!this._slotOverrides[key]) {
            this._slotOverrides[key] = {};
        }
        this._slotOverrides[key][field] = value;
        this._saveOverrides();
        this._render();
    }

    _saveOverrides() {
        try {
            localStorage.setItem('blessOverrides', JSON.stringify(this._slotOverrides));
        } catch {}
    }

    _loadOverrides() {
        try {
            const raw = localStorage.getItem('blessOverrides');
            if (raw) this._slotOverrides = JSON.parse(raw);
        } catch {}
    }

    // ── Drag / position persistence (same pattern as other panels) ──

    _initDrag() {
        const header = this._el.querySelector('.bless-header');
        let startX, startY, origLeft, origTop;
        const onMove = (e) => {
            if (this._locked) return;
            const dx = e.clientX - startX;
            const dy = e.clientY - startY;
            const nl = Math.max(0, Math.min(origLeft + dx, window.innerWidth - 60));
            const nt = Math.max(40, Math.min(origTop + dy, window.innerHeight - 32));
            this._el.style.left = nl + 'px';
            this._el.style.top = nt + 'px';
        };
        const onUp = () => {
            document.removeEventListener('mousemove', onMove);
            document.removeEventListener('mouseup', onUp);
            header.style.cursor = 'grab';
            this._saveState();
        };
        header.addEventListener('mousedown', (e) => {
            if (this._locked) return;
            e.preventDefault();
            startX = e.clientX;
            startY = e.clientY;
            const rect = this._el.getBoundingClientRect();
            origLeft = rect.left;
            origTop = rect.top;
            header.style.cursor = 'grabbing';
            document.addEventListener('mousemove', onMove);
            document.addEventListener('mouseup', onUp);
        });
    }

    _saveState() {
        try {
            const pos = JSON.parse(localStorage.getItem('panelPositions') || '{}');
            pos.bless = {
                left: this._el.style.left,
                top: this._el.style.top,
                visible: this._visible,
                locked: this._locked,
            };
            localStorage.setItem('panelPositions', JSON.stringify(pos));
        } catch {}
    }

    _restoreState() {
        try {
            const pos = JSON.parse(localStorage.getItem('panelPositions') || '{}');
            const s = pos.bless;
            if (s) {
                if (s.left) this._el.style.left = s.left;
                if (s.top) this._el.style.top = s.top;
                if (s.locked) {
                    this._locked = true;
                    this._lockBtn.textContent = '\u{1F512}';
                }
                if (s.visible) this.toggle(true);
            }
        } catch {}
    }
}
