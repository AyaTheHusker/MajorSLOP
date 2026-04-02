/*
 * gen_font_atlas.c — Render Px437 IBM VGA TTF into a C header texture atlas
 * ===========================================================================
 *
 * Reads a Px437 TTF font and renders all 256 CP437 glyphs into a 16x16 grid
 * texture atlas, outputs as a C header with embedded pixel data.
 *
 * Build: gcc -o gen_font_atlas gen_font_atlas.c $(pkg-config --cflags --libs freetype2) -lm
 * Run:   ./gen_font_atlas <font.ttf> <pixel_height> <output.h>
 *
 * Example: ./gen_font_atlas Px437_IBM_VGA_8x16.ttf 32 cp437_hd.h
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ft2build.h>
#include FT_FREETYPE_H

/* CP437 to Unicode mapping — all 256 characters */
static const unsigned int cp437_to_unicode[256] = {
    /* 0x00-0x0F: special/graphical characters */
    0x0000, 0x263A, 0x263B, 0x2665, 0x2666, 0x2663, 0x2660, 0x2022,
    0x25D8, 0x25CB, 0x25D9, 0x2642, 0x2640, 0x266A, 0x266B, 0x263C,
    /* 0x10-0x1F */
    0x25BA, 0x25C4, 0x2195, 0x203C, 0x00B6, 0x00A7, 0x25AC, 0x21A8,
    0x2191, 0x2193, 0x2192, 0x2190, 0x221F, 0x2194, 0x25B2, 0x25BC,
    /* 0x20-0x7E: standard ASCII */
    0x0020, 0x0021, 0x0022, 0x0023, 0x0024, 0x0025, 0x0026, 0x0027,
    0x0028, 0x0029, 0x002A, 0x002B, 0x002C, 0x002D, 0x002E, 0x002F,
    0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
    0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F,
    0x0040, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047,
    0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,
    0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057,
    0x0058, 0x0059, 0x005A, 0x005B, 0x005C, 0x005D, 0x005E, 0x005F,
    0x0060, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
    0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F,
    0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077,
    0x0078, 0x0079, 0x007A, 0x007B, 0x007C, 0x007D, 0x007E, 0x007F,
    /* 0x80-0x8F */
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,
    0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    /* 0x90-0x9F */
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
    0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
    /* 0xA0-0xAF */
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,
    0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    /* 0xB0-0xBF: box-drawing light */
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
    0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    /* 0xC0-0xCF */
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    /* 0xD0-0xDF */
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B,
    0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    /* 0xE0-0xEF */
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,
    0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    /* 0xF0-0xFF */
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248,
    0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
};

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <font.ttf> <pixel_height> <output.h>\n", argv[0]);
        return 1;
    }

    const char *font_path = argv[1];
    int cell_h = atoi(argv[2]);
    const char *out_path = argv[3];

    /* Cell width is half height for 1:2 aspect ratio */
    int cell_w = cell_h / 2;

    /* Atlas: 16x16 grid of cells */
    int atlas_w = cell_w * 16;
    int atlas_h = cell_h * 16;

    printf("Font: %s\n", font_path);
    printf("Cell: %d x %d, Atlas: %d x %d\n", cell_w, cell_h, atlas_w, atlas_h);

    /* Init FreeType */
    FT_Library ft;
    if (FT_Init_FreeType(&ft)) {
        fprintf(stderr, "FreeType init failed\n");
        return 1;
    }

    FT_Face face;
    if (FT_New_Face(ft, font_path, 0, &face)) {
        fprintf(stderr, "Failed to load font: %s\n", font_path);
        return 1;
    }

    /* Set pixel size */
    FT_Set_Pixel_Sizes(face, cell_w, cell_h);

    printf("Font family: %s, style: %s\n", face->family_name, face->style_name);
    printf("Glyphs in font: %ld\n", face->num_glyphs);

    /* Allocate atlas (single channel alpha) */
    unsigned char *atlas = calloc(atlas_w * atlas_h, 1);
    if (!atlas) { fprintf(stderr, "OOM\n"); return 1; }

    int missing = 0;

    for (int cp437 = 0; cp437 < 256; cp437++) {
        unsigned int unicode = cp437_to_unicode[cp437];
        FT_UInt glyph_idx = FT_Get_Char_Index(face, unicode);

        if (glyph_idx == 0 && unicode != 0) {
            /* Try space as fallback for NULL char */
            if (cp437 == 0) continue;
            missing++;
            continue;
        }

        if (FT_Load_Glyph(face, glyph_idx, FT_LOAD_RENDER | FT_LOAD_TARGET_MONO)) {
            missing++;
            continue;
        }

        FT_Bitmap *bmp = &face->glyph->bitmap;

        /* Grid position */
        int grid_x = (cp437 % 16) * cell_w;
        int grid_y = (cp437 / 16) * cell_h;

        /* Center glyph in cell */
        int x_off = face->glyph->bitmap_left;
        int y_off = cell_h - face->glyph->bitmap_top - (cell_h - (face->size->metrics.ascender >> 6));

        /* Clamp offsets */
        if (x_off < 0) x_off = 0;
        if (y_off < 0) y_off = 0;

        for (unsigned int row = 0; row < bmp->rows; row++) {
            for (unsigned int col = 0; col < bmp->width; col++) {
                int px, py;
                px = grid_x + x_off + (int)col;
                py = grid_y + y_off + (int)row;

                if (px < 0 || px >= atlas_w || py < 0 || py >= atlas_h) continue;

                unsigned char val;
                if (bmp->pixel_mode == FT_PIXEL_MODE_MONO) {
                    /* 1-bit: each byte has 8 pixels */
                    val = (bmp->buffer[row * bmp->pitch + col / 8] & (0x80 >> (col % 8))) ? 255 : 0;
                } else {
                    /* 8-bit grayscale */
                    val = bmp->buffer[row * bmp->pitch + col];
                }

                atlas[py * atlas_w + px] = val;
            }
        }
    }

    printf("Missing glyphs: %d / 256\n", missing);

    /* Write as C header */
    FILE *fp = fopen(out_path, "w");
    if (!fp) { perror(out_path); return 1; }

    fprintf(fp, "/* Auto-generated CP437 font atlas from %s at %dx%d */\n", font_path, cell_w, cell_h);
    fprintf(fp, "/* Atlas: %d x %d, 16x16 grid, single-channel alpha */\n\n", atlas_w, atlas_h);
    fprintf(fp, "#ifndef CP437_HD_H\n#define CP437_HD_H\n\n");
    fprintf(fp, "#define CP437_HD_CELL_W %d\n", cell_w);
    fprintf(fp, "#define CP437_HD_CELL_H %d\n", cell_h);
    fprintf(fp, "#define CP437_HD_ATLAS_W %d\n", atlas_w);
    fprintf(fp, "#define CP437_HD_ATLAS_H %d\n\n", atlas_h);
    fprintf(fp, "static const unsigned char cp437_hd_atlas[%d] = {\n", atlas_w * atlas_h);

    for (int i = 0; i < atlas_w * atlas_h; i++) {
        if (i % 16 == 0) fprintf(fp, "    ");
        fprintf(fp, "0x%02X,", atlas[i]);
        if (i % 16 == 15) fprintf(fp, "\n");
    }

    fprintf(fp, "};\n\n#endif /* CP437_HD_H */\n");
    fclose(fp);

    printf("Wrote: %s (%d bytes atlas data)\n", out_path, atlas_w * atlas_h);

    /* Also write a raw .pgm for preview */
    char pgm_path[256];
    snprintf(pgm_path, sizeof(pgm_path), "%s.pgm", out_path);
    FILE *pgm = fopen(pgm_path, "wb");
    if (pgm) {
        fprintf(pgm, "P5\n%d %d\n255\n", atlas_w, atlas_h);
        fwrite(atlas, 1, atlas_w * atlas_h, pgm);
        fclose(pgm);
        printf("Preview: %s\n", pgm_path);
    }

    free(atlas);
    FT_Done_Face(face);
    FT_Done_FreeType(ft);
    return 0;
}
