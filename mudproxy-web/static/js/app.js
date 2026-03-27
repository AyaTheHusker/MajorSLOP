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
            if (typeof chatPanel !== 'undefined' && msg.broadcast_channel !== undefined) {
                chatPanel.setBroadcastChannel(msg.broadcast_channel);
            }
            // Sync proxy settings from server state
            if (typeof roomView !== 'undefined') {
                if (msg.pro_mode) roomView.updateSetting('proMode', msg.pro_mode);
                if (msg.ambient_filter_enabled !== undefined)
                    roomView.updateSetting('ambientFilter', msg.ambient_filter_enabled);
            }
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
            hideTooltip();
            break;

        case 'xp_gain':
            addLog(`+${msg.amount.toLocaleString()} XP`, 'xp');
            hideTooltip();
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
            if (typeof chatPanel !== 'undefined') {
                chatPanel.addChatMessage(msg.sender, msg.message, msg.channel || 'say');
            }
            break;

        case 'connection':
            updateConnection(msg.status);
            addLog(`Connection: ${msg.status}`, 'system');
            break;

        case 'ghost_response':
            addLog(`👻 ${msg.name}: ${msg.response}`, 'ghost');
            break;

        case 'char_name':
            document.title = `MajorSLOP! - ${msg.name}`;
            if (typeof charPanel !== 'undefined') charPanel.setCharName(msg.name);
            break;

        case 'char_portrait':
            if (typeof charPanel !== 'undefined') charPanel.setPortraitKey(msg.key);
            break;

        case 'pro_data':
            if (typeof chatPanel !== 'undefined' && msg.broadcast_channel !== undefined) {
                chatPanel.setBroadcastChannel(msg.broadcast_channel);
            }
            if (msg.map_num !== undefined && msg.room_num !== undefined) {
                document.getElementById('room-location').textContent =
                    `[Map ${msg.map_num}, Room ${msg.room_num}]`;
            }
            break;

        case 'inventory':
            if (typeof charPanel !== 'undefined') {
                charPanel.clearEquipment();
                for (const [slotId, item] of Object.entries(msg.equipped || {})) {
                    charPanel.setEquipment(slotId, item);
                }
                charPanel.setInventory(msg.carried || []);
            }
            break;

        case 'raw_data':
            if (typeof mudTerminal !== 'undefined') {
                mudTerminal.feedRaw(msg.data);
            }
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
    if (typeof roomView !== 'undefined') {
        if (data.room_image_key) {
            roomView.loadRoom(data.room_image_key, data.depth_key);
        } else {
            console.warn(`[Room] No image key for room: "${data.name}", exits: ${JSON.stringify(data.exits)}`);
        }
    }

    // Entity thumbnails — split monsters and players into separate rows
    hideTooltip();
    const allEntities = data.entities || [];
    const monsters = allEntities.filter(e => e.type !== 'player');
    const players = allEntities.filter(e => e.type === 'player');
    updateThumbnails('npc-thumbs', monsters);
    updateThumbnails('player-thumbs', players);
    updateThumbnails('item-thumbs', data.items || []);
}

function updateThumbnails(containerId, entities) {
    const container = document.getElementById(containerId);

    // Clean up old coin canvases
    if (typeof coinRenderer !== 'undefined') coinRenderer.cleanup();
    container.innerHTML = '';

    const isItem = containerId === 'item-thumbs';
    const isPlayer = containerId === 'player-thumbs';

    for (let ei = 0; ei < entities.length; ei++) {
        const ent = entities[ei];
        const div = document.createElement('div');
        div.className = isItem ? 'thumb item-thumb' : (isPlayer ? 'thumb player-thumb' : 'thumb');
        div.title = ent.name + (ent.quantity > 1 ? ` (${ent.quantity})` : '');
        div.dataset.entityName = ent.name;
        div.dataset.entityType = ent.type;
        div.dataset.entityIndex = String(ei);
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
            img.src = `/api/asset/${encodeURIComponent(ent.key)}?_=${Date.now()}`;
            img.loading = 'lazy';
            img.onerror = function() { console.warn(`[Thumb] FAILED: ${ent.name} key=${ent.key}`); };
            div.appendChild(img);
        } else {
            console.warn(`[Thumb] No key for: ${ent.name}`);
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
    tip.dataset.entityName = entity.name;
    tip.dataset.entityIndex = thumbEl.dataset.entityIndex || '';

    // Name
    const nameEl = document.createElement('div');
    nameEl.className = 'tt-name';
    nameEl.textContent = entity.name;
    tip.appendChild(nameEl);

    // HP bar (current/max) — get current fraction from the thumb's HP bar
    const hpFillEl = thumbEl.querySelector('.thumb-hp-fill');
    const hpFrac = hpFillEl ? parseFloat(hpFillEl.style.width) / 100 : 1.0;
    const maxHP = entity.stats?.hp || 0;
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
        if (s.exp) rows.push(['EXP', s.exp.toLocaleString()]);
        if (s.ac != null) rows.push(['AC', s.ac]);
        if (s.dr != null) rows.push(['DR', s.dr]);
        if (s.mr != null) rows.push(['MR', `${s.mr}%`]);
        if (s.bs_def) rows.push(['BS Def', s.bs_def]);
        if (s.regen) rows.push(['HP Regen', `${s.regen}/tick`]);
        if (s.avg_dmg) rows.push(['Avg Dmg', s.avg_dmg]);
        if (s.follow_pct != null) rows.push(['Follow', `${s.follow_pct}%`]);

        // Cash drops
        if (s.cash) {
            const parts = [];
            if (s.cash.R) parts.push(`${s.cash.R}r`);
            if (s.cash.P) parts.push(`${s.cash.P}p`);
            if (s.cash.G) parts.push(`${s.cash.G}g`);
            if (s.cash.S) parts.push(`${s.cash.S}s`);
            if (s.cash.C) parts.push(`${s.cash.C}c`);
            if (parts.length) rows.push(['Cash', parts.join(' ')]);
        }

        const alignMap = {0:'Good',1:'Evil',2:'Chaotic Evil',3:'Neutral',4:'Lawful Good',5:'Neutral Evil',6:'Lawful Evil'};
        if (s.align !== undefined) rows.push(['Align', alignMap[s.align] || s.align]);

        if (s.regen_time && s.regen_time >= 1) {
            const mins = s.regen_time * 60;
            rows.push(['Respawn', mins >= 60 ? `${(mins/60).toFixed(1)}hr` : `${mins}min`]);
        }

        for (const [label, value] of rows) {
            const row = document.createElement('div');
            row.className = 'tt-row';
            row.innerHTML = `<span class="tt-label">${label}</span><span class="tt-value">${value}</span>`;
            tip.appendChild(row);
        }

        // Tags
        const tags = [];
        if (s.mob_type && s.mob_type !== 'Normal') tags.push(s.mob_type);
        if (s.undead) tags.push('Undead');

        if (tags.length > 0) {
            const sec = document.createElement('div');
            sec.className = 'tt-section';
            sec.innerHTML = tags.map(t => `<span class="tt-tag">${t}</span>`).join('');
            tip.appendChild(sec);
        }

        // Drops with thumbnails
        if (entity.drops && entity.drops.length > 0) {
            const sec = document.createElement('div');
            sec.className = 'tt-drops';
            const hdr = document.createElement('div');
            hdr.className = 'tt-label';
            hdr.textContent = 'Drops:';
            sec.appendChild(hdr);

            for (const drop of entity.drops) {
                const dropRow = document.createElement('div');
                dropRow.className = 'tt-drop-row';

                // Tiny thumbnail
                const img = document.createElement('img');
                const thumbKey = drop.key || drop.name?.toLowerCase();
                img.src = `/api/asset/entity_thumb/${encodeURIComponent(thumbKey)}`;
                img.className = 'tt-drop-thumb';
                img.onerror = function() { this.style.display = 'none'; };
                dropRow.appendChild(img);

                const nameSpan = document.createElement('span');
                nameSpan.className = 'tt-drop-name';
                nameSpan.textContent = drop.name;
                dropRow.appendChild(nameSpan);

                const pctSpan = document.createElement('span');
                pctSpan.className = 'tt-drop-pct';
                pctSpan.textContent = `${drop.chance}%`;
                dropRow.appendChild(pctSpan);

                sec.appendChild(dropRow);
            }
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
        const fmtCoin = (v) => { const f = Math.floor(v * 10) / 10; return f % 1 === 0 ? f.toLocaleString() : f.toFixed(1); };
        if (entity.currency === 'copper' && q >= 10)
            rows.push(['Worth', `${fmtCoin(q/10)} silver`]);
        else if (entity.currency === 'silver')
            rows.push(['Worth', `${fmtCoin(q/10)} gold`]);
        else if (entity.currency === 'gold')
            rows.push(['Worth', `${fmtCoin(q/100)} platinum`]);
        else if (entity.currency === 'platinum')
            rows.push(['Worth', `${fmtCoin(q/100)} runic`]);

        for (const [label, value] of rows) {
            const row = document.createElement('div');
            row.className = 'tt-row';
            row.innerHTML = `<span class="tt-label">${label}</span><span class="tt-value">${value}</span>`;
            tip.appendChild(row);
        }
    } else if (entity.type === 'item' && entity.item_data) {
        const d = entity.item_data;
        const rows = [];

        // Item type names
        const ITEM_TYPES = {0:'Other',1:'Weapon',2:'Armour',3:'Shield',4:'Potion',5:'Scroll',
            6:'Wand/Staff',7:'Container',8:'Light',9:'Key',10:'Food',11:'Drink',12:'Instrument',13:'Arrow'};
        const WEAPON_TYPES = {1:'Dagger',2:'One-Handed',3:'Two-Handed',4:'Blunt',5:'Polearm',6:'Thrown',7:'Bow'};
        const WORN_SLOTS = {1:'Weapon Hand',2:'Head',3:'Hands',4:'Finger',5:'Feet',6:'Arms',7:'Back',
            8:'Neck',9:'Legs',10:'Waist',11:'Torso',12:'Off-Hand',14:'Wrist',15:'Ears',16:'Worn',17:'Light',19:'Face'};

        if (d.ItemType != null) rows.push(['Type', ITEM_TYPES[d.ItemType] || `Type ${d.ItemType}`]);
        if (d.WeaponType) rows.push(['Weapon', WEAPON_TYPES[d.WeaponType] || `Type ${d.WeaponType}`]);
        if (d.Worn) rows.push(['Slot', WORN_SLOTS[d.Worn] || `Slot ${d.Worn}`]);
        if (d.Min || d.Max) rows.push(['Damage', `${d.Min || 0}-${d.Max || 0}`]);
        if (d.Speed) rows.push(['Speed', d.Speed]);
        if (d.ArmourClass) rows.push(['AC', d.ArmourClass / 10]);
        if (d.DamageResist) rows.push(['DR', d.DamageResist]);
        if (d.Accy) rows.push(['Accuracy', `+${d.Accy}`]);
        if (d.StrReq) rows.push(['Str Req', d.StrReq]);
        if (d.Encum) rows.push(['Weight', d.Encum]);
        if (d.Price) rows.push(['Value', d.Price.toLocaleString()]);

        // Obtained From — extract shop info (names resolved server-side)
        if (d['Obtained From']) {
            const of = d['Obtained From'];
            const sellMatch = of.match(/Shop\(sell\)\s*#\d+(?:\s*\(([^)]+)\))?/);
            if (sellMatch) rows.push(['Sells at', sellMatch[1] || sellMatch[0]]);
            const buyNames = [];
            for (const m of of.matchAll(/Shop\s*#(\d+)(?:\s*\(([^)]+)\))?/g)) {
                if (of.indexOf(`Shop(sell) #${m[1]}`) !== -1) continue;
                buyNames.push(m[2] || `Shop #${m[1]}`);
            }
            if (buyNames.length) rows.push(['Shops', buyNames.join(', ')]);
        }

        // Abilities
        // Verified from mudinfo.net/Nightmare-Redux source
        const ABILITY_NAMES = {
            1:'Damage',2:'AC',3:'Resist Cold',4:'Max Damage',5:'Resist Fire',
            6:'Enslave',7:'DR',8:'Drain',9:'Shadow',10:'AC (Blur)',
            11:'Energy Level',12:'Summon',13:'Illusion',14:'Room Illusion',
            15:'Alter Hunger',16:'Alter Thirst',17:'Damage (-MR)',18:'Heal',
            19:'Poison',20:'Cure Poison',21:'Immune Poison',22:'Accuracy',
            23:'Affects Undead',24:'Prot Evil',25:'Prot Good',26:'Detect Magic',
            27:'Stealth',28:'Magical',29:'Punch',30:'Kick',31:'Bash',32:'Smash',
            33:'Killblow',34:'Dodge',35:'Jumpkick',36:'MR',37:'Picklocks',
            38:'Tracking',39:'Thievery',40:'Find Traps',41:'Disarm Traps',
            42:'Learn Spell',43:'Cast Spell',44:'Int',45:'Wis',46:'Str',
            47:'Health',48:'Agility',49:'Charm',50:'Magebane Quest',
            51:'Antimagic',52:'Evil in Combat',53:'Blinding Light',
            54:'Illusion Target',55:'Alter Light',56:'Recharge Item',
            57:'See Hidden',58:'Crits',59:'Class OK',60:'Fear',
            61:'Affect Exit',62:'Evil Chance',63:'Experience',64:'Add CP',
            65:'Resist Stone',66:'Resist Lightning',67:'Quickness',68:'Slowness',
            69:'Max Mana',70:'Spellcasting',71:'Confusion',72:'Damage Shield',
            73:'Dispel Magic',74:'Hold Person',75:'Paralyze',76:'Mute',
            77:'Perception',78:'Animal',79:'Magebind',80:'Affects Animal',
            81:'Freedom',82:'Cursed',83:'CURSED',84:'Resist Curse',
            85:'Shatter',86:'Quality',87:'Speed',88:'Alter HP',
            89:'Punch Accuracy',90:'Kick Accuracy',91:'Jumpkick Accuracy',
            92:'Punch Dmg',93:'Kick Dmg',94:'Jumpkick Dmg',95:'Slay',
            96:'Encumbrance',97:'Good',98:'Evil',99:'Alter DR%',
            100:'Loyal Item',101:'Confuse Msg',102:'Race Stealth',
            103:'Class Stealth',104:'Defense Mod',105:'Accuracy (2)',
            106:'Accuracy (3)',107:'Blind User',108:'Affects Living',
            109:'Non-Living',110:'Not Good',111:'Not Evil',112:'Neutral',
            113:'Not Neutral',114:'% Spell',115:'Desc Msg',116:'BS Accuracy',
            117:'BS Min Dmg',118:'BS Max Dmg',119:'Del@Maint',120:'Start Msg',
            121:'Recharge',122:'Remove Spell',123:'HP Regen',
            124:'Negate Ability',125:'Ice Sorc Quest',126:'Good Quest',
            127:'Neutral Quest',128:'Evil Quest',129:'Dark Druid Quest',
            130:'Blood Champ Quest',131:'She-Dragon Quest',132:'Werrat Quest',
            133:'Phoenix Quest',134:'Dao Lord Quest',135:'Min Level',
            136:'Max Level',137:'Shock Oik Quest',138:'Room Visible',
            139:'Spell Immunity',140:'Teleport Room',141:'Teleport Map',
            142:'Hit Magic',143:'Clear Item',144:'Non-Magical Spell',
            145:'Mana Regen',146:'Mons Guards',147:'Resist Water',
            148:'Textblock',149:'Remove@Maint',150:'Heal Mana',
            151:'End Cast',152:'Rune',153:'Kill Spell',154:'Visible@Maint',
            155:'Death Text',156:'Quest Item',157:'Scatter Items',
            158:'Req to Hit',159:'Kai Bind',160:'Give Temp Spell',
            161:'Open Door',162:'Lore',163:'Spell Component',
            164:'Cast on End %',165:'Alter Spell Dmg',166:'Alter Spell Length',
            167:'Unequip Item',168:'Equip Item',169:'Cannot Wear Loc',
            170:'Sleep',171:'Invisibility',172:'See Invisible',173:'Scry',
            174:'Steal Mana',175:'Steal HP to MP',176:'Steal MP to HP',
            177:'Spell Colours',178:'Shadow Form',179:'Find Trap Value',
            180:'Picklocks Value',181:'G-House Deed',182:'G-House Tax',
            183:'G-House Item',184:'G-Shop Item',185:'Bad Attack',
            186:'Perm Stealth',187:'Meditate',188:'Orc King Quest',
            189:'Banshee Quest',190:'Spirit Quest',
        };
        // Internal/system abilities to hide from tooltip
        const HIDDEN_ABILS = new Set([
            6,12,14,15,16,50,52,54,55,56,59,61,62,64,  // system/quest triggers
            101,102,103,107,108,109,110,111,112,113,    // class/race/alignment restrictions
            114,115,119,120,121,122,124,                 // spell internals, del@maint
            125,126,127,128,129,130,131,132,133,134,    // quest flags
            135,136,137,138,140,141,143,144,148,149,    // level reqs, teleport, textblock
            151,152,153,154,155,156,157,159,160,161,    // spell system internals
            163,164,165,166,167,168,169,177,            // spell components, equip triggers
            181,182,183,184,188,189,190,                // g-house, g-shop, quest flags
        ]);
        if (d.abilities && d.abilities.length > 0) {
            for (const ab of d.abilities) {
                if (HIDDEN_ABILS.has(ab.Abil)) continue;
                const aName = ABILITY_NAMES[ab.Abil] || `Abil#${ab.Abil}`;
                const aVal = ab.AbilVal;
                if (aVal > 0) {
                    rows.push([aName, `+${aVal}`]);
                } else if (aVal === 0) {
                    rows.push([aName, '✓']);
                } else {
                    rows.push([aName, `${aVal}`]);
                }
            }
        }

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
    for (let i = 0; i < thumbs.length; i++) {
        const thumb = thumbs[i];
        if (String(i) in hpMap) {
            const frac = hpMap[String(i)];
            const fill = thumb.querySelector('.thumb-hp-fill');
            if (fill) {
                fill.style.width = `${frac * 100}%`;
                fill.style.background = frac > 0.5 ? 'var(--hp-green)' :
                                         frac > 0.25 ? 'var(--hp-yellow)' : 'var(--hp-red)';
            }
            // Visual HP state on thumbnail
            thumb.classList.toggle('hp-critical', frac <= 0.25 && frac > 0.10);
            thumb.classList.toggle('hp-dying', frac <= 0.10);
            // Update open tooltip if it's for this monster
            const name = thumb.dataset.entityName;
            if (_tooltip && _tooltip.dataset.entityName === name && _tooltip.dataset.entityIndex === String(i)) {
                const ttFill = _tooltip.querySelector('.tt-hp-fill');
                const ttText = _tooltip.querySelector('.tt-hp-text');
                if (ttFill) {
                    ttFill.style.width = `${frac * 100}%`;
                    ttFill.style.background = frac > 0.5 ? 'var(--hp-green)' :
                                               frac > 0.25 ? 'var(--hp-yellow)' : 'var(--hp-red)';
                }
                if (ttText) {
                    const maxHP = parseInt(ttText.textContent.split('/')[1]) || 0;
                    ttText.textContent = `${Math.round(frac * maxHP).toLocaleString()} / ${maxHP.toLocaleString()}`;
                }
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
    const playerRow = document.getElementById('player-thumbs');
    const itemRow = document.getElementById('item-thumbs');
    if (npcRow) _makeDraggable(npcRow, 'npc-panel-pos');
    if (playerRow) _makeDraggable(playerRow, 'player-panel-pos');
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
