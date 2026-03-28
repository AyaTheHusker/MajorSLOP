// ── Chat Panel — unified chat window with channel filters and command history ──

const CHAT_CHANNELS = {
    gossip:    { label: 'Gos',   color: '#ffb088' },
    broadcast: { label: 'Broad', color: '#eedd44' },
    gangpath:  { label: 'Gang',  color: '#aa8844' },
    telepath:  { label: 'Tell',  color: '#dd88cc' },
    say:       { label: 'Say',   color: '#8888dd' },
    auction:   { label: 'Auct',  color: '#cccc44' },
};

class ChatPanel {
    constructor(wsSend) {
        this._wsSend = wsSend;
        this._visible = false;
        this._locked = false;
        this._history = [];
        this._histIdx = -1;
        this._tempInput = '';
        this._maxHistory = 200;
        this._maxMessages = 500;
        this._filters = {};  // channel -> bool (true = visible)
        this._broadcastChannel = '';
        this._el = null;
        this._build();
        this._initDrag();
        this._restoreState();
        this._loadHistory();
    }

    _build() {
        const panel = document.createElement('div');
        panel.id = 'chat-panel';
        panel.className = 'chat-panel';
        panel.style.display = 'none';

        // Header
        const header = document.createElement('div');
        header.className = 'chat-panel-header';

        const title = document.createElement('span');
        title.className = 'chat-panel-title';
        title.textContent = 'Chat';

        const bcLabel = document.createElement('span');
        bcLabel.className = 'chat-bc-label';
        bcLabel.textContent = 'BC: None';
        this._bcLabel = bcLabel;

        const controls = document.createElement('div');
        controls.className = 'chat-panel-controls';

        const lockBtn = document.createElement('span');
        lockBtn.className = 'chat-panel-btn';
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
        closeBtn.className = 'chat-panel-btn';
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
        opSlider.value = '92';
        opSlider.title = 'Panel transparency';
        opSlider.addEventListener('input', () => {
            this._applyOpacity(Number(opSlider.value));
            this._saveState();
        });
        this._opSlider = opSlider;

        controls.appendChild(opSlider);
        controls.appendChild(lockBtn);
        controls.appendChild(closeBtn);
        header.appendChild(title);
        header.appendChild(bcLabel);
        header.appendChild(controls);
        panel.appendChild(header);

        // Channel filter bar
        const filterBar = document.createElement('div');
        filterBar.className = 'chat-filter-bar';
        this._filterEls = {};
        for (const [key, ch] of Object.entries(CHAT_CHANNELS)) {
            this._filters[key] = true;
            const lbl = document.createElement('label');
            lbl.className = 'chat-filter';
            lbl.style.color = ch.color;

            const cb = document.createElement('input');
            cb.type = 'checkbox';
            cb.checked = true;
            cb.addEventListener('change', () => {
                this._filters[key] = cb.checked;
                this._applyFilters();
                this._saveState();
            });
            lbl.appendChild(cb);
            lbl.appendChild(document.createTextNode(ch.label));
            filterBar.appendChild(lbl);
            this._filterEls[key] = cb;
        }
        panel.appendChild(filterBar);

        // Messages area
        const msgArea = document.createElement('div');
        msgArea.className = 'chat-messages';
        this._msgArea = msgArea;
        panel.appendChild(msgArea);

        // Input row — raw command input, type "gos hello" etc
        const inputRow = document.createElement('div');
        inputRow.className = 'chat-input-row';

        const input = document.createElement('input');
        input.type = 'text';
        input.className = 'chat-input';
        input.placeholder = '';
        input.autocomplete = 'off';
        input.spellcheck = false;

        input.addEventListener('keydown', (e) => {
            if (e.key === 'Enter') {
                e.preventDefault();
                this._send();
            } else if (e.key === 'ArrowUp') {
                e.preventDefault();
                this._historyUp();
            } else if (e.key === 'ArrowDown') {
                e.preventDefault();
                this._historyDown();
            }
        });
        this._input = input;
        inputRow.appendChild(input);

        panel.appendChild(inputRow);
        document.body.appendChild(panel);
        this._el = panel;
    }

    _applyOpacity(val) {
        const alpha = val / 100;
        const blur = Math.round((1 - alpha) * 20);
        this._el.style.background = `rgba(16, 16, 28, ${alpha * 0.3})`;
        this._el.style.backdropFilter = blur > 0 ? `blur(${blur}px)` : 'none';
        this._el.style.webkitBackdropFilter = blur > 0 ? `blur(${blur}px)` : 'none';
        this._opacity = val;
        // Make message area semi-transparent too
        this._msgArea.style.background = `rgba(16, 16, 28, ${alpha * 0.5})`;
    }

    _send() {
        const text = this._input.value.trim();
        if (!text) return;

        // Add to history
        if (this._history.length === 0 || this._history[this._history.length - 1] !== text) {
            this._history.push(text);
            if (this._history.length > this._maxHistory) this._history.shift();
            this._saveHistory();
        }
        this._histIdx = -1;
        this._tempInput = '';

        // Send raw command to MUD
        this._wsSend('inject', { text: text });

        this._input.value = '';
    }

    _historyUp() {
        if (this._history.length === 0) return;
        if (this._histIdx === -1) {
            this._tempInput = this._input.value;
            this._histIdx = this._history.length - 1;
        } else if (this._histIdx > 0) {
            this._histIdx--;
        }
        this._input.value = this._history[this._histIdx];
    }

    _historyDown() {
        if (this._histIdx === -1) return;
        if (this._histIdx < this._history.length - 1) {
            this._histIdx++;
            this._input.value = this._history[this._histIdx];
        } else {
            this._histIdx = -1;
            this._input.value = this._tempInput;
        }
    }

    addChatMessage(sender, message, channel) {
        const ch = CHAT_CHANNELS[channel] || CHAT_CHANNELS.say;
        const el = document.createElement('div');
        el.className = 'chat-msg';
        el.dataset.channel = channel;

        const chanTag = document.createElement('span');
        chanTag.className = 'chat-msg-channel';
        chanTag.textContent = `[${ch.label}]`;
        chanTag.style.color = ch.color;
        el.appendChild(chanTag);

        const senderEl = document.createElement('span');
        senderEl.className = 'chat-msg-sender';
        senderEl.textContent = ` ${sender}: `;
        el.appendChild(senderEl);

        const msgEl = document.createElement('span');
        msgEl.className = 'chat-msg-text';
        msgEl.textContent = message;
        msgEl.style.color = ch.color;
        el.appendChild(msgEl);

        // Apply filter visibility
        if (!this._filters[channel]) {
            el.style.display = 'none';
        }

        this._msgArea.appendChild(el);

        // Trim old messages
        while (this._msgArea.children.length > this._maxMessages) {
            this._msgArea.removeChild(this._msgArea.firstChild);
        }

        // Auto-scroll if near bottom
        const area = this._msgArea;
        const atBottom = area.scrollHeight - area.scrollTop - area.clientHeight < 40;
        if (atBottom) area.scrollTop = area.scrollHeight;
    }

    _applyFilters() {
        for (const msg of this._msgArea.children) {
            const ch = msg.dataset.channel;
            msg.style.display = this._filters[ch] !== false ? '' : 'none';
        }
    }

    setBroadcastChannel(channel) {
        this._broadcastChannel = channel || '';
        this._bcLabel.textContent = this._broadcastChannel
            ? `BC: ${this._broadcastChannel}`
            : 'BC: None';
        this._bcLabel.style.color = this._broadcastChannel
            ? CHAT_CHANNELS.broadcast.color
            : '#666';
    }

    toggle(show) {
        if (show === undefined) show = !this._visible;
        this._visible = show;
        this._el.style.display = show ? '' : 'none';
        if (show) this._input.focus();
        this._saveState();
    }

    _initDrag() {
        let dragging = false;
        let startX, startY, origLeft, origTop;
        const header = this._el.querySelector('.chat-panel-header');

        header.addEventListener('mousedown', (e) => {
            if (this._locked) return;
            if (e.button !== 0) return;
            if (e.target.closest('.panel-opacity-slider') || e.target.closest('.chat-panel-controls')) return;
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
            nt = Math.max(0, Math.min(nt, window.innerHeight - 32));
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

    _saveState() {
        try {
            const saved = JSON.parse(localStorage.getItem('panelPositions') || '{}');
            saved['chat-panel'] = {
                left: this._el.style.left,
                top: this._el.style.top,
                visible: this._visible,
                locked: this._locked,
                filters: this._filters,
                opacity: this._opacity,
            };
            localStorage.setItem('panelPositions', JSON.stringify(saved));
        } catch {}
    }

    _restoreState() {
        try {
            const saved = JSON.parse(localStorage.getItem('panelPositions') || '{}');
            const s = saved['chat-panel'];
            if (s) {
                if (s.left) this._el.style.left = s.left;
                if (s.top) this._el.style.top = s.top;
                if (s.locked) {
                    this._locked = true;
                    this._lockBtn.textContent = '\u{1F512}';
                }
                if (s.opacity != null) {
                    this._opSlider.value = s.opacity;
                    this._applyOpacity(s.opacity);
                }
                if (s.filters) {
                    for (const [key, val] of Object.entries(s.filters)) {
                        if (key in this._filters) {
                            this._filters[key] = val;
                            if (this._filterEls[key]) this._filterEls[key].checked = val;
                        }
                    }
                }
                if (s.visible) this.toggle(true);
            }
        } catch {}
    }

    _saveHistory() {
        try {
            localStorage.setItem('chatHistory', JSON.stringify(this._history));
        } catch {}
    }

    _loadHistory() {
        try {
            const h = JSON.parse(localStorage.getItem('chatHistory') || '[]');
            if (Array.isArray(h)) this._history = h.slice(-this._maxHistory);
        } catch {}
    }
}
