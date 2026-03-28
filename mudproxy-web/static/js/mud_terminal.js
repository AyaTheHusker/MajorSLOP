// ── MUD Terminal ──
// Inline floating panel with xterm.js terminal emulation.
// Supports direct typing on the terminal surface (arrow keys, escape seqs for BBS menus)
// and a convenience input box with command history.

// ── Fuzzy ghost command matching ──
// All known MegaMUD @commands in exact format
const _GHOST_CMDS = [
    'version','health','exp','level','status','lives','where','path','seen','who',
    'what','wealth','enc','have','home','settings','stop','rego','goto','loop',
    'looponce','get-all','drop-all','equip-all','deposit-all','do','hangup','relog',
    'reset','invite','join','forget','roam','attack-last','auto-all','auto-combat',
    'auto-nuke','auto-heal','auto-bless','auto-light','auto-cash','auto-get',
    'auto-sneak','auto-hide','auto-search','divert','wait','ok','comeback','heal',
    'blind','diseased','held','party','kill','share','panic!',
];

function _levenshtein(a, b) {
    const m = a.length, n = b.length;
    if (m === 0) return n;
    if (n === 0) return m;
    const d = Array.from({length: m + 1}, (_, i) => [i]);
    for (let j = 1; j <= n; j++) d[0][j] = j;
    for (let i = 1; i <= m; i++) {
        for (let j = 1; j <= n; j++) {
            d[i][j] = a[i-1] === b[j-1]
                ? d[i-1][j-1]
                : 1 + Math.min(d[i-1][j], d[i][j-1], d[i-1][j-1]);
        }
    }
    return d[m][n];
}

function _fuzzyGhostCmd(raw) {
    // Split into command word + args: "#goto SBNK" → ["goto", "SBNK"]
    const parts = raw.trim().split(/\s+/);
    let typed = parts[0].toLowerCase();
    const args = parts.slice(1).join(' ');

    // Exact match first
    if (_GHOST_CMDS.includes(typed)) return '@' + typed + (args ? ' ' + args : '');

    // Fuzzy: find closest command within edit distance 2
    let bestCmd = null, bestDist = 999;
    for (const cmd of _GHOST_CMDS) {
        const dist = _levenshtein(typed, cmd);
        if (dist < bestDist) { bestDist = dist; bestCmd = cmd; }
    }

    // Only accept if distance <= 2 (or <= 3 for longer commands)
    const maxDist = bestCmd && bestCmd.length >= 6 ? 3 : 2;
    if (bestDist <= maxDist) typed = bestCmd;
    // else: send as-is, let it fail naturally

    return '@' + typed + (args ? ' ' + args : '');
}

class MudTerminal {
    constructor(sendFn) {
        this._send = sendFn;
        this._term = null;
        this._fitAddon = null;
        this._visible = false;
        this._locked = false;
        this._buffer = [];
        this._cmdHistory = [];
        this._historyIdx = -1;
        this._currentInput = '';
        this._directMode = true;
        this._el = null;
        this._backscroll = [];      // plain text lines
        this._maxBackscroll = 10000;
        this._backscrollWin = null;
        this._build();
        this._initDrag();
        this._initResize();
        this._initBackscrollKey();
        this._restoreState();
        this._loadHistory();
    }

    get isOpen() { return this._visible; }

    _build() {
        const panel = document.createElement('div');
        panel.id = 'mud-terminal';
        panel.className = 'mud-terminal-panel';
        panel.style.display = 'none';

        // Header
        const header = document.createElement('div');
        header.className = 'mud-terminal-header';

        const title = document.createElement('span');
        title.className = 'mud-terminal-title';
        title.textContent = 'Terminal';

        const controls = document.createElement('div');
        controls.className = 'mud-terminal-controls';

        const modeBtn = document.createElement('span');
        modeBtn.className = 'mud-terminal-btn mode-btn active';
        modeBtn.textContent = 'DIRECT';
        modeBtn.title = 'Toggle: type on terminal surface vs input box';
        modeBtn.addEventListener('click', (e) => {
            e.stopPropagation();
            this._toggleMode();
        });
        this._modeBtn = modeBtn;

        const lockBtn = document.createElement('span');
        lockBtn.className = 'mud-terminal-btn';
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
        closeBtn.className = 'mud-terminal-btn';
        closeBtn.textContent = '\u2715';
        closeBtn.title = 'Close';
        closeBtn.addEventListener('click', (e) => {
            e.stopPropagation();
            this.toggle(false);
        });

        // Transparency slider
        const opSlider = document.createElement('input');
        opSlider.type = 'range';
        opSlider.className = 'panel-opacity-slider';
        opSlider.min = '10';
        opSlider.max = '100';
        opSlider.value = '95';
        opSlider.title = 'Panel transparency';
        opSlider.addEventListener('input', () => {
            this._applyOpacity(Number(opSlider.value));
            this._saveState();
        });
        this._opSlider = opSlider;

        controls.appendChild(opSlider);
        controls.appendChild(modeBtn);
        controls.appendChild(lockBtn);
        controls.appendChild(closeBtn);
        header.appendChild(title);
        header.appendChild(controls);
        panel.appendChild(header);

        // Terminal container
        const termContainer = document.createElement('div');
        termContainer.className = 'mud-terminal-body';
        this._termContainer = termContainer;
        panel.appendChild(termContainer);

        // Input bar
        const inputBar = document.createElement('div');
        inputBar.className = 'mud-terminal-input-bar';

        const prompt = document.createElement('span');
        prompt.className = 'mud-terminal-prompt';
        prompt.textContent = '>';

        const input = document.createElement('input');
        input.type = 'text';
        input.className = 'mud-terminal-input';
        input.placeholder = 'Type command...';
        input.autocomplete = 'off';
        input.spellcheck = false;

        input.addEventListener('keydown', (e) => {
            if (e.key === 'Enter') {
                const cmd = input.value;
                if (cmd.length > 0) {
                    this._cmdHistory.push(cmd);
                    if (this._cmdHistory.length > 200) this._cmdHistory.shift();
                    this._saveHistory();
                }
                this._historyIdx = -1;
                this._currentInput = '';
                if (cmd.startsWith('#')) {
                    // Ghost command: #stop → @stop via ghost telepath
                    const ghostName = localStorage.getItem('ghostName') || 'Yoder';
                    const atCmd = _fuzzyGhostCmd(cmd.slice(1));
                    this._send('ghost', { name: ghostName, at_cmd: atCmd });
                } else {
                    this._send('inject', { text: cmd });
                }
                input.value = '';
            } else if (e.key === 'ArrowUp') {
                e.preventDefault();
                if (this._historyIdx === -1) {
                    this._currentInput = input.value;
                    this._historyIdx = this._cmdHistory.length - 1;
                } else if (this._historyIdx > 0) {
                    this._historyIdx--;
                }
                if (this._historyIdx >= 0) {
                    input.value = this._cmdHistory[this._historyIdx];
                }
            } else if (e.key === 'ArrowDown') {
                e.preventDefault();
                if (this._historyIdx >= 0) {
                    this._historyIdx++;
                    if (this._historyIdx >= this._cmdHistory.length) {
                        this._historyIdx = -1;
                        input.value = this._currentInput;
                    } else {
                        input.value = this._cmdHistory[this._historyIdx];
                    }
                }
            }
        });
        this._input = input;

        inputBar.appendChild(prompt);
        inputBar.appendChild(input);
        panel.appendChild(inputBar);

        // ── MegaMUD Remote Actions slide-out ──
        const toggleArrow = document.createElement('div');
        toggleArrow.className = 'mega-tray-toggle';
        toggleArrow.innerHTML = '&#x25BC;';
        toggleArrow.title = 'MegaMUD Remote Actions';
        panel.appendChild(toggleArrow);

        const tray = document.createElement('div');
        tray.className = 'mega-tray';
        tray.style.display = 'none';
        this._megaTray = tray;
        this._megaTrayOpen = false;

        this._positionTray = () => {
            if (!this._megaTrayOpen) return;
            const r = panel.getBoundingClientRect();
            tray.style.left = r.left + 'px';
            tray.style.width = r.width + 'px';
            tray.style.top = r.bottom + 'px';
        };

        toggleArrow.addEventListener('click', (e) => {
            e.stopPropagation();
            this._megaTrayOpen = !this._megaTrayOpen;
            tray.style.display = this._megaTrayOpen ? '' : 'none';
            toggleArrow.innerHTML = this._megaTrayOpen ? '&#x25B2;' : '&#x25BC;';
            toggleArrow.classList.toggle('open', this._megaTrayOpen);
            this._positionTray();
        });

        // Helper: create glossy button
        const ghostName = () => localStorage.getItem('ghostName') || 'Yoder';
        const mkBtn = (emoji, label, atCmd, color) => {
            const btn = document.createElement('button');
            btn.className = 'mega-btn' + (color ? ' mega-btn-' + color : '');
            btn.innerHTML = emoji;
            btn.title = label;
            btn.addEventListener('click', (e) => {
                e.stopPropagation();
                this._send('ghost', { name: ghostName(), at_cmd: atCmd });
                btn.classList.add('mega-btn-pulse');
                setTimeout(() => btn.classList.remove('mega-btn-pulse'), 400);
            });
            return btn;
        };

        // Row 1: Action commands
        const row1 = document.createElement('div');
        row1.className = 'mega-row';
        row1.appendChild(mkBtn('\u{1F6D1}', 'Stop',        '@stop'));       // stop sign
        row1.appendChild(mkBtn('\u{25B6}\u{FE0F}',  'Resume (Rego)', '@rego'));       // play
        row1.appendChild(mkBtn('\u{1F91A}', 'Get All',      '@get-all'));    // grabbing hand
        row1.appendChild(mkBtn('\u{1F6BD}', 'Drop All',     '@drop-all'));   // toilet
        row1.appendChild(mkBtn('\u{1F6E1}\u{FE0F}',  'Equip All',    '@equip-all')); // shield
        row1.appendChild(mkBtn('\u{1F3E6}', 'Deposit All',  '@deposit-all'));// bank
        tray.appendChild(row1);

        // Divider
        const div1 = document.createElement('div');
        div1.className = 'mega-divider';
        tray.appendChild(div1);

        // Row 2+3: Toggle commands (ON green / OFF red stacked pairs)
        const toggles = [
            ['\u{2694}\u{FE0F}',  'Attack Last',  'attack-last'],
            ['\u{1F4A5}', 'Auto Combat',  'auto-combat'],
            ['\u{2622}\u{FE0F}',  'Auto Nuke',    'auto-nuke'],
            ['\u{2764}\u{FE0F}',  'Auto Heal',    'auto-heal'],
            ['\u{1F64F}', 'Auto Bless',   'auto-bless'],
            ['\u{1F4A1}', 'Auto Light',   'auto-light'],
            ['\u{1FA99}', 'Auto Cash',    'auto-cash'],
            ['\u{1F9F2}', 'Auto Get',     'auto-get'],
            ['\u{1F977}', 'Auto Sneak',   'auto-sneak'],
            ['\u{1F648}', 'Auto Hide',    'auto-hide'],
            ['\u{1F50D}', 'Auto Search',  'auto-search'],
        ];

        const rowOn = document.createElement('div');
        rowOn.className = 'mega-row';
        const rowOff = document.createElement('div');
        rowOff.className = 'mega-row';

        for (const [emoji, label, cmd] of toggles) {
            rowOn.appendChild(mkBtn(emoji, label + ' ON',  '@' + cmd + ' on',  'green'));
            rowOff.appendChild(mkBtn(emoji, label + ' OFF', '@' + cmd + ' off', 'red'));
        }
        tray.appendChild(rowOn);
        tray.appendChild(rowOff);

        // Divider
        const div2 = document.createElement('div');
        div2.className = 'mega-divider';
        tray.appendChild(div2);

        // Roam ON/OFF
        const roamRow = document.createElement('div');
        roamRow.className = 'mega-row';
        roamRow.appendChild(mkBtn('\u{1F9ED}', 'Roam ON',  '@roam on',  'green'));  // compass
        roamRow.appendChild(mkBtn('\u{1F9ED}', 'Roam OFF', '@roam off', 'red'));
        tray.appendChild(roamRow);

        document.body.appendChild(panel);
        document.body.appendChild(tray);
        this._el = panel;
    }

    _applyOpacity(val) {
        const alpha = val / 100;
        const blur = Math.round((1 - alpha) * 20);
        // Panel outer shell — light tint + blur
        this._el.style.background = `rgba(10, 10, 20, ${(alpha * 0.3).toFixed(2)})`;
        this._el.style.backdropFilter = blur > 0 ? `blur(${blur}px)` : 'none';
        this._el.style.webkitBackdropFilter = blur > 0 ? `blur(${blur}px)` : 'none';
        this._opacity = val;

        // Terminal body container
        this._termContainer.style.background = `rgba(10, 10, 20, ${(alpha * 0.5).toFixed(2)})`;

        // xterm renders to a canvas — override the viewport/screen DOM layers
        if (this._term) {
            const vp = this._termContainer.querySelector('.xterm-viewport');
            const screen = this._termContainer.querySelector('.xterm-screen');
            if (vp) {
                vp.style.background = `rgba(10, 10, 20, ${(alpha * 0.6).toFixed(2)})`;
            }
            if (screen) {
                screen.style.background = 'transparent';
            }
            // The canvas itself draws cells with the theme bg color.
            // We can make the text layer canvas transparent by setting its opacity.
            const canvases = this._termContainer.querySelectorAll('canvas');
            for (const c of canvases) {
                // The text layer canvas should remain visible;
                // the background is rendered by a separate canvas or the viewport div
                c.style.background = 'transparent';
            }
        }
    }

    _initXterm() {
        if (this._term) return;

        const term = new Terminal({
            cursorBlink: true,
            cursorStyle: 'underline',
            disableStdin: false,
            scrollback: 10000,
            fontSize: 13,
            fontFamily: "'Cascadia Code', 'Fira Code', 'Consolas', monospace",
            theme: {
                background: '#0a0a14',
                foreground: '#cccccc',
                cursor: '#44cc44',
                cursorAccent: '#0a0a14',
                selectionBackground: 'rgba(68, 68, 204, 0.4)',
                black: '#000000',
                red: '#cc4444',
                green: '#44cc44',
                yellow: '#ccaa44',
                blue: '#4444cc',
                magenta: '#cc44cc',
                cyan: '#44cccc',
                white: '#cccccc',
                brightBlack: '#666666',
                brightRed: '#ff6666',
                brightGreen: '#66ff66',
                brightYellow: '#ffff66',
                brightBlue: '#6666ff',
                brightMagenta: '#ff66ff',
                brightCyan: '#66ffff',
                brightWhite: '#ffffff',
            },
            convertEol: false,
            allowProposedApi: true,
        });

        const FitAddon = window.FitAddon?.FitAddon;
        if (FitAddon) {
            this._fitAddon = new FitAddon();
            term.loadAddon(this._fitAddon);
        }

        term.open(this._termContainer);

        if (this._fitAddon) {
            setTimeout(() => this._fitAddon.fit(), 0);
        }

        this._term = term;

        // Re-apply opacity now that xterm DOM exists
        if (this._opacity != null) {
            setTimeout(() => this._applyOpacity(this._opacity), 50);
        }

        // Replay buffer
        for (const chunk of this._buffer) {
            term.write(chunk);
        }
        this._buffer = [];

        // Direct terminal input — arrow keys, escape seqs, everything
        term.onData((data) => {
            this._send('raw_input', { data: data });
        });

        // Click terminal body to focus
        this._termContainer.addEventListener('mousedown', () => {
            if (this._directMode) term.focus();
        });

        // Observe panel resize to refit
        this._resizeObserver = new ResizeObserver(() => {
            if (this._fitAddon && this._visible) {
                this._fitAddon.fit();
            }
        });
        this._resizeObserver.observe(this._el);
    }

    _toggleMode() {
        this._directMode = !this._directMode;
        if (this._directMode) {
            this._modeBtn.classList.add('active');
            this._modeBtn.textContent = 'DIRECT';
            if (this._term) this._term.focus();
        } else {
            this._modeBtn.classList.remove('active');
            this._modeBtn.textContent = 'INPUT';
            this._input.focus();
        }
    }

    toggle(show) {
        if (show === undefined) show = !this._visible;
        this._visible = show;
        this._el.style.display = show ? 'flex' : 'none';
        // Hide mega tray when panel is hidden
        if (!show && this._megaTray) {
            this._megaTray.style.display = 'none';
        }
        if (show) {
            this._initXterm();
            if (this._fitAddon) {
                setTimeout(() => this._fitAddon.fit(), 50);
            }
            if (this._directMode && this._term) {
                this._term.focus();
            } else {
                this._input.focus();
            }
        }
        this._saveState();
    }

    close() {
        this.toggle(false);
    }

    /** Feed raw server data (full ANSI stream) directly to xterm. */
    feedRaw(data) {
        if (this._term) {
            this._term.write(data);
        } else {
            this._buffer.push(data);
            if (this._buffer.length > 2000) this._buffer.shift();
        }
        // Build plain-text backscroll
        this._appendBackscroll(data);
    }

    /** Strip ANSI codes and accumulate plain text lines. */
    _appendBackscroll(data) {
        // Strip ANSI escape sequences
        const plain = data.replace(/\x1b\[[0-9;]*[A-Za-z]/g, '')
                          .replace(/\x1b\][^\x07]*\x07/g, '')
                          .replace(/\x1b[()][0-9A-Za-z]/g, '')
                          .replace(/[\x00-\x08\x0e-\x1f]/g, '');
        // Split on newlines
        const parts = plain.split(/\r?\n/);
        if (!this._backscrollPartial) this._backscrollPartial = '';
        for (let i = 0; i < parts.length; i++) {
            if (i === 0) {
                this._backscrollPartial += parts[0];
            } else {
                // Commit previous line
                const line = this._backscrollPartial.replace(/\r/g, '');
                if (line.length > 0) {
                    this._backscroll.push(line);
                    if (this._backscroll.length > this._maxBackscroll) {
                        this._backscroll.shift();
                    }
                }
                this._backscrollPartial = parts[i];
            }
        }
    }

    _initBackscrollKey() {
        document.addEventListener('keydown', (e) => {
            if (e.altKey && (e.key === 'b' || e.key === 'B')) {
                e.preventDefault();
                e.stopPropagation();
                this._showBackscroll();
            }
        });
    }

    _showBackscroll() {
        // Remove existing
        const existing = document.getElementById('backscroll-overlay');
        if (existing) { existing.remove(); return; }

        const overlay = document.createElement('div');
        overlay.id = 'backscroll-overlay';
        overlay.style.cssText = `
            position: fixed; top: 0; left: 0; width: 100vw; height: 100vh;
            background: rgba(0,0,0,0.85); z-index: 9999;
            display: flex; flex-direction: column;
        `;

        // Header with search
        const header = document.createElement('div');
        header.style.cssText = `
            display: flex; align-items: center; gap: 10px;
            padding: 8px 16px; background: #111; border-bottom: 1px solid #333;
            flex-shrink: 0;
        `;

        const title = document.createElement('span');
        title.textContent = 'Backscroll';
        title.style.cssText = 'color: #ccc; font-size: 14px; font-weight: 600; font-family: monospace;';

        const searchInput = document.createElement('input');
        searchInput.type = 'text';
        searchInput.placeholder = 'Search... (Enter=next, Shift+Enter=prev)';
        searchInput.style.cssText = `
            flex: 1; max-width: 400px; background: #0a0a0a; border: 1px solid #333;
            color: #fff; font-family: monospace; font-size: 13px; padding: 4px 8px;
            border-radius: 3px; outline: none;
        `;

        const matchLabel = document.createElement('span');
        matchLabel.style.cssText = 'color: #666; font-size: 12px; font-family: monospace;';

        const copyBtn = document.createElement('button');
        copyBtn.textContent = 'Copy Selected';
        copyBtn.style.cssText = `
            background: #222; color: #aaa; border: 1px solid #444; padding: 4px 10px;
            border-radius: 3px; cursor: pointer; font-size: 12px; font-family: monospace;
        `;

        const copyAllBtn = document.createElement('button');
        copyAllBtn.textContent = 'Copy All';
        copyAllBtn.style.cssText = `
            background: #222; color: #aaa; border: 1px solid #444; padding: 4px 10px;
            border-radius: 3px; cursor: pointer; font-size: 12px; font-family: monospace;
        `;

        const closeBtn = document.createElement('span');
        closeBtn.textContent = '\u2715';
        closeBtn.style.cssText = `
            color: #888; font-size: 18px; cursor: pointer; margin-left: auto; padding: 0 4px;
        `;

        header.appendChild(title);
        header.appendChild(searchInput);
        header.appendChild(matchLabel);
        header.appendChild(copyBtn);
        header.appendChild(copyAllBtn);
        header.appendChild(closeBtn);
        overlay.appendChild(header);

        // Content area
        const content = document.createElement('pre');
        content.style.cssText = `
            flex: 1; overflow-y: auto; padding: 12px 16px; margin: 0;
            color: #e0e0e0; background: #0a0a0a;
            font-family: 'Cascadia Code', 'Fira Code', 'Consolas', monospace;
            font-size: 13px; line-height: 1.4; white-space: pre-wrap;
            word-wrap: break-word; user-select: text; cursor: text;
        `;
        content.textContent = this._backscroll.join('\n');
        overlay.appendChild(content);

        document.body.appendChild(overlay);

        // Scroll to bottom (newest)
        content.scrollTop = content.scrollHeight;

        // Search logic
        let matches = [];
        let matchIdx = -1;

        const clearHighlights = () => {
            content.querySelectorAll('.bs-highlight').forEach(el => {
                el.replaceWith(document.createTextNode(el.textContent));
            });
            content.normalize();
        };

        const doSearch = (query) => {
            clearHighlights();
            matches = [];
            matchIdx = -1;
            if (!query) { matchLabel.textContent = ''; return; }

            const text = this._backscroll.join('\n');
            const lower = text.toLowerCase();
            const qLower = query.toLowerCase();
            let pos = 0;
            while ((pos = lower.indexOf(qLower, pos)) !== -1) {
                matches.push(pos);
                pos += qLower.length;
            }
            if (matches.length === 0) {
                matchLabel.textContent = '0 matches';
                return;
            }

            // Rebuild content with highlights
            const frag = document.createDocumentFragment();
            let lastEnd = 0;
            for (const start of matches) {
                const end = start + query.length;
                if (start > lastEnd) {
                    frag.appendChild(document.createTextNode(text.slice(lastEnd, start)));
                }
                const mark = document.createElement('span');
                mark.className = 'bs-highlight';
                mark.textContent = text.slice(start, end);
                mark.style.cssText = 'background: #664400; color: #ffcc00; border-radius: 2px;';
                frag.appendChild(mark);
                lastEnd = end;
            }
            if (lastEnd < text.length) {
                frag.appendChild(document.createTextNode(text.slice(lastEnd)));
            }
            content.textContent = '';
            content.appendChild(frag);

            // Jump to last match (newest, near bottom)
            matchIdx = matches.length - 1;
            jumpToMatch();
        };

        const jumpToMatch = () => {
            if (matches.length === 0) return;
            matchLabel.textContent = `${matchIdx + 1} / ${matches.length}`;
            const highlights = content.querySelectorAll('.bs-highlight');
            // Remove active styling
            highlights.forEach(el => el.style.background = '#664400');
            if (highlights[matchIdx]) {
                highlights[matchIdx].style.background = '#886600';
                highlights[matchIdx].scrollIntoView({ block: 'center' });
            }
        };

        let searchTimeout = null;
        searchInput.addEventListener('input', () => {
            clearTimeout(searchTimeout);
            searchTimeout = setTimeout(() => doSearch(searchInput.value), 200);
        });

        searchInput.addEventListener('keydown', (e) => {
            if (e.key === 'Enter') {
                e.preventDefault();
                if (matches.length === 0) return;
                if (e.shiftKey) {
                    matchIdx = (matchIdx - 1 + matches.length) % matches.length;
                } else {
                    matchIdx = (matchIdx + 1) % matches.length;
                }
                jumpToMatch();
            } else if (e.key === 'Escape') {
                overlay.remove();
            }
        });

        // Copy selected text
        copyBtn.addEventListener('click', () => {
            const sel = window.getSelection();
            if (sel && sel.toString()) {
                navigator.clipboard.writeText(sel.toString());
                copyBtn.textContent = 'Copied!';
                setTimeout(() => copyBtn.textContent = 'Copy Selected', 1500);
            }
        });

        // Copy all
        copyAllBtn.addEventListener('click', () => {
            navigator.clipboard.writeText(this._backscroll.join('\n'));
            copyAllBtn.textContent = 'Copied!';
            setTimeout(() => copyAllBtn.textContent = 'Copy All', 1500);
        });

        // Close
        closeBtn.addEventListener('click', () => overlay.remove());
        overlay.addEventListener('keydown', (e) => {
            if (e.key === 'Escape') overlay.remove();
            // Alt+B toggles off too
            if (e.altKey && (e.key === 'b' || e.key === 'B')) {
                e.preventDefault();
                overlay.remove();
            }
        });

        searchInput.focus();
    }

    _initDrag() {
        let dragging = false;
        let startX, startY, origLeft, origTop;
        const header = this._el.querySelector('.mud-terminal-header');

        header.addEventListener('mousedown', (e) => {
            if (this._locked) return;
            if (e.button !== 0) return;
            // Don't drag if clicking controls
            if (e.target.closest('.mud-terminal-controls')) return;
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
            // Clamp so the header always stays on screen
            let newLeft = origLeft + dx;
            let newTop = origTop + dy;
            const rect = this._el.getBoundingClientRect();
            const headerH = 32; // approximate header height
            newTop = Math.max(0, Math.min(newTop, window.innerHeight - headerH));
            newLeft = Math.max(-rect.width + 80, Math.min(newLeft, window.innerWidth - 80));
            this._el.style.left = `${newLeft}px`;
            this._el.style.top = `${newTop}px`;
            this._el.style.right = 'auto';
            this._el.style.bottom = 'auto';
            if (this._positionTray) this._positionTray();
        });

        document.addEventListener('mouseup', () => {
            if (dragging) {
                dragging = false;
                this._el.style.transition = '';
                this._saveState();
            }
        });
    }

    _initResize() {
        // Refit terminal when CSS resize handle is used
        const el = this._el;
        let lastW = 0, lastH = 0;
        const check = () => {
            if (!this._visible) return;
            const w = el.offsetWidth, h = el.offsetHeight;
            if (w !== lastW || h !== lastH) {
                lastW = w; lastH = h;
                if (this._fitAddon) this._fitAddon.fit();
                if (this._positionTray) this._positionTray();
            }
        };
        // Poll during active resize (mousedown on panel)
        let interval = null;
        el.addEventListener('mousedown', () => {
            if (interval) return;
            interval = setInterval(check, 100);
        });
        document.addEventListener('mouseup', () => {
            if (interval) {
                clearInterval(interval);
                interval = null;
                check();
                this._saveState();
            }
        });
    }

    _saveState() {
        try {
            const saved = JSON.parse(localStorage.getItem('panelPositions') || '{}');
            saved['mud-terminal'] = {
                left: this._el.style.left,
                top: this._el.style.top,
                width: this._el.style.width,
                height: this._el.style.height,
                visible: this._visible,
                locked: this._locked,
                opacity: this._opacity,
            };
            localStorage.setItem('panelPositions', JSON.stringify(saved));
        } catch {}
    }

    _restoreState() {
        try {
            const saved = JSON.parse(localStorage.getItem('panelPositions') || '{}');
            const s = saved['mud-terminal'];
            if (s) {
                if (s.left) this._el.style.left = s.left;
                if (s.top) this._el.style.top = s.top;
                if (s.width) this._el.style.width = s.width;
                if (s.height) this._el.style.height = s.height;
                if (s.locked) {
                    this._locked = true;
                    this._lockBtn.textContent = '\u{1F512}';
                }
                if (s.opacity != null) {
                    this._opSlider.value = s.opacity;
                    this._applyOpacity(s.opacity);
                }
                if (s.visible) this.toggle(true);
            }
            // Clamp position to keep header on screen
            requestAnimationFrame(() => {
                const r = this._el.getBoundingClientRect();
                let changed = false;
                if (r.top < 0) { this._el.style.top = '0px'; changed = true; }
                if (r.top > window.innerHeight - 32) { this._el.style.top = (window.innerHeight - 32) + 'px'; changed = true; }
                if (r.left > window.innerWidth - 80) { this._el.style.left = (window.innerWidth - 80) + 'px'; changed = true; }
                if (r.right < 80) { this._el.style.left = (-r.width + 80) + 'px'; changed = true; }
                if (changed) this._saveState();
            });
        } catch {}
    }

    _saveHistory() {
        try {
            localStorage.setItem('termHistory', JSON.stringify(this._cmdHistory));
        } catch {}
    }

    _loadHistory() {
        try {
            const h = JSON.parse(localStorage.getItem('termHistory') || '[]');
            if (Array.isArray(h)) this._cmdHistory = h.slice(-200);
        } catch {}
    }
}
