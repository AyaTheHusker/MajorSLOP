// ── Inventory Panel ──
// Floating, draggable, lockable grid panel showing equipped + carried items
// with coin pile slots at the bottom.

class InventoryPanel {
    constructor(sendFn) {
        this._send = sendFn;
        this._visible = false;
        this._locked = false;
        this._data = null; // { equipped: {}, carried: [], wealth: '', currency: {}, encumbrance: '' }
        this._el = null;
        this._build();
        this._initDrag();
        this._restoreState();
    }

    get isOpen() { return this._visible; }

    _build() {
        const panel = document.createElement('div');
        panel.id = 'inventory-panel';
        panel.className = 'inventory-panel';
        panel.style.display = 'none';

        // Header
        const header = document.createElement('div');
        header.className = 'inventory-header';

        const title = document.createElement('span');
        title.className = 'inventory-title';
        title.textContent = 'Inventory';

        const controls = document.createElement('div');
        controls.className = 'inventory-controls';

        const refreshBtn = document.createElement('span');
        refreshBtn.className = 'inventory-btn';
        refreshBtn.textContent = '\u{21BB}';
        refreshBtn.title = 'Refresh inventory (type "i")';
        refreshBtn.addEventListener('click', (e) => {
            e.stopPropagation();
            this._send('inject', { text: 'i' });
        });

        const lockBtn = document.createElement('span');
        lockBtn.className = 'inventory-btn';
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
        closeBtn.className = 'inventory-btn';
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

        // Item grid container
        const grid = document.createElement('div');
        grid.className = 'inventory-grid';
        grid.id = 'inventory-grid';
        panel.appendChild(grid);

        // Currency row
        const currRow = document.createElement('div');
        currRow.className = 'inventory-currency-row';
        currRow.id = 'inventory-currency';
        panel.appendChild(currRow);

        // Encumbrance footer
        const footer = document.createElement('div');
        footer.className = 'inventory-footer';
        footer.id = 'inventory-footer';
        panel.appendChild(footer);

        document.body.appendChild(panel);
        this._el = panel;
    }

    toggle(show) {
        if (show === undefined) show = !this._visible;
        this._visible = show;
        this._el.style.display = show ? 'flex' : 'none';
        if (show && this._data) this._render();
        this._saveState();
    }

    update(data) {
        this._data = data;
        if (this._visible) this._render();
    }

    /** Optimistically adjust a currency amount and redraw that slot */
    adjustCurrency(coinType, delta) {
        if (!this._data) return;
        if (!this._data.currency) this._data.currency = {};
        const cur = (this._data.currency[coinType] || 0) + delta;
        this._data.currency[coinType] = Math.max(0, cur);
        if (this._visible) this._render();
    }

    /** Get the bounding rect of a currency slot for coin animations */
    getCurrencySlotRect(coinType) {
        const slot = this._el.querySelector(`.inv-coin-slot[data-currency="${coinType}"]`);
        if (slot) {
            const r = slot.getBoundingClientRect();
            return { x: r.left + r.width / 2, y: r.top + r.height / 2 };
        }
        return null;
    }

    _render() {
        if (!this._data) return;
        const { equipped, carried, currency, encumbrance, wealth } = this._data;

        // ── Item Grid ──
        const grid = document.getElementById('inventory-grid');
        grid.innerHTML = '';

        // Equipped items first (with "E" badge)
        const equippedEntries = Object.entries(equipped || {}).sort((a, b) => {
            const na = parseFloat(a[0]), nb = parseFloat(b[0]);
            return na - nb;
        });

        for (const [slotId, item] of equippedEntries) {
            grid.appendChild(this._createItemSlot(item, true, slotId));
        }

        // Carried items
        for (const item of (carried || [])) {
            grid.appendChild(this._createItemSlot(item, false));
        }

        if (equippedEntries.length === 0 && (!carried || carried.length === 0)) {
            const empty = document.createElement('div');
            empty.className = 'inventory-empty';
            empty.textContent = 'Type "i" to load inventory';
            grid.appendChild(empty);
        }

        // ── Currency Row ──
        const currRow = document.getElementById('inventory-currency');
        currRow.innerHTML = '';

        const coinOrder = ['copper', 'silver', 'gold', 'platinum', 'runic'];
        for (const type of coinOrder) {
            const amount = (currency || {})[type] || 0;
            const slot = document.createElement('div');
            slot.className = 'inv-coin-slot';
            slot.dataset.currency = type;
            slot.title = `${amount} ${type}`;

            if (amount > 0 && typeof coinRenderer !== 'undefined') {
                const pile = coinRenderer.createPile(type, amount, 48, 48);
                slot.appendChild(pile);
            } else {
                // Empty slot with abbreviation label
                const abbrs = { copper: 'COP', silver: 'SIL', gold: 'GLD', platinum: 'PLT', runic: 'RUN' };
                const label = document.createElement('span');
                label.className = 'inv-coin-label';
                label.textContent = abbrs[type] || type.slice(0, 3).toUpperCase();
                slot.appendChild(label);
            }

            // Right-click context menu for coin slots
            slot.addEventListener('contextmenu', (e) => {
                e.preventDefault();
                e.stopPropagation();
                this._showCoinMenu(e.clientX, e.clientY, type, amount);
            });

            currRow.appendChild(slot);
        }

        // ── Footer ──
        const footer = document.getElementById('inventory-footer');
        footer.textContent = encumbrance || wealth || '';
    }

    _createItemSlot(item, equipped, slotId) {
        const slot = document.createElement('div');
        slot.className = 'inv-item-slot' + (equipped ? ' inv-equipped' : '');
        slot.dataset.itemName = item.name;

        // Full stat tooltip on hover (same as char panel)
        slot.addEventListener('mouseenter', () => {
            if (typeof showTooltip === 'function') {
                showTooltip(slot, {
                    name: item.name,
                    type: 'item',
                    item_data: item.item_data,
                    key: item.key,
                });
            }
        });
        slot.addEventListener('mouseleave', () => {
            if (typeof hideTooltip === 'function') hideTooltip();
        });

        if (item.key) {
            const img = document.createElement('img');
            img.src = `/api/asset/${encodeURIComponent(item.key)}`;
            img.loading = 'lazy';
            img.onerror = () => { img.style.display = 'none'; };
            slot.appendChild(img);
        } else {
            // Text fallback
            const txt = document.createElement('span');
            txt.className = 'inv-item-text';
            // Show abbreviated name
            const words = item.name.split(' ');
            txt.textContent = words.length > 2 ? words.slice(0, 2).join(' ') : item.name;
            slot.appendChild(txt);
        }

        if (equipped) {
            const badge = document.createElement('span');
            badge.className = 'inv-equip-badge';
            badge.textContent = 'E';
            slot.appendChild(badge);
        }

        // Right-click context menu
        slot.addEventListener('contextmenu', (e) => {
            e.preventDefault();
            e.stopPropagation();
            this._showItemMenu(e.clientX, e.clientY, item, equipped);
        });

        return slot;
    }

    _showItemMenu(x, y, item, equipped) {
        // Remove existing menus
        document.querySelectorAll('.inv-context-menu').forEach(m => m.remove());

        const menu = document.createElement('div');
        menu.className = 'inv-context-menu';
        menu.style.left = `${x}px`;
        menu.style.top = `${y}px`;

        const actions = [];
        if (equipped) {
            actions.push(['Unequip', () => this._send('inject', { text: `rem ${item.name}` })]);
        } else {
            actions.push(['Equip', () => this._send('inject', { text: `equ ${item.name}` })]);
        }
        actions.push(['Drop', () => this._send('inject', { text: `dro ${item.name}` })]);
        actions.push(['Sell', () => this._send('inject', { text: `sel ${item.name}` })]);
        actions.push(['Give...', () => this._showGiveDialog(item)]);

        for (const [label, action] of actions) {
            const menuItem = document.createElement('div');
            menuItem.className = 'inv-context-item';
            menuItem.textContent = label;
            menuItem.onclick = () => {
                menu.remove();
                action();
            };
            menu.appendChild(menuItem);
        }

        document.body.appendChild(menu);

        // Close on click outside
        const closeHandler = (e) => {
            if (!menu.contains(e.target)) {
                menu.remove();
                document.removeEventListener('click', closeHandler);
            }
        };
        setTimeout(() => document.addEventListener('click', closeHandler), 0);
    }

    _showGiveDialog(item) {
        // Remove existing
        document.querySelectorAll('.inv-give-dialog').forEach(d => d.remove());

        const dialog = document.createElement('div');
        dialog.className = 'inv-give-dialog';

        const title = document.createElement('div');
        title.className = 'inv-give-title';
        title.textContent = `Give: ${item.name}`;
        dialog.appendChild(title);

        const select = document.createElement('select');
        select.className = 'inv-give-select';

        // Populate with players in room
        const thumbs = document.querySelectorAll('#player-thumbs .thumb');
        if (thumbs.length === 0) {
            const opt = document.createElement('option');
            opt.textContent = 'No players in room';
            opt.disabled = true;
            select.appendChild(opt);
        } else {
            for (const t of thumbs) {
                const name = t.dataset.entityName || t.title;
                if (name) {
                    const opt = document.createElement('option');
                    opt.value = name.split(' ')[0]; // first name only for command
                    opt.textContent = name;
                    select.appendChild(opt);
                }
            }
        }
        dialog.appendChild(select);

        const btnRow = document.createElement('div');
        btnRow.className = 'inv-give-buttons';

        const giveBtn = document.createElement('button');
        giveBtn.textContent = 'Give';
        giveBtn.onclick = () => {
            const target = select.value;
            if (target) {
                this._send('inject', { text: `giv ${item.name} ${target}` });
            }
            dialog.remove();
        };

        const cancelBtn = document.createElement('button');
        cancelBtn.textContent = 'Cancel';
        cancelBtn.onclick = () => dialog.remove();

        btnRow.appendChild(giveBtn);
        btnRow.appendChild(cancelBtn);
        dialog.appendChild(btnRow);

        document.body.appendChild(dialog);

        // Position near center
        dialog.style.left = `${(window.innerWidth - 240) / 2}px`;
        dialog.style.top = `${(window.innerHeight - 150) / 2}px`;
    }

    _showCoinMenu(x, y, coinType, amount) {
        document.querySelectorAll('.inv-context-menu').forEach(m => m.remove());

        const menu = document.createElement('div');
        menu.className = 'inv-context-menu';
        menu.style.left = `${x}px`;
        menu.style.top = `${y}px`;

        const typeName = coinType.charAt(0).toUpperCase() + coinType.slice(1);
        const actions = [];

        if (amount > 0) {
            actions.push(['Drop...', () => {
                this._showSliderDialog(`Drop ${typeName}`, amount, (val) => {
                    this._send('inject', { text: `dro ${val} ${coinType}` });
                });
            }]);
            actions.push(['Give...', () => {
                this._showCoinGiveDialog(coinType, amount);
            }]);
            actions.push(['Deposit...', () => {
                // Deposit is in copper farthings — show total wealth
                const totalCopper = this._getTotalCopperWealth();
                this._showSliderDialog(`Deposit (copper)`, totalCopper || amount, (val) => {
                    this._send('inject', { text: `dep ${val}` });
                });
            }]);
        }

        if (actions.length === 0) {
            const empty = document.createElement('div');
            empty.className = 'inv-context-item';
            empty.textContent = `No ${typeName}`;
            empty.style.color = '#666';
            menu.appendChild(empty);
        }

        for (const [label, action] of actions) {
            const item = document.createElement('div');
            item.className = 'inv-context-item';
            item.textContent = label;
            item.onclick = () => { menu.remove(); action(); };
            menu.appendChild(item);
        }

        document.body.appendChild(menu);
        const closeHandler = (e) => {
            if (!menu.contains(e.target)) { menu.remove(); document.removeEventListener('click', closeHandler); }
        };
        setTimeout(() => document.addEventListener('click', closeHandler), 0);
    }

    _getTotalCopperWealth() {
        if (!this._data) return 0;
        // Parse from the wealth string (e.g. "4259130 copper farthings")
        const w = this._data.wealth || '';
        const m = w.match(/^(\d+)/);
        return m ? parseInt(m[1]) : 0;
    }

    _showSliderDialog(title, max, onConfirm) {
        document.querySelectorAll('.inv-slider-dialog').forEach(d => d.remove());

        const dialog = document.createElement('div');
        dialog.className = 'inv-slider-dialog';

        const titleEl = document.createElement('div');
        titleEl.className = 'inv-give-title';
        titleEl.textContent = title;
        dialog.appendChild(titleEl);

        // Slider
        const slider = document.createElement('input');
        slider.type = 'range';
        slider.className = 'inv-slider';
        slider.min = 1;
        slider.max = max;
        slider.value = max; // Default ALL

        // Number input
        const numInput = document.createElement('input');
        numInput.type = 'number';
        numInput.className = 'inv-slider-num';
        numInput.min = 1;
        numInput.max = max;
        numInput.value = max;

        // "ALL" label
        const allLabel = document.createElement('span');
        allLabel.className = 'inv-slider-all';
        allLabel.textContent = 'ALL';

        slider.oninput = () => {
            numInput.value = slider.value;
            allLabel.textContent = parseInt(slider.value) === max ? 'ALL' : '';
        };
        numInput.oninput = () => {
            const v = Math.max(1, Math.min(max, parseInt(numInput.value) || 1));
            slider.value = v;
            allLabel.textContent = v === max ? 'ALL' : '';
        };

        const sliderRow = document.createElement('div');
        sliderRow.className = 'inv-slider-row';
        sliderRow.appendChild(slider);
        sliderRow.appendChild(numInput);
        sliderRow.appendChild(allLabel);
        dialog.appendChild(sliderRow);

        // Buttons
        const btnRow = document.createElement('div');
        btnRow.className = 'inv-give-buttons';

        const okBtn = document.createElement('button');
        okBtn.textContent = 'OK';
        okBtn.onclick = () => {
            const val = Math.max(1, Math.min(max, parseInt(numInput.value) || max));
            dialog.remove();
            onConfirm(val);
        };

        const cancelBtn = document.createElement('button');
        cancelBtn.textContent = 'Cancel';
        cancelBtn.onclick = () => dialog.remove();

        btnRow.appendChild(okBtn);
        btnRow.appendChild(cancelBtn);
        dialog.appendChild(btnRow);

        document.body.appendChild(dialog);
        dialog.style.left = `${(window.innerWidth - 260) / 2}px`;
        dialog.style.top = `${(window.innerHeight - 160) / 2}px`;
    }

    _showCoinGiveDialog(coinType, amount) {
        document.querySelectorAll('.inv-slider-dialog').forEach(d => d.remove());

        const typeName = coinType.charAt(0).toUpperCase() + coinType.slice(1);
        const dialog = document.createElement('div');
        dialog.className = 'inv-slider-dialog';

        const titleEl = document.createElement('div');
        titleEl.className = 'inv-give-title';
        titleEl.textContent = `Give ${typeName}`;
        dialog.appendChild(titleEl);

        // Player dropdown
        const select = document.createElement('select');
        select.className = 'inv-give-select';
        const thumbs = document.querySelectorAll('#player-thumbs .thumb');
        if (thumbs.length === 0) {
            const opt = document.createElement('option');
            opt.textContent = 'No players in room';
            opt.disabled = true;
            select.appendChild(opt);
        } else {
            for (const t of thumbs) {
                const name = t.dataset.entityName || t.title;
                if (name) {
                    const opt = document.createElement('option');
                    opt.value = name.split(' ')[0];
                    opt.textContent = name;
                    select.appendChild(opt);
                }
            }
        }
        dialog.appendChild(select);

        // Slider
        const slider = document.createElement('input');
        slider.type = 'range';
        slider.className = 'inv-slider';
        slider.min = 1;
        slider.max = amount;
        slider.value = amount;

        const numInput = document.createElement('input');
        numInput.type = 'number';
        numInput.className = 'inv-slider-num';
        numInput.min = 1;
        numInput.max = amount;
        numInput.value = amount;

        const allLabel = document.createElement('span');
        allLabel.className = 'inv-slider-all';
        allLabel.textContent = 'ALL';

        slider.oninput = () => {
            numInput.value = slider.value;
            allLabel.textContent = parseInt(slider.value) === amount ? 'ALL' : '';
        };
        numInput.oninput = () => {
            const v = Math.max(1, Math.min(amount, parseInt(numInput.value) || 1));
            slider.value = v;
            allLabel.textContent = v === amount ? 'ALL' : '';
        };

        const sliderRow = document.createElement('div');
        sliderRow.className = 'inv-slider-row';
        sliderRow.appendChild(slider);
        sliderRow.appendChild(numInput);
        sliderRow.appendChild(allLabel);
        dialog.appendChild(sliderRow);

        // Buttons
        const btnRow = document.createElement('div');
        btnRow.className = 'inv-give-buttons';

        const giveBtn = document.createElement('button');
        giveBtn.textContent = 'Give';
        giveBtn.onclick = () => {
            const target = select.value;
            const val = Math.max(1, Math.min(amount, parseInt(numInput.value) || amount));
            if (target) {
                this._send('inject', { text: `giv ${val} ${coinType} ${target}` });
            }
            dialog.remove();
        };

        const cancelBtn = document.createElement('button');
        cancelBtn.textContent = 'Cancel';
        cancelBtn.onclick = () => dialog.remove();

        btnRow.appendChild(giveBtn);
        btnRow.appendChild(cancelBtn);
        dialog.appendChild(btnRow);

        document.body.appendChild(dialog);
        dialog.style.left = `${(window.innerWidth - 260) / 2}px`;
        dialog.style.top = `${(window.innerHeight - 180) / 2}px`;
    }

    _initDrag() {
        let dragging = false;
        let startX, startY, origLeft, origTop;
        const header = this._el.querySelector('.inventory-header');

        header.addEventListener('mousedown', (e) => {
            if (this._locked) return;
            if (e.button !== 0) return;
            if (e.target.closest('.inventory-controls')) return;
            dragging = true;
            startX = e.clientX;
            startY = e.clientY;
            const rect = this._el.getBoundingClientRect();
            origLeft = rect.left;
            origTop = rect.top;
            this._el.style.transition = 'none';
            e.preventDefault();
        });

        document.addEventListener('mousemove', (e) => {
            if (!dragging) return;
            const r = this._el.getBoundingClientRect();
            let nl = origLeft + (e.clientX - startX);
            let nt = origTop + (e.clientY - startY);
            nt = Math.max(0, Math.min(nt, window.innerHeight - 32));
            nl = Math.max(-r.width + 80, Math.min(nl, window.innerWidth - 80));
            this._el.style.left = `${nl}px`;
            this._el.style.top = `${nt}px`;
            this._el.style.right = 'auto';
            this._el.style.bottom = 'auto';
        });

        document.addEventListener('mouseup', () => {
            if (dragging) {
                dragging = false;
                this._el.style.transition = '';
                this._saveState();
            }
        });
    }

    _saveState() {
        try {
            const saved = JSON.parse(localStorage.getItem('panelPositions') || '{}');
            saved['inventory-panel'] = {
                left: this._el.style.left,
                top: this._el.style.top,
                visible: this._visible,
                locked: this._locked,
            };
            localStorage.setItem('panelPositions', JSON.stringify(saved));
        } catch {}
    }

    _restoreState() {
        try {
            const saved = JSON.parse(localStorage.getItem('panelPositions') || '{}');
            const s = saved['inventory-panel'];
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
