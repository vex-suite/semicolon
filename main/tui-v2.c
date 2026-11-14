#define _XOPEN_SOURCE_EXTENDED 1

#include "vex.h"
#include "tui/v2/mainpanel.h"
#include "tui/v2/filetree.h"
#include "tui/v2/codecanvas.h"
#include "tui/v2/bottompart.h"
#include "tui/v2/palette.h"
#include "tui/v2/state.h"

#include "ui/surface/terminal_canvas.h"
#include "ui/surface/terminal_surface.h"
#include "events/terminal_input.h"
#include "format/terminal_style.h"
#include "../../re/clipboard/clipboard.h"
#include "system/browser.h"
#include "nio/mem.h"
#include "oop/oop.h"

#include <locale.h>
#include <limits.h>
#include <dirent.h>
#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wchar.h>

;;DEFINITION
/** Semicolon v2 primary application coordinator and terminal host.
 * Complies with the Application Entry & Wire Composition Law and Living Application Preferences.
 * Bridges Determinant cell presentations with standard POSIX terminal IO. */
;;OVERVIEW
/** ========================================================================
 * MODULE: suite/projects/semicolon/src/main/tui-v2.c
 * RESPONSIBILITY: argument parsing, state initialization, top-level wiring,
 *                 ncurses terminal acquisition, event pump and clean reverse teardown.
 * CONVENTIONS: Descriptive cameled identifiers, zero -> operators, Memory_alloc.
 * ======================================================================== */

#define SEMICOLON_V2_MAX_GRID_WIDTH 240u
#define SEMICOLON_V2_MAX_GRID_HEIGHT 80u
#define SEMICOLON_V2_MAX_CELL_COUNT (SEMICOLON_V2_MAX_GRID_WIDTH * SEMICOLON_V2_MAX_GRID_HEIGHT)

static u32 gBackBufferCells[SEMICOLON_V2_MAX_CELL_COUNT];
static u16 gBackBufferStyles[SEMICOLON_V2_MAX_CELL_COUNT];
static u32 gBackBufferForeground[SEMICOLON_V2_MAX_CELL_COUNT];
static u32 gBackBufferBackground[SEMICOLON_V2_MAX_CELL_COUNT];

static u32 gFrontBufferCells[SEMICOLON_V2_MAX_CELL_COUNT];
static u16 gFrontBufferStyles[SEMICOLON_V2_MAX_CELL_COUNT];
static u32 gFrontBufferForeground[SEMICOLON_V2_MAX_CELL_COUNT];
static u32 gFrontBufferBackground[SEMICOLON_V2_MAX_CELL_COUNT];

static int SemicolonV2_colorIndex(u32 colorValue) {
    static const int colorLevels[] = { 0, 95, 135, 175, 215, 255 };
    int redChannel = TERMINAL_COLOR_RED(colorValue);
    int greenChannel = TERMINAL_COLOR_GREEN(colorValue);
    int blueChannel = TERMINAL_COLOR_BLUE(colorValue);

    int redLevel = redChannel < 48 ? 0 : redChannel < 115 ? 1 : redChannel < 155 ? 2
                 : redChannel < 195 ? 3 : redChannel < 235 ? 4 : 5;
    int greenLevel = greenChannel < 48 ? 0 : greenChannel < 115 ? 1 : greenChannel < 155 ? 2
                   : greenChannel < 195 ? 3 : greenChannel < 235 ? 4 : 5;
    int blueLevel = blueChannel < 48 ? 0 : blueChannel < 115 ? 1 : blueChannel < 155 ? 2
                  : blueChannel < 195 ? 3 : blueChannel < 235 ? 4 : 5;

    int cubeIndex = 16 + 36 * redLevel + 6 * greenLevel + blueLevel;
    int grayAverage = (redChannel + greenChannel + blueChannel) / 3;
    int grayIndex = grayAverage < 8 ? 16 : grayAverage > 238 ? 231 : 232 + (grayAverage - 8) / 10;

    int cubeDistance = (redChannel - colorLevels[redLevel]) * (redChannel - colorLevels[redLevel])
                     + (greenChannel - colorLevels[greenLevel]) * (greenChannel - colorLevels[greenLevel])
                     + (blueChannel - colorLevels[blueLevel]) * (blueChannel - colorLevels[blueLevel]);
    int grayValue = grayIndex <= 231 ? 0 : 8 + (grayIndex - 232) * 10;
    int grayDistance = (redChannel - grayValue) * (redChannel - grayValue)
                     + (greenChannel - grayValue) * (greenChannel - grayValue)
                     + (blueChannel - grayValue) * (blueChannel - grayValue);

    return grayDistance < cubeDistance ? grayIndex : cubeIndex;
}

static short SemicolonV2_colorPair(u32 foregroundColor, u32 backgroundColor) {
    static u32 cachedKeysForeground[64];
    static u32 cachedKeysBackground[64];
    static short cachedValuesPair[64];
    static bytes cachedPairCount = 0u;

    u32 effectiveFg = TERMINAL_COLOR_IS_SET(foregroundColor) ? foregroundColor : 0u;
    u32 effectiveBg = TERMINAL_COLOR_IS_SET(backgroundColor) ? backgroundColor : 0u;

    for (bytes cacheIndex = 0u; cacheIndex < cachedPairCount; ++cacheIndex) {
        if (cachedKeysForeground[cacheIndex] == effectiveFg && cachedKeysBackground[cacheIndex] == effectiveBg)
            return cachedValuesPair[cacheIndex];
    }

    if (cachedPairCount >= 64u || cachedPairCount + 1u >= (bytes) COLOR_PAIRS)
        return 0;

    short allocatedPair = (short) (cachedPairCount + 1u);
    short fgIndex = (short) (effectiveFg == 0u ? -1 : SemicolonV2_colorIndex(effectiveFg));
    short bgIndex = (short) (effectiveBg == 0u ? -1 : SemicolonV2_colorIndex(effectiveBg));

    if (init_pair(allocatedPair, fgIndex, bgIndex) == ERR)
        return 0;

    cachedKeysForeground[cachedPairCount] = effectiveFg;
    cachedKeysBackground[cachedPairCount] = effectiveBg;
    cachedValuesPair[cachedPairCount] = allocatedPair;
    ++cachedPairCount;
    return allocatedPair;
}

static attr_t SemicolonV2_terminalAttributes(u16 styleFlags) {
    attr_t resultAttributes = A_NORMAL;
    if ((styleFlags & TERMINAL_STYLE_BOLD) != 0u || (styleFlags & TERMINAL_STYLE_ACCENT) != 0u)
        resultAttributes |= A_BOLD;
    if ((styleFlags & TERMINAL_STYLE_DIM) != 0u || (styleFlags & TERMINAL_STYLE_MUTED) != 0u)
        resultAttributes |= A_DIM;
    if ((styleFlags & TERMINAL_STYLE_UNDERLINE) != 0u)
        resultAttributes |= A_UNDERLINE;
    if ((styleFlags & TERMINAL_STYLE_REVERSE) != 0u)
        resultAttributes |= A_REVERSE;
    if ((styleFlags & TERMINAL_STYLE_ERROR) != 0u)
        resultAttributes |= A_BOLD | A_STANDOUT;
    return resultAttributes;
}

static bool SemicolonV2_presentRun(any callbackContext, TerminalRun terminalRun) {
    (fn) callbackContext;
    if (terminalRun.cells == nullptr || terminalRun.length > SEMICOLON_V2_MAX_GRID_WIDTH
        || terminalRun.x + terminalRun.length > SEMICOLON_V2_MAX_GRID_WIDTH
        || terminalRun.y >= SEMICOLON_V2_MAX_GRID_HEIGHT)
        return false;

    for (bytes cellIndex = 0u; cellIndex < terminalRun.length; ++cellIndex) {
        u32 codepointValue = terminalRun.cells[cellIndex];
        if (codepointValue == 0u)
            continue;
        wchar_t glyphWideString[2] = { (wchar_t) codepointValue, L'\0' };
        cchar_t compositeCell;
        u16 styleBits = terminalRun.styles == nullptr ? TERMINAL_STYLE_NONE : terminalRun.styles[cellIndex];
        u32 foregroundRgb = terminalRun.fg == nullptr ? TERMINAL_COLOR_DEFAULT : terminalRun.fg[cellIndex];
        u32 backgroundRgb = terminalRun.bg == nullptr ? TERMINAL_COLOR_DEFAULT : terminalRun.bg[cellIndex];

        // Alpha blend background over device backdrop so translucent selection highlights stay soft and dark
        u32 compositedBg = TERMINAL_COLOR_IS_SET(backgroundRgb)
            ? TerminalColor_over(backgroundRgb, SEMICOLON_V2_DEVICE_BG)
            : TERMINAL_COLOR_DEFAULT;
        u32 backdropColor = (compositedBg != TERMINAL_COLOR_DEFAULT) ? compositedBg : SEMICOLON_V2_DEVICE_BG;
        u32 compositedFg = TERMINAL_COLOR_IS_SET(foregroundRgb)
            ? TerminalColor_over(foregroundRgb, backdropColor)
            : TERMINAL_COLOR_DEFAULT;

        short activePair = has_colors() ? SemicolonV2_colorPair(compositedFg, compositedBg) : 0;

        if (setcchar(&compositeCell, glyphWideString, SemicolonV2_terminalAttributes(styleBits),
                     activePair, nullptr) == ERR)
            return false;

        int writeStatus = mvadd_wch((int) terminalRun.y, (int) (terminalRun.x + cellIndex), &compositeCell);
        bool isTerminalBottomRight = terminalRun.y == (bytes) (LINES - 1)
                                   && terminalRun.x + cellIndex == (bytes) (COLS - 1);
        if (writeStatus == ERR && !isTerminalBottomRight)
            return false;
    }

    return true;
}

static fn SemicolonV2_frameBracket(any callbackContext, TerminalPresentPhase presentPhase) {
    (fn) callbackContext;
    if (presentPhase == TERMINAL_PRESENT_END) {
        (fn) wnoutrefresh(stdscr);
        (fn) doupdate();
    }
}

#define TUI_MOUSE_SGR (KEY_MAX + 1)
#define TUI_MOUSE_X10 (KEY_MAX + 2)
#define TUI_MOUSE_SUFFIX_MS 25

static u64 SemicolonV2_monotonicNowNs() {
    struct timespec currentTime;
    if (clock_gettime(CLOCK_MONOTONIC, &currentTime) != 0)
        return 0u;
    return (u64) currentTime.tv_sec * 1000000000ULL + (u64) currentTime.tv_nsec;
}

static bool SemicolonV2_setMouseReporting(bool isEnabled) {
    return fputs(isEnabled ? "\033[?1003h\033[?1006h" : "\033[?1003l\033[?1006l", stdout) >= 0
        && fflush(stdout) == 0;
}

static bool SemicolonV2_readMouseSuffix(TerminalInput *terminalInput, bool isSgrMode) {
    static const char *sgrPrefix = "\x1b[<";
    static const char *x10Prefix = "\x1b[M";
    const char *prefixString = isSgrMode ? sgrPrefix : x10Prefix;
    for (bytes byteIndex = 0u; prefixString[byteIndex] != '\0'; ++byteIndex)
        (fn) TerminalInput_feed(terminalInput, (unsigned char) prefixString[byteIndex]);

    timeout(TUI_MOUSE_SUFFIX_MS);
    bool isReady = false;
    for (bytes readIndex = 0u; readIndex < 24u; ++readIndex) {
        int readByte = getch();
        if (readByte == ERR)
            break;
        if (TerminalInput_feed(terminalInput, readByte)) {
            isReady = true;
            break;
        }
        if (!isSgrMode && readIndex == 2u)
            break;
    }
    timeout(50);
    return isReady;
}

static bool SemicolonV2_renderInteractiveFrame(SemicolonState *semicolonState, TerminalPanel *rootPanel,
                                               TerminalSurface *terminalSurface) {
    int terminalColumns = 0;
    int terminalLines = 0;
    getmaxyx(stdscr, terminalLines, terminalColumns);
    if (terminalColumns < 1 || terminalLines < 1
        || terminalColumns > (int) SEMICOLON_V2_MAX_GRID_WIDTH
        || terminalLines > (int) SEMICOLON_V2_MAX_GRID_HEIGHT)
        return false;

    SemicolonState_setTerminalDimensions(semicolonState, (bytes) terminalColumns, (bytes) terminalLines);

    bytes visibleRailRows = SemicolonRail_visibleRows((bytes) terminalLines);
    bytes visibleCodeRows = SemicolonCode_visibleRows((bytes) terminalLines);
    bytes currentScrollTop = SemicolonState_getScrollTop(semicolonState);
    bytes currentScrollLeft = SemicolonState_getScrollLeft(semicolonState);

    SemicolonRail_syncVisibleRows(semicolonState, visibleRailRows);
    SemicolonCode_syncVisibleRows(semicolonState, currentScrollTop, currentScrollLeft, visibleCodeRows);
    SemicolonFooter_updateClock(semicolonState);

    bytes totalCells = (bytes) terminalColumns * (bytes) terminalLines;
    memset(gFrontBufferCells, 0, totalCells * sizeof gFrontBufferCells[0]);
    memset(gFrontBufferStyles, 0, totalCells * sizeof gFrontBufferStyles[0]);
    memset(gFrontBufferForeground, 0, totalCells * sizeof gFrontBufferForeground[0]);
    memset(gFrontBufferBackground, 0, totalCells * sizeof gFrontBufferBackground[0]);

    TerminalCanvas backCanvas = TerminalCanvas(gBackBufferCells, gBackBufferStyles,
                                               gBackBufferForeground, gBackBufferBackground,
                                               (bytes) terminalColumns, (bytes) terminalLines);
    TerminalCanvas frontCanvas = TerminalCanvas(gFrontBufferCells, gFrontBufferStyles,
                                                gFrontBufferForeground, gFrontBufferBackground,
                                                (bytes) terminalColumns, (bytes) terminalLines);
    TerminalCanvas_fill(&backCanvas, ' ', TerminalPen());

    TerminalPanel_paint(rootPanel, &backCanvas);
    SemicolonCode_paintSyntax(semicolonState, &backCanvas, currentScrollTop, currentScrollLeft);
    if (SemicolonState_isMarkdownFile(semicolonState) && SemicolonState_isMarkdownPreview(semicolonState))
        SemicolonCode_paintMarkdownPreview(semicolonState, &backCanvas);
    SemicolonRail_paintColors(semicolonState, &backCanvas);
    bool isSplitterActive = SemicolonState_isSplitterHovered(semicolonState) || SemicolonState_isSplitterDragging(semicolonState);
    SemicolonMainPanel_paintSplitter(semicolonState, &backCanvas, isSplitterActive);
    SemicolonCode_paintCursor(semicolonState, &backCanvas, currentScrollTop, currentScrollLeft);

    if (!TerminalSurface_bind(terminalSurface, &backCanvas, &frontCanvas, SemicolonV2_presentRun, nullptr)
        || !TerminalSurface_setFrame(terminalSurface, SemicolonV2_frameBracket, nullptr))
        return false;

    TerminalPresentResult presentResult = TerminalSurface_present(terminalSurface);
    return presentResult.ok;
}

static bool SemicolonV2_firstFileIn(const char *rootDirectory, char *destinationBuffer, bytes destinationCapacity) {
    DIR *dirHandle = opendir(rootDirectory);
    if (dirHandle == nullptr)
        return false;
    char bestFileName[256];
    bestFileName[0] = '\0';
    struct dirent *dirItem = nullptr;
    while ((dirItem = readdir(dirHandle)) != nullptr) {
        if ((*dirItem).d_name[0] == '.' || strlen((*dirItem).d_name) >= sizeof bestFileName)
            continue;
        char childPath[SEMICOLON_V2_MAX_PATH];
        (fn) snprintf(childPath, sizeof childPath, "%s/%s", rootDirectory, (*dirItem).d_name);
        struct stat fileStat;
        if (stat(childPath, &fileStat) != 0 || !S_ISREG(fileStat.st_mode))
            continue;
        if (bestFileName[0] == '\0' || strcasecmp((*dirItem).d_name, bestFileName) < 0)
            (fn) snprintf(bestFileName, sizeof bestFileName, "%s", (*dirItem).d_name);
    }
    (fn) closedir(dirHandle);
    if (bestFileName[0] == '\0')
        return false;
    (fn) snprintf(destinationBuffer, destinationCapacity, "%s/%s", rootDirectory, bestFileName);
    return true;
}

static bool SemicolonV2_parseHexColor(const char *hexString, u32 *outColor) {
    if (hexString == nullptr || *hexString == '\0')
        return false;
    if (*hexString == '#')
        ++hexString;
    bytes stringLength = strlen(hexString);
    if (stringLength != 6u && stringLength != 8u)
        return false;

    unsigned long parsedValue = strtoul(hexString, nullptr, 16);
    if (stringLength == 6u) {
        u8 redByte = (u8) ((parsedValue >> 16u) & 0xFFu);
        u8 greenByte = (u8) ((parsedValue >> 8u) & 0xFFu);
        u8 blueByte = (u8) (parsedValue & 0xFFu);
        *outColor = TERMINAL_RGB(redByte, greenByte, blueByte);
    } else {
        u8 redByte = (u8) ((parsedValue >> 24u) & 0xFFu);
        u8 greenByte = (u8) ((parsedValue >> 16u) & 0xFFu);
        u8 blueByte = (u8) ((parsedValue >> 8u) & 0xFFu);
        u8 alphaByte = (u8) (parsedValue & 0xFFu);
        *outColor = TERMINAL_RGBA(redByte, greenByte, blueByte, alphaByte);
    }
    return true;
}

int main(int argumentCount, char **argumentVector) {
    (fn) setlocale(LC_CTYPE, "");

    bool isDumpModeRequested = false;
    bool isFileExplicitlyGiven = false;
    bytes requestedDumpWidth = 103u;
    bytes requestedDumpHeight = 23u;
    char targetRootDirectory[SEMICOLON_V2_MAX_PATH];
    char targetFilePath[SEMICOLON_V2_MAX_PATH];
    targetRootDirectory[0] = '\0';
    targetFilePath[0] = '\0';

    bool isNerdIconsConfigured = true;
    u32 parsedAccentColor = SEMICOLON_V2_ACCENT;

    for (int argumentIndex = 1; argumentIndex < argumentCount; ++argumentIndex) {
        const char *currentArgument = argumentVector[argumentIndex];
        if (strcmp(currentArgument, "--help") == 0) {
            puts("smc2 [--dump [WxH]] [--file PATH] [--root PATH] [--icons MODE] [--accent HEX] [--help]");
            puts("  --dump [WxH]  print one frame as plain text to stdout and exit");
            puts("  --file PATH   open file in editor pane");
            puts("  --root PATH   set project browse root (default: current directory)");
            puts("  --icons MODE  nerd (default) or plain");
            puts("  --accent HEX  theme accent color in #RRGGBB format");
            puts("  --help        display this help text and exit");
            return 0;
        }
        if (strcmp(currentArgument, "--dump") == 0) {
            isDumpModeRequested = true;
            if (argumentIndex + 1 < argumentCount && strchr(argumentVector[argumentIndex + 1], 'x') != nullptr) {
                char *widthEndPointer = nullptr;
                long parsedWidth = strtol(argumentVector[++argumentIndex], &widthEndPointer, 10);
                char *heightEndPointer = nullptr;
                long parsedHeight = widthEndPointer != nullptr ? strtol(widthEndPointer + 1, &heightEndPointer, 10) : 0;
                if (parsedWidth > 0 && parsedHeight > 0) {
                    requestedDumpWidth = (bytes) parsedWidth;
                    requestedDumpHeight = (bytes) parsedHeight;
                }
            }
            continue;
        }
        if (strcmp(currentArgument, "--root") == 0 && argumentIndex + 1 < argumentCount) {
            (fn) snprintf(targetRootDirectory, sizeof targetRootDirectory, "%s", argumentVector[++argumentIndex]);
            continue;
        }
        if (strcmp(currentArgument, "--file") == 0 && argumentIndex + 1 < argumentCount) {
            (fn) snprintf(targetFilePath, sizeof targetFilePath, "%s", argumentVector[++argumentIndex]);
            isFileExplicitlyGiven = true;
            continue;
        }
        if (strcmp(currentArgument, "--icons") == 0 && argumentIndex + 1 < argumentCount) {
            const char *modeArgument = argumentVector[++argumentIndex];
            isNerdIconsConfigured = (strcmp(modeArgument, "nerd") == 0);
            continue;
        }
        if (strcmp(currentArgument, "--accent") == 0 && argumentIndex + 1 < argumentCount) {
            (fn) SemicolonV2_parseHexColor(argumentVector[++argumentIndex], &parsedAccentColor);
            continue;
        }
    }

    if (targetRootDirectory[0] == '\0') {
        if (getcwd(targetRootDirectory, sizeof targetRootDirectory) == nullptr) {
            THROW("smc2: cannot determine current directory; specify --root");
            return 1;
        }
    }

    char resolvedRootPath[SEMICOLON_V2_MAX_PATH];
    if (realpath(targetRootDirectory, resolvedRootPath) != nullptr) {
        (fn) snprintf(targetRootDirectory, sizeof targetRootDirectory, "%s", resolvedRootPath);
    }

    SemicolonState *semicolonState = SemicolonState_create();
    if (semicolonState == nullptr) {
        THROW("smc2: state allocation failure");
        return 1;
    }

    SemicolonState_setDumpMode(semicolonState, isDumpModeRequested);
    SemicolonState_setNerdIcons(semicolonState, isNerdIconsConfigured);
    SemicolonState_setThemeAccentColor(semicolonState, parsedAccentColor);

    // Restore persistent session info if available for this root
    SemicolonCache_load(semicolonState, targetRootDirectory);

    if (!isFileExplicitlyGiven) {
        const char *cachedFilePath = SemicolonState_getOpenFilePath(semicolonState);
        if (cachedFilePath[0] != '\0' && access(cachedFilePath, R_OK) == 0) {
            (fn) snprintf(targetFilePath, sizeof targetFilePath, "%s", cachedFilePath);
        } else if (!SemicolonV2_firstFileIn(targetRootDirectory, targetFilePath, sizeof targetFilePath)) {
            THROW("smc2: no file found to open in %s; specify --file", targetRootDirectory);
            SemicolonState_destroy(semicolonState);
            return 1;
        }
    }

    char locationPathBuffer[SEMICOLON_V2_MAX_PATH];
    const char *userHomeDirectory = getenv("HOME");
    bytes homeLength = userHomeDirectory != nullptr ? strlen(userHomeDirectory) : 0u;
    if (homeLength > 0u && strncmp(targetRootDirectory, userHomeDirectory, homeLength) == 0
        && (targetRootDirectory[homeLength] == '/' || targetRootDirectory[homeLength] == '\0')) {
        (fn) snprintf(locationPathBuffer, sizeof locationPathBuffer,
                        "~%s", targetRootDirectory + homeLength);
    } else {
        (fn) snprintf(locationPathBuffer, sizeof locationPathBuffer,
                        "%s", targetRootDirectory);
    }
    SemicolonState_setRailLocationPath(semicolonState, locationPathBuffer);

    (fn) SemicolonRail_scanDirectory(semicolonState, targetRootDirectory);
    (fn) SemicolonCode_loadFile(semicolonState, targetFilePath);
    (fn) SemicolonRail_revealPath(semicolonState, targetFilePath);

    // Phase 1: Build Objects (Single Object Factory Pattern Law)
    TerminalPanel *mainPanel = createMainPanel(semicolonState);
    if (mainPanel == nullptr) {
        SemicolonState_destroy(semicolonState);
        THROW("smc2: mainPanel factory creation failure");
        return 1;
    }

    int exitStatusCode = 0;

    // Phase 3: Run Host Loop
    if (isDumpModeRequested) {
        SemicolonState_setTerminalDimensions(semicolonState, requestedDumpWidth, requestedDumpHeight);

        bytes visibleRailRows = SemicolonRail_visibleRows(requestedDumpHeight);
        bytes visibleCodeRows = SemicolonCode_visibleRows(requestedDumpHeight);

        SemicolonRail_syncVisibleRows(semicolonState, visibleRailRows);
        SemicolonCode_syncVisibleRows(semicolonState, 0u, 0u, visibleCodeRows);
        SemicolonFooter_updateBreadcrumb(semicolonState, SemicolonState_getOpenFilePath(semicolonState));
        SemicolonFooter_updateClock(semicolonState);

        bytes planeCellCount = requestedDumpWidth * requestedDumpHeight;
        u32 *dumpCells = (u32*) Memory_alloc(TYPE_BYTE_ARRAY, planeCellCount * sizeof(u32));
        u16 *dumpStyles = (u16*) Memory_alloc(TYPE_BYTE_ARRAY, planeCellCount * sizeof(u16));
        u32 *dumpFg = (u32*) Memory_alloc(TYPE_BYTE_ARRAY, planeCellCount * sizeof(u32));
        u32 *dumpBg = (u32*) Memory_alloc(TYPE_BYTE_ARRAY, planeCellCount * sizeof(u32));

        if (dumpCells != nullptr && dumpStyles != nullptr && dumpFg != nullptr && dumpBg != nullptr) {
            TerminalCanvas dumpCanvas = TerminalCanvas(dumpCells, dumpStyles, dumpFg, dumpBg,
                                                       requestedDumpWidth, requestedDumpHeight);
            TerminalCanvas_fill(&dumpCanvas, ' ', TerminalPen());

            TerminalPanel_paint(mainPanel, &dumpCanvas);
            SemicolonCode_paintSyntax(semicolonState, &dumpCanvas, 0u, 0u);
            if (SemicolonState_isMarkdownFile(semicolonState) && SemicolonState_isMarkdownPreview(semicolonState))
                SemicolonCode_paintMarkdownPreview(semicolonState, &dumpCanvas);
            SemicolonRail_paintColors(semicolonState, &dumpCanvas);

            for (bytes rowIndex = 0u; rowIndex < requestedDumpHeight; ++rowIndex) {
                TerminalCanvasTextRow canvasRow = TerminalCanvas_copyRow(&dumpCanvas, rowIndex);
                fwrite(canvasRow.data, 1u, canvasRow.bytes, stdout);
                fputc('\n', stdout);
            }

            Memory_free(dumpCells);
            Memory_free(dumpStyles);
            Memory_free(dumpFg);
            Memory_free(dumpBg);
        } else {
            exitStatusCode = 1;
        }
    } else {
        SCREEN *screenInstance = newterm(nullptr, stdout, stdin);
        if (screenInstance == nullptr) {
            TerminalPanel_free(mainPanel);
            SemicolonState_destroy(semicolonState);
            THROW("smc2: could not acquire terminal screen");
            return 1;
        }

        (fn) set_term(screenInstance);
        (fn) cbreak();
        (fn) noecho();
        (fn) keypad(stdscr, true);
        (fn) curs_set(0);
        if (has_colors()) {
            (fn) start_color();
            (fn) use_default_colors();
        }

        (fn) define_key("\033[<", TUI_MOUSE_SGR);
        (fn) define_key("\033[M", TUI_MOUSE_X10);
        (fn) define_key("\033[A", KEY_UP);
        (fn) define_key("\033[B", KEY_DOWN);
        (fn) define_key("\033[C", KEY_RIGHT);
        (fn) define_key("\033[D", KEY_LEFT);
        (fn) define_key("\033OA", KEY_UP);
        (fn) define_key("\033OB", KEY_DOWN);
        (fn) define_key("\033OC", KEY_RIGHT);
        (fn) define_key("\033OD", KEY_LEFT);
        (fn) define_key("\033[H", KEY_HOME);
        (fn) define_key("\033[F", KEY_END);
        (fn) define_key("\033[1~", KEY_HOME);
        (fn) define_key("\033[4~", KEY_END);
        (fn) define_key("\033[5~", KEY_PPAGE);
        (fn) define_key("\033[6~", KEY_NPAGE);

        (fn) mousemask(0, nullptr);
        set_escdelay(25);
        timeout(50);
        (fn) SemicolonV2_setMouseReporting(true);

        TerminalSurface terminalSurface = TerminalSurface();
        TerminalInput terminalInput = TerminalInput_0();
        bool isLoopRunning = true;
        int inputKeyCode = ERR;
        bool isGrabbingDivider = false;
        bool isGrabMoved = false;
        int grabStartX = 0;
        bytes grabStartWidth = 0u;
        bool haveLastDividerClick = false;
        u64 lastDividerClickNs = 0u;
        bytes markdownCopiedBlock = SIZE_MAX;
        u64 markdownCopiedUntilNs = 0u;

        while (isLoopRunning) {
            if (markdownCopiedUntilNs != 0u && SemicolonV2_monotonicNowNs() >= markdownCopiedUntilNs) {
                TerminalMarkdownView *markdownView = SemicolonState_getMarkdownView(semicolonState);
                if (markdownView != nullptr)
                    (fn) TerminalMarkdownView_setCopied(markdownView, markdownCopiedBlock, false);
                markdownCopiedUntilNs = 0u;
                markdownCopiedBlock = SIZE_MAX;
            }

            isLoopRunning = SemicolonV2_renderInteractiveFrame(semicolonState, mainPanel, &terminalSurface);
            if (!isLoopRunning)
                break;

            inputKeyCode = getch();
            if (inputKeyCode == ERR)
                continue;
            if (inputKeyCode == 'q' || inputKeyCode == 'Q' || inputKeyCode == 3)
                break;

            if (inputKeyCode == KEY_RESIZE) {
                (fn) clearok(stdscr, true);
                continue;
            }

            bool havePointer = false;
            if (inputKeyCode == TUI_MOUSE_SGR)
                havePointer = SemicolonV2_readMouseSuffix(&terminalInput, true);
            else if (inputKeyCode == TUI_MOUSE_X10)
                havePointer = SemicolonV2_readMouseSuffix(&terminalInput, false);

            if (havePointer) {
                TerminalPointerEvent pointerEvent = TerminalInput_getEvent(&terminalInput);
                bytes seamColumn = SemicolonState_isRailVisible(semicolonState) ? SemicolonState_getRailWidth(semicolonState) : 0u;
                bytes middleRowY = SemicolonState_getTerminalHeight(semicolonState) / 2u;

                switch (pointerEvent.kind) {
                    case TERMINAL_POINTER_MOVE: {
                        if (!SemicolonState_isRailVisible(semicolonState)) {
                            SemicolonState_setSplitterHovered(semicolonState, pointerEvent.x == 0u
                                && pointerEvent.y >= middleRowY - 1u && pointerEvent.y <= middleRowY + 1u);
                        } else {
                            SemicolonState_setSplitterHovered(semicolonState, pointerEvent.x == seamColumn
                                && pointerEvent.y > 0u && pointerEvent.y + 1u < SemicolonState_getTerminalHeight(semicolonState));
                        }
                        if (SemicolonState_isMarkdownFile(semicolonState) && SemicolonState_isMarkdownPreview(semicolonState)) {
                            TerminalMarkdownView *markdownView = SemicolonState_getMarkdownView(semicolonState);
                            bytes terminalWidth = SemicolonState_getTerminalWidth(semicolonState);
                            bytes terminalHeight = SemicolonState_getTerminalHeight(semicolonState);
                            bytes contentX = (SemicolonState_isRailVisible(semicolonState) ? SemicolonState_getRailWidth(semicolonState) : 0u) + 3u;
                            bytes contentY = 4u;
                            if (markdownView != nullptr && contentX < terminalWidth && contentY + 1u < terminalHeight
                                && pointerEvent.x >= contentX && pointerEvent.x < terminalWidth
                                && pointerEvent.y >= contentY && pointerEvent.y < terminalHeight - 1u) {
                                bytes documentRow = TerminalMarkdownView_getScroll(markdownView) + (pointerEvent.y - contentY);
                                bytes documentCol = TerminalMarkdownView_rowScrollX(markdownView, documentRow) + (pointerEvent.x - contentX);
                                (fn) TerminalMarkdownView_setHover(markdownView, documentRow, documentCol);
                            } else if (markdownView != nullptr) {
                                TerminalMarkdownView_clearHover(markdownView);
                            }
                        }
                        break;
                    }
                    case TERMINAL_POINTER_WHEEL_UP:
                    case TERMINAL_POINTER_WHEEL_DOWN: {
                        bool isDown = (pointerEvent.kind == TERMINAL_POINTER_WHEEL_DOWN);
                        if (SemicolonState_isRailVisible(semicolonState) && pointerEvent.x < seamColumn) {
                            TerminalTreeView *treeView = SemicolonState_getTreeView(semicolonState);
                            if (treeView != nullptr)
                                (fn) TerminalTreeView_scrollBy(treeView, isDown ? 1 : -1);
                        } else if (pointerEvent.shift || pointerEvent.meta || pointerEvent.ctrl) {
                            SemicolonState_scrollHorizontal(semicolonState, isDown ? 4 : -4);
                        } else {
                            SemicolonState_scrollVertical(semicolonState, isDown ? 3 : -3);
                        }
                        break;
                    }
                    case TERMINAL_POINTER_WHEEL_LEFT:
                    case TERMINAL_POINTER_WHEEL_RIGHT: {
                        bool isRight = (pointerEvent.kind == TERMINAL_POINTER_WHEEL_RIGHT);
                        SemicolonState_scrollHorizontal(semicolonState, isRight ? 4 : -4);
                        break;
                    }
                    case TERMINAL_POINTER_PRESS: {
                        // 1. Collapsed grip press
                        if (!SemicolonState_isRailVisible(semicolonState) && pointerEvent.x == 0u
                            && pointerEvent.y >= middleRowY - 1u && pointerEvent.y <= middleRowY + 1u) {
                            SemicolonState_setRailVisible(semicolonState, true);
                            isGrabbingDivider = true;
                            grabStartX = (int) pointerEvent.x;
                            grabStartWidth = SemicolonState_getRailWidth(semicolonState);
                            isGrabMoved = false;
                            SemicolonState_setSplitterDragging(semicolonState, true);
                            break;
                        }

                        // 2. Divider seam press
                        if (SemicolonState_isRailVisible(semicolonState)
                            && (pointerEvent.x == seamColumn || pointerEvent.x + 1u == seamColumn || pointerEvent.x == seamColumn + 1u)
                            && pointerEvent.y > 0u && pointerEvent.y + 1u < SemicolonState_getTerminalHeight(semicolonState)) {
                            isGrabbingDivider = true;
                            grabStartX = (int) pointerEvent.x;
                            grabStartWidth = SemicolonState_getRailWidth(semicolonState);
                            isGrabMoved = false;
                            SemicolonState_setSplitterDragging(semicolonState, true);
                            break;
                        }

                        // 3. Header tabs
                        if (pointerEvent.y == 0u) {
                            if (SemicolonState_isRailVisible(semicolonState) && pointerEvent.x < seamColumn) {
                                TerminalTreeView *treeView = SemicolonState_getTreeView(semicolonState);
                                if (treeView != nullptr)
                                    (fn) TerminalTreeView_setScrollRow(treeView, 0u);
                            } else {
                                (fn) SemicolonHeader_handleTabClick(semicolonState, pointerEvent.x, pointerEvent.button);
                            }
                            break;
                        }

                        // 4. Rail folder tree rows
                        if (SemicolonState_isRailVisible(semicolonState) && pointerEvent.x < seamColumn
                            && pointerEvent.y >= 1u && pointerEvent.y <= SemicolonRail_visibleRows(SemicolonState_getTerminalHeight(semicolonState))) {
                            SemicolonRail_handlePointerClick(semicolonState, pointerEvent.y);
                            break;
                        }

                        // 5. Action / mode chips in editor
                        if (pointerEvent.x >= seamColumn && (pointerEvent.y == 1u || pointerEvent.y == 2u)) {
                            if (SemicolonCode_handleActionClick(semicolonState, pointerEvent.x, pointerEvent.y))
                                break;
                        }

                        // 6. Breadcrumb in footer row
                        if (pointerEvent.y + 1u == SemicolonState_getTerminalHeight(semicolonState)) {
                            if (pointerEvent.x >= 12u && pointerEvent.x < 12u + 44u) {
                                (fn) SemicolonRail_revealPath(semicolonState, SemicolonState_getOpenFilePath(semicolonState));
                            }
                            break;
                        }

                        // 7. Markdown preview area (copy code button & clickable links)
                        if (SemicolonState_isMarkdownFile(semicolonState) && SemicolonState_isMarkdownPreview(semicolonState)) {
                            TerminalMarkdownView *markdownView = SemicolonState_getMarkdownView(semicolonState);
                            bytes terminalWidth = SemicolonState_getTerminalWidth(semicolonState);
                            bytes terminalHeight = SemicolonState_getTerminalHeight(semicolonState);
                            bytes contentX = (SemicolonState_isRailVisible(semicolonState) ? SemicolonState_getRailWidth(semicolonState) : 0u) + 3u;
                            bytes contentY = 4u;
                            if (markdownView != nullptr && contentX < terminalWidth && contentY + 1u < terminalHeight) {
                                bytes contentWidth = terminalWidth - contentX;
                                bytes contentHeight = terminalHeight - contentY - 1u;
                                if (pointerEvent.x >= contentX && pointerEvent.x < contentX + contentWidth
                                    && pointerEvent.y >= contentY && pointerEvent.y < contentY + contentHeight) {
                                    bytes documentRow = TerminalMarkdownView_getScroll(markdownView) + (pointerEvent.y - contentY);
                                    bytes documentCol = TerminalMarkdownView_rowScrollX(markdownView, documentRow) + (pointerEvent.x - contentX);
                                    TerminalMarkdownAction action = TerminalMarkdownView_actionAt(markdownView, documentRow, documentCol);
                                    if (action.kind == TERMINAL_MARKDOWN_ACTION_COPY_CODE) {
                                        char *copyBuffer = (char*) Memory_alloc(TYPE_BYTE_ARRAY, 65536u);
                                        if (copyBuffer != nullptr) {
                                            bytes bytesWritten = 0u;
                                            if (TerminalMarkdownView_copyBlock(markdownView, action.block, copyBuffer, 65536u, &bytesWritten) && bytesWritten > 0u) {
                                                (fn) Clipboard_setText(copyBuffer);
                                                (fn) TerminalMarkdownView_setCopied(markdownView, action.block, true);
                                                markdownCopiedBlock = action.block;
                                                markdownCopiedUntilNs = SemicolonV2_monotonicNowNs() + 2000000000ULL;
                                            }
                                            Memory_free(copyBuffer);
                                        }
                                        break;
                                    } else if (action.kind == TERMINAL_MARKDOWN_ACTION_LINK && action.target != nullptr && action.targetLength > 0u) {
                                        char targetUrl[1024];
                                        bytes copyLength = action.targetLength < sizeof (targetUrl) - 1u ? action.targetLength : sizeof (targetUrl) - 1u;
                                        memcpy(targetUrl, action.target, copyLength);
                                        targetUrl[copyLength] = '\0';
                                        bool isWebUrl = (strncmp(targetUrl, "http://", 7) == 0
                                                         || strncmp(targetUrl, "https://", 8) == 0
                                                         || strncmp(targetUrl, "mailto:", 7) == 0
                                                         || strncmp(targetUrl, "ftp://", 6) == 0);
                                        if (isWebUrl) {
                                            (fn) Browser_open(targetUrl);
                                        } else {
                                            const char *currentOpenFilePath = SemicolonState_getOpenFilePath(semicolonState);
                                            char baseDirectory[PATH_MAX];
                                            (fn) snprintf(baseDirectory, sizeof baseDirectory, "%s", currentOpenFilePath != nullptr ? currentOpenFilePath : ".");
                                            char *lastSlashPointer = strrchr(baseDirectory, '/');
                                            if (lastSlashPointer != nullptr)
                                                *lastSlashPointer = '\0';
                                            else
                                                (fn) snprintf(baseDirectory, sizeof baseDirectory, ".");
                                            char combinedPath[PATH_MAX * 2];
                                            if (targetUrl[0] == '/') {
                                                (fn) snprintf(combinedPath, sizeof combinedPath, "%s", targetUrl);
                                            } else {
                                                (fn) snprintf(combinedPath, sizeof combinedPath, "%s/%s", baseDirectory, targetUrl);
                                            }
                                            char resolvedPath[PATH_MAX];
                                            if (realpath(combinedPath, resolvedPath) != nullptr) {
                                                (fn) SemicolonState_openFile(semicolonState, resolvedPath);
                                            } else {
                                                (fn) SemicolonState_openFile(semicolonState, combinedPath);
                                            }
                                        }
                                        break;
                                    }
                                }
                            }
                        }
                        break;
                    }
                    case TERMINAL_POINTER_DRAG: {
                        if (isGrabbingDivider) {
                            int moveDelta = (int) pointerEvent.x - grabStartX;
                            if (moveDelta != 0)
                                isGrabMoved = true;
                            int wantedWidth = (int) grabStartWidth + moveDelta;
                            if (pointerEvent.x <= 1u || wantedWidth < (int) SEMICOLON_V2_RAIL_MIN_WIDTH) {
                                if (SemicolonState_isRailVisible(semicolonState))
                                    SemicolonState_setRailVisible(semicolonState, false);
                            } else if (wantedWidth >= (int) SEMICOLON_V2_RAIL_MIN_WIDTH
                                       && wantedWidth <= (int) SemicolonState_getTerminalWidth(semicolonState) - 20) {
                                if (!SemicolonState_isRailVisible(semicolonState))
                                    SemicolonState_setRailVisible(semicolonState, true);
                                SemicolonState_setRailWidth(semicolonState, (bytes) wantedWidth);
                            }
                        }
                        break;
                    }
                    case TERMINAL_POINTER_RELEASE: {
                        if (isGrabbingDivider) {
                            if (!isGrabMoved) {
                                u64 currentNs = SemicolonV2_monotonicNowNs();
                                if (haveLastDividerClick && currentNs >= lastDividerClickNs
                                    && currentNs - lastDividerClickNs <= 300000000ULL) {
                                    haveLastDividerClick = false;
                                    SemicolonState_setRailVisible(semicolonState, false);
                                } else {
                                    haveLastDividerClick = true;
                                    lastDividerClickNs = currentNs;
                                }
                            }
                            isGrabbingDivider = false;
                            isGrabMoved = false;
                            SemicolonState_setSplitterDragging(semicolonState, false);
                            SemicolonCache_save(semicolonState);
                        }
                        break;
                    }
                    default:
                        break;
                }
            } else {
                if (inputKeyCode == KEY_UP) {
                    SemicolonState_scrollVertical(semicolonState, -1);
                } else if (inputKeyCode == KEY_DOWN) {
                    SemicolonState_scrollVertical(semicolonState, 1);
                } else if (inputKeyCode == KEY_LEFT) {
                    SemicolonState_scrollHorizontal(semicolonState, -4);
                } else if (inputKeyCode == KEY_RIGHT) {
                    SemicolonState_scrollHorizontal(semicolonState, 4);
                } else if (inputKeyCode == KEY_PPAGE) {
                    bytes stepRows = SemicolonCode_visibleRows(SemicolonState_getTerminalHeight(semicolonState));
                    SemicolonState_scrollVertical(semicolonState, -(int64_t) stepRows);
                } else if (inputKeyCode == KEY_NPAGE) {
                    bytes stepRows = SemicolonCode_visibleRows(SemicolonState_getTerminalHeight(semicolonState));
                    SemicolonState_scrollVertical(semicolonState, (int64_t) stepRows);
                } else if (inputKeyCode == KEY_HOME) {
                    SemicolonState_scrollToTop(semicolonState);
                } else if (inputKeyCode == KEY_END) {
                    SemicolonState_scrollToBottom(semicolonState);
                }
            }
        }

        SemicolonCache_save(semicolonState);

        (fn) SemicolonV2_setMouseReporting(false);
        (fn) curs_set(1);
        (fn) endwin();
        (fn) delscreen(screenInstance);
    }

    // Phase 4: Reverse Teardown
    TerminalPanel_free(mainPanel);
    SemicolonState_destroy(semicolonState);

    // Phase 5: Arena reset last
    return exitStatusCode;
}
