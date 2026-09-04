/*
 * Draws the current message as text on the 1.54" panel, word-wrapped and
 * centred on both axes. The glyph loop is adapted from qr_code_wallet's
 * draw_text() (components/qrcode_gen/qrcode_render.c) -- same column-major
 * 5x7 font (font5x7.h, copied here), generalized to an arbitrary string.
 *
 * Two passes: the first breaks the text into lines (on '\n', on spaces
 * when the next word would overflow, and mid-word when a single word is
 * wider than the panel); the second centres the block vertically and each
 * line horizontally, then stamps the glyphs.
 */
#include <stdbool.h>
#include <stddef.h>
#include "esp_log.h"
#include "epd.h"
#include "font5x7.h"
#include "text_display.h"

static const char *TAG = "text_display";

/* Set via text_display_set_loop_indicator(): when true, every render also
 * stamps the corner "infinite loop" mark. */
static bool s_loop_indicator = false;

#define SCALE        3
#define BOLD_DX      1                                  /* faux-bold: each set pixel is smeared 1px right */
#define SIDE_MARGIN  6                                  /* min L/R padding used for wrapping + clamping */
#define TOP_MARGIN   2                                  /* min top padding when the block is too tall */
#define GLYPH_W      (FONT5X7_GLYPH_W * SCALE + BOLD_DX)/* 16 px */
#define GLYPH_H      (FONT5X7_GLYPH_H * SCALE)          /* 24 px */
#define ADVANCE_X    ((FONT5X7_GLYPH_W + 1) * SCALE)    /* 18 px (1 col spacing) */
#define LINE_H       (FONT5X7_GLYPH_H * SCALE + 5)      /* 29 px */

/* 200 px / LINE_H is ~6 drawable lines; a message that wraps to more than
 * this has its overflow clipped anyway, so extra lines are just dropped. */
#define MAX_LINES    12

typedef struct {
    const char *start; /* first char of the line within the source string */
    int count;         /* number of source chars on the line (glyphs + kept spaces) */
} line_t;

static void draw_glyph(int ox, int oy, unsigned char ch) {
    if (ch < FONT5X7_FIRST_CHAR || ch > FONT5X7_LAST_CHAR) {
        ch = '?';
    }
    const uint8_t *glyph = &FONT5X7[(ch - FONT5X7_FIRST_CHAR) * FONT5X7_GLYPH_W];
    for (int col = 0; col < FONT5X7_GLYPH_W; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < FONT5X7_GLYPH_H; row++) {
            if (!(bits & (1u << row))) {
                continue;
            }
            for (int sy = 0; sy < SCALE; sy++) {
                for (int sx = 0; sx < SCALE + BOLD_DX; sx++) { /* +BOLD_DX = faux-bold smear */
                    epd_draw_pixel((uint16_t)(ox + col * SCALE + sx),
                                   (uint16_t)(oy + row * SCALE + sy), EPD_COLOR_BLACK);
                }
            }
        }
    }
}

/* Hand-plotted lemniscate ("infinity"), 11x5 px, drawn at 2x in the
 * top-left corner. The 5x7 font has no such glyph. Each entry is one row,
 * bit x (LSB = leftmost) set where there should be ink. */
static void draw_loop_mark(void) {
    static const uint16_t rows[5] = {
        (1u << 1) | (1u << 2) | (1u << 8) | (1u << 9),   /* .##....##. */
        (1u << 0) | (1u << 3) | (1u << 7) | (1u << 10),  /* #..#..#..# */
        (1u << 0) | (1u << 4) | (1u << 6) | (1u << 10),  /* #...##...# */
        (1u << 0) | (1u << 3) | (1u << 7) | (1u << 10),  /* #..#..#..# */
        (1u << 1) | (1u << 2) | (1u << 8) | (1u << 9),   /* .##....##. */
    };
    const int ox = 4, oy = 4, scale = 2;
    for (int ry = 0; ry < 5; ry++) {
        for (int rx = 0; rx < 11; rx++) {
            if (!(rows[ry] & (1u << rx))) {
                continue;
            }
            for (int sy = 0; sy < scale; sy++) {
                for (int sx = 0; sx < scale; sx++) {
                    epd_draw_pixel((uint16_t)(ox + rx * scale + sx),
                                   (uint16_t)(oy + ry * scale + sy), EPD_COLOR_BLACK);
                }
            }
        }
    }
}

void text_display_set_loop_indicator(bool on) {
    s_loop_indicator = on;
}

static void push_line(line_t *lines, int *nlines, const char *start, const char *end) {
    if (*nlines < MAX_LINES) {
        lines[*nlines].start = start;
        lines[*nlines].count = (int)(end - start);
        (*nlines)++;
    }
}

/* Pixel width of a line once trailing spaces are ignored: N columns take
 * (N-1) advances plus one glyph box. */
static int line_width_px(const line_t *ln) {
    int n = ln->count;
    while (n > 0 && ln->start[n - 1] == ' ') {
        n--;
    }
    return n > 0 ? (n - 1) * ADVANCE_X + GLYPH_W : 0;
}

void text_display_show_message(const char *text) {
    epd_clear();
    if (text == NULL) {
        text = "";
    }

    /* ---- Pass 1: wrap into contiguous line slices ---- */
    line_t lines[MAX_LINES];
    int nlines = 0;
    const int wrap_limit = EPD_WIDTH - SIDE_MARGIN;   /* right edge for wrap decisions */
    const int last_x = EPD_WIDTH - SIDE_MARGIN - GLYPH_W; /* last glyph origin that still fits */

    const char *ls = text; /* current line start */
    int col = 0;           /* columns placed on the current line so far */

    for (const char *p = text;; p++) {
        char c = *p;

        if (c == '\0') {
            push_line(lines, &nlines, ls, p);
            break;
        }

        if (c == '\n') {
            push_line(lines, &nlines, ls, p);
            ls = p + 1;
            col = 0;
            continue;
        }

        if (c == ' ') {
            if (col == 0) { /* swallow leading spaces */
                ls = p + 1;
                continue;
            }
            int word_px = 0;
            for (const char *q = p + 1; *q && *q != ' ' && *q != '\n'; q++) {
                word_px += ADVANCE_X;
            }
            int x = SIDE_MARGIN + col * ADVANCE_X;
            if (x + ADVANCE_X + word_px > wrap_limit) { /* next word won't fit -- break here */
                push_line(lines, &nlines, ls, p);
                ls = p + 1;
                col = 0;
            } else {
                col++; /* keep the space */
            }
            continue;
        }

        if (SIDE_MARGIN + col * ADVANCE_X > last_x) { /* hard wrap inside an over-long word */
            push_line(lines, &nlines, ls, p);
            ls = p;
            col = 0;
        }
        col++;
    }

    /* ---- Pass 2: centre the block, then each line, and stamp glyphs ---- */
    int block_h = nlines > 0 ? (nlines - 1) * LINE_H + GLYPH_H : 0;
    int y = (EPD_HEIGHT - block_h) / 2;
    if (y < TOP_MARGIN) {
        y = TOP_MARGIN;
    }

    int drawn = 0;
    int clipped = 0;

    for (int i = 0; i < nlines; i++) {
        int w = line_width_px(&lines[i]);
        int x = (EPD_WIDTH - w) / 2;
        if (x < 0) {
            x = 0;
        }

        if (y + GLYPH_H > EPD_HEIGHT) { /* ran off the bottom */
            clipped += lines[i].count;
            y += LINE_H;
            continue;
        }

        for (int k = 0; k < lines[i].count; k++) {
            char c = lines[i].start[k];
            if (c == ' ') {
                x += ADVANCE_X;
                continue;
            }
            if (x + GLYPH_W > EPD_WIDTH) {
                clipped++;
                continue;
            }
            draw_glyph(x, y, (unsigned char)c);
            x += ADVANCE_X;
            drawn++;
        }
        y += LINE_H;
    }

    if (s_loop_indicator) {
        draw_loop_mark();
    }

    if (clipped) {
        ESP_LOGW(TAG, "message too long for the panel -- %d characters clipped", clipped);
    }
    ESP_LOGI(TAG, "Drew message (%d glyphs, %d lines%s) centred into the framebuffer",
             drawn, nlines, s_loop_indicator ? ", +loop mark" : "");
}
