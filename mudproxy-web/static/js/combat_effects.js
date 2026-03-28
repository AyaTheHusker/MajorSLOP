// ── Combat Effects: Canvas 2D Floating Damage Numbers + Shatter ──
// Renders outlined, glowing damage text over monster thumbnails.
// Death shatter: thumbnail explodes into fragments with physics.

class CombatEffects {
    constructor(container) {
        this.containerEl = container;
        this.canvas = document.createElement('canvas');
        this.canvas.style.cssText = 'position:fixed;top:0;left:0;width:100vw;height:100vh;pointer-events:none;z-index:9000;';
        this.containerEl.appendChild(this.canvas);
        this.ctx = this.canvas.getContext('2d');
        this.effects = [];    // damage numbers
        this.shatters = [];   // shatter effects
        this.coinDrops = [];  // coin drop animations
        this._pendingCoinDrops = 0;
        this._shatterCaptured = null;  // pre-captured shatter data for current kill
        this._shatterFired = false;
        this._animating = false;
        this._resize();
        window.addEventListener('resize', () => this._resize());
    }

    _resize() {
        const dpr = window.devicePixelRatio || 1;
        this.canvas.width = window.innerWidth * dpr;
        this.canvas.height = window.innerHeight * dpr;
        this.ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    }

    _findTargetThumb(name) {
        if (!name) return null;
        const lower = name.toLowerCase();
        for (const t of document.querySelectorAll('#npc-thumbs .thumb')) {
            if (t.title && t.title.toLowerCase() === lower) return t;
        }
        const first = document.querySelector('#npc-thumbs .thumb');
        return first || null;
    }

    // ── Damage Numbers ──

    addDamage(amount, type = 'normal', targetEl = null) {
        const now = performance.now() / 1000;
        let x, y;
        if (targetEl) {
            const rect = targetEl.getBoundingClientRect();
            x = rect.left + rect.width / 2;
            y = rect.top + rect.height * 0.25;
        } else {
            x = window.innerWidth * 0.5;
            y = window.innerHeight * 0.35;
        }

        const activeCount = this.effects.filter(e => e.alive).length;
        x += (Math.random() - 0.5) * 30 + (activeCount % 2 ? 16 : -16);
        y += (Math.random() - 0.5) * 12 - activeCount * 18;
        const driftX = (Math.random() - 0.5) * 40;
        const duration = (type === 'crit' || type === 'backstab') ? 1.6 : 1.2;

        let text;
        if (type === 'backstab') text = `BS ${amount}!`;
        else if (type === 'crit') text = `CRIT ${amount}!`;
        else text = String(amount);

        let color;
        if (type === 'backstab') color = [0.7, 0.2, 1.0];
        else if (type === 'crit') color = [1.0, 0.85, 0.15];
        else if (type === 'heal') color = [0.2, 1.0, 0.3];
        else color = [1.0, 0.15, 0.1];

        this.effects.push({ x, y, driftX, text, type, color, amount, birth: now, duration, alive: true });
        if (!this._animating) this._startLoop();
    }

    parseCombatLine(line, target = null) {
        const dmgMatch = line.match(/for (\d+) damage/i);
        if (dmgMatch) {
            const amount = parseInt(dmgMatch[1]);
            let type = 'normal';
            if (/critical|crit/i.test(line)) type = 'crit';
            else if (/backstab|surprise/i.test(line)) type = 'backstab';
            this.addDamage(amount, type, this._findTargetThumb(target));
            return;
        }
        const healMatch = line.match(/heals?\s+(?:you\s+)?(?:for\s+)?(\d+)/i);
        if (healMatch) this.addDamage(parseInt(healMatch[1]), 'heal');
    }

    // ── Shatter Effect ──

    _captureShatter(targetEl) {
        // Capture thumbnail image data before DOM removes it
        if (!targetEl) return null;
        const img = targetEl.querySelector('img');
        if (!img || !img.complete || !img.naturalWidth) return null;

        const rect = targetEl.getBoundingClientRect();
        if (rect.width < 1 || rect.height < 1) return null;

        const offscreen = document.createElement('canvas');
        offscreen.width = Math.ceil(rect.width);
        offscreen.height = Math.ceil(rect.height);
        offscreen.getContext('2d').drawImage(img, 0, 0, rect.width, rect.height);

        return {
            offscreen,
            rect: { left: rect.left, top: rect.top, width: rect.width, height: rect.height },
        };
    }

    // Legacy — kept for non-coin-drop kills
    captureShatter(targetEl) {
        // Capture the thumbnail image data NOW before the DOM removes it,
        // then fire the actual shatter after a 200ms delay for coin drops
        if (!targetEl) return;
        const img = targetEl.querySelector('img');
        if (!img || !img.complete || !img.naturalWidth) return;

        const rect = targetEl.getBoundingClientRect();
        if (rect.width < 1 || rect.height < 1) return;

        const offscreen = document.createElement('canvas');
        offscreen.width = Math.ceil(rect.width);
        offscreen.height = Math.ceil(rect.height);
        const offCtx = offscreen.getContext('2d');
        offCtx.drawImage(img, 0, 0, rect.width, rect.height);

        const captured = {
            offscreen,
            rect: { left: rect.left, top: rect.top, width: rect.width, height: rect.height },
        };
        setTimeout(() => this._spawnShatterFromCapture(captured), 200);
    }

    _spawnShatterFromCapture(captured) {
        const { offscreen, rect } = captured;
        const cols = 6, rows = 6;
        const shardW = rect.width / cols;
        const shardH = rect.height / rows;
        const centerX = rect.left + rect.width / 2;
        const centerY = rect.top + rect.height / 2;

        const shards = [];
        for (let row = 0; row < rows; row++) {
            for (let col = 0; col < cols; col++) {
                const sx = col * shardW;
                const sy = row * shardH;
                const px = rect.left + sx + shardW / 2;
                const py = rect.top + sy + shardH / 2;
                const dx = px - centerX;
                const dy = py - centerY;
                const dist = Math.hypot(dx, dy) || 1;
                const speed = 80 + Math.random() * 180;
                const vx = (dx / dist) * speed + (Math.random() - 0.5) * 60;
                const vy = (dy / dist) * speed + (-60 - Math.random() * 40);
                shards.push({
                    sx, sy, sw: shardW, sh: shardH,
                    x: px, y: py, vx, vy,
                    angle: Math.random() * Math.PI * 2,
                    angularVel: (Math.random() - 0.5) * 16,
                });
            }
        }

        this.shatters.push({
            shards, source: offscreen,
            birth: performance.now() / 1000,
            duration: 1.6, gravity: 350, alive: true,
        });
        if (!this._animating) this._startLoop();
    }

    spawnShatter(targetEl) {
        if (!targetEl) return;
        const img = targetEl.querySelector('img');
        if (!img || !img.complete || !img.naturalWidth) return;

        const rect = targetEl.getBoundingClientRect();
        if (rect.width < 1 || rect.height < 1) return;
        const cols = 6, rows = 6;
        const shardW = rect.width / cols;
        const shardH = rect.height / rows;
        const centerX = rect.left + rect.width / 2;
        const centerY = rect.top + rect.height / 2;

        // Draw thumbnail to offscreen canvas to get pixel data
        const offscreen = document.createElement('canvas');
        offscreen.width = rect.width;
        offscreen.height = rect.height;
        const offCtx = offscreen.getContext('2d');
        offCtx.drawImage(img, 0, 0, rect.width, rect.height);

        const shards = [];
        for (let row = 0; row < rows; row++) {
            for (let col = 0; col < cols; col++) {
                const sx = col * shardW;
                const sy = row * shardH;
                const px = rect.left + sx + shardW / 2;
                const py = rect.top + sy + shardH / 2;

                // Velocity: outward from center
                const dx = px - centerX;
                const dy = py - centerY;
                const dist = Math.hypot(dx, dy) || 1;
                const speed = 80 + Math.random() * 180;
                const vx = (dx / dist) * speed + (Math.random() - 0.5) * 60;
                const vy = (dy / dist) * speed + (-60 - Math.random() * 40); // bias upward

                shards.push({
                    sx, sy, sw: shardW, sh: shardH,
                    x: px, y: py,
                    vx, vy,
                    angle: Math.random() * Math.PI * 2,
                    angularVel: (Math.random() - 0.5) * 16,
                });
            }
        }

        this.shatters.push({
            shards,
            source: offscreen,
            birth: performance.now() / 1000,
            duration: 1.6,
            gravity: 350,
            alive: true,
        });

        if (!this._animating) this._startLoop();
    }

    // ── Coin Drop Effect ──

    queueCoinDrop(amount, coinType, targetName) {
        this._pendingCoinDrops++;
        // Find the monster thumbnail (source)
        const sourceEl = this._findTargetThumb(targetName);
        if (!sourceEl) {
            this._pendingCoinDrops--;
            return;
        }

        // Capture shatter image NOW before DOM removes the thumbnail
        if (!this._shatterCaptured) {
            this._shatterCaptured = this._captureShatter(sourceEl);
        }

        const sourceRect = sourceEl.getBoundingClientRect();
        const srcX = sourceRect.left + sourceRect.width / 2;
        const srcY = sourceRect.top + sourceRect.height / 2;

        // Find where the coin pile is (or will be)
        const destPos = this._findCoinPileDest(coinType);

        const ct = (typeof COIN_TYPES !== 'undefined' && COIN_TYPES[coinType]) ||
            { color: '#ffd700', shine: '#fffacd', edge: '#b8960f', size: 7 };

        // Scale duration by distance — travel at ~800px/sec baseline
        const dist = Math.hypot(destPos.x - srcX, destPos.y - srcY);
        const duration = Math.max(0.35, Math.min(1.0, dist / 800));

        // Generate coin particles
        const visualCount = Math.min(30, Math.max(3, Math.ceil(Math.sqrt(amount) * 2)));
        const coins = [];
        for (let i = 0; i < visualCount; i++) {
            const delay = i * 0.015 + Math.random() * 0.02;
            const startX = srcX + (Math.random() - 0.5) * sourceRect.width * 0.5;
            const startY = srcY + (Math.random() - 0.5) * sourceRect.height * 0.3;
            const endX = destPos.x + (Math.random() - 0.5) * 40;
            const endY = destPos.y + (Math.random() - 0.5) * 20;
            coins.push({
                startX, startY, endX, endY, delay,
                x: startX, y: startY,
                angle: Math.random() * Math.PI * 2,
                angularVel: (Math.random() - 0.5) * 12,
                size: ct.size * 1.8 + Math.random() * 3,
                tilt: 0.3 + Math.random() * 0.5,
            });
        }

        // Fire shatter when coins are ~50% through flight
        const shatterDelay = (duration * 0.5) * 1000;
        if (this._shatterCaptured && !this._shatterFired) {
            this._shatterFired = true;
            const cap = this._shatterCaptured;
            setTimeout(() => {
                this._spawnShatterFromCapture(cap);
            }, shatterDelay);
        }

        // Spawn "+X" text on pile after coins land
        const landDelay = (duration + 0.05) * 1000;
        setTimeout(() => {
            this._pendingCoinDrops = Math.max(0, this._pendingCoinDrops - 1);
            const freshDest = this._findCoinPileDest(coinType);
            this.effects.push({
                x: freshDest.x,
                y: freshDest.y,
                birth: performance.now() / 1000,
                duration: 1.2,
                alive: true,
                amount,
                type: 'coin_text',
                coinColor: ct.color,
                coinShine: ct.shine,
                coinTextColor: ct.textColor || ct.shine,
                coinGlow: ct.glow || null,
                driftX: 0,
            });
            if (!this._animating) this._startLoop();
        }, landDelay);

        this.coinDrops.push({
            coins, ct, amount, coinType,
            birth: performance.now() / 1000,
            duration,
            alive: true,
        });

        if (!this._animating) this._startLoop();
    }

    _findCoinPileDest(coinType) {
        // Look for existing coin pile of this type in item-thumbs
        const existing = document.querySelector(`#item-thumbs .coin-pile[data-currency="${coinType}"]`);
        if (existing) {
            const r = existing.getBoundingClientRect();
            return { x: r.left + r.width / 2, y: r.top + r.height / 2 };
        }
        // No pile yet — target the item-thumbs row area (bottom of screen)
        const itemRow = document.getElementById('item-thumbs');
        if (itemRow) {
            const r = itemRow.getBoundingClientRect();
            // Place roughly where a new pile would appear
            const children = itemRow.children.length;
            return { x: r.left + 30 + children * 60, y: r.top + r.height / 2 };
        }
        // Fallback: bottom-center of screen
        return { x: window.innerWidth / 2, y: window.innerHeight - 60 };
    }

    // ── Bidirectional Coin Transfer ──
    // Used for drop/pickup/give/receive between inventory slots, ground piles, and player portraits

    coinTransfer(amount, coinType, srcPos, destPos, srcText, destText) {
        const ct = (typeof COIN_TYPES !== 'undefined' && COIN_TYPES[coinType]) ||
            { color: '#ffd700', shine: '#fffacd', edge: '#b8960f', size: 7, textColor: '#ffe033' };

        const dist = Math.hypot(destPos.x - srcPos.x, destPos.y - srcPos.y);
        const duration = Math.max(0.35, Math.min(1.0, dist / 800));
        const visualCount = Math.min(20, Math.max(3, Math.ceil(Math.sqrt(amount) * 1.5)));
        const now = performance.now() / 1000;

        const coins = [];
        for (let i = 0; i < visualCount; i++) {
            const delay = i * 0.015 + Math.random() * 0.02;
            coins.push({
                startX: srcPos.x + (Math.random() - 0.5) * 30,
                startY: srcPos.y + (Math.random() - 0.5) * 20,
                endX: destPos.x + (Math.random() - 0.5) * 30,
                endY: destPos.y + (Math.random() - 0.5) * 20,
                delay,
                x: srcPos.x, y: srcPos.y,
                angle: Math.random() * Math.PI * 2,
                angularVel: (Math.random() - 0.5) * 12,
                size: ct.size * 1.8 + Math.random() * 3,
                tilt: 0.3 + Math.random() * 0.5,
            });
        }

        this.coinDrops.push({ coins, ct, amount, coinType, birth: now, duration, alive: true });

        // -X! on source
        if (srcText) {
            this.effects.push({
                x: srcPos.x, y: srcPos.y, birth: now, duration: 1.2, alive: true,
                amount, type: 'coin_text', coinColor: ct.color, coinShine: ct.shine,
                coinTextColor: ct.textColor || ct.shine, coinGlow: ct.glow || null,
                driftX: 0, prefix: '-',
            });
        }

        // +X! on destination after landing
        if (destText) {
            const landDelay = (duration + 0.05) * 1000;
            setTimeout(() => {
                this.effects.push({
                    x: destPos.x, y: destPos.y, birth: performance.now() / 1000,
                    duration: 1.2, alive: true, amount, type: 'coin_text',
                    coinColor: ct.color, coinShine: ct.shine,
                    coinTextColor: ct.textColor || ct.shine, coinGlow: ct.glow || null,
                    driftX: 0, prefix: '+',
                });
                if (!this._animating) this._startLoop();
            }, landDelay);
        }

        if (!this._animating) this._startLoop();
    }

    _findPlayerThumb(name) {
        if (!name) return null;
        const lower = name.toLowerCase();
        for (const t of document.querySelectorAll('#player-thumbs .thumb')) {
            const tn = (t.dataset.entityName || t.title || '').toLowerCase();
            if (tn === lower || tn.startsWith(lower.split(' ')[0])) return t;
        }
        return null;
    }

    _getElementCenter(el) {
        if (!el) return null;
        const r = el.getBoundingClientRect();
        return { x: r.left + r.width / 2, y: r.top + r.height / 2 };
    }

    // ── Engage Sword Indicator ──
    // Shows a tilted sword/dagger over the target thumb on new target engage

    showEngageSword(targetName) {
        const targetEl = this._findTargetThumb(targetName);
        if (!targetEl) return;

        const rect = targetEl.getBoundingClientRect();
        const cx = rect.left + rect.width / 2;
        const cy = rect.top + rect.height / 2;
        // Scale sword with thumbnail size
        const scale = rect.width / 67; // 67 = default thumb width

        this.effects.push({
            x: cx,
            y: cy,
            birth: performance.now() / 1000,
            duration: 0.75,
            alive: true,
            type: 'engage_sword',
            scale,
            driftX: 0,
            amount: 0,
        });
        if (!this._animating) this._startLoop();
    }

    _drawEngageSword(ctx, fx, now) {
        const elapsed = now - fx.birth;
        const t = elapsed / fx.duration;
        if (t >= 1.0) { fx.alive = false; return; }

        // Quick pop in (0-0.1), then fade out (0.1-1.0)
        let alpha, swordScale;
        if (t < 0.1) {
            alpha = t / 0.1;
            swordScale = 0.4 + 0.8 * (t / 0.1); // overshoot to 1.2
        } else if (t < 0.2) {
            alpha = 1.0;
            swordScale = 1.2 - 0.2 * ((t - 0.1) / 0.1); // settle to 1.0
        } else {
            alpha = 1.0 - (t - 0.2) / 0.8;
            swordScale = 1.0;
        }

        const s = fx.scale * swordScale;

        // ── Radial pulse ring ──
        const ringT = t; // ring expands over full duration
        const ringRadius = 10 + 50 * ringT * s;
        const ringAlpha = Math.max(0, (1.0 - ringT) * 0.6);
        if (ringAlpha > 0) {
            ctx.save();
            ctx.translate(fx.x, fx.y);
            ctx.globalAlpha = ringAlpha;
            ctx.strokeStyle = '#ffcc44';
            ctx.lineWidth = Math.max(1, 3 * (1 - ringT));
            ctx.beginPath();
            ctx.arc(0, 0, ringRadius, 0, Math.PI * 2);
            ctx.stroke();
            ctx.restore();
        }

        // ── Sword ──
        ctx.save();
        ctx.translate(fx.x, fx.y);
        ctx.scale(s, s);
        ctx.rotate(-Math.PI / 4); // 45 degree backslash tilt
        ctx.globalAlpha = alpha;

        // Warm glow
        ctx.shadowColor = 'rgba(255, 200, 80, 0.7)';
        ctx.shadowBlur = 12;

        ctx.font = 'bold 32px "Segoe UI Emoji", "Apple Color Emoji", sans-serif';
        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';

        // Shadow
        ctx.fillStyle = `rgba(0,0,0,${alpha * 0.6})`;
        ctx.fillText('\u{2694}', 2, 2);
        // Crossed swords
        ctx.fillStyle = `rgba(255,255,255,${alpha})`;
        ctx.fillText('\u{2694}', 0, 0);

        ctx.shadowBlur = 0;
        ctx.restore();
    }

    // ── Animation Loop ──

    _startLoop() {
        this._animating = true;
        let lastTime = performance.now() / 1000;
        const tick = () => {
            if (!this._animating) return;
            const now = performance.now() / 1000;
            const dt = Math.min(now - lastTime, 0.05);
            lastTime = now;

            // Update shatter physics
            for (const sh of this.shatters) {
                if (!sh.alive) continue;
                for (const shard of sh.shards) {
                    shard.vy += sh.gravity * dt;
                    shard.x += shard.vx * dt;
                    shard.y += shard.vy * dt;
                    shard.angle += shard.angularVel * dt;
                }
                if (now - sh.birth >= sh.duration) sh.alive = false;
            }

            // Update coin drop animations (arc from source to dest)
            for (const cd of this.coinDrops) {
                if (!cd.alive) continue;
                const elapsed = now - cd.birth;
                for (const coin of cd.coins) {
                    const t = Math.max(0, Math.min(1, (elapsed - coin.delay) / cd.duration));
                    const ease = 1 - (1 - t) * (1 - t);
                    const prevX = coin.x, prevY = coin.y;
                    coin.x = coin.startX + (coin.endX - coin.startX) * ease;
                    coin.y = coin.startY + (coin.endY - coin.startY) * ease
                        - Math.sin(ease * Math.PI) * 80;
                    coin.angle += coin.angularVel * dt;
                    // Runic trail — store recent positions
                    if (cd.ct.glow && t > 0) {
                        if (!coin.trail) coin.trail = [];
                        coin.trail.push({ x: prevX, y: prevY, t: now });
                        // Keep last 8 trail points
                        if (coin.trail.length > 8) coin.trail.shift();
                    }
                }
                if (elapsed >= cd.duration + 0.1) cd.alive = false;
            }

            this._render(now);

            const hasWork = this.effects.some(e => e.alive) ||
                this.shatters.some(s => s.alive) ||
                this.coinDrops.some(c => c.alive);
            if (hasWork) {
                requestAnimationFrame(tick);
            } else {
                this._animating = false;
                this.ctx.clearRect(0, 0, this.canvas.width, this.canvas.height);
                // Clean up dead
                this.effects = this.effects.filter(e => e.alive);
                this.shatters = this.shatters.filter(s => s.alive);
                this.coinDrops = this.coinDrops.filter(c => c.alive);
            }
        };
        requestAnimationFrame(tick);
    }

    _render(now) {
        const ctx = this.ctx;
        const w = window.innerWidth, h = window.innerHeight;
        ctx.clearRect(0, 0, w, h);

        const scaleStr = (typeof roomView !== 'undefined' && roomView.settings?.dmgTextScale) || '100%';
        const scaleMap = {'50%':0.5,'75%':0.75,'100%':1.0,'125%':1.25,'150%':1.5,'200%':2.0};
        const globalScale = (scaleMap[scaleStr] || 1.0) * (window._npcCombatTextScale || 1);

        // ── Draw shatters ──
        for (const sh of this.shatters) {
            if (!sh.alive) continue;
            const t = Math.min((now - sh.birth) / sh.duration, 1.0);
            const alpha = Math.max(0, 1.0 - t * t); // ease-out fade

            for (const shard of sh.shards) {
                const scale = Math.max(0.1, 1.0 - t * 0.5);
                ctx.save();
                ctx.translate(shard.x, shard.y);
                ctx.rotate(shard.angle);
                ctx.scale(scale, scale);
                ctx.globalAlpha = alpha;
                try {
                    ctx.drawImage(
                        sh.source,
                        shard.sx, shard.sy, shard.sw, shard.sh,
                        -shard.sw / 2, -shard.sh / 2, shard.sw, shard.sh
                    );
                } catch (e) {
                    sh.alive = false;
                }
                ctx.restore();
            }
        }
        ctx.globalAlpha = 1.0;

        // ── Draw coin drops ──
        for (const cd of this.coinDrops) {
            if (!cd.alive) continue;
            const elapsed = now - cd.birth;
            const ct = cd.ct;

            // Runic trails — draw before coins so they appear behind
            if (ct.glow) {
                for (const coin of cd.coins) {
                    if (!coin.trail || coin.trail.length < 2) continue;
                    for (let ti = 0; ti < coin.trail.length; ti++) {
                        const tp = coin.trail[ti];
                        const age = now - tp.t;
                        const fade = Math.max(0, 1 - age * 6); // fade over ~0.16s
                        if (fade <= 0) continue;
                        const sz = coin.size * 0.6 * fade;
                        ctx.save();
                        ctx.globalAlpha = fade * 0.7;
                        ctx.shadowColor = ct.glow;
                        ctx.shadowBlur = 12 * fade;
                        ctx.beginPath();
                        ctx.arc(tp.x, tp.y, sz, 0, Math.PI * 2);
                        ctx.fillStyle = ct.glow;
                        ctx.fill();
                        ctx.restore();
                    }
                }
            }

            for (const coin of cd.coins) {
                const t = Math.max(0, Math.min(1, (elapsed - coin.delay) / cd.duration));
                if (t <= 0) continue; // not launched yet

                // Fade in at start, slight fade at end
                const alpha = t < 0.15 ? t / 0.15 : (t > 0.8 ? (1 - t) / 0.2 : 1.0);

                ctx.save();
                ctx.translate(coin.x, coin.y);
                ctx.rotate(coin.angle);
                ctx.globalAlpha = alpha;

                const rx = coin.size;
                const ry = coin.size * coin.tilt;

                // Glow — runic gets intense neon plasma
                ctx.shadowColor = ct.glow || ct.color;
                ctx.shadowBlur = ct.glow ? 14 : 8;

                // Edge (thickness)
                ctx.beginPath();
                ctx.ellipse(0, 1.5, rx, ry, 0, 0, Math.PI * 2);
                ctx.fillStyle = ct.edge;
                ctx.fill();

                // Face with gradient
                ctx.beginPath();
                ctx.ellipse(0, 0, rx, ry, 0, 0, Math.PI * 2);
                const grad = ctx.createRadialGradient(-rx * 0.3, -ry * 0.3, 0, 0, 0, rx * 1.2);
                grad.addColorStop(0, ct.shine);
                grad.addColorStop(0.5, ct.color);
                grad.addColorStop(1, ct.edge);
                ctx.fillStyle = grad;
                ctx.fill();

                ctx.shadowBlur = 0;
                ctx.restore();
            }
        }
        ctx.globalAlpha = 1.0;

        // ── Draw damage numbers + effects ──
        for (const fx of this.effects) {
            if (!fx.alive) continue;
            const elapsed = now - fx.birth;
            const t = Math.min(1.0, elapsed / fx.duration);
            if (t >= 1.0) { fx.alive = false; continue; }

            // Engage sword — rendered separately, no rise
            if (fx.type === 'engage_sword') {
                this._drawEngageSword(ctx, fx, now);
                continue;
            }

            const rise = 55 * t * (2.0 - t);
            const x = fx.x + fx.driftX * t;
            const y = fx.y - rise;

            let alpha = t < 0.4 ? 1.0 : Math.max(0, 1.0 - (t - 0.4) / 0.6);
            if (y < 30) alpha *= Math.max(0, y / 30);

            let scale;
            if (t < 0.1) scale = 0.5 + 5.0 * t;
            else if (t < 0.2) scale = 1.0 + 0.3 * (1.0 - (t - 0.1) / 0.1);
            else scale = 1.0;

            let baseSize;
            if (fx.type === 'backstab') baseSize = fx.amount >= 100 ? 40 : fx.amount >= 50 ? 36 : 32;
            else if (fx.type === 'crit') baseSize = fx.amount >= 100 ? 38 : fx.amount >= 50 ? 34 : 30;
            else baseSize = fx.amount >= 100 ? 28 : fx.amount >= 50 ? 24 : 20;

            const fontSize = baseSize * scale * globalScale;

            ctx.save();
            ctx.translate(x, y);
            ctx.font = `bold ${fontSize}px Impact, "Arial Black", sans-serif`;
            ctx.textAlign = 'center';
            ctx.textBaseline = 'middle';
            ctx.lineJoin = 'round';
            ctx.miterLimit = 2;

            if (fx.type === 'coin_text') {
                // Floating "+X" in the coin's distinct color with heavy drop shadow
                const coinSize = 22 * scale * globalScale;
                ctx.font = `bold ${coinSize}px Impact, "Arial Black", sans-serif`;
                ctx.textAlign = 'center';
                ctx.textBaseline = 'middle';
                ctx.lineJoin = 'round';
                const label = `${fx.prefix || '+'}${fx.amount}`;

                // Runic gets neon plasma glow
                if (fx.coinGlow) {
                    ctx.shadowColor = fx.coinGlow;
                    ctx.shadowBlur = 15 + 5 * Math.sin(elapsed * 8);
                }

                // Heavy black outline
                ctx.lineWidth = Math.max(5, coinSize * 0.22);
                ctx.strokeStyle = `rgba(0,0,0,${alpha * 0.95})`;
                ctx.strokeText(label, 0, 0);
                ctx.strokeText(label, 1, 1);

                // Coin-colored fill using distinct textColor
                ctx.globalAlpha = alpha;
                ctx.fillStyle = fx.coinTextColor || fx.coinShine;
                ctx.fillText(label, 0, 0);

                // Bright center highlight
                ctx.globalAlpha = alpha * 0.35;
                ctx.fillStyle = '#ffffff';
                ctx.fillText(label, 0, -1);

                ctx.shadowBlur = 0;
                ctx.restore();
                continue;
            }

            const [r, g, b] = fx.color;
            if (fx.type === 'backstab') this._drawBackstab(ctx, fx.text, alpha, r, g, b, fontSize, elapsed);
            else if (fx.type === 'crit') this._drawCrit(ctx, fx.text, alpha, r, g, b, fontSize, elapsed);
            else if (fx.type === 'heal') this._drawNormal(ctx, fx.text, alpha, 0.2, 1.0, 0.3, fontSize);
            else this._drawNormal(ctx, fx.text, alpha, r, g, b, fontSize);

            ctx.restore();
        }
    }

    // ── Text rendering styles ──

    _drawNormal(ctx, text, alpha, r, g, b, fontSize) {
        ctx.lineWidth = Math.max(4, fontSize * 0.15);
        ctx.strokeStyle = `rgba(0,0,0,${alpha * 0.9})`;
        ctx.strokeText(text, 0, 0);
        ctx.fillStyle = `rgba(${r*255|0},${g*255|0},${b*255|0},${alpha})`;
        ctx.fillText(text, 0, 0);
    }

    _drawCrit(ctx, text, alpha, r, g, b, fontSize, age) {
        const pulse = 0.5 + 0.5 * Math.sin(age * 12.0);
        ctx.save();
        ctx.shadowColor = `rgba(255,170,0,${alpha * 0.6 * pulse})`;
        ctx.shadowBlur = 20 + 10 * pulse;
        ctx.fillStyle = `rgba(255,200,0,${alpha * 0.3})`;
        ctx.fillText(text, 0, 0);
        ctx.restore();

        ctx.lineWidth = Math.max(5, fontSize * 0.18);
        ctx.strokeStyle = `rgba(0,0,0,${alpha * 0.95})`;
        ctx.strokeText(text, 0, 0);

        const gr = 1.0, gg = 0.85 + 0.15 * pulse, gb = 0.15 - 0.1 * pulse;
        ctx.fillStyle = `rgba(${gr*255|0},${gg*255|0},${gb*255|0},${alpha})`;
        ctx.fillText(text, 0, 0);
        ctx.fillStyle = `rgba(255,255,220,${alpha * 0.4 * pulse})`;
        ctx.fillText(text, 0, 0);
    }

    _drawBackstab(ctx, text, alpha, r, g, b, fontSize, age) {
        ctx.save();
        ctx.shadowColor = `rgba(0,0,0,${alpha * 0.9})`;
        ctx.shadowBlur = 8;
        ctx.shadowOffsetX = 3;
        ctx.shadowOffsetY = 3;
        ctx.lineWidth = Math.max(6, fontSize * 0.2);
        ctx.strokeStyle = `rgba(0,0,0,${alpha * 0.95})`;
        ctx.strokeText(text, 0, 0);
        ctx.restore();

        ctx.fillStyle = `rgba(${r*255|0},${g*255|0},${b*255|0},${alpha})`;
        ctx.fillText(text, 0, 0);
        ctx.fillStyle = `rgba(220,130,255,${alpha * 0.3})`;
        ctx.fillText(text, 0, 0);

        ctx.save();
        ctx.shadowColor = `rgba(180,50,255,${alpha * 0.5})`;
        ctx.shadowBlur = 15;
        ctx.fillStyle = 'rgba(0,0,0,0)';
        ctx.fillText(text, 0, 0);
        ctx.restore();
    }
}
