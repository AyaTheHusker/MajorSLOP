// ── Procedural Coin Pile Renderer ──
// Draws sparkling coin piles in item thumbnail boxes for currency on the ground.
// Each currency type gets its own visual style and separate pile.

const COIN_TYPES = {
    copper:   { color: '#b87333', shine: '#e8a050', edge: '#7a4a1a', size: 5,  label: 'Copper',   order: 0 },
    silver:   { color: '#c0c0c0', shine: '#f0f0ff', edge: '#808088', size: 6,  label: 'Silver',   order: 1 },
    gold:     { color: '#ffd700', shine: '#fffacd', edge: '#b8960f', size: 7,  label: 'Gold',     order: 2 },
    platinum: { color: '#e5e5e5', shine: '#ffffff', edge: '#a0a0b0', size: 8,  label: 'Platinum', order: 3 },
    runic:    { color: '#8844cc', shine: '#cc88ff', edge: '#5522aa', size: 9,  label: 'Runic',    order: 4 },
};

// Conversion rates (for display context, not used in rendering)
// 10 copper = 1 silver, 10 silver = 1 gold, 100 gold = 1 platinum, 100 platinum = 1 runic

function formatCoinCount(n) {
    if (n >= 1000000) return (n / 1000000).toFixed(1).replace(/\.0$/, '') + 'M';
    if (n >= 10000) return Math.round(n / 1000) + 'K';
    if (n >= 1000) return (n / 1000).toFixed(1).replace(/\.0$/, '') + 'K';
    return String(n);
}

class CoinRenderer {
    constructor() {
        this._sparkles = new Map(); // canvas element -> sparkle state
        this._animating = false;
    }

    /**
     * Create a coin pile canvas element for a currency item.
     * @param {string} coinType - 'copper', 'silver', 'gold', 'platinum', 'runic'
     * @param {number} quantity - number of coins
     * @param {number} width - canvas width
     * @param {number} height - canvas height
     * @returns {HTMLCanvasElement}
     */
    createPile(coinType, quantity, width = 64, height = 64) {
        const ct = COIN_TYPES[coinType] || COIN_TYPES.gold;
        const canvas = document.createElement('canvas');
        const dpr = window.devicePixelRatio || 1;
        canvas.width = width * dpr;
        canvas.height = height * dpr;
        canvas.style.width = width + 'px';
        canvas.style.height = height + 'px';

        const ctx = canvas.getContext('2d');
        ctx.setTransform(dpr, 0, 0, dpr, 0, 0);

        // How many coins to actually draw (cap visual at ~200 for perf)
        const visualCount = Math.min(200, Math.max(3, Math.round(Math.sqrt(quantity) * 3)));
        const coins = this._generatePileLayout(ct, quantity, visualCount, width, height);

        // Draw pile
        this._drawPile(ctx, coins, ct, width, height);

        // Set up sparkle state
        const sparkles = [];
        const sparkleCount = Math.min(12, Math.max(2, Math.round(visualCount / 8)));
        for (let i = 0; i < sparkleCount; i++) {
            sparkles.push({
                x: 8 + Math.random() * (width - 16),
                y: Math.random() * (height - 16),
                phase: Math.random() * Math.PI * 2,
                speed: 1.5 + Math.random() * 2.5,
                size: 1.5 + Math.random() * 2.5,
            });
        }
        this._sparkles.set(canvas, {
            sparkles,
            coinType: ct,
            coins,
            quantity,
            width,
            height,
            dpr,
        });

        if (!this._animating) this._startSparkleLoop();

        return canvas;
    }

    _generatePileLayout(ct, quantity, visualCount, w, h) {
        const coins = [];
        // Pile center: bottom-center, spreading outward
        const cx = w / 2;
        const baseY = h - 10;
        // More coins = wider and taller pile
        const spread = Math.min(w * 0.42, 8 + visualCount * 0.3);
        const pileHeight = Math.min(h * 0.65, 6 + visualCount * 0.25);

        for (let i = 0; i < visualCount; i++) {
            // Stack from bottom, with randomness
            const layer = i / visualCount;
            const x = cx + (Math.random() - 0.5) * spread * 2 * (1 - layer * 0.3);
            const y = baseY - Math.random() * pileHeight * layer - Math.random() * 4;
            const angle = (Math.random() - 0.5) * 0.8;
            const tilt = 0.3 + Math.random() * 0.5; // how "edge-on" the coin is (ellipse ratio)
            coins.push({ x, y, angle, tilt, size: ct.size * (0.85 + Math.random() * 0.3) });
        }

        // Sort by y so coins in front overlap coins behind
        coins.sort((a, b) => a.y - b.y);
        return coins;
    }

    _drawPile(ctx, coins, ct, w, h) {
        ctx.clearRect(0, 0, w, h);

        for (const coin of coins) {
            ctx.save();
            ctx.translate(coin.x, coin.y);
            ctx.rotate(coin.angle);

            const rx = coin.size;
            const ry = coin.size * coin.tilt;

            // Edge (thickness)
            ctx.beginPath();
            ctx.ellipse(0, 1.2, rx, ry, 0, 0, Math.PI * 2);
            ctx.fillStyle = ct.edge;
            ctx.fill();

            // Face
            ctx.beginPath();
            ctx.ellipse(0, 0, rx, ry, 0, 0, Math.PI * 2);
            const grad = ctx.createRadialGradient(-rx * 0.3, -ry * 0.3, 0, 0, 0, rx * 1.2);
            grad.addColorStop(0, ct.shine);
            grad.addColorStop(0.5, ct.color);
            grad.addColorStop(1, ct.edge);
            ctx.fillStyle = grad;
            ctx.fill();

            // Runic coins get a glow
            if (ct === COIN_TYPES.runic) {
                ctx.shadowColor = '#aa66ff';
                ctx.shadowBlur = 6;
                ctx.beginPath();
                ctx.ellipse(0, 0, rx * 0.6, ry * 0.6, 0, 0, Math.PI * 2);
                ctx.fillStyle = 'rgba(170, 100, 255, 0.3)';
                ctx.fill();
                ctx.shadowBlur = 0;
            }

            ctx.restore();
        }
    }

    _drawSparkles(canvas, state, time) {
        const { sparkles, coins, coinType, width, height, dpr } = state;
        const ctx = canvas.getContext('2d');
        ctx.setTransform(dpr, 0, 0, dpr, 0, 0);

        // Redraw base pile
        this._drawPile(ctx, coins, coinType, width, height);

        // Draw sparkles
        for (const sp of sparkles) {
            const brightness = Math.sin(time * sp.speed + sp.phase);
            if (brightness < 0.2) continue; // only show when bright

            const alpha = (brightness - 0.2) / 0.8;
            const s = sp.size * (0.5 + brightness * 0.5);

            ctx.save();
            ctx.translate(sp.x, sp.y);
            ctx.globalAlpha = alpha * 0.9;

            // Four-pointed star sparkle
            ctx.fillStyle = coinType.shine;
            ctx.beginPath();
            ctx.moveTo(0, -s);
            ctx.lineTo(s * 0.25, -s * 0.25);
            ctx.lineTo(s, 0);
            ctx.lineTo(s * 0.25, s * 0.25);
            ctx.lineTo(0, s);
            ctx.lineTo(-s * 0.25, s * 0.25);
            ctx.lineTo(-s, 0);
            ctx.lineTo(-s * 0.25, -s * 0.25);
            ctx.closePath();
            ctx.fill();

            // Central bright dot
            ctx.beginPath();
            ctx.arc(0, 0, s * 0.3, 0, Math.PI * 2);
            ctx.fillStyle = '#ffffff';
            ctx.globalAlpha = alpha;
            ctx.fill();

            ctx.restore();
        }

        // Draw quantity label
        this._drawQuantityLabel(ctx, state.quantity, width, height);
    }

    _drawQuantityLabel(ctx, quantity, w, h) {
        const text = formatCoinCount(quantity);
        const fontSize = quantity >= 10000 ? 11 : 12;
        ctx.font = `bold ${fontSize}px 'Segoe UI', sans-serif`;
        ctx.textAlign = 'center';
        ctx.textBaseline = 'top';

        const x = w / 2;
        const y = 2;

        // Drop shadow
        ctx.fillStyle = 'rgba(0,0,0,0.9)';
        ctx.fillText(text, x + 1, y + 1);
        ctx.fillText(text, x - 1, y + 1);
        ctx.fillText(text, x, y + 2);

        // Text
        ctx.fillStyle = '#ffffff';
        ctx.fillText(text, x, y);
    }

    _startSparkleLoop() {
        this._animating = true;
        const tick = () => {
            if (this._sparkles.size === 0) {
                this._animating = false;
                return;
            }
            const time = performance.now() / 1000;

            for (const [canvas, state] of this._sparkles) {
                // Only animate if visible
                if (!canvas.isConnected) {
                    this._sparkles.delete(canvas);
                    continue;
                }
                this._drawSparkles(canvas, state, time);
            }

            requestAnimationFrame(tick);
        };
        requestAnimationFrame(tick);
    }

    /**
     * Remove a canvas from sparkle tracking.
     */
    destroy(canvas) {
        this._sparkles.delete(canvas);
    }

    /**
     * Clean up all canvases that are no longer in the DOM.
     */
    cleanup() {
        for (const [canvas] of this._sparkles) {
            if (!canvas.isConnected) this._sparkles.delete(canvas);
        }
    }
}

// Global instance
const coinRenderer = new CoinRenderer();
