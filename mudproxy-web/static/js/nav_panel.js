// ── Navigation Panel (Goto / Loop / Path) ──
// Right-click context menu on room canvas opens this panel.
// Tree view with collapsible categories, matching MegaMud's Goto/Roam dialogs.

class NavPanel {
    constructor(sendCommandFn) {
        this._sendCommand = sendCommandFn;
        this._data = null;
        this._visible = false;
        this._tab = 'goto';    // 'goto' | 'loop'
        this._showHidden = false;
        this._search = '';
        this._expanded = {};   // category -> bool
        this._el = null;
        this._recent = JSON.parse(localStorage.getItem('navRecent') || '[]'); // [{name, code, type, ts}]
        this._setupContextMenu();
    }

    _setupContextMenu() {
        // Right-click anywhere on the page background (canvas, overlays, etc.)
        // We listen on document and only trigger when the click isn't on a UI element
        document.addEventListener('contextmenu', (e) => {
            // Skip if clicking on actual UI controls (menus, panels, inputs, buttons, thumbs)
            const tag = e.target.tagName.toLowerCase();
            const skip = e.target.closest('#header, #side-panel, .menu-dropdown, .nav-panel-overlay, ' +
                '.context-menu, .thumb, .char-panel, .chat-panel, .mud-terminal-container, ' +
                '.data-picker-overlay, .thumb-tooltip');
            if (skip) return;
            if (tag === 'input' || tag === 'textarea' || tag === 'button' || tag === 'select') return;

            e.preventDefault();
            this._showCanvasContextMenu(e.clientX, e.clientY);
        });
    }

    _showCanvasContextMenu(x, y) {
        // Remove any existing context menus
        document.querySelectorAll('.context-menu').forEach(m => m.remove());

        const menu = document.createElement('div');
        menu.className = 'context-menu';
        menu.style.left = `${x}px`;
        menu.style.top = `${y}px`;

        const items = [
            ['Goto...', () => this.open('goto')],
            ['Loop...', () => this.open('loop')],
        ];

        for (const [label, action] of items) {
            const item = document.createElement('div');
            item.className = 'context-menu-item';
            item.textContent = label;
            item.onclick = () => {
                menu.remove();
                action();
            };
            menu.appendChild(item);
        }

        // Recent submenu
        if (this._recent.length > 0) {
            const sep = document.createElement('div');
            sep.className = 'context-menu-sep';
            menu.appendChild(sep);

            const recentItem = document.createElement('div');
            recentItem.className = 'context-menu-item has-submenu';
            recentItem.innerHTML = `<span>Recent</span><span class="submenu-arrow">&#x25B6;</span>`;

            const sub = document.createElement('div');
            sub.className = 'context-menu context-submenu';

            for (const r of this._recent.slice(0, 12)) {
                const icon = r.type === 'loop' ? '\u{1f501}' : '\u{1f4cd}';
                const si = document.createElement('div');
                si.className = 'context-menu-item';
                si.textContent = `${icon} ${r.name}`;
                si.onclick = () => {
                    menu.remove();
                    this._gotoRoom(r.code, r.name, r.type);
                };
                sub.appendChild(si);
            }

            recentItem.appendChild(sub);
            recentItem.addEventListener('mouseenter', () => {
                sub.style.display = 'block';
                // Position submenu to the right
                const rect = recentItem.getBoundingClientRect();
                sub.style.left = `${rect.width - 2}px`;
                sub.style.top = '0px';
            });
            recentItem.addEventListener('mouseleave', () => {
                sub.style.display = 'none';
            });
            menu.appendChild(recentItem);
        }

        document.body.appendChild(menu);
    }

    async open(tab) {
        this._tab = tab || 'goto';
        if (!this._data) {
            await this._fetchData();
        }
        if (this._visible) {
            this._switchTab(this._tab);
            return;
        }
        this._visible = true;
        this._render();
    }

    close() {
        if (this._el) {
            this._el.remove();
            this._el = null;
        }
        this._visible = false;
    }

    async _fetchData() {
        try {
            const res = await fetch('/api/megamud/nav');
            if (res.ok) {
                this._data = await res.json();
            } else {
                this._data = { rooms: {}, hidden_rooms: [], loops: {}, paths: {}, stats: {} };
            }
        } catch (e) {
            console.error('Failed to fetch nav data:', e);
            this._data = { rooms: {}, hidden_rooms: [], loops: {}, paths: {}, stats: {} };
        }
    }

    _render() {
        // Remove existing
        if (this._el) this._el.remove();

        const overlay = document.createElement('div');
        overlay.className = 'nav-panel-overlay';
        overlay.addEventListener('click', (e) => {
            if (e.target === overlay) this.close();
        });

        const dialog = document.createElement('div');
        dialog.className = 'nav-panel-dialog';

        // Header with title bar
        const header = document.createElement('div');
        header.className = 'nav-panel-header';
        header.innerHTML = `
            <span class="nav-panel-title">${this._tabTitle()}</span>
            <span class="nav-panel-close">&times;</span>
        `;
        header.querySelector('.nav-panel-close').onclick = () => this.close();
        dialog.appendChild(header);

        // Tab bar
        const tabBar = document.createElement('div');
        tabBar.className = 'nav-panel-tabs';
        for (const [key, label] of [['goto', 'Goto'], ['loop', 'Loop']]) {
            const tab = document.createElement('div');
            tab.className = 'nav-panel-tab' + (this._tab === key ? ' active' : '');
            tab.textContent = label;
            tab.onclick = () => this._switchTab(key);
            tabBar.appendChild(tab);
        }
        dialog.appendChild(tabBar);

        // Toolbar: search + hidden checkbox
        const toolbar = document.createElement('div');
        toolbar.className = 'nav-panel-toolbar';

        const searchInput = document.createElement('input');
        searchInput.type = 'text';
        searchInput.className = 'nav-panel-search';
        searchInput.placeholder = 'Search...';
        searchInput.value = this._search;
        searchInput.addEventListener('input', (e) => {
            this._search = e.target.value.toLowerCase();
            this._rebuildTree();
        });
        toolbar.appendChild(searchInput);

        if (this._tab === 'goto') {
            const hiddenLabel = document.createElement('label');
            hiddenLabel.className = 'nav-panel-hidden-toggle';
            const cb = document.createElement('input');
            cb.type = 'checkbox';
            cb.checked = this._showHidden;
            cb.onchange = () => {
                this._showHidden = cb.checked;
                this._rebuildTree();
            };
            hiddenLabel.appendChild(cb);
            hiddenLabel.appendChild(document.createTextNode(' Hidden'));
            toolbar.appendChild(hiddenLabel);
        }

        dialog.appendChild(toolbar);

        // Tree container
        const tree = document.createElement('div');
        tree.className = 'nav-panel-tree';
        tree.id = 'nav-panel-tree';
        dialog.appendChild(tree);

        // Stats footer
        const footer = document.createElement('div');
        footer.className = 'nav-panel-footer';
        const s = this._data.stats || {};
        if (this._tab === 'goto') {
            footer.textContent = `${s.visible_rooms || 0} rooms in ${s.categories || 0} areas` +
                (this._showHidden ? ` (+${s.hidden_rooms || 0} hidden)` : '');
        } else {
            footer.textContent = `${s.loops || 0} loops`;
        }
        dialog.appendChild(footer);

        overlay.appendChild(dialog);
        document.body.appendChild(overlay);
        this._el = overlay;

        this._rebuildTree();
        searchInput.focus();
    }

    _tabTitle() {
        if (this._tab === 'goto') return 'Goto location...';
        return 'Roam area...';
    }

    _switchTab(tab) {
        this._tab = tab;
        this._search = '';
        this._render();
    }

    _rebuildTree() {
        const tree = document.getElementById('nav-panel-tree');
        if (!tree) return;
        tree.innerHTML = '';

        if (this._tab === 'goto') {
            this._buildRoomTree(tree);
        } else {
            this._buildLoopTree(tree, this._data.loops || {});
        }
    }

    _buildRoomTree(container) {
        const rooms = this._data.rooms || {};
        const hidden = this._data.hidden_rooms || [];
        const search = this._search;

        // Merge hidden rooms into categories if checkbox is on
        let mergedCategories = {};
        for (const [cat, list] of Object.entries(rooms)) {
            mergedCategories[cat] = list.map(r => ({ ...r, hidden: false }));
        }
        if (this._showHidden) {
            for (const r of hidden) {
                const cat = r.category || 'Uncategorized';
                if (!mergedCategories[cat]) mergedCategories[cat] = [];
                mergedCategories[cat].push({ ...r, hidden: true });
            }
            // Re-sort each category
            for (const cat of Object.keys(mergedCategories)) {
                mergedCategories[cat].sort((a, b) => a.name.localeCompare(b.name));
            }
        }

        const sortedCats = Object.keys(mergedCategories).sort();

        for (const cat of sortedCats) {
            let roomList = mergedCategories[cat];

            // Filter by search
            if (search) {
                roomList = roomList.filter(r =>
                    r.name.toLowerCase().includes(search) ||
                    r.code.toLowerCase().includes(search) ||
                    cat.toLowerCase().includes(search)
                );
                if (roomList.length === 0) continue;
            }

            const catEl = this._buildCategoryNode(cat, roomList.length, search !== '');
            container.appendChild(catEl);

            const childrenEl = catEl.querySelector('.nav-tree-children');
            for (const room of roomList) {
                const roomEl = document.createElement('div');
                roomEl.className = 'nav-tree-room' + (room.hidden ? ' nav-hidden-room' : '');
                roomEl.innerHTML = `<span class="nav-room-icon">${room.hidden ? '&#x1f441;' : '&#x1f4cd;'}</span>` +
                    `<span class="nav-room-name">${this._highlight(room.name, search)}</span>` +
                    `<span class="nav-room-code">${room.code}</span>`;
                roomEl.addEventListener('click', () => this._gotoRoom(room.code, room.name, 'goto'));
                roomEl.addEventListener('dblclick', () => {
                    this._gotoRoom(room.code, room.name, 'goto');
                    this.close();
                });
                childrenEl.appendChild(roomEl);
            }
        }

        if (container.children.length === 0) {
            const empty = document.createElement('div');
            empty.className = 'nav-panel-empty';
            empty.textContent = search ? 'No matching rooms.' : 'No room data loaded.';
            container.appendChild(empty);
        }
    }

    _buildLoopTree(container, data) {
        const search = this._search;
        const sortedCats = Object.keys(data).sort();

        for (const cat of sortedCats) {
            let entries = data[cat];

            if (search) {
                entries = entries.filter(e =>
                    e.name.toLowerCase().includes(search) ||
                    (e.creator && e.creator.toLowerCase().includes(search)) ||
                    cat.toLowerCase().includes(search) ||
                    e.start_room.toLowerCase().includes(search)
                );
                if (entries.length === 0) continue;
            }

            const catEl = this._buildCategoryNode(cat, entries.length, search !== '');
            container.appendChild(catEl);

            const childrenEl = catEl.querySelector('.nav-tree-children');
            for (const entry of entries) {
                const el = document.createElement('div');
                el.className = 'nav-tree-room nav-tree-loop';
                const steps = entry.steps ? `${entry.steps} steps` : '';
                el.innerHTML = `<span class="nav-room-icon">&#x1f501;</span>` +
                    `<span class="nav-room-name">${this._highlight(entry.name, search)}</span>` +
                    (steps ? ` <span class="nav-loop-steps">${steps}</span>` : '');
                // Ctrl+hover tooltip with full details
                el.addEventListener('mouseenter', (ev) => {
                    if (ev.ctrlKey) this._showLoopTip(el, entry);
                });
                el.addEventListener('mousemove', (ev) => {
                    if (ev.ctrlKey && !el.querySelector('.nav-loop-tip')) {
                        this._showLoopTip(el, entry);
                    } else if (!ev.ctrlKey) {
                        const tip = el.querySelector('.nav-loop-tip');
                        if (tip) tip.remove();
                    }
                });
                el.addEventListener('mouseleave', () => {
                    const tip = el.querySelector('.nav-loop-tip');
                    if (tip) tip.remove();
                });
                el.addEventListener('click', () => this._gotoRoom(entry.start_code, entry.name, this._tab, entry.file));
                el.addEventListener('dblclick', () => {
                    this._gotoRoom(entry.start_code, entry.name, this._tab, entry.file);
                    this.close();
                });
                childrenEl.appendChild(el);
            }
        }

        if (container.children.length === 0) {
            const empty = document.createElement('div');
            empty.className = 'nav-panel-empty';
            empty.textContent = search ? 'No matching entries.' : 'No data loaded.';
            container.appendChild(empty);
        }
    }

    _buildCategoryNode(name, count, forceOpen) {
        const isExpanded = forceOpen || this._expanded[name] || false;

        const cat = document.createElement('div');
        cat.className = 'nav-tree-category';

        const header = document.createElement('div');
        header.className = 'nav-tree-cat-header';
        header.innerHTML = `<span class="nav-tree-arrow">${isExpanded ? '&#x25BC;' : '&#x25B6;'}</span>` +
            `<span class="nav-tree-cat-icon">&#x1f4c1;</span>` +
            `<span class="nav-tree-cat-name">${name}</span>` +
            `<span class="nav-tree-cat-count">(${count})</span>`;

        const children = document.createElement('div');
        children.className = 'nav-tree-children';
        children.style.display = isExpanded ? 'block' : 'none';

        header.addEventListener('click', () => {
            const open = children.style.display === 'none';
            children.style.display = open ? 'block' : 'none';
            header.querySelector('.nav-tree-arrow').innerHTML = open ? '&#x25BC;' : '&#x25B6;';
            this._expanded[name] = open;
        });

        cat.appendChild(header);
        cat.appendChild(children);
        return cat;
    }

    _highlight(text, search) {
        if (!search) return text;
        const idx = text.toLowerCase().indexOf(search);
        if (idx === -1) return text;
        return text.substring(0, idx) +
            `<mark>${text.substring(idx, idx + search.length)}</mark>` +
            text.substring(idx + search.length);
    }

    _showLoopTip(el, entry) {
        if (el.querySelector('.nav-loop-tip')) return;
        const tip = document.createElement('div');
        tip.className = 'nav-loop-tip';
        const lines = [];
        if (entry.name) lines.push(entry.name);
        if (entry.start_room) lines.push(`Starts: ${entry.start_room}`);
        if (entry.creator) lines.push(`Author: ${entry.creator}`);
        if (entry.steps) lines.push(`${entry.steps} steps`);
        tip.textContent = lines.join(' \u2022 ');
        el.appendChild(tip);
    }

    _gotoRoom(code, name, type, file) {
        // Send via ghost character telepath so MegaMud processes the @ command
        const ghostName = localStorage.getItem('ghostName') || 'Yoder';
        let atCmd;
        if (type === 'loop') {
            // @loop takes the .mp filename
            atCmd = `@loop ${file || code}`;
        } else {
            // @goto takes the full room name
            atCmd = `@goto ${name}`;
        }
        this._sendCommand('ghost', { name: ghostName, at_cmd: atCmd });
        // Track in recent list
        if (name && code) {
            this._recent = this._recent.filter(r => r.code !== code);
            this._recent.unshift({ name, code, type: type || 'goto', ts: Date.now() });
            if (this._recent.length > 20) this._recent.length = 20;
            try { localStorage.setItem('navRecent', JSON.stringify(this._recent)); } catch {}
        }
    }
}
