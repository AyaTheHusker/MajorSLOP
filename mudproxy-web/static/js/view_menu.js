// ── View Menu ──
// Dropdown menu system for room viewer settings, matching GTK4 app menus.

class ViewMenu {
    constructor(roomView) {
        this.rv = roomView;
        this.openMenu = null;
        this._buildMenuBar();
        this._setupClickAway();
    }

    _buildMenuBar() {
        const bar = document.getElementById('menu-bar');

        // View menu
        this._addMenu(bar, 'View', [
            { type: 'toggle', label: '3D Mode', key: 'depth3d', default: true },
            { type: 'toggle', label: 'Hi-Res Images', key: 'preferHires', default: true },
            { type: 'submenu', label: 'Image Scaling', items: [
                { type: 'radio', label: 'Fill (no black bars)', key: 'fillMode', value: 'fill' },
                { type: 'radio', label: 'Fit (letterbox)', key: 'fillMode', value: 'fit' },
            ]},
            { type: 'toggle', label: 'Scanlines', key: 'showScanlines', default: false },
            { type: 'toggle', label: 'Warp Zoom', key: 'showWarpZoom', default: false },
            { type: 'separator' },
            { type: 'toggle', label: 'Console', key: 'showConsole', default: true },
            { type: 'toggle', label: 'Monsters', key: 'showMonsters', default: true },
            { type: 'toggle', label: 'Items', key: 'showItems', default: true },
            { type: 'separator' },
            { type: 'submenu', label: 'Scanline Thickness', items:
                [1,2,3,4,5,6,7,8].map(n => ({
                    type: 'radio', label: `${n}px`, key: 'scanlineThickness', value: n
                }))
            },
            { type: 'submenu', label: 'NPC Location', items: [
                { type: 'radio', label: 'Above Image', key: 'npcLocation', value: 'above' },
                { type: 'radio', label: 'Below Image', key: 'npcLocation', value: 'below' },
                { type: 'radio', label: 'Floating', key: 'npcLocation', value: 'floating' },
            ]},
            { type: 'submenu', label: 'Loot Location', items: [
                { type: 'radio', label: 'Above Image', key: 'lootLocation', value: 'above' },
                { type: 'radio', label: 'Below Image', key: 'lootLocation', value: 'below' },
                { type: 'radio', label: 'Floating', key: 'lootLocation', value: 'floating' },
            ]},
            { type: 'separator' },
            { type: 'toggle', label: 'Lock NPC Panel', key: 'npcLocked', default: false },
            { type: 'toggle', label: 'Lock Loot Panel', key: 'lootLocked', default: false },
            { type: 'separator' },
            { type: 'submenu', label: 'NPC Scale', items:
                ['50%','75%','100%','125%','150%','200%'].map(p => ({
                    type: 'radio', label: p, key: 'npcThumbScale', value: p
                }))
            },
            { type: 'submenu', label: 'Loot Scale', items:
                ['50%','75%','100%','125%','150%','200%'].map(p => ({
                    type: 'radio', label: p, key: 'lootThumbScale', value: p
                }))
            },
            { type: 'submenu', label: 'Damage Text Scale', items:
                ['50%','75%','100%','125%','150%','200%'].map(p => ({
                    type: 'radio', label: p, key: 'dmgTextScale', value: p
                }))
            },
        ]);

        // Data menu — MDB + SLOP file selection
        this._addMenu(bar, 'Data', [
            { type: 'action', label: 'Load MDB File...', action: () => this._showMDBPicker() },
            { type: 'action', label: 'Load SLOP File...', action: () => this._showSLOPPicker() },
            { type: 'separator' },
            { type: 'status', id: 'mdb-status', label: 'MDB: checking...' },
            { type: 'status', id: 'slop-status', label: 'SLOP: checking...' },
        ]);
        this._refreshDataStatus();

        // Camera menu
        this._addMenu(bar, 'Camera', [
            { type: 'submenu', label: 'Camera Mode', items: [
                { type: 'radio', label: 'Carousel', key: 'cameraMode', value: 0 },
                { type: 'radio', label: 'Horizontal', key: 'cameraMode', value: 1 },
                { type: 'radio', label: 'Vertical', key: 'cameraMode', value: 2 },
                { type: 'radio', label: 'Circle', key: 'cameraMode', value: 3 },
                { type: 'radio', label: 'Zoom', key: 'cameraMode', value: 4 },
                { type: 'radio', label: 'Dolly', key: 'cameraMode', value: 5 },
                { type: 'radio', label: 'Orbital', key: 'cameraMode', value: 6 },
                { type: 'radio', label: 'Explore', key: 'cameraMode', value: 7 },
            ]},
            { type: 'separator' },
            { type: 'slider', label: 'Depth Scale', key: 'depthScale', min: 0, max: 0.5, step: 0.01 },
            { type: 'slider', label: 'Intensity', key: 'cameraIntensity', min: 0, max: 0.5, step: 0.01 },
            { type: 'slider', label: 'Speed', key: 'cameraSpeed', min: 0, max: 2.0, step: 0.05 },
            { type: 'slider', label: 'Isometric', key: 'isometric', min: 0, max: 1.0, step: 0.05 },
            { type: 'slider', label: 'Steady', key: 'steady', min: 0, max: 1.0, step: 0.05 },
            { type: 'slider', label: 'Overscan', key: 'overscan', min: 0, max: 0.2, step: 0.01 },
            { type: 'slider', label: 'Depth Contrast', key: 'depthContrast', min: 0.1, max: 3.0, step: 0.1 },
            { type: 'separator' },
            { type: 'slider', label: 'Pan Speed', key: 'panSpeed', min: 0, max: 0.3, step: 0.01 },
            { type: 'slider', label: 'Pan X', key: 'panAmountX', min: 0, max: 1.0, step: 0.05 },
            { type: 'slider', label: 'Pan Y', key: 'panAmountY', min: 0, max: 1.0, step: 0.05 },
            { type: 'separator' },
            { type: 'slider', label: 'Vignette', key: 'vignetteAmount', min: 0, max: 1.0, step: 0.05 },
            { type: 'slider', label: 'Vignette Size', key: 'vignetteFeather', min: 0.2, max: 1.5, step: 0.05 },
        ]);
    }

    _addMenu(bar, label, items) {
        const btn = document.createElement('div');
        btn.className = 'menu-btn';
        btn.textContent = label;

        const dropdown = this._buildDropdown(items);
        btn.appendChild(dropdown);

        btn.addEventListener('click', (e) => {
            e.stopPropagation();
            if (this.openMenu === dropdown) {
                this._closeAll();
            } else {
                this._closeAll();
                dropdown.style.display = 'block';
                this.openMenu = dropdown;
            }
        });

        // Hover-open when another menu is already open
        btn.addEventListener('mouseenter', () => {
            if (this.openMenu && this.openMenu !== dropdown) {
                this._closeAll();
                dropdown.style.display = 'block';
                this.openMenu = dropdown;
            }
        });

        bar.appendChild(btn);
    }

    _buildDropdown(items) {
        const dd = document.createElement('div');
        dd.className = 'menu-dropdown';
        dd.style.display = 'none';

        for (const item of items) {
            if (item.type === 'separator') {
                const sep = document.createElement('div');
                sep.className = 'menu-separator';
                dd.appendChild(sep);
                continue;
            }

            if (item.type === 'submenu') {
                const sub = document.createElement('div');
                sub.className = 'menu-item has-submenu';
                sub.innerHTML = `<span>${item.label}</span><span class="submenu-arrow">▸</span>`;

                const subDropdown = this._buildDropdown(item.items);
                subDropdown.className = 'menu-dropdown submenu';
                sub.appendChild(subDropdown);

                sub.addEventListener('mouseenter', () => {
                    subDropdown.style.display = 'block';
                });
                sub.addEventListener('mouseleave', () => {
                    subDropdown.style.display = 'none';
                });

                dd.appendChild(sub);
                continue;
            }

            if (item.type === 'toggle') {
                const el = document.createElement('div');
                el.className = 'menu-item toggle';
                const checked = this.rv.settings[item.key] ?? item.default;
                el.innerHTML = `<span class="menu-check">${checked ? '✓' : ''}</span><span>${item.label}</span>`;
                el.addEventListener('click', (e) => {
                    e.stopPropagation();
                    const newVal = !this.rv.settings[item.key];
                    this.rv.updateSetting(item.key, newVal);
                    el.querySelector('.menu-check').textContent = newVal ? '✓' : '';
                });
                dd.appendChild(el);
                continue;
            }

            if (item.type === 'radio') {
                const el = document.createElement('div');
                el.className = 'menu-item radio';
                const current = this.rv.settings[item.key];
                el.innerHTML = `<span class="menu-check">${current === item.value ? '●' : '○'}</span><span>${item.label}</span>`;
                el.addEventListener('click', (e) => {
                    e.stopPropagation();
                    this.rv.updateSetting(item.key, item.value);
                    // Update all siblings
                    const parent = el.parentElement;
                    for (const sib of parent.querySelectorAll('.menu-item.radio')) {
                        const check = sib.querySelector('.menu-check');
                        if (check) check.textContent = '○';
                    }
                    el.querySelector('.menu-check').textContent = '●';
                });
                dd.appendChild(el);
                continue;
            }

            if (item.type === 'action') {
                const el = document.createElement('div');
                el.className = 'menu-item action';
                el.innerHTML = `<span>${item.label}</span>`;
                el.addEventListener('click', (e) => {
                    e.stopPropagation();
                    this._closeAll();
                    item.action();
                });
                dd.appendChild(el);
                continue;
            }

            if (item.type === 'status') {
                const el = document.createElement('div');
                el.className = 'menu-item status-item';
                el.id = item.id || '';
                el.innerHTML = `<span class="status-text">${item.label}</span>`;
                dd.appendChild(el);
                continue;
            }

            if (item.type === 'slider') {
                const el = document.createElement('div');
                el.className = 'menu-item slider-item';
                const current = this.rv.settings[item.key] ?? item.min;
                el.innerHTML = `
                    <label>${item.label}</label>
                    <input type="range" min="${item.min}" max="${item.max}" step="${item.step}" value="${current}">
                    <span class="slider-value">${current.toFixed(2)}</span>
                `;
                const input = el.querySelector('input');
                const valSpan = el.querySelector('.slider-value');
                input.addEventListener('input', (e) => {
                    e.stopPropagation();
                    const v = parseFloat(e.target.value);
                    this.rv.updateSetting(item.key, v);
                    valSpan.textContent = v.toFixed(2);
                });
                // Prevent menu close on slider interaction
                el.addEventListener('click', (e) => e.stopPropagation());
                dd.appendChild(el);
                continue;
            }
        }
        return dd;
    }

    _closeAll() {
        for (const dd of document.querySelectorAll('.menu-dropdown')) {
            if (!dd.classList.contains('submenu')) {
                dd.style.display = 'none';
            }
        }
        this.openMenu = null;
    }

    _setupClickAway() {
        document.addEventListener('click', () => this._closeAll());
    }

    // ── Data file pickers ──

    async _refreshDataStatus() {
        try {
            const [gdRes, slopRes] = await Promise.all([
                fetch('/api/gamedata/status').then(r => r.json()),
                fetch('/api/slop/stats').then(r => r.json()),
            ]);
            const mdbEl = document.getElementById('mdb-status');
            if (mdbEl) {
                if (gdRes.loaded) {
                    const src = gdRes.mdb_source?.name || 'cached';
                    const total = Object.values(gdRes.counts).reduce((a,b) => a+b, 0);
                    mdbEl.querySelector('.status-text').textContent = `MDB: ${src} (${total.toLocaleString()} entries)`;
                } else {
                    mdbEl.querySelector('.status-text').textContent = 'MDB: not loaded';
                }
            }
            const slopEl = document.getElementById('slop-status');
            if (slopEl) {
                const n = slopRes.total_assets || 0;
                const f = slopRes.files || 0;
                slopEl.querySelector('.status-text').textContent = `SLOP: ${n.toLocaleString()} assets from ${f} file(s)`;
            }
        } catch (e) {
            console.error('Failed to refresh data status:', e);
        }
    }

    async _showMDBPicker() {
        const existing = document.getElementById('data-picker-overlay');
        if (existing) existing.remove();

        const overlay = document.createElement('div');
        overlay.id = 'data-picker-overlay';
        overlay.className = 'data-picker-overlay';

        const dialog = document.createElement('div');
        dialog.className = 'data-picker-dialog';

        dialog.innerHTML = `
            <div class="data-picker-header">
                <span>Select MDB File</span>
                <span class="data-picker-close">&times;</span>
            </div>
            <div class="data-picker-body">
                <div class="data-picker-status">Scanning for .mdb files...</div>
                <div class="data-picker-list"></div>
                <div class="data-picker-custom">
                    <input type="text" placeholder="Or enter path: /path/to/database.mdb" class="data-picker-input">
                    <button class="data-picker-btn">Load</button>
                </div>
            </div>
        `;

        overlay.appendChild(dialog);
        document.body.appendChild(overlay);

        overlay.querySelector('.data-picker-close').onclick = () => overlay.remove();
        overlay.addEventListener('click', (e) => { if (e.target === overlay) overlay.remove(); });

        // Scan for files
        const listEl = dialog.querySelector('.data-picker-list');
        const statusEl = dialog.querySelector('.data-picker-status');
        try {
            const res = await fetch('/api/gamedata/scan-mdb');
            const data = await res.json();
            if (data.files.length === 0) {
                statusEl.textContent = 'No .mdb files found in common locations.';
            } else {
                statusEl.textContent = `Found ${data.files.length} file(s):`;
                for (const f of data.files) {
                    const row = document.createElement('div');
                    row.className = 'data-picker-row';
                    row.innerHTML = `<span class="data-picker-name">${f.name}</span><span class="data-picker-detail">${f.dir} (${f.size_mb} MB)</span>`;
                    row.addEventListener('click', () => this._loadMDB(f.path, overlay));
                    listEl.appendChild(row);
                }
            }
        } catch (e) {
            statusEl.textContent = 'Error scanning for files.';
        }

        // Custom path
        const input = dialog.querySelector('.data-picker-input');
        const btn = dialog.querySelector('.data-picker-btn');
        btn.onclick = () => {
            const path = input.value.trim();
            if (path) this._loadMDB(path, overlay);
        };
        input.addEventListener('keydown', (e) => {
            if (e.key === 'Enter') btn.click();
        });
    }

    async _loadMDB(path, overlay) {
        const statusEl = overlay.querySelector('.data-picker-status');
        statusEl.textContent = 'Loading MDB... (exporting gamedata, this may take a moment)';
        overlay.querySelectorAll('.data-picker-row').forEach(r => r.style.pointerEvents = 'none');

        try {
            const res = await fetch('/api/gamedata/load-mdb', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ path }),
            });
            const data = await res.json();
            if (data.ok) {
                const total = Object.values(data.counts).reduce((a,b) => a+b, 0);
                statusEl.textContent = `Loaded! ${total.toLocaleString()} entries.`;
                this._refreshDataStatus();
                setTimeout(() => overlay.remove(), 1500);
            } else {
                statusEl.textContent = `Error: ${data.error || 'Unknown error'}`;
            }
        } catch (e) {
            statusEl.textContent = `Error: ${e.message}`;
        }
    }

    async _showSLOPPicker() {
        const existing = document.getElementById('data-picker-overlay');
        if (existing) existing.remove();

        const overlay = document.createElement('div');
        overlay.id = 'data-picker-overlay';
        overlay.className = 'data-picker-overlay';

        const dialog = document.createElement('div');
        dialog.className = 'data-picker-dialog';

        dialog.innerHTML = `
            <div class="data-picker-header">
                <span>Select SLOP File</span>
                <span class="data-picker-close">&times;</span>
            </div>
            <div class="data-picker-body">
                <div class="data-picker-status">Scanning for .slop files...</div>
                <div class="data-picker-list"></div>
                <div class="data-picker-custom">
                    <input type="text" placeholder="Or enter path: /path/to/assets.slop" class="data-picker-input">
                    <button class="data-picker-btn">Load</button>
                </div>
            </div>
        `;

        overlay.appendChild(dialog);
        document.body.appendChild(overlay);

        overlay.querySelector('.data-picker-close').onclick = () => overlay.remove();
        overlay.addEventListener('click', (e) => { if (e.target === overlay) overlay.remove(); });

        const listEl = dialog.querySelector('.data-picker-list');
        const statusEl = dialog.querySelector('.data-picker-status');
        try {
            const res = await fetch('/api/slop/scan');
            const data = await res.json();
            if (data.files.length === 0) {
                statusEl.textContent = 'No .slop files found.';
            } else {
                statusEl.textContent = `Found ${data.files.length} file(s):`;
                for (const f of data.files) {
                    const row = document.createElement('div');
                    row.className = 'data-picker-row' + (f.loaded ? ' loaded' : '');
                    row.innerHTML = `<span class="data-picker-name">${f.name}${f.loaded ? ' ✓' : ''}</span><span class="data-picker-detail">${f.dir} (${f.size_mb} MB)</span>`;
                    if (!f.loaded) {
                        row.addEventListener('click', () => this._loadSLOP(f.path, overlay));
                    } else {
                        row.title = 'Already loaded';
                        row.style.opacity = '0.6';
                    }
                    listEl.appendChild(row);
                }
            }
        } catch (e) {
            statusEl.textContent = 'Error scanning for files.';
        }

        const input = dialog.querySelector('.data-picker-input');
        const btn = dialog.querySelector('.data-picker-btn');
        btn.onclick = () => {
            const path = input.value.trim();
            if (path) this._loadSLOP(path, overlay);
        };
        input.addEventListener('keydown', (e) => {
            if (e.key === 'Enter') btn.click();
        });
    }

    async _loadSLOP(path, overlay) {
        const statusEl = overlay.querySelector('.data-picker-status');
        statusEl.textContent = 'Loading SLOP file...';

        try {
            const res = await fetch('/api/slop/load-path', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ path }),
            });
            const data = await res.json();
            if (data.ok) {
                statusEl.textContent = `Loaded ${data.loaded}: ${data.entries} entries.`;
                this._refreshDataStatus();
                setTimeout(() => overlay.remove(), 1500);
            } else {
                statusEl.textContent = `Error: ${data.error || 'Unknown error'}`;
            }
        } catch (e) {
            statusEl.textContent = `Error: ${e.message}`;
        }
    }
}
