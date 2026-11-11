#define _XOPEN_SOURCE_EXTENDED 1
#include "tui/v2/mainpanel.h"
#include "tui/v2/palette.h"

#include "ui/surface/terminal_canvas.h"
#include "ui/surface/terminal_surface.h"
#include "format/terminal_style.h"
#include "annotation/annotation.h"
#include "exception/throw.h"

#include <locale.h>
#include <ncurses.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define SEMICOLON_V2_MAX_WIDTH 240u
#define SEMICOLON_V2_MAX_HEIGHT 80u
#define SEMICOLON_V2_MAX_CELLS (SEMICOLON_V2_MAX_WIDTH * SEMICOLON_V2_MAX_HEIGHT)

static u32 gBackCells[SEMICOLON_V2_MAX_CELLS];
static u16 gBackStyles[SEMICOLON_V2_MAX_CELLS];
static u32 gBackForeground[SEMICOLON_V2_MAX_CELLS];
static u32 gBackBackground[SEMICOLON_V2_MAX_CELLS];
static u32 gFrontCells[SEMICOLON_V2_MAX_CELLS];
static u16 gFrontStyles[SEMICOLON_V2_MAX_CELLS];
static u32 gFrontForeground[SEMICOLON_V2_MAX_CELLS];
static u32 gFrontBackground[SEMICOLON_V2_MAX_CELLS];

static int colorIndex(u32 color) {
    static const int levels[] = { 0, 95, 135, 175, 215, 255 };
    int red = TERMINAL_COLOR_RED(color);
    int green = TERMINAL_COLOR_GREEN(color);
    int blue = TERMINAL_COLOR_BLUE(color);
    int redLevel = red < 48 ? 0 : red < 115 ? 1 : red < 155 ? 2
                 : red < 195 ? 3 : red < 235 ? 4 : 5;
    int greenLevel = green < 48 ? 0 : green < 115 ? 1 : green < 155 ? 2
                   : green < 195 ? 3 : green < 235 ? 4 : 5;
    int blueLevel = blue < 48 ? 0 : blue < 115 ? 1 : blue < 155 ? 2
                  : blue < 195 ? 3 : blue < 235 ? 4 : 5;
    int cube = 16 + 36 * redLevel + 6 * greenLevel + blueLevel;
    int gray = (red + green + blue) / 3;
    int grayIndex = gray < 8 ? 16 : gray > 238 ? 231 : 232 + (gray - 8) / 10;
    int cubeDistance = (red - levels[redLevel]) * (red - levels[redLevel])
                     + (green - levels[greenLevel]) * (green - levels[greenLevel])
                     + (blue - levels[blueLevel]) * (blue - levels[blueLevel]);
    int grayValue = grayIndex <= 231 ? 0 : 8 + (grayIndex - 232) * 10;
    int grayDistance = (red - grayValue) * (red - grayValue)
                     + (green - grayValue) * (green - grayValue)
                     + (blue - grayValue) * (blue - grayValue);

    return grayDistance < cubeDistance ? grayIndex : cube;
}

static short colorPair(u32 foreground, u32 background) {
    static u32 keysFg[64];
    static u32 keysBg[64];
    static short values[64];
    static bytes count;
    u32 fg = TERMINAL_COLOR_IS_SET(foreground) ? foreground : 0u;
    u32 bg = TERMINAL_COLOR_IS_SET(background) ? background : 0u;
    for (bytes index = 0u; index < count; ++index)
        if (keysFg[index] == fg && keysBg[index] == bg)
            return values[index];

    if (count >= 64u || count + 1u >= (bytes) COLOR_PAIRS)
        return 0;

    short pair = (short) (count + 1u);
    short fgIndex = (short) (fg == 0u ? -1 : colorIndex(fg));
    short bgIndex = (short) (bg == 0u ? -1 : colorIndex(bg));
    if (init_pair(pair, fgIndex, bgIndex) == ERR)
        return 0;

    keysFg[count] = fg;
    keysBg[count] = bg;
    values[count] = pair;
    ++count;
    return pair;
}

;;DEFINITION
/** Semicolon v2 preview host. It borrows Determinant's panel, anchor, canvas and
 * surface contracts, and only adapts changed cell runs to the local ncurses tty.
 * The screen is a static layout preview: file editing, tree events, process
 * running and clipboard behavior are not connected in this draft. */
;;OVERVIEW
/** ========================================================================
 * MODULE: main-tui-v2.c
 * OWNED STATE: bounded back/front canvas planes for the preview host.
 * HOST: terminal acquisition, ncurses event wait, and cell sink only.
 * COMPOSITION: SemicolonV2MainPanel_create() returns the owned UI tree.
 * FUNCTION REGISTRY: main / presentRun / frameBracket / renderPreview.
 * LIFETIME: create panel, create screen/surface, render; endwin, then free panel.
 * ======================================================================== */

static attr_t terminalAttributes(u16 style) {
    attr_t attributes = A_NORMAL;
    if ((style & TERMINAL_STYLE_BOLD) != 0u || (style & TERMINAL_STYLE_ACCENT) != 0u)
        attributes |= A_BOLD;
    if ((style & TERMINAL_STYLE_DIM) != 0u || (style & TERMINAL_STYLE_MUTED) != 0u)
        attributes |= A_DIM;
    if ((style & TERMINAL_STYLE_UNDERLINE) != 0u)
        attributes |= A_UNDERLINE;
    if ((style & TERMINAL_STYLE_REVERSE) != 0u)
        attributes |= A_REVERSE;
    if ((style & TERMINAL_STYLE_ERROR) != 0u)
        attributes |= A_BOLD | A_STANDOUT;
    return attributes;
}

static bool presentRun(void *context, TerminalRun run) {
    (void) context;
    if (run.cells == nullptr || run.length > SEMICOLON_V2_MAX_WIDTH
        || run.x + run.length > SEMICOLON_V2_MAX_WIDTH || run.y >= SEMICOLON_V2_MAX_HEIGHT)
        return false;

    for (bytes index = 0u; index < run.length; ++index) {
        u32 codepoint = run.cells[index];
        if (codepoint == 0u)
            continue;
        wchar_t glyph[2] = { (wchar_t) codepoint, L'\0' };
        cchar_t cell;
        u16 style = run.styles == nullptr ? TERMINAL_STYLE_NONE : run.styles[index];
        u32 fg = run.fg == nullptr ? TERMINAL_COLOR_DEFAULT : run.fg[index];
        u32 bg = run.bg == nullptr ? TERMINAL_COLOR_DEFAULT : run.bg[index];
        short pair = has_colors() ? colorPair(fg, bg) : 0;
        if (setcchar(&cell, glyph, terminalAttributes(style), pair, nullptr) == ERR)
            return false;
        int written = mvadd_wch((int) run.y, (int) (run.x + index), &cell);
        bool terminalBottomRight = run.y == (bytes) (LINES - 1)
            && run.x + index == (bytes) (COLS - 1);
        if (written == ERR && !terminalBottomRight)
            return false;
    }

    return true;
}

static void frameBracket(void *context, TerminalPresentPhase phase) {
    (void) context;
    if (phase == TERMINAL_PRESENT_END) {
        (void) wnoutrefresh(stdscr);
        (void) doupdate();
    }
}

static bool renderPreview(TerminalPanel *root, TerminalSurface *surface) {
    int width = 0;
    int height = 0;
    getmaxyx(stdscr, height, width);
    if (width < 1 || height < 1 || width > (int) SEMICOLON_V2_MAX_WIDTH
        || height > (int) SEMICOLON_V2_MAX_HEIGHT) {
        THROW("smc2: terminal dimensions must be within 1..%u columns and 1..%u rows",
              SEMICOLON_V2_MAX_WIDTH, SEMICOLON_V2_MAX_HEIGHT);
        return false;
    }

    bytes cellCount = (bytes) width * (bytes) height;
    memset(gFrontCells, 0, cellCount * sizeof gFrontCells[0]);
    memset(gFrontStyles, 0, cellCount * sizeof gFrontStyles[0]);
    memset(gFrontForeground, 0, cellCount * sizeof gFrontForeground[0]);
    memset(gFrontBackground, 0, cellCount * sizeof gFrontBackground[0]);

    TerminalCanvas back = TerminalCanvas(gBackCells, gBackStyles, gBackForeground,
                                         gBackBackground, (bytes) width, (bytes) height);
    TerminalCanvas front = TerminalCanvas(gFrontCells, gFrontStyles, gFrontForeground,
                                          gFrontBackground, (bytes) width, (bytes) height);
    TerminalCanvas_fill(&back, ' ', TerminalPen());
    TerminalPanel_paint(root, &back);

    if (!TerminalSurface_bind(surface, &back, &front, presentRun, nullptr)
        || !TerminalSurface_setFrame(surface, frameBracket, nullptr))
        return false;

    TerminalPresentResult result = TerminalSurface_present(surface);
    return result.ok;
}

static void restoreTerminal(SCREEN *screen) {
    if (screen == nullptr)
        return;

    (void) curs_set(1);
    (void) endwin();
    (void) delscreen(screen);
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--help") == 0) {
        puts("smc2: show the Semicolon v2 layout preview; press q to quit");
        return 0;
    }
    if (argc > 1) {
        THROW("smc2: this preview accepts only --help or no arguments");
        return 1;
    }

    (void) setlocale(LC_CTYPE, "");
    // Unfinished: the v2 panel tree builder (SemicolonV2MainPanel_create) is not
    // written yet, so the call is parked here and the preview reports it instead
    // of failing to compile. Restore the call when the builder lands.
    TerminalPanel *root = nullptr;
    // TerminalPanel *root = SemicolonV2MainPanel_create();
    if (root == nullptr) {
        THROW("smc2: could not create the v2 panel tree");
        return 1;
    }

    SCREEN *screen = newterm(nullptr, stdout, stdin);
    if (screen == nullptr) {
        TerminalPanel_free(root);
        THROW("smc2: could not acquire a terminal screen");
        return 1;
    }

    (void) set_term(screen);
    (void) cbreak();
    (void) noecho();
    (void) keypad(stdscr, true);
    (void) curs_set(0);
    if (has_colors()) {
        (void) start_color();
        (void) use_default_colors();
    }

    TerminalSurface surface = TerminalSurface();
    bool ok = true;
    int key = ERR;
    while (ok && key != 'q' && key != 'Q') {
        ok = renderPreview(root, &surface);
        if (ok)
            key = getch();
        if (key == KEY_RESIZE)
            (void) clearok(stdscr, true);
    }

    restoreTerminal(screen);
    TerminalPanel_free(root);
    if (!ok) {
        THROW("smc2: failed to render the v2 preview");
        return 1;
    }

    return 0;
}
