"""Player Character window — shows portrait, description, and inventory with thumbnails."""
import logging
import math
import time as _time
from typing import Optional, Callable

import gi
gi.require_version('Gtk', '4.0')
gi.require_version('Adw', '1')
from gi.repository import Gtk, Gdk, GLib, GdkPixbuf, Adw, Pango, Gio

from .hold_menu import HoldMenu

logger = logging.getLogger(__name__)

WINDOW_WIDTH = 500
WINDOW_HEIGHT = 850


class CharacterWindow(Gtk.Window):
    def __init__(self):
        super().__init__(title="MajorSLOP! — Character")
        self.set_default_size(WINDOW_WIDTH, WINDOW_HEIGHT)
        self.set_resizable(True)
        self.set_decorated(True)

        style_manager = Adw.StyleManager.get_default()
        style_manager.set_color_scheme(Adw.ColorScheme.FORCE_DARK)

        # Root overlay for hold-menu
        self._root_overlay = Gtk.Overlay()
        self.set_child(self._root_overlay)

        # Scrollable content
        scroll = Gtk.ScrolledWindow()
        scroll.set_policy(Gtk.PolicyType.AUTOMATIC, Gtk.PolicyType.AUTOMATIC)
        self._root_overlay.set_child(scroll)

        self._vbox = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        scroll.set_child(self._vbox)

        # Hold-right-click context menu
        self._hold_menu = HoldMenu(self._root_overlay)
        self._hold_menu.on_menu_open = self._on_hold_menu_open
        self._hold_menu.on_menu_close = self._on_hold_menu_close

        # Character name header
        self._name_label = Gtk.Label(label="No character data")
        self._name_label.add_css_class("char-name")
        self._name_label.set_halign(Gtk.Align.CENTER)
        self._name_label.set_margin_top(16)
        self._name_label.set_margin_bottom(10)
        self._vbox.append(self._name_label)

        # Character portrait (AI generated)
        self._portrait = Gtk.Picture()
        self._portrait.set_content_fit(Gtk.ContentFit.CONTAIN)
        self._portrait.set_size_request(384, 384)
        self._portrait.set_halign(Gtk.Align.CENTER)
        self._portrait.set_margin_bottom(12)
        self._vbox.append(self._portrait)

        # Hold-right-click on portrait for context menu
        portrait_drag = self._hold_menu.create_gesture(self._portrait)
        portrait_drag.connect("drag-begin", self._on_portrait_right_drag)

        # Description text — centered under portrait
        self._desc_label = Gtk.Label(label="")
        self._desc_label.add_css_class("char-desc")
        self._desc_label.set_wrap(True)
        self._desc_label.set_wrap_mode(Pango.WrapMode.WORD_CHAR)
        self._desc_label.set_halign(Gtk.Align.CENTER)
        self._desc_label.set_justify(Gtk.Justification.CENTER)
        self._desc_label.set_margin_start(24)
        self._desc_label.set_margin_end(24)
        self._desc_label.set_margin_bottom(12)
        self._desc_label.set_max_width_chars(55)
        self._vbox.append(self._desc_label)

        # Separator
        sep = Gtk.Separator(orientation=Gtk.Orientation.HORIZONTAL)
        sep.set_margin_start(20)
        sep.set_margin_end(20)
        sep.set_margin_bottom(10)
        self._vbox.append(sep)

        # Equipment header
        self._equip_header = Gtk.Label()
        self._equip_header.add_css_class("section-header")
        self._equip_header.set_markup(
            "<span weight='bold' size='large' color='#e8a87c'>Equipped:</span>"
        )
        self._equip_header.set_halign(Gtk.Align.START)
        self._equip_header.set_margin_start(20)
        self._equip_header.set_margin_bottom(6)
        self._vbox.append(self._equip_header)

        # Equipment list container
        self._equip_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=4)
        self._equip_box.set_margin_start(20)
        self._equip_box.set_margin_end(20)
        self._equip_box.set_margin_bottom(12)
        self._vbox.append(self._equip_box)
        self._equip_widgets: list[Gtk.Widget] = []

        # Separator
        self._sep2 = Gtk.Separator(orientation=Gtk.Orientation.HORIZONTAL)
        self._sep2.set_margin_start(20)
        self._sep2.set_margin_end(20)
        self._sep2.set_margin_bottom(10)
        self._vbox.append(self._sep2)

        # Inventory header
        self._inv_header = Gtk.Label()
        self._inv_header.add_css_class("section-header")
        self._inv_header.set_markup(
            "<span weight='bold' size='large' color='#81b29a'>Inventory:</span>"
        )
        self._inv_header.set_halign(Gtk.Align.START)
        self._inv_header.set_margin_start(20)
        self._inv_header.set_margin_bottom(6)
        self._vbox.append(self._inv_header)

        # Inventory list container
        self._inv_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=4)
        self._inv_box.set_margin_start(20)
        self._inv_box.set_margin_end(20)
        self._inv_box.set_margin_bottom(10)
        self._vbox.append(self._inv_box)
        self._inv_widgets: list[Gtk.Widget] = []

        # Wealth
        self._wealth_label = Gtk.Label(label="")
        self._wealth_label.add_css_class("wealth-label")
        self._wealth_label.set_halign(Gtk.Align.START)
        self._wealth_label.set_margin_start(20)
        self._wealth_label.set_margin_end(20)
        self._wealth_label.set_margin_bottom(4)
        self._vbox.append(self._wealth_label)

        # Encumbrance
        self._encumbrance_label = Gtk.Label(label="")
        self._encumbrance_label.add_css_class("encumbrance-label")
        self._encumbrance_label.set_halign(Gtk.Align.START)
        self._encumbrance_label.set_margin_start(20)
        self._encumbrance_label.set_margin_end(20)
        self._encumbrance_label.set_margin_bottom(16)
        self._vbox.append(self._encumbrance_label)

        # Hover popup for enlarged item thumbnail
        self._hover_popup = Gtk.Window()
        self._hover_popup.set_decorated(False)
        self._hover_popup.set_default_size(240, 270)
        self._hover_popup.set_resizable(False)
        self._hover_anim_timer = None
        self._hover_anim_start = 0.0
        self._hover_anim_phase = "idle"
        self._ctx_menu_active = False
        self.warp_enabled = True
        self._hover_thumb_rect = (0, 0, 48, 48)
        self._hover_final_rect = (0, 0, 240, 270)
        self._hover_popup_image = Gtk.Picture()
        self._hover_popup_image.set_content_fit(Gtk.ContentFit.CONTAIN)
        self._hover_popup_image.set_size_request(200, 200)
        self._hover_popup_image.set_margin_start(12)
        self._hover_popup_image.set_margin_end(12)
        self._hover_popup_image.set_margin_top(12)
        popup_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        popup_box.add_css_class("hover-popup")
        popup_box.append(self._hover_popup_image)
        self._hover_popup_label = Gtk.Label(label="")
        self._hover_popup_label.set_margin_start(12)
        self._hover_popup_label.set_margin_end(12)
        self._hover_popup_label.set_margin_top(6)
        self._hover_popup_label.set_margin_bottom(10)
        self._hover_popup_label.add_css_class("hover-label")
        popup_box.append(self._hover_popup_label)
        self._hover_popup.set_child(popup_box)

        # Ripple effect: store source pixbuf for distortion
        self._hover_src_pixbuf = None

        # State
        self._char_name = ""
        self._get_thumb_fn = None
        self._inject_fn: Optional[Callable[[str], None]] = None
        self._regenerate_entity_fn: Optional[Callable[[str], None]] = None
        self._equip_badges: list[Gtk.Label] = []
        self._badge_pulse_timer: Optional[int] = None
        # Map entity_key -> progress label for live % updates
        self._thumb_progress_labels: dict[str, Gtk.Label] = {}

        # Action group for context menu actions (Gtk.Window doesn't implement Gio.ActionMap)
        self._action_group = Gio.SimpleActionGroup()
        self.insert_action_group("ctx", self._action_group)

        # CSS
        css = Gtk.CssProvider()
        css.load_from_string("""
            window {
                background-color: #1a1a2e;
            }
            label {
                color: #e0e0e0;
            }
            .char-name {
                color: #ffffff;
                font-family: 'Serif', 'DejaVu Serif', 'Liberation Serif', serif;
                font-size: 22px;
                font-weight: bold;
            }
            .char-desc {
                color: #c8c8d0;
                font-family: 'Serif', 'DejaVu Serif', 'Liberation Serif', serif;
                font-size: 15px;
                font-weight: 500;
            }
            .section-header {
                font-family: 'Sans', 'DejaVu Sans', sans-serif;
                font-size: 16px;
                font-weight: bold;
            }
            .item-label {
                color: #d4c5e2;
                font-family: 'Sans', 'DejaVu Sans', sans-serif;
                font-size: 14px;
                font-weight: bold;
            }
            .slot-text {
                color: #999;
                font-family: 'Sans', 'DejaVu Sans', sans-serif;
                font-size: 13px;
            }
            .wealth-label {
                color: #c9b458;
                font-family: 'Sans', 'DejaVu Sans', sans-serif;
                font-size: 14px;
                font-weight: bold;
            }
            .encumbrance-label {
                color: #999;
                font-family: 'Sans', 'DejaVu Sans', sans-serif;
                font-size: 13px;
            }
            .inv-icon {
                border: 1px solid #444;
                border-radius: 4px;
                background-color: #222233;
            }
            .inv-icon:hover {
                border-color: #888;
            }
            .inv-icon-equipped {
                border-color: #3a7a3a;
            }
            .inv-icon-placeholder {
                color: #666;
                font-family: 'Sans', 'DejaVu Sans', sans-serif;
                font-size: 18px;
                font-weight: bold;
            }
            .equip-badge {
                color: #44dd44;
                font-family: 'Sans', 'DejaVu Sans', sans-serif;
                font-size: 14px;
                font-weight: 900;
                margin-end: 2px;
                margin-bottom: 1px;
            }
            .hover-popup {
                background-color: #0d0d1a;
                border: 2px solid #444;
                border-radius: 10px;
            }
            .hover-label {
                color: #ffffff;
                font-family: 'Sans', 'DejaVu Sans', sans-serif;
                font-size: 16px;
                font-weight: bold;
            }
        """)
        Gtk.StyleContext.add_provider_for_display(
            Gdk.Display.get_default(),
            css,
            Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION + 1,
        )

    def update_character(self, name: str, description: str,
                         portrait_bytes: Optional[bytes] = None) -> None:
        """Update character name, description, and portrait."""
        self._char_name = name
        def _do():
            self._name_label.set_text(name)
            self._desc_label.set_text(description)

            if portrait_bytes:
                try:
                    loader = GdkPixbuf.PixbufLoader()
                    loader.write(portrait_bytes)
                    loader.close()
                    pixbuf = loader.get_pixbuf()
                    pixbuf = pixbuf.scale_simple(384, 384, GdkPixbuf.InterpType.BILINEAR)
                    texture = Gdk.Texture.new_for_pixbuf(pixbuf)
                    self._portrait.set_paintable(texture)
                except Exception as e:
                    logger.error(f"Failed to load portrait: {e}")
            return False
        GLib.idle_add(_do)

    def set_portrait(self, png_bytes: bytes) -> None:
        """Update just the portrait image."""
        def _do():
            try:
                loader = GdkPixbuf.PixbufLoader()
                loader.write(png_bytes)
                loader.close()
                pixbuf = loader.get_pixbuf()
                pixbuf = pixbuf.scale_simple(384, 384, GdkPixbuf.InterpType.BILINEAR)
                texture = Gdk.Texture.new_for_pixbuf(pixbuf)
                self._portrait.set_paintable(texture)
            except Exception as e:
                logger.error(f"Failed to load portrait: {e}")
            return False
        GLib.idle_add(_do)

    def update_equipment(self, equipment: list[dict], get_thumb_fn) -> None:
        """Update equipped items list.
        equipment: list of {name, slot, thumb_key, has_thumb}
        """
        self._get_thumb_fn = get_thumb_fn
        def _do():
            self._thumb_progress_labels.clear()
            for w in self._equip_widgets:
                self._equip_box.remove(w)
            self._equip_widgets.clear()

            if not equipment:
                self._equip_header.set_visible(False)
                return False

            self._equip_header.set_visible(True)

            for item in equipment:
                row = self._build_item_row(
                    item["name"], item.get("slot", ""),
                    item.get("thumb_key", item["name"]),
                    item.get("has_thumb", False),
                    get_thumb_fn
                )
                self._equip_box.append(row)
                self._equip_widgets.append(row)

            return False
        GLib.idle_add(_do)

    def update_inventory(self, items: list[dict], get_thumb_fn,
                         wealth: str = "", encumbrance: str = "") -> None:
        """Update inventory as icon grid — thumbnails only, name on hover.
        items: list of {name, slot, thumb_key, has_thumb}
        """
        self._get_thumb_fn = get_thumb_fn
        def _do():
            for w in self._inv_widgets:
                self._inv_box.remove(w)
            self._inv_widgets.clear()
            self._equip_badges.clear()

            if not items:
                self._inv_header.set_visible(False)
                self._sep2.set_visible(False)
            else:
                self._inv_header.set_visible(True)
                self._sep2.set_visible(True)

                ICON_SIZE = 48
                COLS = 8  # icons per row

                grid = Gtk.Grid()
                grid.set_row_spacing(4)
                grid.set_column_spacing(4)
                grid.set_halign(Gtk.Align.START)

                for i, item in enumerate(items):
                    name = item["name"]
                    slot = item.get("slot", "")
                    thumb_key = item.get("thumb_key", name)
                    has_thumb = item.get("has_thumb", False)
                    png = get_thumb_fn(thumb_key) if has_thumb else None

                    # Icon with overlay for equipped badge
                    overlay = Gtk.Overlay()
                    overlay.set_size_request(ICON_SIZE, ICON_SIZE)

                    frame = Gtk.Frame()
                    frame.set_size_request(ICON_SIZE, ICON_SIZE)
                    frame.add_css_class("inv-icon")
                    if slot:
                        frame.add_css_class("inv-icon-equipped")

                    if png:
                        try:
                            loader = GdkPixbuf.PixbufLoader()
                            loader.write(png)
                            loader.close()
                            pixbuf = loader.get_pixbuf()
                            pixbuf = pixbuf.scale_simple(
                                ICON_SIZE, ICON_SIZE, GdkPixbuf.InterpType.BILINEAR
                            )
                            texture = Gdk.Texture.new_for_pixbuf(pixbuf)
                            pic = Gtk.Picture()
                            pic.set_paintable(texture)
                            pic.set_size_request(ICON_SIZE, ICON_SIZE)
                            pic.set_content_fit(Gtk.ContentFit.COVER)
                            frame.set_child(pic)
                        except Exception:
                            placeholder = Gtk.Label(label="?")
                            placeholder.set_size_request(ICON_SIZE, ICON_SIZE)
                            frame.set_child(placeholder)
                    else:
                        # Animated loading placeholder with progress %
                        ph = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
                        ph.set_size_request(ICON_SIZE, ICON_SIZE)
                        ph.set_valign(Gtk.Align.CENTER)
                        ph.set_halign(Gtk.Align.CENTER)
                        spinner = Gtk.Spinner()
                        spinner.set_size_request(20, 20)
                        spinner.start()
                        ph.append(spinner)
                        pct_lbl = Gtk.Label(label="...")
                        pct_lbl.add_css_class("dim-label")
                        ph.append(pct_lbl)
                        frame.set_child(ph)
                        self._thumb_progress_labels[thumb_key] = pct_lbl

                    overlay.set_child(frame)

                    # Equipped badge — bold green "E" in bottom-right corner
                    if slot:
                        badge = Gtk.Label(label="E")
                        badge.add_css_class("equip-badge")
                        badge.set_halign(Gtk.Align.END)
                        badge.set_valign(Gtk.Align.END)
                        overlay.add_overlay(badge)
                        self._equip_badges.append(badge)

                    # Tooltip shows name
                    display = f"{name} ({slot})" if slot else name
                    overlay.set_tooltip_text(display)

                    # Hover for enlarged popup
                    hover = Gtk.EventControllerMotion()
                    hover.connect(
                        "enter", self._on_item_hover_enter,
                        name, thumb_key, get_thumb_fn
                    )
                    hover.connect("leave", self._on_item_hover_leave)
                    overlay.add_controller(hover)

                    # Right-click hold menu
                    drag = self._hold_menu.create_gesture(overlay)
                    drag.connect("drag-begin", self._on_inv_right_drag,
                                 name, bool(slot))

                    col = i % COLS
                    row = i // COLS
                    grid.attach(overlay, col, row, 1, 1)

                self._inv_box.append(grid)
                self._inv_widgets.append(grid)

                # Start badge pulse timer if we have equipped items
                if self._equip_badges and not self._badge_pulse_timer:
                    self._badge_pulse_timer = GLib.timeout_add(50, self._pulse_badges)

            # Always show wealth/encumbrance if available
            if wealth:
                self._wealth_label.set_text(f"Wealth: {wealth}")
                self._wealth_label.set_visible(True)
            else:
                self._wealth_label.set_visible(False)

            if encumbrance:
                self._encumbrance_label.set_text(f"Encumbrance: {encumbrance}")
                self._encumbrance_label.set_visible(True)
            else:
                self._encumbrance_label.set_visible(False)

            return False
        GLib.idle_add(_do)

    def _build_item_row(self, name: str, slot: str, thumb_key: str,
                        has_thumb: bool, get_thumb_fn) -> Gtk.Box:
        """Build a single item row: [thumbnail] item name (slot)."""
        row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        row.set_margin_bottom(3)

        # Thumbnail (36x36)
        frame = Gtk.Frame()
        frame.set_size_request(36, 36)

        png = get_thumb_fn(thumb_key) if has_thumb else None
        if png:
            try:
                loader = GdkPixbuf.PixbufLoader()
                loader.write(png)
                loader.close()
                pixbuf = loader.get_pixbuf()
                pixbuf = pixbuf.scale_simple(36, 36, GdkPixbuf.InterpType.BILINEAR)
                texture = Gdk.Texture.new_for_pixbuf(pixbuf)
                pic = Gtk.Picture()
                pic.set_paintable(texture)
                pic.set_size_request(36, 36)
                pic.set_content_fit(Gtk.ContentFit.COVER)
                frame.set_child(pic)
            except Exception:
                pass
        if not png:
            placeholder = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
            placeholder.set_size_request(36, 36)
            placeholder.set_valign(Gtk.Align.CENTER)
            placeholder.set_halign(Gtk.Align.CENTER)
            spinner = Gtk.Spinner()
            spinner.set_size_request(18, 18)
            spinner.start()
            placeholder.append(spinner)
            pct_lbl = Gtk.Label(label="...")
            pct_lbl.add_css_class("dim-label")
            placeholder.append(pct_lbl)
            frame.set_child(placeholder)
            self._thumb_progress_labels[thumb_key] = pct_lbl

        # Hover for enlarged view
        hover = Gtk.EventControllerMotion()
        hover.connect("enter", self._on_item_hover_enter, name, thumb_key, get_thumb_fn)
        hover.connect("leave", self._on_item_hover_leave)
        frame.add_controller(hover)

        # Right-click hold menu for equipped items
        drag = self._hold_menu.create_gesture(row)
        drag.connect("drag-begin", self._on_equip_right_drag, name)

        row.append(frame)

        # Item name + slot
        name_lbl = Gtk.Label()
        name_lbl.add_css_class("item-label")
        if slot:
            name_lbl.set_markup(
                f"<span color='#d4c5e2' weight='bold'>"
                f"{GLib.markup_escape_text(name)}</span> "
                f"<span color='#999' size='small'>"
                f"({GLib.markup_escape_text(slot)})</span>"
            )
        else:
            name_lbl.set_markup(
                f"<span color='#d4c5e2' weight='bold'>"
                f"{GLib.markup_escape_text(name)}</span>"
            )
        name_lbl.set_halign(Gtk.Align.START)
        name_lbl.set_valign(Gtk.Align.CENTER)
        row.append(name_lbl)

        return row

    def _on_item_hover_enter(self, controller, x, y, name, thumb_key, get_thumb_fn):
        self._hover_src_pixbuf = None
        png = get_thumb_fn(thumb_key) if get_thumb_fn else None
        if png:
            try:
                loader = GdkPixbuf.PixbufLoader()
                loader.write(png)
                loader.close()
                pixbuf = loader.get_pixbuf()
                pixbuf = pixbuf.scale_simple(200, 200, GdkPixbuf.InterpType.BILINEAR)
                self._hover_src_pixbuf = pixbuf
                texture = Gdk.Texture.new_for_pixbuf(pixbuf)
                self._hover_popup_image.set_paintable(texture)
            except Exception:
                self._hover_popup_image.set_paintable(None)
        else:
            self._hover_popup_image.set_paintable(None)

        self._hover_popup_label.set_text(name)

        # Get thumbnail screen position for zoom origin
        final_w, final_h = 240, 270
        widget = controller.get_widget()
        native = widget.get_native()
        thumb_sx, thumb_sy = 0, 0
        thumb_w, thumb_h = 48, 48
        popup_x, popup_y = 0, 0
        if native:
            bounds = widget.compute_bounds(native)
            if bounds[0]:
                rect = bounds[1]
                surface = native.get_surface()
                win_x, win_y = (0, 0)
                if surface and hasattr(surface, 'get_position'):
                    win_x, win_y = surface.get_position()
                thumb_sx = int(win_x + rect.get_x())
                thumb_sy = int(win_y + rect.get_y())
                thumb_w = int(rect.get_width())
                thumb_h = int(rect.get_height())
                popup_x = thumb_sx - (final_w - thumb_w) // 2
                popup_y = thumb_sy - final_h - 10
                if popup_y < 0:
                    popup_y = 0

        self._hover_thumb_rect = (thumb_sx, thumb_sy, thumb_w, thumb_h)
        self._hover_final_rect = (popup_x, popup_y, final_w, final_h)

        # Start tiny, transparent
        self._hover_popup.set_default_size(thumb_w, thumb_h)
        self._hover_popup.set_opacity(0.0)
        self._hover_popup.set_transient_for(self)
        self._hover_popup.present()

        self._hover_anim_phase = "zoom_in"
        self._hover_anim_start = _time.monotonic()
        if self._hover_anim_timer is not None:
            GLib.source_remove(self._hover_anim_timer)
        self._hover_anim_timer = GLib.timeout_add(16, self._hover_anim_tick)

    @staticmethod
    def _ease_out_cubic(t: float) -> float:
        return 1.0 - (1.0 - t) ** 3

    @staticmethod
    def _ease_in_cubic(t: float) -> float:
        return t * t * t

    def _hover_anim_tick(self) -> bool:
        """Animate hover popup: zoom-in → float → zoom-out."""
        now = _time.monotonic()
        t = now - self._hover_anim_start
        tx, ty, tw, th = self._hover_thumb_rect
        fx, fy, fw, fh = self._hover_final_rect

        if self._hover_anim_phase == "zoom_in":
            duration = 0.25
            progress = min(t / duration, 1.0)
            ease = self._ease_out_cubic(progress)

            cur_w = int(tw + (fw - tw) * ease)
            cur_h = int(th + (fh - th) * ease)
            self._hover_popup.set_default_size(max(cur_w, 1), max(cur_h, 1))
            self._hover_popup.set_opacity(ease)

            img_size = int(20 + (200 - 20) * ease)
            self._hover_popup_image.set_size_request(img_size, img_size)

            if progress >= 1.0:
                self._hover_anim_phase = "float"
                self._hover_anim_start = now
            return True

        elif self._hover_anim_phase == "float":
            if self._hover_src_pixbuf is not None and not self._ctx_menu_active and self.warp_enabled:
                self._apply_ripple(t)
            return True

        elif self._hover_anim_phase == "zoom_out":
            duration = 0.18
            progress = min(t / duration, 1.0)
            ease = self._ease_in_cubic(progress)

            cur_w = int(fw + (tw - fw) * ease)
            cur_h = int(fh + (th - fh) * ease)
            self._hover_popup.set_default_size(max(cur_w, 1), max(cur_h, 1))
            self._hover_popup.set_opacity(1.0 - ease)

            img_size = int(200 + (20 - 200) * ease)
            self._hover_popup_image.set_size_request(max(img_size, 1), max(img_size, 1))

            if progress >= 1.0:
                self._hover_popup.set_visible(False)
                self._hover_anim_phase = "idle"
                self._hover_anim_timer = None
                self._hover_popup_image.set_size_request(200, 200)
                self._hover_popup_image.set_margin_top(12)
                self._hover_popup.set_default_size(240, 270)
                return False
            return True

        self._hover_anim_timer = None
        return False

    def _apply_ripple(self, t: float) -> None:
        """Apply subtle liquid ripple distortion to the hover image."""
        try:
            import numpy as np

            pb = self._hover_src_pixbuf
            w = pb.get_width()
            h = pb.get_height()
            n_channels = pb.get_n_channels()
            rowstride = pb.get_rowstride()

            pixels = pb.get_pixels()
            src_2d = np.lib.stride_tricks.as_strided(
                np.frombuffer(pixels, dtype=np.uint8),
                shape=(h, w, n_channels),
                strides=(rowstride, n_channels, 1),
            )

            ys = np.linspace(0, 1, h, dtype=np.float32)
            xs = np.linspace(0, 1, w, dtype=np.float32)
            xg, yg = np.meshgrid(xs, ys)

            intensity = 2.5
            dx = np.zeros_like(xg)
            dy = np.zeros_like(yg)

            for cx, cy, freq, phase in [
                (0.3, 0.4, 8.0, 0.0),
                (0.7, 0.6, 10.0, 1.5),
                (0.5, 0.2, 6.0, 3.0),
            ]:
                dist = np.sqrt((xg - cx) ** 2 + (yg - cy) ** 2)
                wave = np.sin(dist * freq * 2 * np.pi - t * 3.0 + phase)
                decay = np.exp(-dist * 3.0)
                wave *= decay
                ddx = np.where(dist > 0.001, (xg - cx) / dist, 0)
                ddy = np.where(dist > 0.001, (yg - cy) / dist, 0)
                dx += ddx * wave * intensity
                dy += ddy * wave * intensity

            map_x = np.clip((xg * w + dx).astype(np.int32), 0, w - 1)
            map_y = np.clip((yg * h + dy).astype(np.int32), 0, h - 1)

            dst = src_2d[map_y, map_x]
            dst_bytes = dst.tobytes()
            new_pb = GdkPixbuf.Pixbuf.new_from_data(
                dst_bytes, GdkPixbuf.Colorspace.RGB,
                n_channels == 4, 8, w, h, w * n_channels,
            )
            new_pb._keep_alive = dst_bytes
            texture = Gdk.Texture.new_for_pixbuf(new_pb)
            self._hover_popup_image.set_paintable(texture)

        except Exception as e:
            logger.debug(f"Ripple effect error: {e}")

    def _on_item_hover_leave(self, controller, *args):
        """Start zoom-out animation back to thumbnail."""
        if self._ctx_menu_active or self._hover_anim_phase == "menu_frozen":
            return
        if self._hover_anim_phase in ("zoom_in", "float"):
            self._hover_anim_phase = "zoom_out"
            self._hover_anim_start = _time.monotonic()
            if self._hover_anim_timer is None:
                self._hover_anim_timer = GLib.timeout_add(16, self._hover_anim_tick)
        else:
            self._hover_popup.set_visible(False)
            if self._hover_anim_timer is not None:
                GLib.source_remove(self._hover_anim_timer)
                self._hover_anim_timer = None

    def _on_hold_menu_open(self):
        """Called when hold-right-click menu opens."""
        self._ctx_menu_active = True
        if self._hover_anim_timer is not None:
            GLib.source_remove(self._hover_anim_timer)
            self._hover_anim_timer = None
        self._hover_anim_phase = "menu_frozen"

    def _on_hold_menu_close(self):
        """Called when hold-right-click menu closes."""
        self._ctx_menu_active = False
        self._hover_anim_phase = "zoom_out"
        self._hover_anim_start = _time.monotonic()
        if self._hover_anim_timer is None:
            self._hover_anim_timer = GLib.timeout_add(16, self._hover_anim_tick)

    def _on_popover_closed(self, popover):
        """Legacy — no longer used but kept for safety."""
        self._ctx_menu_active = False
        if self._hover_anim_phase in ("zoom_in", "float"):
            self._hover_anim_phase = "zoom_out"
            self._hover_anim_start = _time.monotonic()
            if self._hover_anim_timer is None:
                self._hover_anim_timer = GLib.timeout_add(16, self._hover_anim_tick)
        else:
            self._hover_popup.set_visible(False)
            if self._hover_anim_timer is not None:
                GLib.source_remove(self._hover_anim_timer)
                self._hover_anim_timer = None

    def update_thumb_progress(self, entity_key: str, fraction: float) -> None:
        """Update the progress % on a generating thumbnail. Thread-safe."""
        def _do():
            lbl = self._thumb_progress_labels.get(entity_key)
            if lbl:
                lbl.set_text(f"{int(fraction * 100)}%")
            return False
        GLib.idle_add(_do)

    def _pulse_badges(self) -> bool:
        """Animate equipped badge opacity with a smooth pulse."""
        import math, time
        t = time.monotonic()
        opacity = 0.5 + 0.5 * math.sin(t * 2.5)
        for badge in self._equip_badges:
            try:
                badge.set_opacity(opacity)
            except Exception:
                pass
        if not self._equip_badges:
            self._badge_pulse_timer = None
            return False
        return True

    def set_inject_fn(self, fn: Callable[[str], None]) -> None:
        """Set command injection callback."""
        self._inject_fn = fn

    def _ctx_inject(self, cmd: str) -> None:
        """Send a command via the inject callback."""
        if self._inject_fn:
            self._inject_fn(cmd)

    def _ctx_inject_and_refresh(self, cmd: str) -> None:
        """Send a command then refresh inventory."""
        if self._inject_fn:
            self._inject_fn(cmd)
            GLib.timeout_add(1500, lambda: self._inject_fn("i") or False)

    def _ctx_save_thumb_as_png(self, entity_name: str) -> None:
        """Save an entity thumbnail as PNG via file dialog."""
        png = None
        if self._get_thumb_fn:
            png = self._get_thumb_fn(entity_name)
        if not png:
            return
        safe_name = "".join(c if c.isalnum() or c in " -_" else "_" for c in entity_name).strip()
        self._save_file_dialog(
            "Save Image", f"{safe_name}.png", "PNG images", "*.png",
            lambda path: self._write_png(png, path))

    def _save_file_dialog(self, title: str, default_name: str, filter_name: str,
                          filter_pattern: str, callback) -> None:
        dialog = Gtk.FileDialog()
        dialog.set_title(title)
        dialog.set_initial_name(default_name)
        file_filter = Gtk.FileFilter()
        file_filter.set_name(filter_name)
        file_filter.add_pattern(filter_pattern)
        filters = Gio.ListStore.new(Gtk.FileFilter)
        filters.append(file_filter)
        dialog.set_filters(filters)
        dialog.set_default_filter(file_filter)
        from pathlib import Path
        pictures = Path.home() / "Pictures"
        if pictures.is_dir():
            dialog.set_initial_folder(Gio.File.new_for_path(str(pictures)))
        dialog.save(self, None, lambda d, res: self._on_save_done(d, res, callback))

    def _on_save_done(self, dialog, result, callback):
        try:
            gfile = dialog.save_finish(result)
            if gfile:
                callback(gfile.get_path())
        except GLib.Error:
            pass

    def _write_png(self, png_bytes: bytes, path: str) -> None:
        from pathlib import Path
        if not path.lower().endswith('.png'):
            path += '.png'
        Path(path).write_bytes(png_bytes)

    def _ctx_regenerate(self, entity_name: str) -> None:
        """Regenerate a thumbnail with a new seed."""
        if self._regenerate_entity_fn:
            self._regenerate_entity_fn(entity_name)

    def set_regenerate_entity_callback(self, fn) -> None:
        self._regenerate_entity_fn = fn

    def _on_portrait_right_drag(self, gesture, start_x, start_y):
        """Hold-right-click on character portrait."""
        if not self._char_name:
            return
        char_first = self._char_name.split()[0]
        self._hold_menu.show([
            (f"  Save as PNG  ",
             lambda: self._ctx_save_thumb_as_png(char_first)),
            (f"  Regenerate Portrait  ",
             lambda: self._ctx_regenerate(char_first)),
        ], gesture, start_x, start_y)

    def _on_equip_right_drag(self, gesture, start_x, start_y, item_name):
        """Hold-right-click on equipped item."""
        self._hold_menu.show([
            (f"  Save as PNG  ",
             lambda: self._ctx_save_thumb_as_png(item_name)),
            (f"  Look at {item_name}  ",
             lambda: self._ctx_inject(f"l {item_name}")),
            (f"  Unequip {item_name}  ",
             lambda: self._ctx_inject_and_refresh(f"unequip {item_name}")),
            (f"  Sell {item_name}  ",
             lambda: self._ctx_inject_and_refresh(f"sell {item_name}")),
            (f"  Drop {item_name}  ",
             lambda: self._ctx_inject_and_refresh(f"drop {item_name}")),
            (f"  Regenerate Image  ",
             lambda: self._ctx_regenerate(item_name)),
        ], gesture, start_x, start_y)

    def _on_inv_right_drag(self, gesture, start_x, start_y, item_name, has_slot):
        """Hold-right-click on inventory item."""
        items = [
            (f"  Save as PNG  ",
             lambda: self._ctx_save_thumb_as_png(item_name)),
            (f"  Look at {item_name}  ",
             lambda: self._ctx_inject(f"l {item_name}")),
        ]
        if has_slot:
            items.append((f"  Unequip {item_name}  ",
                          lambda: self._ctx_inject_and_refresh(f"unequip {item_name}")))
        else:
            items.append((f"  Equip {item_name}  ",
                          lambda: self._ctx_inject_and_refresh(f"equip {item_name}")))
        items.extend([
            (f"  Sell {item_name}  ",
             lambda: self._ctx_inject_and_refresh(f"sell {item_name}")),
            (f"  Drop {item_name}  ",
             lambda: self._ctx_inject_and_refresh(f"drop {item_name}")),
            (f"  Use {item_name}  ",
             lambda: self._ctx_inject_and_refresh(f"use {item_name}")),
            (f"  Regenerate Image  ",
             lambda: self._ctx_regenerate(item_name)),
        ])
        self._hold_menu.show(items, gesture, start_x, start_y)
