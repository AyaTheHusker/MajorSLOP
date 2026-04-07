/* mudradio_ui.c — MudAMP Panel Rendering + Mouse/Key Handling
 * Vulkan-rendered floating panel, themed by ui_themes[current_theme].
 * Included into vk_terminal.c
 */

/* ---- Resize constants ---- */
#define MR_MIN_W 300.0f
#define MR_MIN_H 340.0f
#define MR_RESIZE_GRIP 12  /* pixels for resize handle corner */

/* ---- Toggle ---- */

static void mr_toggle(void) {
    mr_visible = !mr_visible;
    if (mr_visible && mr_x < 1.0f && mr_y < 1.0f) {
        int vp_w = (int)vk_sc_extent.width, vp_h = (int)vk_sc_extent.height;
        mr_x = (float)(vp_w - (int)mr_w) / 2.0f;
        mr_y = (float)(vp_h - (int)mr_h) / 2.0f;
    }
}

/* ---- Transport layout: all buttons same width ---- */

#define MR_BTN_W  38
#define MR_BTN_H  26
#define MR_BTN_GAP 4
#define MR_NUM_BTNS 5  /* prev, play/pause, stop, next, shuffle */

/* ---- Panel Draw ---- */

static void mr_draw(int vp_w, int vp_h)
{
    if (!mr_visible) return;

    const ui_theme_t *t = &ui_themes[current_theme];
    void (*psolid)(float, float, float, float, float, float, float, float, int, int) =
        ui_font_ready ? push_solid_ui : push_solid;
    void (*ptext)(int, int, const char *, float, float, float, int, int, int, int) =
        ui_font_ready ? push_text_ui : push_text;
    int cw = VSB_CHAR_W, ch = VSB_CHAR_H;

    float x0 = mr_x, y0 = mr_y;
    float pw = mr_w, ph = mr_h;
    float x1 = x0 + pw, y1 = y0 + ph;

    float bgr = t->bg[0], bgg = t->bg[1], bgb = t->bg[2];
    float txr = t->text[0], txg = t->text[1], txb = t->text[2];
    float dmr = t->dim[0], dmg = t->dim[1], dmb = t->dim[2];
    float acr = t->accent[0], acg = t->accent[1], acb = t->accent[2];

    int titlebar_h = ch + 10;
    int tab_h = ch + 8;
    int row_h = ch + 4;
    int pad = 8;

    /* ---- Panel background ---- */
    psolid(x0, y0, x1, y1,
           bgr + 0.04f, bgg + 0.04f, bgb + 0.04f, 0.96f,
           vp_w, vp_h);

    /* ---- Outer bevel ---- */
    psolid(x0, y0, x1, y0 + 1.0f,
           bgr + 0.18f, bgg + 0.18f, bgb + 0.18f, 0.8f, vp_w, vp_h);
    psolid(x0, y0, x0 + 1.0f, y1,
           bgr + 0.14f, bgg + 0.14f, bgb + 0.14f, 0.7f, vp_w, vp_h);
    psolid(x0, y1 - 1.0f, x1, y1,
           bgr * 0.4f, bgg * 0.4f, bgb * 0.4f, 0.9f, vp_w, vp_h);
    psolid(x1 - 1.0f, y0, x1, y1,
           bgr * 0.5f, bgg * 0.5f, bgb * 0.5f, 0.85f, vp_w, vp_h);
    /* Inner bevel */
    psolid(x0 + 1.0f, y0 + 1.0f, x1 - 1.0f, y0 + 2.0f,
           bgr + 0.10f, bgg + 0.10f, bgb + 0.10f, 0.5f, vp_w, vp_h);
    psolid(x0 + 1.0f, y0 + 1.0f, x0 + 2.0f, y1 - 1.0f,
           bgr + 0.08f, bgg + 0.08f, bgb + 0.08f, 0.4f, vp_w, vp_h);

    /* ---- Title bar ---- */
    float tb_y0 = y0 + 2.0f, tb_y1 = y0 + (float)titlebar_h;
    psolid(x0 + 2.0f, tb_y0, x1 - 2.0f, tb_y1,
           acr * 0.25f + bgr * 0.5f, acg * 0.25f + bgg * 0.5f, acb * 0.25f + bgb * 0.5f, 0.95f,
           vp_w, vp_h);
    /* Gloss */
    psolid(x0 + 2.0f, tb_y0, x1 - 2.0f, tb_y0 + (float)(titlebar_h / 2),
           1.0f, 1.0f, 1.0f, 0.06f, vp_w, vp_h);
    /* Bottom edge */
    psolid(x0 + 2.0f, tb_y1 - 1.0f, x1 - 2.0f, tb_y1,
           acr, acg, acb, 0.5f, vp_w, vp_h);

    int title_tx = (int)x0 + pad;
    int title_ty = (int)tb_y0 + (titlebar_h - ch) / 2;
    ptext(title_tx + 1, title_ty + 1, "MudAMP", 0.0f, 0.0f, 0.0f, vp_w, vp_h, cw, ch);
    ptext(title_tx, title_ty, "MudAMP", acr, acg, acb, vp_w, vp_h, cw, ch);

    /* Close [X] */
    int close_tx = (int)x1 - pad - cw;
    ptext(close_tx, title_ty, "X", 1.0f, 0.3f, 0.3f, vp_w, vp_h, cw, ch);

    int cy = (int)tb_y1 + 2;

    /* ---- Tab bar ---- */
    {
        float tab_y0f = (float)cy, tab_y1f = (float)(cy + tab_h);
        /* Radio tab */
        float r_bg = (mr_tab == MR_TAB_RADIO) ? 0.12f : 0.0f;
        psolid(x0 + 3.0f, tab_y0f, x0 + 3.0f + 7.0f * cw + 8, tab_y1f,
               acr * r_bg + bgr * 0.6f, acg * r_bg + bgg * 0.6f, acb * r_bg + bgb * 0.6f,
               (mr_tab == MR_TAB_RADIO) ? 0.9f : 0.5f, vp_w, vp_h);
        ptext((int)x0 + pad + 2, cy + (tab_h - ch) / 2, "Radio",
              (mr_tab == MR_TAB_RADIO) ? acr : dmr,
              (mr_tab == MR_TAB_RADIO) ? acg : dmg,
              (mr_tab == MR_TAB_RADIO) ? acb : dmb,
              vp_w, vp_h, cw, ch);

        /* Player tab */
        int ptab_x = (int)x0 + pad + 7 * cw + 12;
        float p_bg = (mr_tab == MR_TAB_PLAYER) ? 0.12f : 0.0f;
        psolid((float)ptab_x, tab_y0f, (float)ptab_x + 8.0f * cw + 8, tab_y1f,
               acr * p_bg + bgr * 0.6f, acg * p_bg + bgg * 0.6f, acb * p_bg + bgb * 0.6f,
               (mr_tab == MR_TAB_PLAYER) ? 0.9f : 0.5f, vp_w, vp_h);
        ptext(ptab_x + 4, cy + (tab_h - ch) / 2, "Player",
              (mr_tab == MR_TAB_PLAYER) ? acr : dmr,
              (mr_tab == MR_TAB_PLAYER) ? acg : dmg,
              (mr_tab == MR_TAB_PLAYER) ? acb : dmb,
              vp_w, vp_h, cw, ch);

        /* Tab bottom border */
        psolid(x0 + 3.0f, tab_y1f - 1.0f, x1 - 3.0f, tab_y1f,
               acr, acg, acb, 0.3f, vp_w, vp_h);
        cy += tab_h + 2;
    }

    /* ---- Now Playing ---- */
    {
        char np_line[128];
        EnterCriticalSection(&mr_meta_lock);
        char track_copy[128], artist_copy[128], station_copy[128];
        strncpy(track_copy, mr_meta.track, sizeof(track_copy));
        strncpy(artist_copy, mr_meta.artist, sizeof(artist_copy));
        strncpy(station_copy, mr_meta.station_name, sizeof(station_copy));
        track_copy[sizeof(track_copy) - 1] = 0;
        artist_copy[sizeof(artist_copy) - 1] = 0;
        station_copy[sizeof(station_copy) - 1] = 0;
        LeaveCriticalSection(&mr_meta_lock);

        ptext((int)x0 + pad, cy, "Now Playing:", dmr, dmg, dmb, vp_w, vp_h, cw, ch);
        cy += row_h;

        /* Station or track name */
        const char *display = station_copy[0] ? station_copy : track_copy;
        if (!display[0]) display = "(nothing)";
        int max_chars = ((int)pw - pad * 2) / cw;
        _snprintf(np_line, sizeof(np_line), "%.*s", max_chars, display);
        np_line[sizeof(np_line) - 1] = 0;
        ptext((int)x0 + pad + cw, cy, np_line, txr, txg, txb, vp_w, vp_h, cw, ch);
        cy += row_h;

        /* Artist - Title (if available) */
        if (artist_copy[0]) {
            _snprintf(np_line, sizeof(np_line), "%.*s", max_chars, artist_copy);
            np_line[sizeof(np_line) - 1] = 0;
            ptext((int)x0 + pad + cw, cy, np_line, dmr, dmg, dmb, vp_w, vp_h, cw, ch);
        }
        /* BPM display (right side) */
        EnterCriticalSection(&mr_beat_lock);
        float bpm = mr_beat_snap.bpm;
        LeaveCriticalSection(&mr_beat_lock);
        if (bpm > 1.0f) {
            char bpm_buf[16];
            _snprintf(bpm_buf, sizeof(bpm_buf), "%d BPM", (int)(bpm + 0.5f));
            int bpm_x = (int)x1 - pad - (int)strlen(bpm_buf) * cw;
            ptext(bpm_x, cy, bpm_buf, acr, acg, acb, vp_w, vp_h, cw, ch);
        }
        cy += row_h + 2;
    }

    /* ---- Transport Buttons (uniform size) ---- */
    {
        /* Center the button row */
        int total_w = MR_NUM_BTNS * MR_BTN_W + (MR_NUM_BTNS - 1) * MR_BTN_GAP;
        int bx = (int)x0 + ((int)pw - total_w) / 2;
        int by = cy;

        const char *glyphs[MR_NUM_BTNS];
        float btn_r[MR_NUM_BTNS], btn_g[MR_NUM_BTNS], btn_b[MR_NUM_BTNS];

        /* Prev */
        glyphs[0] = "\x11\x11";  btn_r[0] = txr; btn_g[0] = txg; btn_b[0] = txb;
        /* Play/Pause */
        if (mr_transport == MR_STATE_PLAYING) {
            glyphs[1] = "\xB3\xB3";
            btn_r[1] = 0.3f; btn_g[1] = 1.0f; btn_b[1] = 0.3f;
        } else {
            glyphs[1] = "\x10";
            btn_r[1] = acr; btn_g[1] = acg; btn_b[1] = acb;
        }
        /* Stop */
        glyphs[2] = "\xFE";
        if (mr_transport == MR_STATE_PLAYING || mr_transport == MR_STATE_PAUSED) {
            btn_r[2] = 1.0f; btn_g[2] = 0.4f; btn_b[2] = 0.4f;
        } else {
            btn_r[2] = dmr; btn_g[2] = dmg; btn_b[2] = dmb;
        }
        /* Next */
        glyphs[3] = "\x10\x10"; btn_r[3] = txr; btn_g[3] = txg; btn_b[3] = txb;
        /* Shuffle/Repeat combined into one button area */
        glyphs[4] = mr_shuffle ? "Shf" : "shf";
        btn_r[4] = mr_shuffle ? acr : dmr;
        btn_g[4] = mr_shuffle ? acg : dmg;
        btn_b[4] = mr_shuffle ? acb : dmb;

        for (int i = 0; i < MR_NUM_BTNS; i++) {
            float bx0f = (float)bx, bx1f = (float)(bx + MR_BTN_W);
            float by0f = (float)by, by1f = (float)(by + MR_BTN_H);

            /* Button background — raised 3D look */
            psolid(bx0f, by0f, bx1f, by1f,
                   bgr + 0.12f, bgg + 0.12f, bgb + 0.12f, 0.95f, vp_w, vp_h);
            /* Top highlight (bright) */
            psolid(bx0f, by0f, bx1f, by0f + 2.0f,
                   1.0f, 1.0f, 1.0f, 0.12f, vp_w, vp_h);
            /* Left highlight */
            psolid(bx0f, by0f, bx0f + 1.0f, by1f,
                   1.0f, 1.0f, 1.0f, 0.08f, vp_w, vp_h);
            /* Bottom shadow (dark) */
            psolid(bx0f, by1f - 2.0f, bx1f, by1f,
                   0.0f, 0.0f, 0.0f, 0.20f, vp_w, vp_h);
            /* Right shadow */
            psolid(bx1f - 1.0f, by0f, bx1f, by1f,
                   0.0f, 0.0f, 0.0f, 0.15f, vp_w, vp_h);
            /* Inner glow for play button when playing */
            if (i == 1 && mr_transport == MR_STATE_PLAYING) {
                psolid(bx0f + 2.0f, by0f + 2.0f, bx1f - 2.0f, by1f - 2.0f,
                       0.2f, 0.8f, 0.2f, 0.10f, vp_w, vp_h);
            }
            /* Inner glow for stop button when active */
            if (i == 2 && (mr_transport == MR_STATE_PLAYING || mr_transport == MR_STATE_PAUSED)) {
                psolid(bx0f + 2.0f, by0f + 2.0f, bx1f - 2.0f, by1f - 2.0f,
                       0.8f, 0.2f, 0.2f, 0.08f, vp_w, vp_h);
            }

            /* Center glyph with shadow */
            int glyph_w = (int)strlen(glyphs[i]) * cw;
            int gx = bx + (MR_BTN_W - glyph_w) / 2;
            int gy = by + (MR_BTN_H - ch) / 2;
            ptext(gx + 1, gy + 1, glyphs[i], 0.0f, 0.0f, 0.0f, vp_w, vp_h, cw, ch);
            ptext(gx, gy, glyphs[i], btn_r[i], btn_g[i], btn_b[i], vp_w, vp_h, cw, ch);

            bx += MR_BTN_W + MR_BTN_GAP;
        }

        /* Repeat toggle (to the right of buttons) */
        bx += MR_BTN_GAP;
        {
            const char *rep_txt = mr_repeat == 1 ? "R1" : (mr_repeat == 2 ? "RA" : "R-");
            float rr = mr_repeat ? acr : dmr;
            float rg = mr_repeat ? acg : dmg;
            float rb = mr_repeat ? acb : dmb;
            ptext(bx, by + (MR_BTN_H - ch) / 2, rep_txt, rr, rg, rb, vp_w, vp_h, cw, ch);
        }

        cy += MR_BTN_H + 4;
    }

    /* ---- Volume Slider ---- */
    {
        int vx0 = (int)x0 + pad;
        int vx1 = (int)x1 - pad;
        int vy = cy + 4;
        int slider_h = 8;

        /* Volume label (left) */
        ptext(vx0, vy - ch - 2, "VOL", dmr, dmg, dmb, vp_w, vp_h, cw, ch);
        /* Volume % label (right) */
        char vol_buf[8];
        _snprintf(vol_buf, sizeof(vol_buf), "%d%%", (int)(mr_volume * 100.0f + 0.5f));
        ptext(vx1 - (int)strlen(vol_buf) * cw, vy - ch - 2, vol_buf,
              txr, txg, txb, vp_w, vp_h, cw, ch);

        /* Track background — inset groove */
        psolid((float)vx0, (float)vy, (float)vx1, (float)(vy + slider_h),
               0.0f, 0.0f, 0.0f, 0.5f, vp_w, vp_h);
        psolid((float)vx0, (float)vy, (float)vx1, (float)(vy + 1),
               0.0f, 0.0f, 0.0f, 0.3f, vp_w, vp_h);
        /* Filled portion with gradient */
        float fill_x = (float)vx0 + (float)(vx1 - vx0) * mr_volume;
        psolid((float)vx0 + 1.0f, (float)(vy + 1), fill_x, (float)(vy + slider_h - 1),
               acr, acg, acb, 0.85f, vp_w, vp_h);
        /* Gloss on filled portion */
        psolid((float)vx0 + 1.0f, (float)(vy + 1), fill_x, (float)(vy + slider_h / 2),
               1.0f, 1.0f, 1.0f, 0.12f, vp_w, vp_h);
        /* Knob — larger, raised look */
        float knob_w = 5.0f;
        psolid(fill_x - knob_w, (float)(vy - 3), fill_x + knob_w, (float)(vy + slider_h + 3),
               bgr + 0.25f, bgg + 0.25f, bgb + 0.25f, 0.98f, vp_w, vp_h);
        /* Knob highlight */
        psolid(fill_x - knob_w, (float)(vy - 3), fill_x + knob_w, (float)(vy - 1),
               1.0f, 1.0f, 1.0f, 0.15f, vp_w, vp_h);
        /* Knob center line */
        psolid(fill_x - 0.5f, (float)(vy), fill_x + 0.5f, (float)(vy + slider_h),
               txr, txg, txb, 0.3f, vp_w, vp_h);

        cy += slider_h + 6;
    }

    /* ---- Codec Indicator (glowing badge) ---- */
    {
        const char *codec_str = "";
        float cr = dmr, cg = dmg, cb = dmb;
        float glow_a = 0.0f;
        switch (mr_codec) {
            case MR_CODEC_MP3:    codec_str = "MP3";  cr = 0.2f; cg = 0.8f; cb = 1.0f; glow_a = 0.15f; break;
            case MR_CODEC_AAC:    codec_str = "AAC";  cr = 1.0f; cg = 0.6f; cb = 0.2f; glow_a = 0.15f; break;
            case MR_CODEC_OPUS:   codec_str = "OPUS"; cr = 0.3f; cg = 1.0f; cb = 0.4f; glow_a = 0.18f; break;
            case MR_CODEC_VORBIS: codec_str = "OGG";  cr = 0.8f; cg = 0.4f; cb = 1.0f; glow_a = 0.15f; break;
            case MR_CODEC_FLAC:   codec_str = "FLAC"; cr = 1.0f; cg = 0.9f; cb = 0.3f; glow_a = 0.15f; break;
            case MR_CODEC_WAV:    codec_str = "WAV";  cr = 0.7f; cg = 0.7f; cb = 0.7f; glow_a = 0.10f; break;
            case MR_CODEC_HLS:    codec_str = "HLS";  cr = 1.0f; cg = 0.3f; cb = 0.7f; glow_a = 0.15f; break;
            default: break;
        }
        if (mr_transport == MR_STATE_PLAYING && codec_str[0]) {
            int codec_len = (int)strlen(codec_str);
            int badge_w = codec_len * cw + 10;
            int badge_x = (int)x1 - pad - badge_w;
            int badge_y = cy - 2;
            int badge_h = ch + 6;
            /* Glow behind badge */
            psolid((float)(badge_x - 3), (float)(badge_y - 2),
                   (float)(badge_x + badge_w + 3), (float)(badge_y + badge_h + 2),
                   cr, cg, cb, glow_a, vp_w, vp_h);
            /* Badge background */
            psolid((float)badge_x, (float)badge_y,
                   (float)(badge_x + badge_w), (float)(badge_y + badge_h),
                   cr * 0.15f, cg * 0.15f, cb * 0.15f, 0.9f, vp_w, vp_h);
            /* Badge border */
            psolid((float)badge_x, (float)badge_y,
                   (float)(badge_x + badge_w), (float)(badge_y + 1),
                   cr, cg, cb, 0.5f, vp_w, vp_h);
            psolid((float)badge_x, (float)(badge_y + badge_h - 1),
                   (float)(badge_x + badge_w), (float)(badge_y + badge_h),
                   cr, cg, cb, 0.3f, vp_w, vp_h);
            /* Codec text */
            ptext(badge_x + 5, badge_y + 3, codec_str, cr, cg, cb, vp_w, vp_h, cw, ch);
            cy += badge_h + 2;
        }
    }

    /* ---- Visualizer (spectrum bars or oscilloscope waveform) ---- */
    {
        int viz_h = 48;
        float viz_y0 = (float)cy, viz_y1 = (float)(cy + viz_h);

        /* Background — darker inset */
        psolid(x0 + 3.0f, viz_y0, x1 - 3.0f, viz_y1,
               0.0f, 0.0f, 0.0f, 0.5f, vp_w, vp_h);
        /* Top inset shadow */
        psolid(x0 + 3.0f, viz_y0, x1 - 3.0f, viz_y0 + 1.0f,
               0.0f, 0.0f, 0.0f, 0.3f, vp_w, vp_h);

        /* OSC toggle button (top-right corner of viz) */
        {
            const char *mode_txt = mr_viz_mode ? "OSC" : "FFT";
            int btn_w = 3 * cw + 6;
            int btn_x = (int)x1 - 3 - btn_w;
            int btn_y = (int)viz_y0 + 2;
            psolid((float)btn_x, (float)btn_y,
                   (float)(btn_x + btn_w), (float)(btn_y + ch + 2),
                   bgr + 0.08f, bgg + 0.08f, bgb + 0.08f, 0.85f, vp_w, vp_h);
            psolid((float)btn_x, (float)btn_y,
                   (float)(btn_x + btn_w), (float)(btn_y + 1),
                   1.0f, 1.0f, 1.0f, 0.08f, vp_w, vp_h);
            ptext(btn_x + 3, btn_y + 1, mode_txt, acr, acg, acb, vp_w, vp_h, cw, ch);
        }

        EnterCriticalSection(&mr_beat_lock);
        mr_beat_t snap = mr_beat_snap;
        LeaveCriticalSection(&mr_beat_lock);

        float viz_area_w = pw - 6.0f;

        if (mr_viz_mode == 0) {
            /* ---- Spectrum Bars ---- */
            int num_bars = 32;
            float bar_gap = 1.0f;
            float bar_w = (viz_area_w - (float)(num_bars - 1) * bar_gap) / (float)num_bars;

            for (int i = 0; i < num_bars; i++) {
                int bin_start = (i * i * 256) / (num_bars * num_bars);
                int bin_end = ((i + 1) * (i + 1) * 256) / (num_bars * num_bars);
                if (bin_end <= bin_start) bin_end = bin_start + 1;
                if (bin_end > 512) bin_end = 512;

                float mag = 0.0f;
                for (int b = bin_start; b < bin_end; b++)
                    if (snap.spectrum[b] > mag) mag = snap.spectrum[b];

                if (mag > 1.0f) mag = 1.0f;
                float bar_h = mag * (float)viz_h;
                if (bar_h < 1.0f && mag > 0.01f) bar_h = 1.0f;

                float bx0 = x0 + 3.0f + (float)i * (bar_w + bar_gap);
                float bx1 = bx0 + bar_w;

                float grad = (float)i / (float)(num_bars - 1);
                float br = acr * (1.0f - grad * 0.3f) + grad * 0.3f;
                float bg2 = acg * (1.0f - grad * 0.3f) + grad * 0.3f;
                float bb = acb * (1.0f - grad * 0.3f) + grad * 0.3f;

                if (i < 8 && snap.onset_detected) {
                    br += 0.3f; bg2 += 0.1f; bb += 0.1f;
                }

                /* Bar with gloss */
                psolid(bx0, viz_y1 - bar_h, bx1, viz_y1,
                       br, bg2, bb, 0.9f, vp_w, vp_h);
                if (bar_h > 4.0f) {
                    psolid(bx0, viz_y1 - bar_h, bx1, viz_y1 - bar_h + 2.0f,
                           1.0f, 1.0f, 1.0f, 0.12f, vp_w, vp_h);
                }
            }
        } else {
            /* ---- Oscilloscope Waveform ---- */
            float mid_y = (viz_y0 + viz_y1) * 0.5f;
            float amp = (float)viz_h * 0.45f;

            /* Center line (dim) */
            psolid(x0 + 3.0f, mid_y - 0.5f, x1 - 3.0f, mid_y + 0.5f,
                   acr * 0.3f, acg * 0.3f, acb * 0.3f, 0.3f, vp_w, vp_h);

            /* Draw waveform as connected vertical bars between adjacent samples */
            int num_pts = 256;
            float step = viz_area_w / (float)(num_pts - 1);
            for (int i = 0; i < num_pts - 1; i++) {
                float s0 = snap.waveform[i];
                float s1 = snap.waveform[i + 1];
                if (s0 > 1.0f) s0 = 1.0f; if (s0 < -1.0f) s0 = -1.0f;
                if (s1 > 1.0f) s1 = 1.0f; if (s1 < -1.0f) s1 = -1.0f;

                float px0 = x0 + 3.0f + (float)i * step;
                float px1 = px0 + step;
                float py0 = mid_y - s0 * amp;
                float py1 = mid_y - s1 * amp;

                /* Draw a filled quad between the two sample points */
                float top = (py0 < py1) ? py0 : py1;
                float bot = (py0 > py1) ? py0 : py1;
                if (bot - top < 2.0f) { top -= 1.0f; bot += 1.0f; }

                /* Color based on amplitude */
                float intensity = (fabsf(s0) + fabsf(s1)) * 0.5f;
                float wr = acr + intensity * 0.3f;
                float wg = acg + intensity * 0.2f;
                float wb = acb + intensity * 0.1f;
                if (wr > 1.0f) wr = 1.0f;
                if (wg > 1.0f) wg = 1.0f;
                if (wb > 1.0f) wb = 1.0f;

                psolid(px0, top, px1, bot, wr, wg, wb, 0.85f, vp_w, vp_h);
                /* Glow around waveform */
                psolid(px0, top - 2.0f, px1, bot + 2.0f,
                       wr, wg, wb, 0.12f, vp_w, vp_h);
            }
        }

        cy += viz_h + 4;
    }

    /* ---- Divider ---- */
    psolid(x0 + 3.0f, (float)cy, x1 - 3.0f, (float)(cy + 1),
           acr, acg, acb, 0.3f, vp_w, vp_h);
    cy += 3;

    /* ---- Content area (depends on tab) ---- */
    if (mr_tab == MR_TAB_RADIO) {
        /* Search box */
        float sb_x1 = x1 - (float)pad - 4.0f * (float)cw - 4.0f;
        psolid(x0 + (float)pad, (float)cy, sb_x1, (float)(cy + row_h + 4),
               bgr * 0.6f, bgg * 0.6f, bgb * 0.6f, 0.9f, vp_w, vp_h);
        if (mr_search_focus) {
            psolid(x0 + (float)pad, (float)cy, sb_x1, (float)(cy + 1),
                   acr, acg, acb, 0.8f, vp_w, vp_h);
        }
        char search_display[130];
        if (mr_search_len > 0)
            _snprintf(search_display, sizeof(search_display), "%s_", mr_search_buf);
        else
            strcpy(search_display, mr_search_focus ? "_" : "Search...");
        ptext((int)x0 + pad + 4, cy + 3,
              search_display,
              mr_search_len > 0 ? txr : dmr,
              mr_search_len > 0 ? txg : dmg,
              mr_search_len > 0 ? txb : dmb,
              vp_w, vp_h, cw, ch);

        /* [Go] button */
        int go_x = (int)x1 - pad - 3 * cw;
        psolid((float)(go_x - 2), (float)cy, (float)(go_x + 3 * cw + 2), (float)(cy + row_h + 4),
               bgr + 0.10f, bgg + 0.10f, bgb + 0.10f, 0.9f, vp_w, vp_h);
        ptext(go_x, cy + 3, mr_searching ? "..." : "Go",
              acr, acg, acb, vp_w, vp_h, cw, ch);
        cy += row_h + 8;

        /* Sub-tabs: Favorites / Results */
        {
            float sr = (mr_list_mode == MR_LIST_FAVORITES) ? acr : dmr;
            float sg = (mr_list_mode == MR_LIST_FAVORITES) ? acg : dmg;
            float sb = (mr_list_mode == MR_LIST_FAVORITES) ? acb : dmb;
            ptext((int)x0 + pad, cy, "Favorites", sr, sg, sb, vp_w, vp_h, cw, ch);

            sr = (mr_list_mode == MR_LIST_SEARCH) ? acr : dmr;
            sg = (mr_list_mode == MR_LIST_SEARCH) ? acg : dmg;
            sb = (mr_list_mode == MR_LIST_SEARCH) ? acb : dmb;
            ptext((int)x0 + pad + 11 * cw, cy, "Results", sr, sg, sb, vp_w, vp_h, cw, ch);
            cy += row_h + 2;
        }

        /* Station list */
        mr_station_t *list = (mr_list_mode == MR_LIST_FAVORITES) ? mr_favorites : mr_search_results;
        int count = (mr_list_mode == MR_LIST_FAVORITES) ? mr_fav_count : mr_search_count;
        int max_visible = ((int)y1 - cy - 20) / row_h;
        if (max_visible < 1) max_visible = 1;

        if (count == 0) {
            const char *empty_msg = (mr_list_mode == MR_LIST_FAVORITES)
                ? "No favorites yet." : (mr_searching ? "Searching..." : "No results.");
            ptext((int)x0 + pad, cy + 2, empty_msg, dmr, dmg, dmb, vp_w, vp_h, cw, ch);
        } else {
            /* Clamp scroll to valid range */
            int max_scroll = count - max_visible;
            if (max_scroll < 0) max_scroll = 0;
            if (mr_scroll > max_scroll) mr_scroll = max_scroll;
            if (mr_scroll < 0) mr_scroll = 0;

            int list_top_y = cy;
            float sb_w = 6.0f; /* scrollbar width */
            float list_right = x1 - 3.0f;
            int need_scrollbar = (count > max_visible);

            for (int i = mr_scroll; i < count && i < mr_scroll + max_visible; i++) {
                float row_right = need_scrollbar ? (list_right - sb_w - 2.0f) : list_right;
                if (i == mr_hover_item) {
                    psolid(x0 + 3.0f, (float)cy, row_right, (float)(cy + row_h),
                           acr, acg, acb, 0.08f, vp_w, vp_h);
                }
                if ((i - mr_scroll) & 1) {
                    psolid(x0 + 3.0f, (float)cy, row_right, (float)(cy + row_h),
                           1.0f, 1.0f, 1.0f, 0.015f, vp_w, vp_h);
                }
                int sb_chars = need_scrollbar ? (int)(sb_w / cw + 3) : 0;
                int name_max = ((int)pw - pad * 2 - 4 * cw - sb_chars * cw) / cw;
                char name_buf[128];
                _snprintf(name_buf, sizeof(name_buf), "%.*s", name_max, list[i].name);
                name_buf[sizeof(name_buf) - 1] = 0;
                ptext((int)x0 + pad, cy + 2, name_buf, txr, txg, txb, vp_w, vp_h, cw, ch);

                /* Favorite star */
                int star_x = need_scrollbar ? (int)(row_right - 2 * cw) : (int)x1 - pad - 2 * cw;
                ptext(star_x, cy + 2,
                      list[i].is_favorite ? "\x0F" : "\xF9",
                      list[i].is_favorite ? 1.0f : dmr,
                      list[i].is_favorite ? 0.85f : dmg,
                      list[i].is_favorite ? 0.2f : dmb,
                      vp_w, vp_h, cw, ch);

                cy += row_h;
            }

            /* ---- Scrollbar ---- */
            if (need_scrollbar) {
                float sb_x0 = list_right - sb_w;
                float sb_y0 = (float)list_top_y;
                float sb_y1 = (float)(list_top_y + max_visible * row_h);
                float track_h = sb_y1 - sb_y0;

                /* Track background */
                psolid(sb_x0, sb_y0, list_right, sb_y1,
                       0.0f, 0.0f, 0.0f, 0.2f, vp_w, vp_h);

                /* Thumb */
                float thumb_frac = (float)max_visible / (float)count;
                float thumb_h = track_h * thumb_frac;
                if (thumb_h < 12.0f) thumb_h = 12.0f;
                float thumb_pos = (max_scroll > 0)
                    ? ((float)mr_scroll / (float)max_scroll) * (track_h - thumb_h)
                    : 0.0f;
                float thumb_y0 = sb_y0 + thumb_pos;
                float thumb_y1 = thumb_y0 + thumb_h;

                /* Thumb body */
                psolid(sb_x0 + 1.0f, thumb_y0, list_right - 1.0f, thumb_y1,
                       acr, acg, acb, 0.4f, vp_w, vp_h);
                /* Thumb highlight */
                psolid(sb_x0 + 1.0f, thumb_y0, list_right - 1.0f, thumb_y0 + 1.0f,
                       1.0f, 1.0f, 1.0f, 0.1f, vp_w, vp_h);
            }
        }
    } else {
        /* Player tab — placeholder for Phase 8 */
        ptext((int)x0 + pad, cy + 2, "Playlist (coming soon)", dmr, dmg, dmb, vp_w, vp_h, cw, ch);
        cy += row_h;
    }

    /* ---- Resize grip (bottom-right corner) ---- */
    {
        for (int i = 0; i < 3; i++) {
            float off = (float)(i * 4 + 2);
            psolid(x1 - off - 1.0f, y1 - 2.0f, x1 - off, y1 - off - 1.0f,
                   dmr, dmg, dmb, 0.4f, vp_w, vp_h);
        }
    }

    /* ---- Status line at very bottom ---- */
    {
        const char *status = "Stopped";
        float sr = dmr, sg = dmg, sb = dmb;
        if (mr_transport == MR_STATE_PLAYING) { status = "Playing"; sr = 0.3f; sg = 1.0f; sb = 0.3f; }
        else if (mr_transport == MR_STATE_PAUSED) { status = "Paused"; sr = 1.0f; sg = 0.85f; sb = 0.2f; }
        else if (mr_transport == MR_STATE_CONNECTING) { status = "Connecting..."; sr = acr; sg = acg; sb = acb; }
        else if (mr_transport == MR_STATE_ERROR) { status = "Error"; sr = 1.0f; sg = 0.3f; sb = 0.3f; }
        /* Status bar background */
        psolid(x0 + 2.0f, y1 - (float)ch - 8.0f, x1 - 2.0f, y1 - 2.0f,
               bgr * 0.5f, bgg * 0.5f, bgb * 0.5f, 0.6f, vp_w, vp_h);
        /* Status dot (pulsing green when playing) */
        if (mr_transport == MR_STATE_PLAYING) {
            psolid((float)((int)x0 + pad), y1 - (float)ch - 4.0f,
                   (float)((int)x0 + pad + cw / 2), y1 - 4.0f,
                   0.3f, 1.0f, 0.3f, 0.9f, vp_w, vp_h);
            ptext((int)x0 + pad + cw, (int)y1 - ch - 4, status, sr, sg, sb, vp_w, vp_h, cw, ch);
        } else {
            ptext((int)x0 + pad, (int)y1 - ch - 4, status, sr, sg, sb, vp_w, vp_h, cw, ch);
        }
    }
}

/* ---- Layout Y offsets (shared between draw and mouse) ---- */
/* These must match mr_draw layout exactly */
static void mr_get_layout(int *out_transport_y, int *out_vol_y, int *out_search_y,
                          int *out_subtab_y, int *out_list_y)
{
    int ch = VSB_CHAR_H;
    int titlebar_h = ch + 10;
    int tab_h = ch + 8;
    int row_h = ch + 4;
    int slider_h = 8;
    int viz_h = 48;

    int cy = titlebar_h + 2;       /* after title bar */
    cy += tab_h + 2;               /* after tab bar */
    cy += row_h;                   /* "Now Playing:" label */
    cy += row_h;                   /* station/track name */
    cy += row_h + 2;              /* artist/BPM row */
    *out_transport_y = cy;
    cy += MR_BTN_H + 4;           /* after transport */
    *out_vol_y = cy + 4;
    cy += slider_h + 6;           /* after volume slider */
    /* codec badge (variable height, approximate) */
    if (mr_transport == MR_STATE_PLAYING && mr_codec != MR_CODEC_NONE)
        cy += VSB_CHAR_H + 6 + 2;
    cy += viz_h + 4;              /* after spectrum viz */
    cy += 1 + 3;                  /* after divider */
    *out_search_y = cy;
    cy += row_h + 8;              /* after search box */
    *out_subtab_y = cy;
    cy += row_h + 2;              /* after Favorites/Results tabs */
    *out_list_y = cy;
}

/* ---- Mouse Handling ---- */

static int mr_mouse_down(int mx, int my) {
    if (!mr_visible) return 0;
    if (mx < (int)mr_x || mx >= (int)(mr_x + mr_w) ||
        my < (int)mr_y || my >= (int)(mr_y + mr_h)) {
        /* Click outside panel — unfocus search so keyboard goes back to terminal */
        mr_search_focus = 0;
        return 0;
    }

    int cw = VSB_CHAR_W, ch = VSB_CHAR_H;
    int titlebar_h = ch + 10;
    int tab_h = ch + 8;
    int row_h = ch + 4;
    int pad = 8;
    int lx = mx - (int)mr_x;
    int ly = my - (int)mr_y;

    /* Resize grip */
    if (lx >= (int)mr_w - MR_RESIZE_GRIP && ly >= (int)mr_h - MR_RESIZE_GRIP) {
        mr_resizing = 1;
        mr_drag_ox = (float)mx;
        mr_drag_oy = (float)my;
        return 1;
    }

    /* Close button */
    if (ly < titlebar_h && lx >= (int)mr_w - 24) {
        mr_visible = 0;
        return 1;
    }

    /* Title bar drag */
    if (ly < titlebar_h) {
        mr_dragging = 1;
        mr_drag_ox = (float)mx - mr_x;
        mr_drag_oy = (float)my - mr_y;
        return 1;
    }

    /* Tab clicks */
    int tab_top = titlebar_h + 2;
    if (ly >= tab_top && ly < tab_top + tab_h) {
        int radio_tab_w = 7 * cw + 8;
        if (lx < pad + radio_tab_w)
            mr_tab = MR_TAB_RADIO;
        else
            mr_tab = MR_TAB_PLAYER;
        return 1;
    }

    /* Get layout offsets */
    int transport_y, vol_y, search_y, subtab_y, list_y;
    mr_get_layout(&transport_y, &vol_y, &search_y, &subtab_y, &list_y);

    /* Transport buttons */
    if (ly >= transport_y && ly < transport_y + MR_BTN_H) {
        int total_w = MR_NUM_BTNS * MR_BTN_W + (MR_NUM_BTNS - 1) * MR_BTN_GAP;
        int btn_x0 = ((int)mr_w - total_w) / 2;
        int rel = lx - btn_x0;
        if (rel >= 0 && rel < total_w) {
            int btn_idx = rel / (MR_BTN_W + MR_BTN_GAP);
            if (btn_idx >= MR_NUM_BTNS) btn_idx = MR_NUM_BTNS - 1;
            switch (btn_idx) {
            case 0: mr_prev(); break;
            case 1:
                if (mr_transport == MR_STATE_PLAYING) mr_pause();
                else if (mr_transport == MR_STATE_PAUSED) mr_resume();
                break;
            case 2: mr_stop(); break;
            case 3: mr_next(); break;
            case 4: mr_shuffle = !mr_shuffle; break;
            }
        }
        return 1;
    }

    /* Volume slider */
    if (ly >= vol_y - 4 && ly < vol_y + 12) {
        float slider_x0 = (float)pad;
        float slider_x1 = mr_w - (float)pad;
        float pct = ((float)lx - slider_x0) / (slider_x1 - slider_x0);
        if (pct < 0.0f) pct = 0.0f;
        if (pct > 1.0f) pct = 1.0f;
        mr_volume = pct;
        mr_vol_dragging = 1;
        return 1;
    }

    /* OSC/FFT toggle button — sits at top-right of viz area */
    {
        int viz_top = vol_y + 12;  /* after volume slider */
        if (mr_transport == MR_STATE_PLAYING && mr_codec != MR_CODEC_NONE)
            viz_top += ch + 6 + 2; /* skip codec badge */
        int btn_w = 3 * cw + 6;
        int btn_x = (int)mr_w - pad - btn_w - 3;
        int btn_y = viz_top + 2;
        if (lx >= btn_x && lx < btn_x + btn_w &&
            ly >= btn_y && ly < btn_y + ch + 2) {
            mr_viz_mode = !mr_viz_mode;
            return 1;
        }
    }

    /* Radio tab content */
    if (mr_tab == MR_TAB_RADIO) {
        /* Search box click */
        if (ly >= search_y && ly < search_y + row_h + 4) {
            /* Check if [Go] button area */
            int go_x = (int)mr_w - pad - 3 * cw - 2;
            if (lx >= go_x) {
                /* Go button clicked — submit search */
                if (mr_search_len > 0 && !mr_searching) {
                    mr_do_search(mr_search_buf);
                }
            } else {
                mr_search_focus = 1;
            }
            return 1;
        }

        /* Sub-tab clicks (Favorites / Results) */
        if (ly >= subtab_y && ly < subtab_y + row_h) {
            if (lx < pad + 10 * cw) {
                mr_list_mode = MR_LIST_FAVORITES;
                mr_scroll = 0;
            } else {
                mr_list_mode = MR_LIST_SEARCH;
                mr_scroll = 0;
            }
            return 1;
        }

        /* Station list click */
        if (ly >= list_y) {
            int item_idx = (ly - list_y) / row_h + mr_scroll;
            mr_station_t *slist = (mr_list_mode == MR_LIST_FAVORITES) ? mr_favorites : mr_search_results;
            int scount = (mr_list_mode == MR_LIST_FAVORITES) ? mr_fav_count : mr_search_count;
            if (item_idx >= 0 && item_idx < scount) {
                /* Check if star was clicked (right side) */
                int star_x = (int)mr_w - pad - 2 * cw;
                if (lx >= star_x - cw) {
                    mr_toggle_favorite(item_idx);
                } else {
                    /* Click on station name — play it */
                    mr_play_station(item_idx);
                }
            }
            return 1;
        }
    }

    /* Click anywhere else in panel unfocuses search */
    mr_search_focus = 0;
    return 1;
}

static void mr_mouse_move(int mx, int my) {
    if (mr_dragging) {
        mr_x = (float)mx - mr_drag_ox;
        mr_y = (float)my - mr_drag_oy;
    }
    if (mr_resizing) {
        float dx = (float)mx - mr_drag_ox;
        float dy = (float)my - mr_drag_oy;
        mr_w += dx;
        mr_h += dy;
        if (mr_w < MR_MIN_W) mr_w = MR_MIN_W;
        if (mr_h < MR_MIN_H) mr_h = MR_MIN_H;
        mr_drag_ox = (float)mx;
        mr_drag_oy = (float)my;
    }
    if (mr_vol_dragging) {
        float slider_x0 = mr_x + 8.0f;
        float slider_x1 = mr_x + mr_w - 8.0f;
        float pct = ((float)mx - slider_x0) / (slider_x1 - slider_x0);
        if (pct < 0.0f) pct = 0.0f;
        if (pct > 1.0f) pct = 1.0f;
        mr_volume = pct;
    }
    /* Update hover item for station list */
    if (mr_visible && !mr_dragging && !mr_resizing && !mr_vol_dragging) {
        int lx = mx - (int)mr_x;
        int ly = my - (int)mr_y;
        int transport_y, vol_y, search_y, subtab_y, list_y;
        mr_get_layout(&transport_y, &vol_y, &search_y, &subtab_y, &list_y);
        int row_h = VSB_CHAR_H + 4;
        if (ly >= list_y && mr_tab == MR_TAB_RADIO) {
            int count = (mr_list_mode == MR_LIST_FAVORITES) ? mr_fav_count : mr_search_count;
            int idx = (ly - list_y) / row_h + mr_scroll;
            mr_hover_item = (idx >= 0 && idx < count) ? idx : -1;
        } else {
            mr_hover_item = -1;
        }
    }
}

static void mr_mouse_up(void) {
    mr_dragging = 0;
    mr_resizing = 0;
    mr_vol_dragging = 0;
}

/* ---- Scroll Wheel ---- */
static int mr_scroll_wheel(int mx, int my, int delta) {
    if (!mr_visible) return 0;
    if (mx < (int)mr_x || mx >= (int)(mr_x + mr_w) ||
        my < (int)mr_y || my >= (int)(mr_y + mr_h))
        return 0;
    if (mr_tab != MR_TAB_RADIO) return 0;

    int count = (mr_list_mode == MR_LIST_FAVORITES) ? mr_fav_count : mr_search_count;
    int row_h = VSB_CHAR_H + 4;
    int transport_y, vol_y, search_y, subtab_y, list_y;
    mr_get_layout(&transport_y, &vol_y, &search_y, &subtab_y, &list_y);
    int max_visible = ((int)mr_h - list_y - 20) / row_h;
    if (max_visible < 1) max_visible = 1;
    int max_scroll = count - max_visible;
    if (max_scroll < 0) max_scroll = 0;

    mr_scroll += (delta > 0) ? -2 : 2;
    if (mr_scroll < 0) mr_scroll = 0;
    if (mr_scroll > max_scroll) mr_scroll = max_scroll;
    return 1;
}

/* ---- Keyboard (when search focused) ---- */

static int mr_key_char(int ch_code) {
    if (!mr_visible || !mr_search_focus) return 0;
    if (ch_code == '\r' || ch_code == '\n') {
        /* Submit search */
        if (mr_search_len > 0 && !mr_searching)
            mr_do_search(mr_search_buf);
        mr_search_focus = 0;
        return 1;
    }
    if (ch_code == '\b') {
        if (mr_search_len > 0) {
            mr_search_len--;
            mr_search_buf[mr_search_len] = 0;
        }
        return 1;
    }
    if (ch_code == 27) { /* ESC */
        mr_search_focus = 0;
        return 1;
    }
    if (ch_code >= 32 && ch_code < 127 && mr_search_len < (int)sizeof(mr_search_buf) - 1) {
        mr_search_buf[mr_search_len++] = (char)ch_code;
        mr_search_buf[mr_search_len] = 0;
        return 1;
    }
    return 0;
}
