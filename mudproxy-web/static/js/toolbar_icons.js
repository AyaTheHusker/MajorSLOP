// ── MegaMUD Toolbar Icons — Emoji toggle buttons ──
// Mirrors MegaMUD's toolbar layout. Icons depress when enabled, pop flat when disabled.
// Clicking toggles the flag via memory write API.
// Layout matches MegaMUD: Goto | Loop | Roam | Backtrack | STOP || Combat toggles...

class ToolbarIcons {
    constructor(sendCommand) {
        this._sendCommand = sendCommand;
        this._state = {};
        this._buttons = {};
        this._pathingState = {};
        this._build();
    }

    // Button definitions: [key, emoji, title, offset_hex, type]
    // type: 'toggle' = read/write flag, 'menu' = opens nav panel, 'stop' = stop/re-go, 'action' = one-shot
    static BUTTONS = [
        // ── Path/movement group (matches MegaMUD left side) ──
        ['goto',         '\uD83D\uDEB6', 'Walk To...',     null,     'menu'],
        ['looping',      '\uD83D\uDD04', 'Loop...',        null,     'menu'],
        ['auto_roaming', '\uD83D\uDDFA\uFE0F', 'Auto-Roam', '0x566C', 'toggle'],
        ['backtrack',    '\u23EA',        'Backtrack',      null,     'action'],
        ['stop',         '\uD83D\uDED1', 'Stop',           null,     'stop'],
        ['sep1'],
        // ── Auto toggles (matches MegaMUD right side) ──
        ['auto_combat',  '\u2694\uFE0F', 'Auto-Combat',  '0x4D00', 'toggle'],
        ['auto_nuke',    '\uD83D\uDCA5', 'Auto-Nuke',    '0x4D04', 'toggle'],
        ['auto_heal',    '\u2764\uFE0F', 'Auto-Heal',    '0x4D08', 'toggle'],
        ['auto_bless',   '\u2728',       'Auto-Bless',   '0x4D0C', 'toggle'],
        ['auto_light',   '\uD83D\uDCA1', 'Auto-Light',   '0x4D10', 'toggle'],
        ['auto_cash',    '\uD83D\uDCB0', 'Auto-Cash',    '0x4D14', 'toggle'],
        ['auto_get',     '\uD83C\uDF92', 'Auto-Get',     '0x4D18', 'toggle'],
        ['auto_search',  '\uD83D\uDD0D', 'Auto-Search',  '0x4D1C', 'toggle'],
        ['auto_sneak',   '\uD83E\uDD77', 'Auto-Sneak',   '0x4D20', 'toggle'],
        ['auto_hide',    '\uD83D\uDC7B', 'Auto-Hide',    '0x4D24', 'toggle'],
        ['auto_track',   '\uD83D\uDC3E', 'Auto-Track',   '0x4D28', 'toggle'],
    ];

    _build() {
        const bar = document.createElement('div');
        bar.id = 'mega-toolbar';
        bar.className = 'mega-toolbar';

        for (const def of ToolbarIcons.BUTTONS) {
            if (def[0].startsWith('sep')) {
                const sep = document.createElement('div');
                sep.className = 'tb-sep';
                bar.appendChild(sep);
                continue;
            }
            const [key, emoji, title, offset, type] = def;
            const btn = document.createElement('button');
            btn.className = 'tb-btn';
            if (key === 'stop') btn.className += ' tb-stop';
            btn.title = title;
            btn.textContent = emoji;
            btn.dataset.key = key;
            btn.dataset.offset = offset || '';
            btn.dataset.type = type;

            btn.addEventListener('click', () => this._handleClick(key, offset, type));

            bar.appendChild(btn);
            this._buttons[key] = btn;
        }

        const wrap = document.getElementById('mega-toolbar-wrap');
        wrap.appendChild(bar);
        this._el = bar;
        this._wrap = wrap;

        // Create fist tab as a real DOM element (above header z-index)
        const fist = document.createElement('div');
        fist.className = 'mega-fist-tab';
        fist.title = 'MegaMUD';
        fist.addEventListener('click', () => {
            // Toggle toolbar on click
            if (wrap.classList.contains('collapsed')) {
                wrap.classList.remove('collapsed');
                const rf = document.getElementById('room-name-float');
                if (rf) rf.classList.remove('toolbar-collapsed');
            }
        });
        document.body.appendChild(fist);
        this._fistTab = fist;

        this._setupAutoHide();
    }

    _setupAutoHide() {
        const wrap = this._wrap;
        const roomFloat = document.getElementById('room-name-float');
        let hideTimer = null;
        let collapsed = false;
        let mouseNear = false;

        const fist = this._fistTab;
        const collapse = () => {
            if (collapsed || mouseNear) return;
            collapsed = true;
            wrap.classList.add('collapsed');
            if (roomFloat) roomFloat.classList.add('toolbar-collapsed');
            if (fist) fist.style.top = '36px';
        };

        const expand = () => {
            collapsed = false;
            wrap.classList.remove('collapsed');
            if (roomFloat) roomFloat.classList.remove('toolbar-collapsed');
            // Position fist below the toolbar bar
            if (fist) fist.style.top = (36 + wrap.offsetHeight) + 'px';
            resetTimer();
        };

        const resetTimer = () => {
            if (hideTimer) clearTimeout(hideTimer);
            if (!mouseNear) {
                hideTimer = setTimeout(collapse, 3000);
            }
        };

        // Track mouse near the toolbar area (top 80px of screen)
        document.addEventListener('mousemove', (e) => {
            const wasNear = mouseNear;
            mouseNear = e.clientY < 80;
            if (mouseNear && collapsed) {
                expand();
            }
            if (mouseNear && !wasNear) {
                // Entered zone — cancel any hide timer
                if (hideTimer) clearTimeout(hideTimer);
            }
            if (!mouseNear && wasNear) {
                // Left zone — start hide timer
                resetTimer();
            }
        });

        // Any click on a toolbar button resets the timer
        this._el.addEventListener('click', () => resetTimer());

        // Start collapsed
        collapse();
    }

    _handleClick(key, offset, type) {
        if (type === 'menu') {
            // Open the nav panel to the appropriate tab
            if (typeof navPanel !== 'undefined') {
                if (key === 'goto') navPanel.open('goto');
                else if (key === 'looping') navPanel.open('loop');
            }
        } else if (type === 'stop') {
            // Depressed = stopped. Click to re-go.
            // Flat = running. Click to stop.
            const isStopped = !(this._pathingState.pathing || this._pathingState.go);
            if (isStopped) {
                // Re-go: set MODE=14 (walking), GO_FLAG=1, PATHING_ACTIVE=1
                fetch('/api/mem/toggle', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ offset: '0x54BC', value: 14 })
                });
                fetch('/api/mem/toggle', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ offset: '0x564C', value: 1 })
                });
                fetch('/api/mem/toggle', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ offset: '0x5664', value: 1 })
                });
            } else {
                // Stop: call the stop endpoint
                fetch('/api/mem/stop', { method: 'POST' });
            }
        } else if (type === 'action') {
            if (key === 'backtrack') {
                // Write backtrack rooms = 1 to step back one room
                fetch('/api/mem/toggle', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ offset: '0x358C', value: 1 })
                });
            }
        } else if (type === 'toggle') {
            fetch('/api/mem/toggle', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ offset: offset })
            });
        }
    }

    // Called from app.js when mem_state arrives
    update(toggles, pathing) {
        if (!toggles) return;
        this._state = toggles;
        if (pathing) this._pathingState = pathing;

        for (const [key, btn] of Object.entries(this._buttons)) {
            const type = btn.dataset.type;

            if (type === 'toggle') {
                btn.classList.toggle('pressed', !!toggles[key]);
            } else if (type === 'stop') {
                // Stop is depressed when stopped (pathing NOT active)
                const stopped = !(pathing && (pathing.pathing || pathing.go));
                btn.classList.toggle('pressed', stopped);
            } else if (type === 'menu') {
                // Goto button depressed when walking/running (pathing but not looping)
                if (key === 'goto') {
                    const walking = !!(pathing && pathing.pathing && !pathing.looping);
                    btn.classList.toggle('pressed', walking);
                }
                // Loop button depressed when looping
                if (key === 'looping') {
                    const looping = !!(pathing && pathing.looping);
                    btn.classList.toggle('pressed', looping);
                }
            }
        }
    }
}
