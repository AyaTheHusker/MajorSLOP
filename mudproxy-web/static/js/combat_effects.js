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

    spawnShatter(targetEl) {
        if (!targetEl) return;
        const img = targetEl.querySelector('img');
        if (!img || !img.complete || !img.naturalWidth) return;

        const rect = targetEl.getBoundingClientRect();
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

            this._render(now);

            const hasWork = this.effects.some(e => e.alive) || this.shatters.some(s => s.alive);
            if (hasWork) {
                requestAnimationFrame(tick);
            } else {
                this._animating = false;
                this.ctx.clearRect(0, 0, this.canvas.width, this.canvas.height);
                // Clean up dead
                this.effects = this.effects.filter(e => e.alive);
                this.shatters = this.shatters.filter(s => s.alive);
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
        const globalScale = scaleMap[scaleStr] || 1.0;

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
                // Draw the shard from the source canvas
                ctx.drawImage(
                    sh.source,
                    shard.sx, shard.sy, shard.sw, shard.sh,
                    -shard.sw / 2, -shard.sh / 2, shard.sw, shard.sh
                );
                ctx.restore();
            }
        }
        ctx.globalAlpha = 1.0;

        // ── Draw damage numbers ──
        for (const fx of this.effects) {
            if (!fx.alive) continue;
            const elapsed = now - fx.birth;
            const t = Math.min(1.0, elapsed / fx.duration);
            if (t >= 1.0) { fx.alive = false; continue; }

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
