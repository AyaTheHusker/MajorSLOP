// ── Character Equipment & Inventory Panel ──
// Floating, draggable panel with ARPG-style equipment slots around a portrait
// and an inventory grid below.

const EQUIP_SLOTS = {
    2:  { name: 'Head',     col: 2, row: 0 },
    15: { name: 'Ears',     col: 0, row: 0 },
    17: { name: 'Light',    col: 4, row: 0 },
    19: { name: 'Face',     col: 0, row: 1 },
    8:  { name: 'Neck',     col: 4, row: 1 },
    1:  { name: 'Weapon',   col: 0, row: 2 },
    6:  { name: 'Arms',     col: 0, row: 3 },
    7:  { name: 'Back',     col: 4, row: 2 },
    12: { name: 'Off-hand', col: 4, row: 3 },
    14: { name: 'Wrist',    col: 0, row: 4 },
    4:  { name: 'Ring',     col: 4, row: 4 },
    3:  { name: 'Hands',    col: 0, row: 5 },
    11: { name: 'Torso',    col: 2, row: 5 },
    10: { name: 'Waist',    col: 4, row: 5 },
    16: { name: 'Trophy',   col: 0, row: 6 },
    9:  { name: 'Legs',     col: 2, row: 6 },
    5:  { name: 'Feet',     col: 2, row: 7 },
};

// Portrait spans the center area (cols 1-3, rows 1-4)
const PORTRAIT_COL_START = 1;
const PORTRAIT_COL_END = 4;   // exclusive
const PORTRAIT_ROW_START = 1;
const PORTRAIT_ROW_END = 5;   // exclusive

class CharPanel {
    constructor() {
        this._visible = false;
        this._locked = false;
        this._equipment = {};   // slotId -> {name, key, item_data}
        this._inventory = [];   // [{name, key, item_data}, ...]
        this._charName = '';
        this._portraitUrl = '';
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
        equipGrid.appendChild(portrait);

        // Equipment slots
        this._slotEls = {};
        for (const [slotId, info] of Object.entries(EQUIP_SLOTS)) {
            const slot = document.createElement('div');
            slot.className = 'equip-slot';
            slot.dataset.slot = slotId;
            slot.style.gridColumn = info.col + 1;
            slot.style.gridRow = info.row + 1;
            slot.title = info.name;

            const label = document.createElement('span');
            label.className = 'equip-slot-label';
            label.textContent = info.name;

            const icon = document.createElement('div');
            icon.className = 'equip-slot-icon';

            slot.appendChild(icon);
            slot.appendChild(label);
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
            this._el.style.left = `${origLeft + dx}px`;
            this._el.style.top = `${origTop + dy}px`;
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

    setEquipment(slotId, item) {
        // item: {name, key, item_data} or null to clear
        this._equipment[slotId] = item;
        const slotEl = this._slotEls[slotId];
        if (!slotEl) return;

        if (item && item.key) {
            slotEl.icon.textContent = '';
            slotEl.icon.style.backgroundImage = `url(/asset/${item.key})`;
            slotEl.icon.classList.add('has-item');
            slotEl.el.title = `${EQUIP_SLOTS[slotId].name}: ${item.name}`;
        } else if (item && item.name) {
            slotEl.icon.textContent = item.name.charAt(0).toUpperCase();
            slotEl.icon.style.backgroundImage = '';
            slotEl.icon.classList.add('has-item');
            slotEl.el.title = `${EQUIP_SLOTS[slotId].name}: ${item.name}`;
        } else {
            slotEl.icon.textContent = '';
            slotEl.icon.style.backgroundImage = '';
            slotEl.icon.classList.remove('has-item');
            slotEl.el.title = EQUIP_SLOTS[slotId].name;
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
                cell.title = item.name + (item.quantity > 1 ? ` (×${item.quantity})` : '');

                if (item.key) {
                    const img = document.createElement('img');
                    img.src = `/asset/${item.key}`;
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
            }

            this._invGrid.appendChild(cell);
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
