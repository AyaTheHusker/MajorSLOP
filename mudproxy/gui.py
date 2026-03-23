import gi
gi.require_version('Gtk', '4.0')
gi.require_version('Adw', '1')
gi.require_version('Vte', '3.91')
from gi.repository import Gtk, Adw, GLib, Pango, Vte


class MudProxyGUI(Adw.Application):
    def __init__(self):
        super().__init__(application_id='com.mudproxy.gui')
        self.connect('activate', self._on_activate)

        self._terminal: Vte.Terminal = None
        self._room_name_label: Gtk.Label = None
        self._room_desc_view: Gtk.TextView = None
        self._exits_label: Gtk.Label = None
        self._items_label: Gtk.Label = None
        self._monsters_label: Gtk.Label = None
        self._combat_view: Gtk.TextView = None
        self._cmd_entry: Gtk.Entry = None
        self._status_label: Gtk.Label = None
        self._host_entry: Gtk.Entry = None
        self._port_entry: Gtk.Entry = None
        self._user_entry: Gtk.Entry = None
        self._pass_entry: Gtk.Entry = None
        self._reconnect_check: Gtk.CheckButton = None
        self._delay_spin: Gtk.SpinButton = None
        self._connect_btn: Gtk.Button = None

        self._grind_btn: Gtk.Button = None
        self._flee_hp_spin: Gtk.SpinButton = None
        self._rest_hp_spin: Gtk.SpinButton = None
        self._grind_status_label: Gtk.Label = None
        self._grind_running = False
        self._loot_copper: Gtk.CheckButton = None
        self._loot_silver: Gtk.CheckButton = None
        self._loot_gold: Gtk.CheckButton = None
        self._loot_platinum: Gtk.CheckButton = None
        self._loot_runic: Gtk.CheckButton = None

        self._inject_callback = None
        self._connect_callback = None
        self._disconnect_callback = None
        self._save_config_callback = None
        self._grind_start_callback = None
        self._grind_stop_callback = None
        self._room_view_callback = None
        self._room_view_btn: Gtk.ToggleButton = None
        self._char_view_btn: Gtk.ToggleButton = None
        self._char_view_callback = None
        self._chatbot_callback = None
        self._portrait_style_callback = None
        self._depth_3d_callback = None
        self._depth_params_callback = None
        self._gen_activity_label: Gtk.Label = None
        self._gen_progress_bar: Gtk.ProgressBar = None

    def _on_activate(self, app) -> None:
        win = Adw.ApplicationWindow(application=app)
        win.set_title("MajorSLOP!")
        win.set_default_size(1400, 900)

        main_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)

        # Header bar
        header = Adw.HeaderBar()
        header.set_title_widget(Gtk.Label(label="MajorSLOP!"))

        self._char_view_btn = Gtk.ToggleButton(label="Character")
        self._char_view_btn.set_active(False)
        self._char_view_btn.connect('toggled', self._on_char_view_toggled)
        header.pack_end(self._char_view_btn)

        self._room_view_btn = Gtk.ToggleButton(label="Room View")
        self._room_view_btn.set_active(True)
        self._room_view_btn.connect('toggled', self._on_room_view_toggled)
        header.pack_end(self._room_view_btn)

        main_box.append(header)

        # Connection bar
        conn_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
        conn_box.set_margin_start(12)
        conn_box.set_margin_end(12)
        conn_box.set_margin_top(6)
        conn_box.set_margin_bottom(6)

        conn_box.append(Gtk.Label(label="Host:"))
        self._host_entry = Gtk.Entry()
        self._host_entry.set_text("sos-bbs.net")
        self._host_entry.set_width_chars(20)
        conn_box.append(self._host_entry)

        conn_box.append(Gtk.Label(label="Port:"))
        self._port_entry = Gtk.Entry()
        self._port_entry.set_text("23")
        self._port_entry.set_width_chars(6)
        conn_box.append(self._port_entry)

        conn_box.append(Gtk.Label(label="User:"))
        self._user_entry = Gtk.Entry()
        self._user_entry.set_width_chars(12)
        conn_box.append(self._user_entry)

        conn_box.append(Gtk.Label(label="Pass:"))
        self._pass_entry = Gtk.Entry()
        self._pass_entry.set_visibility(False)
        self._pass_entry.set_width_chars(12)
        conn_box.append(self._pass_entry)

        self._connect_btn = Gtk.Button(label="Connect")
        self._connect_btn.connect('clicked', self._on_connect_clicked)
        self._connect_btn.add_css_class('suggested-action')
        conn_box.append(self._connect_btn)

        disconnect_btn = Gtk.Button(label="Disconnect")
        disconnect_btn.connect('clicked', self._on_disconnect_clicked)
        disconnect_btn.add_css_class('destructive-action')
        conn_box.append(disconnect_btn)

        main_box.append(conn_box)

        # Separator
        main_box.append(Gtk.Separator(orientation=Gtk.Orientation.HORIZONTAL))

        # Main content: paned
        paned = Gtk.Paned(orientation=Gtk.Orientation.HORIZONTAL)
        paned.set_vexpand(True)
        paned.set_position(850)

        # LEFT: VTE Terminal
        left_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=4)
        left_box.set_margin_start(12)
        left_box.set_margin_end(6)
        left_box.set_margin_top(8)
        left_box.set_margin_bottom(8)

        left_box.append(Gtk.Label(label="BBS Terminal", xalign=0))

        # Create VTE terminal - full terminal emulator with ANSI support
        self._terminal = Vte.Terminal()
        self._terminal.set_vexpand(True)
        self._terminal.set_hexpand(True)

        # Terminal appearance
        self._terminal.set_font(Pango.FontDescription("DejaVu Sans Mono 13"))
        fg = self._parse_rgba('#aaaaaa')
        bg = self._parse_rgba('#000000')
        self._terminal.set_color_foreground(fg)
        self._terminal.set_color_background(bg)

        # Set 16-color ANSI palette
        palette = [
            '#000000', '#aa0000', '#00aa00', '#aa5500',
            '#0000aa', '#aa00aa', '#00aaaa', '#aaaaaa',
            '#555555', '#ff5555', '#55ff55', '#ffff55',
            '#5555ff', '#ff55ff', '#55ffff', '#ffffff',
        ]
        self._terminal.set_colors(
            fg, bg,
            [self._parse_rgba(c) for c in palette],
        )

        self._terminal.set_size(80, 24)
        self._terminal.set_scroll_on_output(True)
        self._terminal.set_scrollback_lines(10000)

        left_box.append(self._terminal)

        paned.set_start_child(left_box)

        # RIGHT: Room info + Combat log
        right_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
        right_box.set_margin_start(6)
        right_box.set_margin_end(12)
        right_box.set_margin_top(8)
        right_box.set_margin_bottom(8)

        # Room info frame
        room_frame = Gtk.Frame(label="Room Info")
        room_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=4)
        room_box.set_margin_start(8)
        room_box.set_margin_end(8)
        room_box.set_margin_top(8)
        room_box.set_margin_bottom(8)

        self._room_name_label = Gtk.Label(label="Room: --")
        self._room_name_label.set_xalign(0)
        self._room_name_label.set_wrap(True)
        self._room_name_label.add_css_class("title-3")
        room_box.append(self._room_name_label)

        # Room description (scrollable)
        desc_scroll = Gtk.ScrolledWindow()
        desc_scroll.set_min_content_height(100)
        desc_scroll.set_max_content_height(200)
        desc_scroll.set_propagate_natural_height(True)
        self._room_desc_view = Gtk.TextView()
        self._room_desc_view.set_editable(False)
        self._room_desc_view.set_wrap_mode(Gtk.WrapMode.WORD)
        self._room_desc_view.set_cursor_visible(False)
        desc_scroll.set_child(self._room_desc_view)
        room_box.append(desc_scroll)

        self._exits_label = Gtk.Label(label="Exits: --")
        self._exits_label.set_xalign(0)
        self._exits_label.set_wrap(True)
        room_box.append(self._exits_label)

        self._items_label = Gtk.Label(label="Items: --")
        self._items_label.set_xalign(0)
        self._items_label.set_wrap(True)
        room_box.append(self._items_label)

        self._monsters_label = Gtk.Label(label="Monsters: --")
        self._monsters_label.set_xalign(0)
        self._monsters_label.set_wrap(True)
        room_box.append(self._monsters_label)

        room_frame.set_child(room_box)
        right_box.append(room_frame)

        # Combat log frame
        combat_frame = Gtk.Frame(label="Combat Log")
        combat_scroll = Gtk.ScrolledWindow()
        combat_scroll.set_vexpand(True)
        combat_scroll.set_min_content_height(150)
        self._combat_view = Gtk.TextView()
        self._combat_view.set_editable(False)
        self._combat_view.set_wrap_mode(Gtk.WrapMode.WORD)
        self._combat_view.set_cursor_visible(False)
        self._combat_view.set_monospace(True)
        combat_scroll.set_child(self._combat_view)
        combat_frame.set_child(combat_scroll)
        right_box.append(combat_frame)

        paned.set_end_child(right_box)
        main_box.append(paned)

        # Separator
        main_box.append(Gtk.Separator(orientation=Gtk.Orientation.HORIZONTAL))

        # Command entry row
        cmd_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
        cmd_box.set_margin_start(12)
        cmd_box.set_margin_end(12)
        cmd_box.set_margin_top(6)

        cmd_box.append(Gtk.Label(label="Command:"))
        self._cmd_entry = Gtk.Entry()
        self._cmd_entry.set_hexpand(True)
        self._cmd_entry.set_placeholder_text("Type command to send to MUD...")
        self._cmd_entry.connect('activate', self._on_cmd_enter)
        cmd_box.append(self._cmd_entry)

        send_btn = Gtk.Button(label="Send")
        send_btn.connect('clicked', lambda _: self._on_cmd_enter(self._cmd_entry))
        cmd_box.append(send_btn)

        main_box.append(cmd_box)

        # Grind controls row (hidden — MegaMUD handles grinding for now)
        # grind_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
        # ... grind UI hidden ...

        # --- 3D Depth Settings Expander ---
        self._depth_expander = Gtk.Expander(label="3D Depth Settings")
        self._depth_expander.set_margin_start(12)
        self._depth_expander.set_margin_end(12)
        self._depth_expander.set_margin_top(4)

        depth_grid = Gtk.Grid()
        depth_grid.set_column_spacing(8)
        depth_grid.set_row_spacing(4)
        depth_grid.set_margin_start(8)
        depth_grid.set_margin_end(8)
        depth_grid.set_margin_top(6)
        depth_grid.set_margin_bottom(6)

        # Row 0: Enable 3D + Camera Mode
        self._depth_3d_check = Gtk.CheckButton(label="Enable 3D Mode")
        self._depth_3d_check.set_active(False)
        self._depth_3d_check.connect('toggled', self._on_depth_3d_toggled)
        depth_grid.attach(self._depth_3d_check, 0, 0, 2, 1)

        depth_grid.attach(Gtk.Label(label="Camera:", xalign=1), 2, 0, 1, 1)
        self._depth_cam_mode = Gtk.ComboBoxText()
        for name in ["Carousel", "Horizontal", "Vertical", "Circle",
                      "Zoom", "Dolly", "Orbital", "Explore"]:
            self._depth_cam_mode.append_text(name)
        self._depth_cam_mode.set_active(3)  # Circle
        self._depth_cam_mode.connect('changed', self._on_depth_cam_mode_changed)
        depth_grid.attach(self._depth_cam_mode, 3, 0, 1, 1)

        self._depth_inpaint_check = Gtk.CheckButton(label="Inpainting")
        self._depth_inpaint_check.set_active(True)
        depth_grid.attach(self._depth_inpaint_check, 4, 0, 1, 1)

        # Row 1: Depth Scale, Intensity, Speed
        def _make_scale(label, val, lo, hi, step, col, row):
            depth_grid.attach(Gtk.Label(label=label, xalign=1), col, row, 1, 1)
            adj = Gtk.Adjustment(value=val, lower=lo, upper=hi, step_increment=step)
            spin = Gtk.SpinButton(adjustment=adj, climb_rate=step, digits=2)
            spin.set_width_chars(5)
            spin.connect('value-changed', self._on_depth_param_changed)
            depth_grid.attach(spin, col + 1, row, 1, 1)
            return spin

        self._d_scale = _make_scale("Depth:", 0.15, 0.0, 1.0, 0.01, 0, 1)
        self._d_intensity = _make_scale("Intensity:", 0.3, 0.0, 1.0, 0.01, 2, 1)
        self._d_speed = _make_scale("Speed:", 0.4, 0.0, 2.0, 0.05, 4, 1)

        # Row 2: Isometric, Steady, Overscan
        self._d_isometric = _make_scale("Isometric:", 0.0, 0.0, 1.0, 0.05, 0, 2)
        self._d_steady = _make_scale("Steady:", 0.0, 0.0, 1.0, 0.05, 2, 2)
        self._d_overscan = _make_scale("Overscan:", 0.05, 0.0, 0.3, 0.01, 4, 2)

        # Row 3: Pan Speed, Pan X, Pan Y
        self._d_pan_speed = _make_scale("Pan Spd:", 0.1, 0.0, 1.0, 0.01, 0, 3)
        self._d_pan_x = _make_scale("Pan X:", 0.3, 0.0, 1.0, 0.05, 2, 3)
        self._d_pan_y = _make_scale("Pan Y:", 0.2, 0.0, 1.0, 0.05, 4, 3)

        # Row 4: Edge Fade, Depth Contrast
        self._d_edge_start = _make_scale("Edge Start:", 0.15, 0.0, 1.0, 0.01, 0, 4)
        self._d_edge_end = _make_scale("Edge End:", 0.4, 0.0, 1.0, 0.01, 2, 4)
        self._d_contrast = _make_scale("Contrast:", 1.0, 0.1, 3.0, 0.1, 4, 4)

        # Row 5: Reset to Defaults button
        reset_btn = Gtk.Button(label="Reset to Defaults")
        reset_btn.connect('clicked', self._on_depth_reset_defaults)
        depth_grid.attach(reset_btn, 0, 5, 2, 1)

        self._depth_expander.set_child(depth_grid)
        main_box.append(self._depth_expander)

        # Generation activity bar
        gen_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=2)
        gen_box.set_margin_start(12)
        gen_box.set_margin_end(12)
        gen_box.set_margin_top(4)

        self._gen_activity_label = Gtk.Label(label="")
        self._gen_activity_label.set_xalign(0)
        self._gen_activity_label.add_css_class("dim-label")
        gen_box.append(self._gen_activity_label)

        self._gen_progress_bar = Gtk.ProgressBar()
        self._gen_progress_bar.set_show_text(False)
        self._gen_progress_bar.set_fraction(0.0)
        gen_box.append(self._gen_progress_bar)

        main_box.append(gen_box)

        # Status bar
        status_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=12)
        status_box.set_margin_start(12)
        status_box.set_margin_end(12)
        status_box.set_margin_top(4)
        status_box.set_margin_bottom(6)

        self._status_label = Gtk.Label(label="Disconnected")
        self._status_label.set_xalign(0)
        self._status_label.add_css_class("dim-label")
        status_box.append(self._status_label)

        spacer = Gtk.Box()
        spacer.set_hexpand(True)
        status_box.append(spacer)

        self._chatbot_check = Gtk.CheckButton(label="Chatbot")
        self._chatbot_check.set_active(False)
        self._chatbot_check.connect('toggled', self._on_chatbot_toggled)
        status_box.append(self._chatbot_check)

        self._reconnect_check = Gtk.CheckButton(label="Auto-reconnect")
        self._reconnect_check.set_active(True)
        status_box.append(self._reconnect_check)

        status_box.append(Gtk.Label(label="Delay:"))
        adj = Gtk.Adjustment(value=5, lower=1, upper=60, step_increment=1)
        self._delay_spin = Gtk.SpinButton(adjustment=adj, climb_rate=1, digits=0)
        self._delay_spin.set_width_chars(3)
        status_box.append(self._delay_spin)
        status_box.append(Gtk.Label(label="sec"))

        main_box.append(status_box)

        # Portrait art style
        style_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
        style_box.set_margin_start(12)
        style_box.set_margin_end(12)
        style_box.set_margin_bottom(4)
        style_box.append(Gtk.Label(label="Portrait Style:"))
        self._portrait_style_entry = Gtk.Entry()
        self._portrait_style_entry.set_placeholder_text("e.g. photorealistic, anime, oil painting, 3d rendered")
        self._portrait_style_entry.set_hexpand(True)
        self._portrait_style_entry.set_text("fantasy art")
        self._portrait_style_entry.connect("changed", self._on_portrait_style_changed)
        style_box.append(self._portrait_style_entry)
        main_box.append(style_box)

        win.set_content(main_box)
        win.present()

    @staticmethod
    def _parse_rgba(hex_color: str):
        from gi.repository import Gdk
        rgba = Gdk.RGBA()
        rgba.parse(hex_color)
        return rgba

    # --- Public update methods (called via GLib.idle_add) ---

    def append_raw_log(self, text: str, ansi: bool = False) -> None:
        """Feed text to VTE terminal."""
        def _do():
            if self._terminal:
                self._terminal.feed(text.encode('utf-8', errors='replace'))
            return False
        GLib.idle_add(_do)

    def feed_raw_bytes(self, data: bytes) -> None:
        """Feed raw bytes directly to the VTE terminal."""
        def _do():
            if self._terminal:
                self._terminal.feed(data)
            return False
        GLib.idle_add(_do)

    def update_room(self, name: str, description: str,
                    exits: list[str], items: list[str],
                    monsters: list[str]) -> None:
        def _do():
            self._room_name_label.set_label(name)
            buf = self._room_desc_view.get_buffer()
            buf.set_text(description)
            self._exits_label.set_label(
                f"Exits: {', '.join(exits) if exits else '--'}")
            self._items_label.set_label(
                f"Items: {', '.join(items) if items else '--'}")
            self._monsters_label.set_label(
                f"Monsters: {', '.join(monsters) if monsters else '--'}")
            return False
        GLib.idle_add(_do)

    def add_combat_line(self, text: str) -> None:
        def _do():
            buf = self._combat_view.get_buffer()
            end = buf.get_end_iter()
            buf.insert(end, text + '\n')
            line_count = buf.get_line_count()
            if line_count > 500:
                start = buf.get_start_iter()
                _, trim_to = buf.get_iter_at_line(line_count - 400)
                buf.delete(start, trim_to)
            end = buf.get_end_iter()
            self._combat_view.scroll_to_iter(end, 0.0, False, 0.0, 1.0)
            return False
        GLib.idle_add(_do)

    def update_status(self, text: str) -> None:
        def _do():
            self._status_label.set_label(text)
            return False
        GLib.idle_add(_do)

    def update_generation_status(self, activity: str, progress: float = -1) -> None:
        """Update the generation activity label and progress bar.
        activity: text like 'Loading pipeline...' or '' to hide
        progress: 0.0-1.0 for determinate, -1 for pulse/indeterminate
        """
        def _do():
            if not activity:
                self._gen_activity_label.set_label("")
                self._gen_progress_bar.set_fraction(0.0)
                self._gen_progress_bar.set_visible(False)
                self._gen_activity_label.set_visible(False)
            else:
                self._gen_activity_label.set_label(activity)
                self._gen_activity_label.set_visible(True)
                self._gen_progress_bar.set_visible(True)
                if progress < 0:
                    self._gen_progress_bar.pulse()
                else:
                    self._gen_progress_bar.set_fraction(min(progress, 1.0))
            return False
        GLib.idle_add(_do)

    def set_connected_state(self, connected: bool) -> None:
        def _do():
            if connected:
                self._connect_btn.set_label("Connected")
                self._connect_btn.set_sensitive(False)
            else:
                self._connect_btn.set_label("Connect")
                self._connect_btn.set_sensitive(True)
            return False
        GLib.idle_add(_do)

    def load_config(self, config) -> None:
        def _do():
            self._host_entry.set_text(config.bbs_host)
            self._port_entry.set_text(str(config.bbs_port))
            self._user_entry.set_text(config.username)
            self._pass_entry.set_text(config.password)
            self._reconnect_check.set_active(config.auto_reconnect)
            self._delay_spin.set_value(config.reconnect_delay)
            if config.portrait_style:
                self._portrait_style_entry.set_text(config.portrait_style)
            return False
        GLib.idle_add(_do)

    def get_config_from_gui(self) -> dict:
        return {
            'bbs_host': self._host_entry.get_text(),
            'bbs_port': int(self._port_entry.get_text() or '23'),
            'username': self._user_entry.get_text(),
            'password': self._pass_entry.get_text(),
            'auto_reconnect': self._reconnect_check.get_active(),
            'reconnect_delay': int(self._delay_spin.get_value()),
        }

    # --- Callback setters ---

    def set_inject_callback(self, cb) -> None:
        self._inject_callback = cb

    def set_connect_callback(self, cb) -> None:
        self._connect_callback = cb

    def set_disconnect_callback(self, cb) -> None:
        self._disconnect_callback = cb

    def set_save_config_callback(self, cb) -> None:
        self._save_config_callback = cb

    def set_grind_start_callback(self, cb) -> None:
        self._grind_start_callback = cb

    def set_grind_stop_callback(self, cb) -> None:
        self._grind_stop_callback = cb

    def set_room_view_callback(self, cb) -> None:
        self._room_view_callback = cb

    def update_grind_status(self, text: str) -> None:
        def _do():
            if self._grind_status_label:
                self._grind_status_label.set_label(text)
            return False
        GLib.idle_add(_do)

    def get_grind_settings(self) -> dict:
        return {
            'flee_hp': int(self._flee_hp_spin.get_value()) if self._flee_hp_spin else 10,
            'rest_to_hp': int(self._rest_hp_spin.get_value()) if self._rest_hp_spin else 24,
            'loot_copper': self._loot_copper.get_active() if self._loot_copper else True,
            'loot_silver': self._loot_silver.get_active() if self._loot_silver else True,
            'loot_gold': self._loot_gold.get_active() if self._loot_gold else True,
            'loot_platinum': self._loot_platinum.get_active() if self._loot_platinum else True,
            'loot_runic': self._loot_runic.get_active() if self._loot_runic else True,
        }

    # --- Internal handlers ---

    def _on_cmd_enter(self, entry) -> None:
        text = entry.get_text()
        if self._inject_callback:
            entry.set_text("")
            self._inject_callback(text)

    def _on_connect_clicked(self, _btn) -> None:
        if self._save_config_callback:
            self._save_config_callback(self.get_config_from_gui())
        if self._connect_callback:
            self._connect_callback()

    def _on_disconnect_clicked(self, _btn) -> None:
        if self._disconnect_callback:
            self._disconnect_callback()

    def _on_room_view_toggled(self, btn) -> None:
        if self._room_view_callback:
            self._room_view_callback(btn.get_active())

    def set_char_view_callback(self, cb) -> None:
        self._char_view_callback = cb

    def _on_char_view_toggled(self, btn) -> None:
        if self._char_view_callback:
            self._char_view_callback(btn.get_active())

    def _on_portrait_style_changed(self, entry) -> None:
        if self._portrait_style_callback:
            self._portrait_style_callback(entry.get_text().strip())

    def set_portrait_style_callback(self, cb) -> None:
        self._portrait_style_callback = cb

    @property
    def portrait_style(self) -> str:
        return self._portrait_style_entry.get_text().strip()

    def _on_chatbot_toggled(self, btn) -> None:
        if self._chatbot_callback:
            self._chatbot_callback(btn.get_active())

    def set_chatbot_callback(self, cb) -> None:
        self._chatbot_callback = cb

    @property
    def chatbot_enabled(self) -> bool:
        return hasattr(self, '_chatbot_check') and self._chatbot_check.get_active()

    def set_depth_3d_callback(self, cb) -> None:
        self._depth_3d_callback = cb

    def set_depth_params_callback(self, cb) -> None:
        self._depth_params_callback = cb

    def _on_depth_3d_toggled(self, btn) -> None:
        if self._depth_3d_callback:
            self._depth_3d_callback(btn.get_active())

    def _on_depth_param_changed(self, *args) -> None:
        if self._depth_params_callback:
            self._depth_params_callback(self.get_depth_params())

    def _on_depth_cam_mode_changed(self, combo) -> None:
        """Apply per-mode defaults when camera mode changes."""
        # Skip during initial config load
        if getattr(self, '_loading_depth_config', False):
            self._on_depth_param_changed()
            return
        from mudproxy.vulkan_renderer import CAMERA_MODE_DEFAULTS
        mode = combo.get_active()
        if mode < 0:
            return
        defaults = CAMERA_MODE_DEFAULTS.get(mode, {})
        self._apply_depth_defaults(defaults)
        # Trigger param update callback
        self._on_depth_param_changed()

    def _on_depth_reset_defaults(self, btn) -> None:
        """Reset depth params to defaults for current camera mode."""
        from mudproxy.vulkan_renderer import CAMERA_MODE_DEFAULTS
        mode = self._depth_cam_mode.get_active()
        if mode < 0:
            mode = 3
        defaults = CAMERA_MODE_DEFAULTS.get(mode, {})
        self._apply_depth_defaults(defaults)
        self._on_depth_param_changed()

    def _apply_depth_defaults(self, defaults: dict) -> None:
        """Set spin button values from a defaults dict, blocking signals."""
        mapping = {
            "depth_scale": self._d_scale,
            "camera_intensity": self._d_intensity,
            "camera_speed": self._d_speed,
            "isometric": self._d_isometric,
            "steady": self._d_steady,
            "overscan": self._d_overscan,
            "pan_speed": self._d_pan_speed,
            "pan_amount_x": self._d_pan_x,
            "pan_amount_y": self._d_pan_y,
        }
        for key, spin in mapping.items():
            if key in defaults:
                spin.set_value(defaults[key])

    def get_depth_params(self) -> dict:
        return {
            "depth_scale": self._d_scale.get_value(),
            "camera_mode": self._depth_cam_mode.get_active(),
            "camera_intensity": self._d_intensity.get_value(),
            "camera_speed": self._d_speed.get_value(),
            "isometric": self._d_isometric.get_value(),
            "steady": self._d_steady.get_value(),
            "overscan": self._d_overscan.get_value(),
            "pan_speed": self._d_pan_speed.get_value(),
            "pan_amount_x": self._d_pan_x.get_value(),
            "pan_amount_y": self._d_pan_y.get_value(),
            "pan_phase": 0.0,
            "pan_shape": 0.0,
            "edge_fade_start": self._d_edge_start.get_value(),
            "edge_fade_end": self._d_edge_end.get_value(),
            "inpaint_blend_start": 0.02,
            "inpaint_blend_end": 0.08,
            "depth_contrast": self._d_contrast.get_value(),
            "bg_r": 0.1, "bg_g": 0.1, "bg_b": 0.18,
        }

    def load_depth_config(self, config) -> None:
        def _do():
            if hasattr(self, '_depth_3d_check'):
                self._loading_depth_config = True
                self._depth_3d_check.set_active(config.depth_3d_enabled)
                p = config.depth_3d_params
                self._depth_cam_mode.set_active(p.get("camera_mode", 3))
                self._d_scale.set_value(p.get("depth_scale", 0.15))
                self._d_intensity.set_value(p.get("camera_intensity", 0.3))
                self._d_speed.set_value(p.get("camera_speed", 0.4))
                self._d_isometric.set_value(p.get("isometric", 0.0))
                self._d_steady.set_value(p.get("steady", 0.0))
                self._d_overscan.set_value(p.get("overscan", 0.05))
                self._d_pan_speed.set_value(p.get("pan_speed", 0.1))
                self._d_pan_x.set_value(p.get("pan_amount_x", 0.3))
                self._d_pan_y.set_value(p.get("pan_amount_y", 0.2))
                self._d_edge_start.set_value(p.get("edge_fade_start", 0.15))
                self._d_edge_end.set_value(p.get("edge_fade_end", 0.4))
                self._d_contrast.set_value(p.get("depth_contrast", 1.0))
                self._depth_inpaint_check.set_active(config.depth_inpaint_enabled)
                self._loading_depth_config = False
            return False
        GLib.idle_add(_do)

    def is_depth_3d_enabled(self) -> bool:
        return hasattr(self, '_depth_3d_check') and self._depth_3d_check.get_active()

    def _on_grind_clicked(self, _btn) -> None:
        if self._grind_running:
            if self._grind_stop_callback:
                self._grind_stop_callback()
            self._grind_running = False
            self._grind_btn.set_label("Start Grind")
            self._grind_btn.remove_css_class('destructive-action')
            self._grind_btn.add_css_class('suggested-action')
            self._grind_status_label.set_label("Grind: Off")
        else:
            if self._grind_start_callback:
                settings = self.get_grind_settings()
                self._grind_start_callback(settings)
            self._grind_running = True
            self._grind_btn.set_label("Stop Grind")
            self._grind_btn.remove_css_class('suggested-action')
            self._grind_btn.add_css_class('destructive-action')
            self._grind_status_label.set_label("Grind: Running")
