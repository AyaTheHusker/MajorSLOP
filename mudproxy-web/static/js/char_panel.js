// ── Character Equipment & Inventory Panel ──
// Floating, draggable panel with ARPG-style equipment slots around a portrait
// and an inventory grid below.

const EQUIP_SLOTS = {
    //            Outer ring around portrait
    15:   { name: 'Ears',      col: 0, row: 0 },
    2:    { name: 'Head',      col: 2, row: 0 },
    17:   { name: 'Light',     col: 4, row: 0 },
    19:   { name: 'Face',      col: 0, row: 1 },
    8:    { name: 'Neck',      col: 4, row: 1 },
    1:    { name: 'Weapon',    col: 0, row: 2 },
    7:    { name: 'Back',      col: 4, row: 2 },
    6:    { name: 'Arms',      col: 0, row: 3 },
    12:   { name: 'Off-hand',  col: 4, row: 3 },
    3:    { name: 'Hands',     col: 0, row: 4 },
    11:   { name: 'Torso',     col: 4, row: 4 },
    9:    { name: 'Legs',      col: 0, row: 5 },
    5:    { name: 'Feet',      col: 4, row: 5 },
    //            Bottom row: wrists, waist, worn
    '14a': { name: 'Wrist 1',  col: 0, row: 6 },
    10:   { name: 'Waist',     col: 1, row: 6 },
    16:   { name: 'Worn',      col: 2, row: 6 },
    '14b': { name: 'Wrist 2',  col: 3, row: 6 },
    //            Top row: rings flanking helm
    '4a':  { name: 'Ring 1',   col: 1, row: 0 },
    '4b':  { name: 'Ring 2',   col: 3, row: 0 },
};

// Portrait spans the center area (cols 1-3, rows 1-5)
const PORTRAIT_COL_START = 1;
const PORTRAIT_COL_END = 4;   // exclusive
const PORTRAIT_ROW_START = 1;
const PORTRAIT_ROW_END = 6;   // exclusive

class CharPanel {
    constructor() {
        this._visible = false;
        this._locked = false;
        this._equipment = {};   // slotId -> {name, key, item_data}
        this._inventory = [];   // [{name, key, item_data}, ...]
        this._charName = '';
        this._portraitUrl = '';
        this._portraitKey = '';
        this._el = null;
        this._build();
        this._initDrag();
        this._restoreState();
    }

    _build() {
        const panel = document.createElement('div');
        panel.id = 'char-panel';
        panel.className = 'char-panel';
        panel.style.display = 'none';

        // Header with title + controls
        const header = document.createElement('div');
        header.className = 'char-panel-header';

        const title = document.createElement('span');
        title.className = 'char-panel-title';
        title.textContent = 'Character';
        this._titleEl = title;

        const controls = document.createElement('div');
        controls.className = 'char-panel-controls';

        const refreshBtn = document.createElement('span');
        refreshBtn.className = 'char-panel-btn';
        refreshBtn.textContent = '↻';
        refreshBtn.title = 'Refresh (stat + look self)';
        refreshBtn.addEventListener('click', (e) => {
            e.stopPropagation();
            this._refresh();
        });

        const lockBtn = document.createElement('span');
        lockBtn.className = 'char-panel-btn';
        lockBtn.textContent = '🔓';
        lockBtn.title = 'Lock/unlock position';
        lockBtn.addEventListener('click', (e) => {
            e.stopPropagation();
            this._locked = !this._locked;
            lockBtn.textContent = this._locked ? '🔒' : '🔓';
            this._saveState();
        });
        this._lockBtn = lockBtn;

        const closeBtn = document.createElement('span');
        closeBtn.className = 'char-panel-btn';
        closeBtn.textContent = '✕';
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

        // Equipment grid
        const equipGrid = document.createElement('div');
        equipGrid.className = 'equip-grid';

        // Portrait (center area)
        const portrait = document.createElement('div');
        portrait.className = 'char-portrait';
        portrait.style.gridColumn = `${PORTRAIT_COL_START + 1} / ${PORTRAIT_COL_END + 1}`;
        portrait.style.gridRow = `${PORTRAIT_ROW_START + 1} / ${PORTRAIT_ROW_END + 1}`;
        const portraitImg = document.createElement('div');
        portraitImg.className = 'char-portrait-img';
        portraitImg.textContent = '?';
        this._portraitImg = portraitImg;
        portrait.appendChild(portraitImg);

        // Right-click portrait to set a custom image
        portrait.addEventListener('contextmenu', (e) => {
            e.preventDefault();
            e.stopPropagation();
            this._pickPortrait();
        });

        equipGrid.appendChild(portrait);

        // Equipment slots
        this._slotEls = {};
        for (const [slotId, info] of Object.entries(EQUIP_SLOTS)) {
            const slot = document.createElement('div');
            slot.className = 'equip-slot';
            slot.dataset.slot = slotId;
            slot.style.gridColumn = info.col + 1;
            slot.style.gridRow = info.row + 1;

            const label = document.createElement('span');
            label.className = 'equip-slot-label';
            label.textContent = info.name;

            const icon = document.createElement('div');
            icon.className = 'equip-slot-icon';

            slot.appendChild(icon);
            slot.appendChild(label);

            // Hover tooltip for equipped items
            slot.addEventListener('mouseenter', () => {
                const item = this._equipment[slotId];
                if (item && item.name && typeof showTooltip === 'function') {
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

            // Right-click context menu for equipped items
            slot.addEventListener('contextmenu', (e) => {
                const item = this._equipment[slotId];
                if (!item || !item.name) return;
                e.preventDefault();
                e.stopPropagation();
                this._showEquipMenu(e.clientX, e.clientY, item);
            });

            equipGrid.appendChild(slot);
            this._slotEls[slotId] = { el: slot, icon, label };
        }

        panel.appendChild(equipGrid);

        // Inventory section
        const invHeader = document.createElement('div');
        invHeader.className = 'inv-header';
        invHeader.textContent = 'Inventory';
        panel.appendChild(invHeader);

        const invGrid = document.createElement('div');
        invGrid.className = 'inv-grid';
        this._invGrid = invGrid;
        panel.appendChild(invGrid);

        document.body.appendChild(panel);
        this._el = panel;
    }

    _initDrag() {
        let dragging = false;
        let startX, startY, origLeft, origTop;
        const header = this._el.querySelector('.char-panel-header');

        header.addEventListener('mousedown', (e) => {
            if (this._locked) return;
            if (e.button !== 0) return;
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
            const dx = e.clientX - startX;
            const dy = e.clientY - startY;
            const r = this._el.getBoundingClientRect();
            let nl = origLeft + dx, nt = origTop + dy;
            nt = Math.max(40, Math.min(nt, window.innerHeight - 32));
            nl = Math.max(-r.width + 80, Math.min(nl, window.innerWidth - 80));
            this._el.style.left = `${nl}px`;
            this._el.style.top = `${nt}px`;
        });

        document.addEventListener('mouseup', () => {
            if (dragging) {
                dragging = false;
                this._el.style.transition = '';
                this._saveState();
            }
        });
    }

    toggle(show) {
        if (show === undefined) show = !this._visible;
        this._visible = show;
        this._el.style.display = show ? '' : 'none';
        this._saveState();
    }

    setCharName(name) {
        this._charName = name;
        this._titleEl.textContent = name || 'Character';
        this.loadPortrait();
    }

    setPortrait(url) {
        this._portraitUrl = url;
        if (url) {
            this._portraitImg.textContent = '';
            this._portraitImg.style.backgroundImage = `url(${url})`;
        } else {
            this._portraitImg.textContent = '?';
            this._portraitImg.style.backgroundImage = '';
        }
    }

    _showInvItemMenu(x, y, item) {
        document.querySelectorAll('.inv-context-menu').forEach(m => m.remove());

        const menu = document.createElement('div');
        menu.className = 'inv-context-menu';
        menu.style.left = `${x}px`;
        menu.style.top = `${y}px`;

        const actions = [
            ['Equip', () => sendCommand('inject', { text: `equ ${item.name}` })],
            ['Drop', () => sendCommand('inject', { text: `dro ${item.name}` })],
            ['Sell', () => sendCommand('inject', { text: `sel ${item.name}` })],
        ];

        for (const [label, action] of actions) {
            const menuItem = document.createElement('div');
            menuItem.className = 'inv-context-item';
            menuItem.textContent = label;
            menuItem.onclick = () => { menu.remove(); action(); };
            menu.appendChild(menuItem);
        }

        document.body.appendChild(menu);
        const closeHandler = (e) => {
            if (!menu.contains(e.target)) {
                menu.remove();
                document.removeEventListener('click', closeHandler);
            }
        };
        setTimeout(() => document.addEventListener('click', closeHandler), 0);
    }

    _showEquipMenu(x, y, item) {
        document.querySelectorAll('.inv-context-menu').forEach(m => m.remove());

        const menu = document.createElement('div');
        menu.className = 'inv-context-menu';
        menu.style.left = `${x}px`;
        menu.style.top = `${y}px`;

        const actions = [
            ['Unequip', () => sendCommand('inject', { text: `rem ${item.name}` })],
            ['Drop', () => sendCommand('inject', { text: `dro ${item.name}` })],
            ['Sell', () => sendCommand('inject', { text: `sel ${item.name}` })],
        ];

        for (const [label, action] of actions) {
            const menuItem = document.createElement('div');
            menuItem.className = 'inv-context-item';
            menuItem.textContent = label;
            menuItem.onclick = () => { menu.remove(); action(); };
            menu.appendChild(menuItem);
        }

        document.body.appendChild(menu);
        const closeHandler = (e) => {
            if (!menu.contains(e.target)) {
                menu.remove();
                document.removeEventListener('click', closeHandler);
            }
        };
        setTimeout(() => document.addEventListener('click', closeHandler), 0);
    }

    setEquipment(slotId, item) {
        // item: {name, key, item_data} or null to clear
        this._equipment[slotId] = item;
        const slotEl = this._slotEls[slotId];
        if (!slotEl) return;

        if (item && item.key) {
            slotEl.icon.textContent = '';
            const safeKey = encodeURIComponent(item.key).replace(/'/g, '%27');
            slotEl.icon.style.backgroundImage = `url(/api/asset/${safeKey})`;
            slotEl.icon.classList.add('has-item');
        } else if (item && item.name) {
            slotEl.icon.textContent = item.name.charAt(0).toUpperCase();
            slotEl.icon.style.backgroundImage = '';
            slotEl.icon.classList.add('has-item');
        } else {
            slotEl.icon.textContent = '';
            slotEl.icon.style.backgroundImage = '';
            slotEl.icon.classList.remove('has-item');
        }
    }

    clearEquipment() {
        for (const slotId of Object.keys(EQUIP_SLOTS)) {
            this.setEquipment(slotId, null);
        }
    }

    setInventory(items) {
        // items: [{name, key, item_data, quantity}, ...]
        this._inventory = items || [];
        this._renderInventory();
    }

    _renderInventory() {
        this._invGrid.innerHTML = '';
        const items = this._inventory;

        // Always show at least 16 cells (4x4 minimum)
        const cellCount = Math.max(16, items.length);

        for (let i = 0; i < cellCount; i++) {
            const cell = document.createElement('div');
            cell.className = 'inv-cell';

            if (i < items.length) {
                const item = items[i];
                cell.classList.add('has-item');

                if (item.key) {
                    const img = document.createElement('img');
                    img.src = `/api/asset/${encodeURIComponent(item.key)}`;
                    img.alt = item.name;
                    cell.appendChild(img);
                } else {
                    cell.textContent = item.name.charAt(0).toUpperCase();
                }

                if (item.quantity > 1) {
                    const qty = document.createElement('span');
                    qty.className = 'inv-qty';
                    qty.textContent = item.quantity;
                    cell.appendChild(qty);
                }

                // Hover tooltip
                cell.addEventListener('mouseenter', () => {
                    if (typeof showTooltip === 'function') {
                        showTooltip(cell, {
                            name: item.name,
                            type: 'item',
                            item_data: item.item_data,
                            key: item.key,
                        });
                    }
                });
                cell.addEventListener('mouseleave', () => {
                    if (typeof hideTooltip === 'function') hideTooltip();
                });

                // Right-click context menu for inventory items
                cell.addEventListener('contextmenu', (e) => {
                    e.preventDefault();
                    e.stopPropagation();
                    this._showInvItemMenu(e.clientX, e.clientY, item);
                });
            }

            this._invGrid.appendChild(cell);
        }
    }

    _pickPortrait() {
        const input = document.createElement('input');
        input.type = 'file';
        input.accept = 'image/*';
        input.onchange = async () => {
            const file = input.files[0];
            if (!file) return;
            const name = this._charName || 'default';
            const form = new FormData();
            form.append('file', file);
            form.append('char_name', name);
            try {
                const res = await fetch('/api/portrait/upload', { method: 'POST', body: form });
                const data = await res.json();
                if (data.ok) {
                    this.setPortrait(`/api/portrait/${encodeURIComponent(name)}?_=${Date.now()}`);
                }
            } catch (e) {
                console.error('Portrait upload failed:', e);
            }
        };
        input.click();
    }

    loadPortrait() {
        const name = this._charName || 'default';
        // Check custom upload first, then fall back to slop portrait
        fetch(`/api/portrait/${encodeURIComponent(name)}`)
            .then(res => {
                if (res.ok) {
                    this.setPortrait(`/api/portrait/${encodeURIComponent(name)}?_=${Date.now()}`);
                } else if (this._portraitKey) {
                    this.setPortrait(`/api/asset/${encodeURIComponent(this._portraitKey)}`);
                }
            })
            .catch(() => {});
    }

    setPortraitKey(key) {
        this._portraitKey = key;
        // If no custom portrait loaded, use the slop portrait
        if (!this._portraitUrl || this._portraitUrl.includes('/api/asset/')) {
            if (key) {
                this.setPortrait(`/api/asset/${encodeURIComponent(key)}`);
            }
        }
    }

    _refresh() {
        // Send stat, i, exp, then l <firstname> for portrait data
        if (typeof sendCommand === 'function') {
            sendCommand('inject', { text: 'stat' });
            setTimeout(() => sendCommand('inject', { text: 'i' }), 500);
            setTimeout(() => sendCommand('inject', { text: 'exp' }), 1000);
            if (this._charName) {
                const firstName = this._charName.split(' ')[0];
                setTimeout(() => sendCommand('inject', { text: `l ${firstName}` }), 1500);
            }
        }
    }

    _saveState() {
        try {
            const saved = JSON.parse(localStorage.getItem('panelPositions') || '{}');
            saved['char-panel'] = {
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
            const s = saved['char-panel'];
            if (s) {
                if (s.left) this._el.style.left = s.left;
                if (s.top) this._el.style.top = s.top;
                if (s.locked) {
                    this._locked = true;
                    this._lockBtn.textContent = '🔒';
                }
                if (s.visible) this.toggle(true);
            }
        } catch {}
    }
}
