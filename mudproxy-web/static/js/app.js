// ── WebSocket connection and event routing ──

let ws = null;
let gameState = {};
let logFilter = 'combat'; // 'combat', 'chat', 'all'

// These are set by the init script in index.html
// let roomView, combatFx, viewMenu;

function connectWS() {
    const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
    ws = new WebSocket(`${proto}//${location.host}/ws`);

    ws.onopen = () => {
        console.log('WebSocket connected');
        addLog('Connected to proxy', 'system');
    };

    ws.onclose = () => {
        console.log('WebSocket disconnected, reconnecting...');
        addLog('Disconnected from proxy, reconnecting...', 'system');
        setTimeout(connectWS, 2000);
    };

    ws.onmessage = (e) => {
        const msg = JSON.parse(e.data);
        handleEvent(msg);
    };
}

function sendCommand(cmd, data) {
    if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify({ command: cmd, ...data }));
    }
}

function handleEvent(msg) {
    switch (msg.type) {
        case 'state_sync':
            gameState = msg;
            updateHeader(msg);
            if (msg.room_name) updateRoom(msg);
            updateHP(msg.hp, msg.max_hp, msg.mana, msg.max_mana);
            updateConnection(msg.connected ? 'connected' : 'disconnected');
            break;

        case 'room_update':
            updateRoom(msg);
            break;

        case 'hp_update':
            updateHP(msg.hp, msg.max_hp, msg.mana, msg.max_mana);
            break;

        case 'combat':
            addLog(msg.line, 'combat');
            if (msg.target) gameState.combat_target = msg.target;
            if (msg.monster_hp) updateMonsterHP(msg.monster_hp);
            // Floating damage numbers over the target monster thumbnail
            if (typeof combatFx !== 'undefined') {
                combatFx.parseCombatLine(msg.line, msg.target);
            }
            break;

        case 'death':
            addLog('You have died!', 'death');
            break;

        case 'xp_gain':
            addLog(`+${msg.amount.toLocaleString()} XP`, 'xp');
            // XP gain usually means the combat target just died — shatter it
            if (typeof combatFx !== 'undefined' && gameState.combat_target) {
                const targetEl = combatFx._findTargetThumb(gameState.combat_target);
                if (targetEl) combatFx.spawnShatter(targetEl);
            }
            break;

        case 'chat':
            const cls = `chat-${msg.channel || 'say'}`;
            const prefix = msg.channel ? `[${msg.channel}] ` : '';
            addLog(`${prefix}${msg.sender}: ${msg.message}`, cls);
            break;

        case 'connection':
            updateConnection(msg.status);
            addLog(`Connection: ${msg.status}`, 'system');
            break;

        case 'char_name':
            document.title = `MajorSLOP! - ${msg.name}`;
            if (typeof charPanel !== 'undefined') charPanel.setCharName(msg.name);
            break;
    }
}

// ── UI updates ──

function updateHeader(state) {
    document.getElementById('room-name').textContent = state.room_name || 'Not Connected';
}

function updateConnection(status) {
    const dot = document.getElementById('connection-status');
    dot.className = `status-dot ${status === 'connected' ? 'connected' : 'disconnected'}`;
}

function updateHP(hp, maxHp, mana, maxMana) {
    const hpFrac = maxHp > 0 ? hp / maxHp : 0;
    const manaFrac = maxMana > 0 ? mana / maxMana : 0;

    const hpFill = document.getElementById('hp-fill');
    hpFill.style.width = `${hpFrac * 100}%`;
    if (hpFrac > 0.5) hpFill.style.background = 'var(--hp-green)';
    else if (hpFrac > 0.25) hpFill.style.background = 'var(--hp-yellow)';
    else hpFill.style.background = 'var(--hp-red)';

    document.getElementById('hp-text').textContent = `${hp}/${maxHp}`;

    const manaFill = document.getElementById('mana-fill');
    manaFill.style.width = `${manaFrac * 100}%`;
    document.getElementById('mana-text').textContent = `${mana}/${maxMana}`;
}

function updateRoom(data) {
    document.getElementById('room-name').textContent = data.name || data.room_name || '';
    document.getElementById('room-description').textContent = data.description || '';

    // Exits
    const exitsEl = document.getElementById('room-exits');
    exitsEl.innerHTML = (data.exits || [])
        .map(e => `<span>${e}</span>`)
        .join(' ');

    // Room image via parallax renderer
    if (data.room_image_key && typeof roomView !== 'undefined') {
        roomView.loadRoom(data.room_image_key, data.depth_key);
    }

    // Entity thumbnails
    updateThumbnails('npc-thumbs', data.entities || []);
    updateThumbnails('item-thumbs', data.items || []);
}

function updateThumbnails(containerId, entities) {
    const container = document.getElementById(containerId);

    // Clean up old coin canvases
    if (typeof coinRenderer !== 'undefined') coinRenderer.cleanup();
    container.innerHTML = '';

    const isItem = containerId === 'item-thumbs';

    for (const ent of entities) {
        const div = document.createElement('div');
        div.className = isItem ? 'thumb item-thumb' : 'thumb';
        div.title = ent.name + (ent.quantity > 1 ? ` (${ent.quantity})` : '');
        div.dataset.entityName = ent.name;
        div.dataset.entityType = ent.type;
        if (ent.key) div.dataset.entityKey = ent.key;

        // Currency items: procedural coin pile
        if (ent.currency && typeof coinRenderer !== 'undefined') {
            div.classList.add('coin-pile');
            div.dataset.currency = ent.currency;
            div.dataset.quantity = ent.quantity || 1;

            // Coin piles get 52x52 (overridden in CSS too)
            const pileCanvas = coinRenderer.createPile(ent.currency, ent.quantity || 1, 52, 52);
            div.appendChild(pileCanvas);

            // Double-click to loot this specific currency pile
            div.addEventListener('dblclick', (e) => {
                e.preventDefault();
                e.stopPropagation();
                const qty = ent.quantity || 1;
                sendCommand('inject', { text: `g ${qty} ${ent.name}` });
            });
        } else if (ent.key) {
            const img = document.createElement('img');
            img.src = `/api/asset/${encodeURIComponent(ent.key)}`;
            img.loading = 'lazy';
            div.appendChild(img);
        }

        // HP bar for NPCs
        if (ent.type === 'npc') {
            const hpBar = document.createElement('div');
            hpBar.className = 'thumb-hp';
            const hpFill = document.createElement('div');
            hpFill.className = 'thumb-hp-fill';
            const frac = ent.hp_fraction != null ? ent.hp_fraction : 1.0;
            hpFill.style.width = `${frac * 100}%`;
            hpFill.style.background = frac > 0.5 ? 'var(--hp-green)' :
                                       frac > 0.25 ? 'var(--hp-yellow)' : 'var(--hp-red)';
            hpBar.appendChild(hpFill);
            div.appendChild(hpBar);
        }

        // Name label (skip for coins — quantity is drawn on canvas)
        if (!ent.currency) {
            const nameEl = document.createElement('div');
            nameEl.className = 'thumb-name';
            nameEl.textContent = ent.name;
            div.appendChild(nameEl);
        }

        // Hover tooltip
        div.addEventListener('mouseenter', () => {
            if (_isDraggingPanel) return;
            showTooltip(div, ent);
        });
        div.addEventListener('mouseleave', () => {
            hideTooltip();
        });

        // Context menu
        div.addEventListener('contextmenu', (e) => {
            e.preventDefault();
            showContextMenu(e.clientX, e.clientY, ent);
        });

        container.appendChild(div);
    }

    // Apply scale setting
    if (typeof roomView !== 'undefined') {
        const scaleKey = isItem ? 'lootThumbScale' : 'npcThumbScale';
        roomView._applyThumbScale(containerId, roomView.settings[scaleKey] || '100%');
    }
}

// ── Hover stats tooltip ──

let _tooltip = null;
let _isDraggingPanel = false;

function showTooltip(thumbEl, entity) {
    hideTooltip();

    const rect = thumbEl.getBoundingClientRect();
    const tip = document.createElement('div');
    tip.className = 'thumb-tooltip';

    // Name
    const nameEl = document.createElement('div');
    nameEl.className = 'tt-name';
    nameEl.textContent = entity.name;
    tip.appendChild(nameEl);

    // HP bar (current/max) — get current fraction from the thumb's HP bar
    const hpFillEl = thumbEl.querySelector('.thumb-hp-fill');
    const hpFrac = hpFillEl ? parseFloat(hpFillEl.style.width) / 100 : 1.0;
    const maxHP = entity.stats?.HP || 0;
    const curHP = Math.round(hpFrac * maxHP);

    if (maxHP > 0) {
        const hpRow = document.createElement('div');
        hpRow.className = 'tt-hp';
        const bar = document.createElement('div');
        bar.className = 'tt-hp-bar';
        const fill = document.createElement('div');
        fill.className = 'tt-hp-fill';
        fill.style.width = `${hpFrac * 100}%`;
        fill.style.background = hpFrac > 0.5 ? 'var(--hp-green)' :
                                 hpFrac > 0.25 ? 'var(--hp-yellow)' : 'var(--hp-red)';
        bar.appendChild(fill);
        const txt = document.createElement('span');
        txt.className = 'tt-hp-text';
        txt.textContent = `${curHP.toLocaleString()} / ${maxHP.toLocaleString()}`;
        hpRow.appendChild(bar);
        hpRow.appendChild(txt);
        tip.appendChild(hpRow);
    }

    // Stats rows
    if (entity.stats) {
        const s = entity.stats;
        const rows = [];
        if (s.EXP) rows.push(['EXP', s.EXP.toLocaleString()]);
        if (s.ArmourClass != null) rows.push(['AC', s.ArmourClass]);
        if (s.DamageResist != null) rows.push(['DR', s.DamageResist]);
        if (s.MagicRes != null) rows.push(['MR', s.MagicRes]);
        if (s.HPRegen) rows.push(['Regen', `${s.HPRegen}/tick`]);
        if (s.AvgDmg) rows.push(['Avg Dmg', s.AvgDmg]);
        if (s.attacks && s.attacks.length > 0) rows.push(['Attacks', s.attacks.length]);
        const alignMap = {0:'Neutral',1:'Lawful',2:'Neutral',3:'Evil','-1':'Good'};
        if (s.Align !== undefined) rows.push(['Align', alignMap[s.Align] || s.Align]);

        for (const [label, value] of rows) {
            const row = document.createElement('div');
            row.className = 'tt-row';
            row.innerHTML = `<span class="tt-label">${label}</span><span class="tt-value">${value}</span>`;
            tip.appendChild(row);
        }

        // Tags section (undead, special abilities, etc.)
        const tags = [];
        if (s.Undead) tags.push('Undead');
        if (s.Poisonous) tags.push('Poisonous');
        if (s.StealPercent) tags.push('Thief');
        if (s.Charm) tags.push('Charms');

        if (tags.length > 0) {
            const sec = document.createElement('div');
            sec.className = 'tt-section';
            sec.innerHTML = tags.map(t => `<span class="tt-tag">${t}</span>`).join('');
            tip.appendChild(sec);
        }

        // Drops section if available
        if (entity.drops && entity.drops.length > 0) {
            const sec = document.createElement('div');
            sec.className = 'tt-section';
            sec.innerHTML = '<div class="tt-label" style="margin-bottom:2px">Drops:</div>' +
                entity.drops.slice(0, 5).map(d => `<span class="tt-tag">${d.item || d}</span>`).join('');
            if (entity.drops.length > 5) sec.innerHTML += `<span class="tt-tag">+${entity.drops.length - 5} more</span>`;
            tip.appendChild(sec);
        }
    } else if (entity.currency) {
        const ct = COIN_TYPES ? COIN_TYPES[entity.currency] : null;
        const rows = [
            ['Type', ct ? ct.label : entity.currency],
            ['Quantity', (entity.quantity || 1).toLocaleString()],
        ];
        // Show conversion context
        const q = entity.quantity || 1;
        if (entity.currency === 'copper' && q >= 10)
            rows.push(['Worth', `${Math.floor(q/10).toLocaleString()} silver`]);
        else if (entity.currency === 'silver' && q >= 10)
            rows.push(['Worth', `${Math.floor(q/10).toLocaleString()} gold`]);
        else if (entity.currency === 'gold' && q >= 100)
            rows.push(['Worth', `${Math.floor(q/100).toLocaleString()} platinum`]);
        else if (entity.currency === 'platinum' && q >= 100)
            rows.push(['Worth', `${Math.floor(q/100).toLocaleString()} runic`]);

        for (const [label, value] of rows) {
            const row = document.createElement('div');
            row.className = 'tt-row';
            row.innerHTML = `<span class="tt-label">${label}</span><span class="tt-value">${value}</span>`;
            tip.appendChild(row);
        }
    } else if (entity.type === 'item' && entity.item_data) {
        const d = entity.item_data;
        const rows = [];
        if (d.Type) rows.push(['Type', d.Type]);
        if (d.MinDam || d.MaxDam) rows.push(['Damage', `${d.MinDam || 0}-${d.MaxDam || 0}`]);
        if (d.ArmourClass) rows.push(['AC', d.ArmourClass]);
        if (d.DamageResist) rows.push(['DR', d.DamageResist]);
        if (d.Weight) rows.push(['Weight', d.Weight]);
        if (d.Cost) rows.push(['Value', d.Cost.toLocaleString()]);
        for (const [label, value] of rows) {
            const row = document.createElement('div');
            row.className = 'tt-row';
            row.innerHTML = `<span class="tt-label">${label}</span><span class="tt-value">${value}</span>`;
            tip.appendChild(row);
        }
    }

    // Position above thumbnail
    document.body.appendChild(tip);
    const tipRect = tip.getBoundingClientRect();
    let x = rect.left + rect.width / 2 - tipRect.width / 2;
    let y = rect.top - tipRect.height - 8;

    if (y < 4) {
        x = rect.right + 8;
        y = rect.top;
    }
    if (x < 4) x = 4;
    if (x + tipRect.width > window.innerWidth - 4) x = window.innerWidth - tipRect.width - 4;

    tip.style.left = `${x}px`;
    tip.style.top = `${y}px`;
    _tooltip = tip;
}

function hideTooltip() {
    if (_tooltip) {
        if (_tooltip.parentNode) _tooltip.parentNode.removeChild(_tooltip);
        _tooltip = null;
    }
}

function updateMonsterHP(hpMap) {
    const thumbs = document.querySelectorAll('#npc-thumbs .thumb');
    for (const thumb of thumbs) {
        const name = thumb.title;
        if (name in hpMap) {
            const fill = thumb.querySelector('.thumb-hp-fill');
            if (fill) {
                const frac = hpMap[name];
                fill.style.width = `${frac * 100}%`;
                fill.style.background = frac > 0.5 ? 'var(--hp-green)' :
                                         frac > 0.25 ? 'var(--hp-yellow)' : 'var(--hp-red)';
            }
        }
    }
}

// ── Log ──

function addLog(text, cls) {
    const body = document.getElementById('log-body');
    const line = document.createElement('div');
    line.className = `log-line log-${cls || 'system'}`;

    const now = new Date();
    const ts = `${now.getHours().toString().padStart(2,'0')}:${now.getMinutes().toString().padStart(2,'0')}`;
    line.textContent = `[${ts}] ${text}`;

    // Filter
    if (logFilter === 'combat' && !['combat', 'death', 'xp'].includes(cls)) {
        line.style.display = 'none';
    } else if (logFilter === 'chat' && !cls.startsWith('chat-')) {
        line.style.display = 'none';
    }

    body.appendChild(line);

    // Auto-scroll
    if (body.scrollHeight - body.scrollTop - body.clientHeight < 100) {
        body.scrollTop = body.scrollHeight;
    }

    // Limit log lines
    while (body.children.length > 500) {
        body.removeChild(body.firstChild);
    }
}

// ── Tabs ──

document.addEventListener('click', (e) => {
    if (e.target.classList.contains('tab')) {
        const tabs = e.target.parentElement.querySelectorAll('.tab');
        tabs.forEach(t => t.classList.remove('active'));
        e.target.classList.add('active');
        logFilter = e.target.dataset.tab;

        // Re-filter existing lines
        const lines = document.querySelectorAll('#log-body .log-line');
        for (const line of lines) {
            if (logFilter === 'all') {
                line.style.display = '';
            } else if (logFilter === 'combat') {
                const show = line.classList.contains('log-combat') ||
                             line.classList.contains('log-death') ||
                             line.classList.contains('log-xp');
                line.style.display = show ? '' : 'none';
            } else if (logFilter === 'chat') {
                const show = Array.from(line.classList).some(c => c.startsWith('log-chat-'));
                line.style.display = show ? '' : 'none';
            }
        }
    }

    // Close context menu on click elsewhere
    const menu = document.querySelector('.context-menu');
    if (menu && !menu.contains(e.target)) {
        menu.remove();
    }
});

// ── Context menu ──

function showContextMenu(x, y, entity) {
    document.querySelectorAll('.context-menu').forEach(m => m.remove());

    const menu = document.createElement('div');
    menu.className = 'context-menu';
    menu.style.left = `${x}px`;
    menu.style.top = `${y}px`;

    let actions;
    if (entity.type === 'npc') {
        actions = [['Look', `look ${entity.name}`], ['Attack', `a ${entity.name}`], ['Backstab', `ba ${entity.name}`]];
    } else if (entity.currency) {
        const qty = entity.quantity || 1;
        actions = [
            ['Loot All', `g ${qty} ${entity.name}`],
            ['Loot 1', `g 1 ${entity.name}`],
        ];
        if (qty >= 100) actions.splice(1, 0, ['Loot 100', `g 100 ${entity.name}`]);
        if (qty >= 10) actions.splice(1, 0, ['Loot 10', `g 10 ${entity.name}`]);
    } else {
        actions = [['Look', `look ${entity.name}`], ['Get', `get ${entity.name}`]];
    }

    for (const [label, cmd] of actions) {
        const item = document.createElement('div');
        item.className = 'context-menu-item';
        item.textContent = label;
        item.onclick = () => {
            sendCommand('inject', { text: cmd });
            menu.remove();
        };
        menu.appendChild(item);
    }

    document.body.appendChild(menu);
}

// ── Draggable floating panels ──

function initDraggablePanels() {
    const npcRow = document.getElementById('npc-thumbs');
    const itemRow = document.getElementById('item-thumbs');
    if (npcRow) _makeDraggable(npcRow, 'npc-panel-pos');
    if (itemRow) _makeDraggable(itemRow, 'item-panel-pos');
}

function _makeDraggable(el, storageKey) {
    let dragging = false;
    let moved = false;
    let startX, startY, origLeft, origTop;

    el.addEventListener('mousedown', (e) => {
        const panel = document.getElementById('entity-panel');
        if (!panel.classList.contains('floating')) return;
        if (e.button !== 0) return;
        // Check lock setting
        if (typeof roomView !== 'undefined') {
            const isNpc = el.id === 'npc-thumbs';
            const locked = isNpc ? roomView.settings.npcLocked : roomView.settings.lootLocked;
            if (locked) return;
        }

        dragging = true;
        moved = false;
        _isDraggingPanel = true;
        hideTooltip(); // kill any open tooltip
        startX = e.clientX;
        startY = e.clientY;

        const rect = el.getBoundingClientRect();
        el.style.transform = 'none';
        el.style.left = `${rect.left}px`;
        el.style.top = `${rect.top}px`;
        el.style.bottom = 'auto';
        origLeft = rect.left;
        origTop = rect.top;

        e.preventDefault();
    });

    document.addEventListener('mousemove', (e) => {
        if (!dragging) return;
        const dx = e.clientX - startX;
        const dy = e.clientY - startY;
        if (Math.abs(dx) > 3 || Math.abs(dy) > 3) moved = true;
        el.style.left = `${origLeft + dx}px`;
        el.style.top = `${origTop + dy}px`;
    });

    document.addEventListener('mouseup', () => {
        if (dragging) {
            dragging = false;
            _isDraggingPanel = false;
            if (moved) {
                try {
                    const saved = JSON.parse(localStorage.getItem('panelPositions') || '{}');
                    saved[storageKey] = { left: el.style.left, top: el.style.top };
                    localStorage.setItem('panelPositions', JSON.stringify(saved));
                } catch {}
            }
        }
    });

    // Restore saved position
    try {
        const saved = JSON.parse(localStorage.getItem('panelPositions') || '{}');
        if (saved[storageKey]) {
            el.style.transform = 'none';
            el.style.bottom = 'auto';
            el.style.left = saved[storageKey].left;
            el.style.top = saved[storageKey].top;
        }
    } catch {}
}

initDraggablePanels();

// ── Init ──

connectWS();
