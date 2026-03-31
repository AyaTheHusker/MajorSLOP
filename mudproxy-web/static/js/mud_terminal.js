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

        const locSpan = document.createElement('span');
        locSpan.className = 'mud-terminal-location';
        locSpan.id = 'terminal-rm-location';
        locSpan.style.cssText = 'margin-left:12px;color:#888;font-size:0.85em;';

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

        // ANSI Gradient dropdown
        this._ansiGradient = new AnsiGradient();
        const gradBtn = document.createElement('span');
        gradBtn.className = 'mud-terminal-btn grad-btn';
        gradBtn.title = 'ANSI color gradient scheme';
        const gradLabel = document.createElement('span');
        gradLabel.textContent = 'GRAD';
        gradBtn.appendChild(gradLabel);
        const gradMenu = document.createElement('div');
        gradMenu.className = 'grad-scheme-menu';
        gradMenu.style.display = 'none';
        for (const [key, scheme] of Object.entries(AnsiGradient.SCHEMES)) {
            const opt = document.createElement('div');
            opt.className = 'grad-scheme-opt' + (key === this._ansiGradient.scheme ? ' active' : '');
            opt.dataset.scheme = key;
            opt.innerHTML = key === 'none'
                ? 'None'
                : `<span class="grad-scheme-name">${scheme.name}</span><span class="grad-scheme-desc">${scheme.desc}</span>`;
            opt.addEventListener('click', (e) => {
                e.stopPropagation();
                this._ansiGradient.scheme = key;
                gradMenu.style.display = 'none';
                for (const o of gradMenu.querySelectorAll('.grad-scheme-opt')) {
                    o.classList.toggle('active', o.dataset.scheme === key);
                }
                gradLabel.textContent = key === 'none' ? 'GRAD' : scheme.name.split(' ')[0].toUpperCase();
            });
            gradMenu.appendChild(opt);
        }
        gradBtn.addEventListener('click', (e) => {
            e.stopPropagation();
            gradMenu.style.display = gradMenu.style.display === 'none' ? 'block' : 'none';
        });
        document.addEventListener('click', () => { gradMenu.style.display = 'none'; });
        gradBtn.appendChild(gradMenu);
        // Set initial button label
        if (this._ansiGradient.scheme !== 'none') {
            const s = AnsiGradient.SCHEMES[this._ansiGradient.scheme];
            if (s) gradLabel.textContent = s.name.split(' ')[0].toUpperCase();
        }

        // FX palette dropdown (typing glow)
        const fxBtn = document.createElement('span');
        fxBtn.className = 'mud-terminal-btn fx-btn';
        fxBtn.textContent = 'FX';
        fxBtn.title = 'Typing glow palette';
        const fxMenu = document.createElement('div');
        fxMenu.className = 'fx-palette-menu';
        fxMenu.style.display = 'none';
        const palettes = ['none','fire','ice','poison','electric','rainbow','purple','blood','gold'];
        for (const p of palettes) {
            const opt = document.createElement('div');
            opt.className = 'fx-palette-opt';
            const info = MudTerminal.FX_PALETTES[p];
            opt.textContent = p === 'none' ? 'None' : info.name;
            opt.dataset.palette = p;
            opt.addEventListener('click', (e) => {
                e.stopPropagation();
                this._setFxPalette(p);
                fxMenu.style.display = 'none';
                for (const o of fxMenu.querySelectorAll('.fx-palette-opt')) {
                    o.classList.toggle('active', o.dataset.palette === p);
                }
            });
            if (p === this._fxPalette) opt.classList.add('active');
            fxMenu.appendChild(opt);
        }
        fxBtn.addEventListener('click', (e) => {
            e.stopPropagation();
            fxMenu.style.display = fxMenu.style.display === 'none' ? 'block' : 'none';
        });
        document.addEventListener('click', () => { fxMenu.style.display = 'none'; });
        fxBtn.appendChild(fxMenu);

        // ANSI weight dropdown (Original / All Bold / All Thin)
        this._ansiWeight = localStorage.getItem('ansiWeight') || 'bold';
        const weightBtn = document.createElement('span');
        weightBtn.className = 'mud-terminal-btn weight-btn';
        weightBtn.title = 'ANSI text weight';
        const weightLabel = document.createElement('span');
        weightLabel.textContent = this._ansiWeight === 'original' ? 'ANSI' :
            this._ansiWeight === 'bold' ? 'BOLD' : 'THIN';
        weightBtn.appendChild(weightLabel);
        const weightMenu = document.createElement('div');
        weightMenu.className = 'grad-scheme-menu';
        weightMenu.style.display = 'none';
        for (const [key, label, desc] of [
            ['original', 'Original', 'Use ANSI bold/normal as sent by game'],
            ['bold', 'All Bold', 'Force all text bold (uniform thick)'],
            ['thin', 'All Thin', 'Force all text normal weight (uniform thin)'],
        ]) {
            const opt = document.createElement('div');
            opt.className = 'grad-scheme-opt' + (key === this._ansiWeight ? ' active' : '');
            opt.dataset.weight = key;
            opt.innerHTML = `<span class="grad-scheme-name">${label}</span><span class="grad-scheme-desc">${desc}</span>`;
            opt.addEventListener('click', (e) => {
                e.stopPropagation();
                this._ansiWeight = key;
                localStorage.setItem('ansiWeight', key);
                weightMenu.style.display = 'none';
                for (const o of weightMenu.querySelectorAll('.grad-scheme-opt')) {
                    o.classList.toggle('active', o.dataset.weight === key);
                }
                weightLabel.textContent = key === 'original' ? 'ANSI' :
                    key === 'bold' ? 'BOLD' : 'THIN';
                this._applyAnsiWeight();
            });
            weightMenu.appendChild(opt);
        }
        weightBtn.addEventListener('click', (e) => {
            e.stopPropagation();
            weightMenu.style.display = weightMenu.style.display === 'none' ? 'block' : 'none';
        });
        document.addEventListener('click', () => { weightMenu.style.display = 'none'; });
        weightBtn.appendChild(weightMenu);

        // Text shadow checkbox (experimental)
        const shadowLabel = document.createElement('label');
        shadowLabel.className = 'mud-terminal-shadow-label';
        shadowLabel.title = 'Text shadow (experimental)';
        const shadowCheck = document.createElement('input');
        shadowCheck.type = 'checkbox';
        shadowCheck.checked = this._ansiGradient.shadowEnabled;
        shadowCheck.addEventListener('change', () => {
            this._ansiGradient.shadowEnabled = shadowCheck.checked;
            this._applyTextShadow(shadowCheck.checked);
        });
        shadowLabel.appendChild(shadowCheck);
        shadowLabel.appendChild(document.createTextNode(' Shadow'));
        this._shadowCheck = shadowCheck;

        controls.appendChild(gradBtn);
        controls.appendChild(weightBtn);
        controls.appendChild(fxBtn);
        controls.appendChild(shadowLabel);
        controls.appendChild(opSlider);
        controls.appendChild(modeBtn);
        controls.appendChild(lockBtn);
        controls.appendChild(closeBtn);
        header.appendChild(title);
        header.appendChild(locSpan);
        header.appendChild(controls);
        panel.appendChild(header);

        // Terminal container
        const termContainer = document.createElement('div');
        termContainer.className = 'mud-terminal-body';
        this._termContainer = termContainer;
        panel.appendChild(termContainer);

        // ── Input bar with auto-expanding textarea + chunk preview ──
        const inputBar = document.createElement('div');
        inputBar.className = 'mud-terminal-input-bar';

        // Chunk preview area (shows above input when multi-chunk)
        const chunkPreview = document.createElement('div');
        chunkPreview.className = 'mud-chunk-preview';
        chunkPreview.style.display = 'none';
        this._chunkPreview = chunkPreview;
        inputBar.appendChild(chunkPreview);

        const inputRow = document.createElement('div');
        inputRow.className = 'mud-input-row';

        const prompt = document.createElement('span');
        prompt.className = 'mud-terminal-prompt';
        prompt.textContent = '>';

        const input = document.createElement('textarea');
        input.className = 'mud-terminal-input';
        input.placeholder = 'Type command...';
        input.autocomplete = 'off';
        input.spellcheck = false;
        input.rows = 1;

        // Max chars per MUD line (after prefix)
        this._maxLineLen = 76;
        this._sending = false;

        // Auto-resize textarea height
        const autoResize = () => {
            input.style.height = 'auto';
            input.style.height = Math.min(input.scrollHeight, 120) + 'px';
        };

        input.addEventListener('input', () => {
            autoResize();
            this._onInputChange();
            this._updateChunkPreview();
        });

        input.addEventListener('keydown', (e) => {
            if (e.key === 'Enter' && !e.shiftKey) {
                e.preventDefault();
                if (this._sending) return; // already sending chunks
                const raw = input.value;
                if (raw.length === 0) {
                    // Empty enter — send bare CRLF
                    this._send('inject', { text: '' });
                    return;
                }

                // Save to history
                this._cmdHistory.push(raw);
                if (this._cmdHistory.length > 200) this._cmdHistory.shift();
                this._saveHistory();
                this._historyIdx = -1;
                this._currentInput = '';

                if (raw.startsWith('#')) {
                    const ghostName = localStorage.getItem('ghostName') || 'Yoder';
                    const atCmd = _fuzzyGhostCmd(raw.slice(1));
                    this._send('ghost', { name: ghostName, at_cmd: atCmd });
                    input.value = '';
                    this._charTimestamps = [];
                    autoResize();
                    this._updateChunkPreview();
                } else {
                    const chunks = this._buildChunks(raw);
                    input.value = '';
                    this._charTimestamps = [];
                    autoResize();
                    this._updateChunkPreview();
                    this._sendChunks(chunks);
                }
            } else if (e.key === 'Enter' && e.shiftKey) {
                // Allow Shift+Enter for manual newline
            } else if (e.key === 'ArrowUp' && input.selectionStart === 0) {
                e.preventDefault();
                if (this._historyIdx === -1) {
                    this._currentInput = input.value;
                    this._historyIdx = this._cmdHistory.length - 1;
                } else if (this._historyIdx > 0) {
                    this._historyIdx--;
                }
                if (this._historyIdx >= 0) {
                    input.value = this._cmdHistory[this._historyIdx];
                    autoResize();
                    this._onInputChange();
                    this._updateChunkPreview();
                }
            } else if (e.key === 'ArrowDown' && input.selectionStart === input.value.length) {
                e.preventDefault();
                if (this._historyIdx >= 0) {
                    this._historyIdx++;
                    if (this._historyIdx >= this._cmdHistory.length) {
                        this._historyIdx = -1;
                        input.value = this._currentInput;
                    } else {
                        input.value = this._cmdHistory[this._historyIdx];
                    }
                    autoResize();
                    this._onInputChange();
                    this._updateChunkPreview();
                }
            }
        });
        this._input = input;

        // Canvas overlay for glow typing FX
        const inputWrap = document.createElement('div');
        inputWrap.className = 'mud-input-wrap';

        const fxCanvas = document.createElement('canvas');
        fxCanvas.className = 'mud-input-fx';
        fxCanvas.style.cssText = 'position:absolute;top:0;left:0;width:100%;height:100%;pointer-events:none;';
        this._fxCanvas = fxCanvas;
        this._fxCtx = fxCanvas.getContext('2d');
        this._charTimestamps = [];
        this._fxPalette = localStorage.getItem('termFxPalette') || 'none';
        this._fxRaf = null;

        inputWrap.appendChild(input);
        inputWrap.appendChild(fxCanvas);

        inputRow.appendChild(prompt);
        inputRow.appendChild(inputWrap);
        inputBar.appendChild(inputRow);
        panel.appendChild(inputBar);

        // Start FX loop and apply saved palette
        this._startFxLoop();
        if (this._fxPalette !== 'none') this._setFxPalette(this._fxPalette);

        document.body.appendChild(panel);
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

    _applyAnsiWeight() {
        if (!this._term) return;
        if (this._ansiWeight === 'bold') {
            // Force bold: set normal weight to bold, bold stays bold
            this._term.options.fontWeight = 'bold';
            this._term.options.fontWeightBold = 'bold';
        } else if (this._ansiWeight === 'thin') {
            // Force thin: set both to normal
            this._term.options.fontWeight = 'normal';
            this._term.options.fontWeightBold = 'normal';
        } else {
            // Original: normal weight for normal text, bold for bold
            this._term.options.fontWeight = 'normal';
            this._term.options.fontWeightBold = 'bold';
        }
        // Refit to recalculate char metrics after weight change
        if (this._fitAddon) {
            setTimeout(() => this._fitAddon.fit(), 10);
        }
    }

    _applyTextShadow(enabled) {
        // xterm renders text to canvas, so CSS text-shadow doesn't apply directly.
        // Instead we add a CSS class that applies a drop-shadow filter to the text canvas layer.
        if (this._termContainer) {
            this._termContainer.classList.toggle('text-shadow-on', enabled);
        }
    }

    _initXterm() {
        if (this._term) return;

        const term = new Terminal({
            cursorBlink: true,
            cursorStyle: 'underline',
            disableStdin: false,
            scrollback: 0,  // no scroll — use Alt+B backscroll popup instead
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

        // Apply text shadow if enabled
        if (this._ansiGradient.shadowEnabled) {
            setTimeout(() => this._applyTextShadow(true), 50);
        }

        // Apply ANSI weight mode
        setTimeout(() => this._applyAnsiWeight(), 50);

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

        // Observe panel resize to refit + scale font
        this._resizeObserver = new ResizeObserver(() => {
            if (this._visible) this._scaleToFit();
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
        if (show) {
            this._initXterm();
            setTimeout(() => this._scaleToFit(), 50);
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
        // Apply ANSI gradient if enabled
        const processed = this._ansiGradient ? this._ansiGradient.process(data) : data;
        if (this._term) {
            this._term.write(processed);
        } else {
            this._buffer.push(processed);
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
            const topBarH = 40; // height of top bar — panels must stay below
            const headerH = 32; // panel's own header height
            newTop = Math.max(topBarH, Math.min(newTop, window.innerHeight - headerH));
            newLeft = Math.max(-rect.width + 80, Math.min(newLeft, window.innerWidth - 80));
            this._el.style.left = `${newLeft}px`;
            this._el.style.top = `${newTop}px`;
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

    _scaleToFit() {
        const el = this._el;
        const BASE_WIDTH = 700;
        const BASE_FONT = 13;
        const MIN_FONT = 9;
        const MAX_FONT = 24;
        const w = el.offsetWidth;
        const scale = w / BASE_WIDTH;
        const newFont = Math.round(Math.min(MAX_FONT, Math.max(MIN_FONT, BASE_FONT * scale)));
        if (this._term && this._term.options.fontSize !== newFont) {
            this._term.options.fontSize = newFont;
        }
        // Scale input bar to match
        const inputEl = el.querySelector('.mud-terminal-input');
        const promptEl = el.querySelector('.mud-terminal-prompt');
        if (inputEl) {
            inputEl.style.fontSize = newFont + 'px';
            inputEl.style.padding = Math.round(4 * scale) + 'px ' + Math.round(8 * scale) + 'px';
            inputEl.style.minHeight = Math.round(24 * scale) + 'px';
            inputEl.style.maxHeight = Math.round(120 * scale) + 'px';
        }
        if (promptEl) {
            promptEl.style.fontSize = newFont + 'px';
            promptEl.style.padding = Math.round(5 * scale) + 'px ' + Math.round(4 * scale) + 'px ' +
                Math.round(4 * scale) + 'px ' + Math.round(8 * scale) + 'px';
        }
        if (this._fitAddon) this._fitAddon.fit();
    }

    _initResize() {
        // Save state after CSS resize handle is released
        const el = this._el;
        let resizing = false;
        el.addEventListener('mousedown', () => { resizing = true; });
        document.addEventListener('mouseup', () => {
            if (resizing) {
                resizing = false;
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
            // Clamp position to keep header accessible (below top bar, on screen)
            requestAnimationFrame(() => {
                const r = this._el.getBoundingClientRect();
                const topBarH = 40;
                let changed = false;
                if (r.top < topBarH) { this._el.style.top = topBarH + 'px'; changed = true; }
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

    // ── Typing Glow FX ──

    static FX_PALETTES = {
        none:     null,
        fire:     { colors: ['#ff4400','#ff8800','#ffcc00','#ffee88'], glow: '#ff6600', name: 'Fire' },
        ice:      { colors: ['#00ccff','#44ddff','#88eeff','#ccf8ff'], glow: '#00aaff', name: 'Ice' },
        poison:   { colors: ['#00ff44','#44ff88','#88ffaa','#ccffcc'], glow: '#00ff44', name: 'Poison' },
        electric: { colors: ['#ffff00','#ffee44','#eedd88','#ddddaa'], glow: '#ffff00', name: 'Electric' },
        rainbow:  { colors: null, glow: null, name: 'Rainbow', rainbow: true },
        purple:   { colors: ['#cc44ff','#dd88ff','#eeaaff','#f0ccff'], glow: '#bb44ff', name: 'Arcane' },
        blood:    { colors: ['#ff0000','#cc0022','#ff4444','#ff8888'], glow: '#ff0000', name: 'Blood' },
        gold:     { colors: ['#ffd700','#ffcc00','#eeaa00','#ddcc88'], glow: '#ffcc00', name: 'Gold' },
    };

    _onInputChange() {
        const val = this._input.value;
        const now = performance.now();
        // Grow timestamps array to match, trimming if chars deleted
        while (this._charTimestamps.length < val.length) {
            this._charTimestamps.push(now);
        }
        this._charTimestamps.length = val.length;
    }

    _startFxLoop() {
        const loop = () => {
            this._fxRaf = requestAnimationFrame(loop);
            if (this._fxPalette === 'none' || !this._visible) return;
            this._renderFx();
        };
        loop();
    }

    _renderFx() {
        const canvas = this._fxCanvas;
        const ctx = this._fxCtx;
        const input = this._input;
        const val = input.value;
        const dpr = window.devicePixelRatio || 1;
        const rect = input.getBoundingClientRect();
        const w = rect.width, h = rect.height;
        const cw = Math.round(w * dpr);
        const ch = Math.round(h * dpr);

        // Resize backing store to match input at device resolution
        if (canvas.width !== cw || canvas.height !== ch) {
            canvas.width = cw;
            canvas.height = ch;
        }

        // MUST set transform every frame (canvas state resets on resize)
        ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
        ctx.clearRect(0, 0, w, h);

        if (!val) return;

        const palette = MudTerminal.FX_PALETTES[this._fxPalette];
        if (!palette) return;

        const style = getComputedStyle(input);
        const fontSize = parseFloat(style.fontSize) || 13;
        const fontFamily = style.fontFamily || 'monospace';
        ctx.font = `${fontSize}px ${fontFamily}`;
        ctx.textBaseline = 'middle';

        const now = performance.now();
        const fadeMs = 2000;
        const padLeft = (parseFloat(style.paddingLeft) || 8) + (parseFloat(style.borderLeftWidth) || 1);
        const scrollLeft = input.scrollLeft || 0;
        const yMid = h / 2;

        let x = padLeft - scrollLeft;
        for (let i = 0; i < val.length; i++) {
            const c = val[i];
            const charW = ctx.measureText(c).width;
            const age = now - (this._charTimestamps[i] || now);
            const t = Math.min(1, age / fadeMs); // 0 = fresh, 1 = fully faded

            // ── Per-character color from palette ──
            let r, g, b;
            if (palette.rainbow) {
                const hue = (i * 47) % 360;
                // HSL → RGB for smooth fade
                const s = 1 - t * 0.7;
                const l = 0.65 - t * 0.2;
                const [cr, cg, cb] = this._hsl2rgb(hue / 360, s, l);
                r = cr; g = cg; b = cb;
            } else {
                // Interpolate through palette colors based on age
                const pos = t * (palette.colors.length - 1);
                const idx = Math.min(Math.floor(pos), palette.colors.length - 2);
                const frac = pos - idx;
                const c1 = this._hexToRgb(palette.colors[idx]);
                const c2 = this._hexToRgb(palette.colors[idx + 1]);
                r = Math.round(c1[0] + (c2[0] - c1[0]) * frac);
                g = Math.round(c1[1] + (c2[1] - c1[1]) * frac);
                b = Math.round(c1[2] + (c2[2] - c1[2]) * frac);
            }

            // Fade to dull white in the last 30%
            if (t > 0.7) {
                const f = (t - 0.7) / 0.3;
                r = Math.round(r + (180 - r) * f);
                g = Math.round(g + (180 - g) * f);
                b = Math.round(b + (180 - b) * f);
            }

            // Glow halo — strong when fresh, fades out
            const glow = Math.max(0, 1 - t);
            if (glow > 0.05) {
                const glowAlpha = (glow * 0.7).toFixed(2);
                if (palette.rainbow) {
                    ctx.shadowColor = `rgba(${r},${g},${b},${glowAlpha})`;
                } else {
                    const gc = this._hexToRgb(palette.glow);
                    ctx.shadowColor = `rgba(${gc[0]},${gc[1]},${gc[2]},${glowAlpha})`;
                }
                ctx.shadowBlur = 12 * glow;
            } else {
                ctx.shadowColor = 'transparent';
                ctx.shadowBlur = 0;
            }

            ctx.fillStyle = `rgb(${r},${g},${b})`;
            ctx.fillText(c, x, yMid);
            x += charW;
        }

        ctx.shadowColor = 'transparent';
        ctx.shadowBlur = 0;
    }

    _hexToRgb(hex) {
        const m = hex.match(/^#?([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i);
        if (m) return [parseInt(m[1],16), parseInt(m[2],16), parseInt(m[3],16)];
        return [180, 180, 180];
    }

    _hsl2rgb(h, s, l) {
        let r, g, b;
        if (s === 0) { r = g = b = l; }
        else {
            const hue2rgb = (p, q, t) => {
                if (t < 0) t += 1; if (t > 1) t -= 1;
                if (t < 1/6) return p + (q - p) * 6 * t;
                if (t < 1/2) return q;
                if (t < 2/3) return p + (q - p) * (2/3 - t) * 6;
                return p;
            };
            const q = l < 0.5 ? l * (1 + s) : l + s - l * s;
            const p = 2 * l - q;
            r = hue2rgb(p, q, h + 1/3);
            g = hue2rgb(p, q, h);
            b = hue2rgb(p, q, h - 1/3);
        }
        return [Math.round(r * 255), Math.round(g * 255), Math.round(b * 255)];
    }

    _setFxPalette(name) {
        this._fxPalette = name;
        localStorage.setItem('termFxPalette', name);
        // Make input text transparent when FX active so canvas shows through
        if (name === 'none') {
            this._input.style.color = '';
        } else {
            this._input.style.color = 'transparent';
            this._input.style.caretColor = '#88ff88';
        }
        // Reset timestamps so current text glows fresh
        const now = performance.now();
        this._charTimestamps = Array(this._input.value.length).fill(now);
    }

    // ── Chat command prefixes and chunking ──

    // Known MUD chat prefixes: command → what to prepend on continuation lines
    static CHAT_PREFIXES = [
        { re: /^(gos(?:sip)?)\s+/i,      prefix: 'gos ' },
        { re: /^(br(?:oadcast)?)\s+/i,    prefix: 'br ' },
        { re: /^(say)\s+/i,              prefix: 'say ' },
        { re: /^\.\s*/,                   prefix: '. ' },      // say shorthand
        { re: /^(tell)\s+(\S+)\s+/i,     prefix: null, build: (m) => `tell ${m[2]} ` },
        { re: /^(bg|gb)\s+/i,            prefix: 'bg ' },     // gangpath
        { re: /^(yell)\s+/i,             prefix: 'yell ' },
        { re: /^\/(\S+)\s+/i,            prefix: null, build: (m) => `/${m[1]} ` },  // telepath
        { re: /^>(\S+)\s+/i,             prefix: null, build: (m) => `>${m[1]} ` },  // diverted say
        { re: /^(auction)\s+/i,          prefix: 'auction ' },
    ];

    _detectPrefix(text) {
        for (const p of MudTerminal.CHAT_PREFIXES) {
            const m = text.match(p.re);
            if (m) {
                const pfx = p.build ? p.build(m) : p.prefix;
                const body = text.slice(m[0].length);
                return { prefix: pfx, body, fullFirst: m[0] };
            }
        }
        return null; // not a chat command — send as-is
    }

    _buildChunks(raw) {
        const detected = this._detectPrefix(raw);
        if (!detected) return [raw]; // single command, no chunking

        const { prefix, body, fullFirst } = detected;
        const maxBody = this._maxLineLen - prefix.length;
        if (maxBody <= 10) return [raw]; // prefix too long, just send raw

        // Split body into word-wrapped chunks
        const words = body.split(/\s+/);
        const chunks = [];
        let current = '';

        for (const word of words) {
            if (!word) continue;
            const test = current ? current + ' ' + word : word;
            if (test.length > maxBody && current) {
                chunks.push(current);
                current = word;
            } else {
                current = test;
            }
        }
        if (current) chunks.push(current);

        // Build final commands: first uses original prefix, rest use continuation prefix
        return chunks.map((chunk, i) => {
            if (i === 0) return fullFirst + chunk;
            return prefix + chunk;
        });
    }

    _updateChunkPreview() {
        const raw = this._input.value;
        const preview = this._chunkPreview;
        if (!raw || raw.length <= this._maxLineLen) {
            preview.style.display = 'none';
            preview.innerHTML = '';
            return;
        }

        const chunks = this._buildChunks(raw);
        if (chunks.length <= 1) {
            preview.style.display = 'none';
            preview.innerHTML = '';
            return;
        }

        preview.style.display = '';
        preview.innerHTML = '';
        const colors = ['rgba(60,80,120,0.3)', 'rgba(80,60,100,0.3)'];
        for (let i = 0; i < chunks.length; i++) {
            const row = document.createElement('div');
            row.className = 'mud-chunk-row';
            row.style.background = colors[i % 2];
            const label = document.createElement('span');
            label.className = 'mud-chunk-label';
            label.textContent = `${i + 1}`;
            const text = document.createElement('span');
            text.className = 'mud-chunk-text';
            text.textContent = chunks[i];
            row.appendChild(label);
            row.appendChild(text);
            preview.appendChild(row);
        }
    }

    async _sendChunks(chunks) {
        if (chunks.length <= 1) {
            this._send('inject', { text: chunks[0] || '' });
            return;
        }
        this._sending = true;
        this._input.disabled = true;
        this._input.placeholder = 'Sending...';
        for (let i = 0; i < chunks.length; i++) {
            this._send('inject', { text: chunks[i] });
            if (i < chunks.length - 1) {
                await new Promise(r => setTimeout(r, 3000));
            }
        }
        this._input.disabled = false;
        this._input.placeholder = 'Type command...';
        this._sending = false;
        this._input.focus();
    }
}
