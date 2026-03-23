"""Combat visual effects: floating damage numbers and thumbnail shatter on death.

Uses a transparent Gtk.DrawingArea overlay with Cairo rendering.
Effects run at ~60fps via GLib.timeout_add, auto-stopping when idle.
"""
import math
import random
import time
import logging
from dataclasses import dataclass, field

import gi
gi.require_version('Gtk', '4.0')
from gi.repository import Gtk, Gdk, GLib, GdkPixbuf, Graphene

import cairo

logger = logging.getLogger(__name__)


@dataclass
class FloatingNumber:
    """A damage number that floats upward and fades out."""
    x: float  # center x in overlay coords
    y: float  # start y in overlay coords
    damage: int
    birth: float  # time.monotonic()
    duration: float = 1.2
    # Slight random horizontal drift
    drift_x: float = 0.0
    # Color (r, g, b)
    color: tuple = (1.0, 0.15, 0.1)
    crit: bool = False  # critical hit — golden blazing text
    surprise: bool = False  # backstab/surprise — purple text

    def progress(self, now: float) -> float:
        return min(1.0, (now - self.birth) / self.duration)

    def alive(self, now: float) -> bool:
        return now - self.birth < self.duration


@dataclass
class Shard:
    """A fragment of a shattered thumbnail."""
    # Source rect in the original pixbuf
    src_x: float
    src_y: float
    src_w: float
    src_h: float
    # Current position (center, in overlay coords)
    x: float
    y: float
    # Velocity
    vx: float
    vy: float
    # Rotation
    angle: float = 0.0
    angular_vel: float = 0.0
    # Scale (shrinks over time)
    scale: float = 1.0


@dataclass
class ShatterEffect:
    """A thumbnail exploding into shards."""
    shards: list[Shard]
    surface: cairo.ImageSurface  # source image as cairo surface
    birth: float
    duration: float = 1.6
    gravity: float = 350.0  # pixels/s²

    def progress(self, now: float) -> float:
        return min(1.0, (now - self.birth) / self.duration)

    def alive(self, now: float) -> bool:
        return now - self.birth < self.duration


class CombatEffects:
    """Manages combat visual effects rendered on a transparent overlay."""

    def __init__(self, overlay: Gtk.Overlay):
        self._overlay = overlay
        self._numbers: list[FloatingNumber] = []
        self._shatters: list[ShatterEffect] = []
        self._ticking = False
        self._last_tick = 0.0

        # Glowing red border around active combat target
        self._target_widget: Gtk.Widget | None = None
        self._target_rect: tuple | None = None  # (x, y, w, h) cached position
        self._target_glow_time: float = 0.0  # for pulsing

        # Global font scale for thumbnail damage numbers
        self._font_scale = 1.0

        # Lazy-init DrawingArea overlay on first use
        self._draw: Gtk.DrawingArea | None = None

    def _ensure_draw_area(self) -> None:
        """Create the transparent DrawingArea overlay on first use."""
        if self._draw is not None:
            return
        self._draw = Gtk.DrawingArea()
        self._draw.set_can_target(False)  # click-through
        self._draw.set_draw_func(self._on_draw)
        self._draw.set_hexpand(True)
        self._draw.set_vexpand(True)
        self._overlay.add_overlay(self._draw)

    def set_font_scale(self, scale: float) -> None:
        """Set global font scale for thumbnail damage numbers."""
        self._font_scale = scale

    def raise_to_top(self) -> None:
        """Re-add the DrawingArea so it renders above all other overlay children."""
        if self._draw is None:
            return
        self._overlay.remove_overlay(self._draw)
        self._overlay.add_overlay(self._draw)

    def set_target(self, widget: Gtk.Widget | None) -> None:
        """Set the active combat target widget (glowing red border)."""
        self._target_widget = widget
        self._target_rect = None
        if widget and widget.get_mapped():
            ok, pt = widget.compute_point(self._overlay, Graphene.Point().init(0, 0))
            if ok:
                self._target_rect = (pt.x, pt.y, widget.get_width(), widget.get_height())
        self._target_glow_time = time.monotonic()
        self._ensure_ticking()

    def update_target_position(self) -> None:
        """Refresh cached target position (call when widget may have moved)."""
        if self._target_widget and self._target_widget.get_mapped():
            ok, pt = self._target_widget.compute_point(self._overlay, Graphene.Point().init(0, 0))
            if ok:
                self._target_rect = (pt.x, pt.y,
                                     self._target_widget.get_width(),
                                     self._target_widget.get_height())

    def clear_target(self) -> None:
        """Clear the combat target highlight."""
        self._target_widget = None
        self._target_rect = None

    def spawn_damage(self, widget: Gtk.Widget, damage: int,
                     color: tuple = (1.0, 0.15, 0.1),
                     crit: bool = False,
                     surprise: bool = False) -> None:
        """Spawn a floating damage number over a widget."""
        # Get widget position relative to overlay
        ok, pt = widget.compute_point(self._overlay, Graphene.Point().init(0, 0))
        if not ok:
            return

        w = widget.get_width()
        h = widget.get_height()
        cx = pt.x + w / 2
        cy = pt.y + h / 4  # start near top of thumbnail

        # Stagger based on how many recent numbers are still alive
        # Alternate left/right and offset vertically so they don't stack
        now = time.monotonic()
        active_count = sum(1 for n in self._numbers if n.alive(now))
        cx += random.uniform(-15, 15) + (16 if active_count % 2 else -16)
        cy += random.uniform(-6, 6) - active_count * 18

        if surprise:
            color = (0.7, 0.2, 1.0)  # purple
        elif crit:
            color = (1.0, 0.85, 0.15)  # golden

        num = FloatingNumber(
            x=cx, y=cy, damage=damage,
            birth=time.monotonic(),
            drift_x=random.uniform(-20, 20),
            color=color,
            crit=crit,
            surprise=surprise,
            duration=1.6 if (crit or surprise) else 1.2,
        )
        num._font_scale = self._font_scale
        self._numbers.append(num)
        self._ensure_ticking()

    def spawn_damage_at(self, cx: float, cy: float, damage: int,
                        color: tuple = (1.0, 0.15, 0.1),
                        crit: bool = False, surprise: bool = False,
                        font_scale: float = 1.0) -> None:
        """Spawn a floating damage number at explicit overlay coords."""
        now = time.monotonic()
        active_count = sum(1 for n in self._numbers if n.alive(now))
        cx += random.uniform(-20, 20) + (15 if active_count % 2 else -15)
        cy += random.uniform(-8, 8) - active_count * 10

        if surprise:
            color = (0.7, 0.2, 1.0)
        elif crit:
            color = (1.0, 0.85, 0.15)

        num = FloatingNumber(
            x=cx, y=cy, damage=damage,
            birth=now,
            drift_x=random.uniform(-25, 25),
            color=color,
            crit=crit,
            surprise=surprise,
            duration=1.6 if (crit or surprise) else 1.2,
        )
        # Scale font via damage field hack — use font_scale stored on the object
        num._font_scale = font_scale
        self._numbers.append(num)
        self._ensure_ticking()

    def spawn_shatter_at(self, origin_x: float, origin_y: float,
                         w: float, h: float,
                         pixbuf: GdkPixbuf.Pixbuf,
                         cols: int = 8, rows: int = 8,
                         speed_range: tuple = (120, 350)) -> None:
        """Shatter at an explicit position in overlay coords (for external windows)."""
        surface = self._pixbuf_to_surface(pixbuf)
        if not surface:
            return

        center_x = origin_x + w / 2
        center_y = origin_y + h / 2
        shard_w = w / cols
        shard_h = h / rows
        src_w = pixbuf.get_width() / cols
        src_h = pixbuf.get_height() / rows

        shards = []
        for row in range(rows):
            for col in range(cols):
                sx = col * src_w
                sy = row * src_h
                px = origin_x + col * shard_w + shard_w / 2
                py = origin_y + row * shard_h + shard_h / 2

                dx = px - center_x
                dy = py - center_y
                dist = math.hypot(dx, dy) or 1.0
                speed = random.uniform(*speed_range)
                vx = (dx / dist) * speed + random.uniform(-40, 40)
                vy = (dy / dist) * speed + random.uniform(-80, -20)

                shard = Shard(
                    src_x=sx, src_y=sy,
                    src_w=src_w, src_h=src_h,
                    x=px, y=py,
                    vx=vx, vy=vy,
                    angle=random.uniform(0, math.tau),
                    angular_vel=random.uniform(-8, 8),
                )
                shards.append(shard)

        effect = ShatterEffect(
            shards=shards,
            surface=surface,
            birth=time.monotonic(),
        )
        self._shatters.append(effect)
        self._ensure_ticking()

    def spawn_shatter(self, widget: Gtk.Widget, pixbuf: GdkPixbuf.Pixbuf) -> None:
        """Shatter a thumbnail into fragments with physics."""
        ok, pt = widget.compute_point(self._overlay, Graphene.Point().init(0, 0))
        if not ok:
            return

        w = widget.get_width()
        h = widget.get_height()
        origin_x = pt.x
        origin_y = pt.y
        center_x = origin_x + w / 2
        center_y = origin_y + h / 2

        # Convert pixbuf to cairo surface
        surface = self._pixbuf_to_surface(pixbuf)
        if not surface:
            return

        # Create grid of shards
        cols, rows = 6, 6
        shard_w = w / cols
        shard_h = h / rows
        src_w = pixbuf.get_width() / cols
        src_h = pixbuf.get_height() / rows

        shards = []
        for row in range(rows):
            for col in range(cols):
                sx = col * src_w
                sy = row * src_h
                # Position in overlay coords
                px = origin_x + col * shard_w + shard_w / 2
                py = origin_y + row * shard_h + shard_h / 2

                # Velocity: outward from center with randomness
                dx = px - center_x
                dy = py - center_y
                dist = math.hypot(dx, dy) or 1.0
                speed = random.uniform(80, 220)
                vx = (dx / dist) * speed + random.uniform(-30, 30)
                vy = (dy / dist) * speed + random.uniform(-60, -20)  # bias upward

                shard = Shard(
                    src_x=sx, src_y=sy,
                    src_w=src_w, src_h=src_h,
                    x=px, y=py,
                    vx=vx, vy=vy,
                    angle=random.uniform(0, math.tau),
                    angular_vel=random.uniform(-8, 8),
                )
                shards.append(shard)

        effect = ShatterEffect(
            shards=shards,
            surface=surface,
            birth=time.monotonic(),
        )
        self._shatters.append(effect)
        self._ensure_ticking()

    def _pixbuf_to_surface(self, pixbuf: GdkPixbuf.Pixbuf) -> cairo.ImageSurface | None:
        """Convert a GdkPixbuf to a cairo ImageSurface row by row."""
        try:
            w = pixbuf.get_width()
            h = pixbuf.get_height()
            has_alpha = pixbuf.get_has_alpha()
            n_channels = pixbuf.get_n_channels()
            rowstride = pixbuf.get_rowstride()
            px = pixbuf.get_pixels()

            surface = cairo.ImageSurface(cairo.FORMAT_ARGB32, w, h)
            out = bytearray(surface.get_data())
            s_stride = surface.get_stride()

            for y in range(h):
                src_row = y * rowstride
                dst_row = y * s_stride
                for x in range(w):
                    si = src_row + x * n_channels
                    di = dst_row + x * 4
                    r, g, b = px[si], px[si + 1], px[si + 2]
                    a = px[si + 3] if has_alpha else 255
                    # Cairo premultiplied BGRA
                    out[di] = b * a // 255
                    out[di + 1] = g * a // 255
                    out[di + 2] = r * a // 255
                    out[di + 3] = a

            # Write back
            surface.get_data()[:] = bytes(out)
            surface.mark_dirty()
            return surface
        except Exception as e:
            logger.error(f"Failed to convert pixbuf to surface: {e}")
            return None

    def _ensure_ticking(self) -> None:
        self._ensure_draw_area()
        if not self._ticking:
            self._ticking = True
            self._last_tick = time.monotonic()
            GLib.timeout_add(16, self._tick)  # ~60fps

    def _tick(self) -> bool:
        now = time.monotonic()
        dt = now - self._last_tick
        self._last_tick = now

        # Update shatters physics
        for effect in self._shatters:
            for shard in effect.shards:
                shard.vy += effect.gravity * dt
                shard.x += shard.vx * dt
                shard.y += shard.vy * dt
                shard.angle += shard.angular_vel * dt

        # Prune dead effects
        self._numbers = [n for n in self._numbers if n.alive(now)]
        self._shatters = [s for s in self._shatters if s.alive(now)]

        # Redraw
        if self._draw:
            self._draw.queue_draw()

        if not self._numbers and not self._shatters and not self._target_widget:
            self._ticking = False
            return False  # stop ticking
        return True

    def _on_draw(self, area, cr, width, height):
        now = time.monotonic()

        # Draw glowing red border around combat target
        if self._target_rect:
            # Refresh position from widget if still valid
            self.update_target_position()
        if self._target_rect:
            tx, ty, tw, th = self._target_rect
            # Pulsing glow intensity
            pulse = 0.5 + 0.5 * math.sin((now - self._target_glow_time) * 3.5)
            pad = 3  # border padding outside the widget

            # Outer glow (soft, wide)
            cr.save()
            cr.set_source_rgba(1.0, 0.1, 0.05, 0.3 * pulse)
            cr.set_line_width(6)
            cr.rectangle(tx - pad - 2, ty - pad - 2,
                         tw + (pad + 2) * 2, th + (pad + 2) * 2)
            cr.stroke()

            # Inner glow (bright, crisp)
            cr.set_source_rgba(1.0, 0.15, 0.1, 0.6 + 0.4 * pulse)
            cr.set_line_width(2)
            cr.rectangle(tx - pad, ty - pad,
                         tw + pad * 2, th + pad * 2)
            cr.stroke()
            cr.restore()

        # Draw shatter effects
        for effect in self._shatters:
            t = effect.progress(now)
            # Ease-out fade
            alpha = max(0.0, 1.0 - t * t)

            for shard in effect.shards:
                cr.save()
                cr.translate(shard.x, shard.y)
                cr.rotate(shard.angle)

                # Scale down over time
                scale = max(0.1, 1.0 - t * 0.5)
                cr.scale(scale, scale)

                # Calculate display size from source proportions
                sw = shard.src_w
                sh = shard.src_h
                half_w = sw / 2
                half_h = sh / 2

                # Clip to shard rectangle
                cr.rectangle(-half_w, -half_h, sw, sh)
                cr.clip()

                # Draw the source image portion
                cr.set_source_surface(
                    effect.surface,
                    -shard.src_x - half_w,
                    -shard.src_y - half_h,
                )
                cr.paint_with_alpha(alpha)
                cr.restore()

        # Draw floating damage numbers
        for num in self._numbers:
            t = num.progress(now)

            # Position: float upward with deceleration
            rise = 55 * t * (2.0 - t)  # ease-out
            x = num.x + num.drift_x * t
            y = num.y - rise

            # Alpha: hold solid then fade
            if t < 0.4:
                alpha = 1.0
            else:
                alpha = max(0.0, 1.0 - (t - 0.4) / 0.6)

            # Fade out near top edge so numbers don't clip harshly
            if y < 30:
                alpha *= max(0.0, y / 30.0)

            # Scale: pop in then settle
            if t < 0.1:
                scale = 0.5 + 5.0 * t  # 0.5 → 1.0
            elif t < 0.2:
                scale = 1.0 + 0.3 * (1.0 - (t - 0.1) / 0.1)  # 1.3 → 1.0
            else:
                scale = 1.0

            # Font size based on damage magnitude
            if num.surprise:
                base_size = 32
                if num.damage >= 100:
                    base_size = 40
                elif num.damage >= 50:
                    base_size = 36
            elif num.crit:
                base_size = 30
                if num.damage >= 100:
                    base_size = 38
                elif num.damage >= 50:
                    base_size = 34
            else:
                base_size = 20
                if num.damage >= 100:
                    base_size = 28
                elif num.damage >= 50:
                    base_size = 24

            font_size = base_size * scale * getattr(num, '_font_scale', 1.0)

            cr.save()
            cr.select_font_face("Sans", cairo.FONT_SLANT_NORMAL, cairo.FONT_WEIGHT_BOLD)
            cr.set_font_size(font_size)

            if num.surprise:
                text = f"BS {num.damage}!"
            elif num.crit:
                text = f"CRIT {num.damage}!"
            else:
                text = str(num.damage)

            extents = cr.text_extents(text)
            tx = x - extents.width / 2
            ty = y

            if num.surprise:
                # Backstab/surprise: large purple with heavy black drop shadow
                # Drop shadow (offset down-right)
                cr.set_source_rgba(0, 0, 0, alpha * 0.9)
                for ox, oy in [(3, 3), (2, 3), (3, 2),
                                (-2, -2), (2, -2), (-2, 2), (2, 2),
                                (0, -2), (0, 2), (-2, 0), (2, 0)]:
                    cr.move_to(tx + ox, ty + oy)
                    cr.show_text(text)

                # Main purple text
                cr.set_source_rgba(0.7, 0.2, 1.0, alpha)
                cr.move_to(tx, ty)
                cr.show_text(text)

                # Bright highlight overlay
                cr.set_source_rgba(0.85, 0.5, 1.0, alpha * 0.3)
                cr.move_to(tx, ty)
                cr.show_text(text)

            elif num.crit:
                # Crit: golden blazing pulsing glow
                pulse = 0.5 + 0.5 * math.sin((now - num.birth) * 12.0)

                # Outer golden glow
                cr.set_source_rgba(1.0, 0.6, 0.0, alpha * 0.4 * pulse)
                for ox, oy in [(-3, -3), (3, -3), (-3, 3), (3, 3),
                                (0, -3), (0, 3), (-3, 0), (3, 0)]:
                    cr.move_to(tx + ox, ty + oy)
                    cr.show_text(text)

                # Dark outline for readability
                cr.set_source_rgba(0, 0, 0, alpha * 0.9)
                for ox, oy in [(-2, -2), (2, -2), (-2, 2), (2, 2),
                                (0, -2), (0, 2), (-2, 0), (2, 0)]:
                    cr.move_to(tx + ox, ty + oy)
                    cr.show_text(text)

                # Main golden text with pulsing brightness
                r = 1.0
                g = 0.85 + 0.15 * pulse
                b = 0.15 - 0.1 * pulse
                cr.set_source_rgba(r, g, b, alpha)
                cr.move_to(tx, ty)
                cr.show_text(text)

                # Hot white center highlight
                cr.set_source_rgba(1.0, 1.0, 0.8, alpha * 0.4 * pulse)
                cr.move_to(tx, ty)
                cr.show_text(text)
            else:
                # Normal damage: dark outline + colored text
                cr.set_source_rgba(0, 0, 0, alpha * 0.8)
                for ox, oy in [(-2, -2), (2, -2), (-2, 2), (2, 2),
                                (0, -2), (0, 2), (-2, 0), (2, 0)]:
                    cr.move_to(tx + ox, ty + oy)
                    cr.show_text(text)

                r, g, b = num.color
                cr.set_source_rgba(r, g, b, alpha)
                cr.move_to(tx, ty)
                cr.show_text(text)

            cr.restore()
