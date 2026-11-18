#pragma once
#include <stdint.h>

// Finds the handwritten words on a photo of a backup (any paper: plain,
// ruled, a printed card; one or more columns; numbered or not) and cuts each
// into a strip for word_reader. Experimental.
//
//   1. paper brightness per 16x16 block -> ink = clearly darker than its paper
//   2. tilt: the angle (within +-15 degrees) at which ink rows line up best
//   3. rows: where ink lies along that angle; ruled lines are too thin to count
//   4. words: within a row, ink separated by a gap wider than the row height
//   5. order: columns left to right (where words sit across rows), then rows
//      top to bottom
//   6. gaps: where a column skips a row (a word too faint to find), a guessed
//      word in its place, so the words after it keep their numbers
// Each word is cut along the tilt into a WR_H x WR_W strip, its row filling
// about two thirds of the height, the way the strips were made for training.
//
// No perspective correction yet: a photo taken at a steep angle won't line up.
// Every buffer is wiped and freed before page_find returns; Page holds only
// geometry.

#define PAGE_MAX_WORDS 160

struct PageWord {
    float u0, u1, v0, v1;   // the word's box along the tilt (u across, v down)
    int column, row;        // reading order
    bool guessed;           // no ink found here, but the rows around it leave a gap for a word
};

struct Page {
    int w, h;
    float cos_a, sin_a;     // tilt: u = x cos + y sin, v = -x sin + y cos
    int n;
    PageWord words[PAGE_MAX_WORDS];
};

// gray: w x h, one byte per pixel. False if no rows of words were found.
bool page_find(const uint8_t *gray, int w, int h, Page *page);
// The strip of page->words[i], WR_H x WR_W (word_reader.h).
void page_strip(const uint8_t *gray, const Page *page, int i, uint8_t *strip);
// Corners of a word's box in image pixels (x0, y0 .. x3, y3), for drawing it.
void page_corners(const Page *page, int i, float xy[8]);
