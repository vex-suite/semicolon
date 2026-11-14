#ifndef SEMICOLON_V2_STATE_H
#define SEMICOLON_V2_STATE_H

#include "ui/panel/terminal_panel.h"
#include "ui/panel/terminal_anchor.h"
#include "ui/text/terminal_text.h"
#include "ui/surface/terminal_canvas.h"
#include "ui/tree/terminal_tree_view.h"
#include "ui/markdown/terminal_markdown_view.h"
#include "editor/semicolon_syntax.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define SEMICOLON_V2_MAX_TABS 16u
#define SEMICOLON_V2_MAX_ROWS 128u
#define SEMICOLON_V2_MAX_PATH 1024u
#define SEMICOLON_V2_RAIL_DEFAULT_WIDTH 50u
#define SEMICOLON_V2_RAIL_MIN_WIDTH 20u
#define SEMICOLON_V2_GUTTER_MIN_WIDTH 1u

typedef struct SemicolonIgnorePattern {
    char *patternText;
    bool isNegated;
    bool isDirectoryOnly;
    bool isAnchored;
    bool hasSlash;
} SemicolonIgnorePattern;

typedef struct SemicolonRailEntry {
    char entryLabel[256];
    char entryPath[SEMICOLON_V2_MAX_PATH];
    TerminalTreeNodeId parentNodeId;
    bool isFolder;
    bool isLoaded;
} SemicolonRailEntry;

typedef struct SemicolonEditorTab {
    char tabPath[SEMICOLON_V2_MAX_PATH];
    char tabLabel[256];
    char tabDisplay[256];
    char tabShown[256];
    TerminalText *tabTextView;
    TerminalChildId tabChildId;
    bytes tabPixelX;
    bytes tabPixelWidth;
    bool isPreviewMode;
} SemicolonEditorTab;

typedef struct SemicolonState {
    // Project environment paths
    char railRootDirectory[SEMICOLON_V2_MAX_PATH];
    char railLocationPath[SEMICOLON_V2_MAX_PATH];
    char openFilePath[SEMICOLON_V2_MAX_PATH];

    // Styling & mode configuration
    u32 themeAccentColor;
    bool isNerdIcons;
    bool isDumpMode;
    bytes terminalWidth;
    bytes terminalHeight;

    // Rail model
    TerminalTreeView *treeView;
    SemicolonRailEntry *railEntries;
    bytes railEntryCount;
    bytes railEntryCapacity;
    TerminalTreeNodeId railRootNodeId;
    bytes railPanelWidth;
    bytes railRestoreWidth;
    bool isRailVisible;
    bool isRailWidthCustomized;
    bytes railScrollRow;
    bytes railColumnOffset;
    bytes railMaxColumn;

    // Gitignore model
    SemicolonIgnorePattern *ignorePatterns;
    bytes ignorePatternCount;
    bytes ignorePatternCapacity;
    struct timespec ignoreFileModifiedTime;
    bool isIgnoreStatValid;

    // File buffer & editor model
    char openFileName[256];
    char **fileLines;
    bytes *fileLineLengths;
    bytes fileLineCount;
    bytes fileLineCapacity;
    bytes cursorLineIndex;
    bytes cursorByteOffset;
    bool isCursorVisible;
    bool isFileDirty;
    bytes editorScrollTop;
    bytes editorScrollLeft;
    bytes lineNumberWidth;
    SemicolonSyntax *syntaxHighlighter;
    bool isSyntaxCurrent;
    bool isMarkdownFile;
    bool isMarkdownPreview;
    TerminalMarkdownView *markdownView;

    // Editor tab strip
    SemicolonEditorTab tabs[SEMICOLON_V2_MAX_TABS];
    bytes tabCount;
    bytes activeTabIndex;

    // Layout panels (owned hierarchy)
    TerminalPanel *rootMainPanel;
    TerminalPanel *headerPanel;
    TerminalPanel *railPanel;
    TerminalPanel *codePanel;
    TerminalPanel *footerPanel;
    TerminalPanel *editorSubPanel;
    TerminalPanel *gutterSubPanel;
    TerminalPanel *actionSubPanel;

    // Child identities inside container panels
    TerminalChildId headerChildId;
    TerminalChildId railChildId;
    TerminalChildId codeChildId;
    TerminalChildId footerChildId;
    TerminalChildId gutterChildId;
    TerminalChildId headerProjectSpacerChildId;

    // Header UI elements
    TerminalText *headerGripText;
    TerminalText *headerProjectSpacerText;

    // Rail text widget pool
    TerminalText *railRowTexts[SEMICOLON_V2_MAX_ROWS];
    char railRowBuffers[SEMICOLON_V2_MAX_ROWS][512];
    bytes railRowCount;

    // Editor & gutter text widget pool
    TerminalText *gutterRowTexts[SEMICOLON_V2_MAX_ROWS];
    char gutterRowBuffers[SEMICOLON_V2_MAX_ROWS][32];
    TerminalText *codeRowTexts[SEMICOLON_V2_MAX_ROWS];
    bytes codeRowCount;

    // Action chip widgets
    TerminalText *hintChipText;
    TerminalText *runChipText;
    TerminalText *codeModeChipText;
    TerminalText *previewModeChipText;

    // Footer chip widgets
    TerminalText *codeModeBadgeText;
    TerminalText *agentsModeBadgeText;
    TerminalText *breadcrumbText;
    char breadcrumbBuffer[512];
    TerminalText *encodingChipText;
    TerminalText *timeChipText;
    char timeChipBuffer[32];
    TerminalText *lineChipText;
    char lineChipBuffer[32];
    TerminalText *projectChipText;
    char projectChipBuffer[64];

    // Interaction & dragging state
    bool isSplitterHovered;
    bool isSplitterDragging;
    int dragStartXCoordinate;
    bytes dragStartRailWidth;
} SemicolonState;

SemicolonState *SemicolonState_create();
fn SemicolonState_destroy(SemicolonState *semicolonState);
bool SemicolonState_openFile(SemicolonState *semicolonState, const char *filePath);
bool SemicolonState_closeTab(SemicolonState *semicolonState, bytes tabIndex);
bool SemicolonState_switchTab(SemicolonState *semicolonState, bytes tabIndex);

// Session Cache (persisting state across runs)
fn SemicolonCache_load(SemicolonState *semicolonState, const char *rootDirectory);
fn SemicolonCache_save(const SemicolonState *semicolonState);

// Complete symmetric accessors and mutators (Java Law / Preferences 2.1)
bytes SemicolonState_getTerminalWidth(const SemicolonState *semicolonState);
bytes SemicolonState_getTerminalHeight(const SemicolonState *semicolonState);
fn SemicolonState_setTerminalDimensions(SemicolonState *semicolonState, bytes terminalWidth, bytes terminalHeight);

bytes SemicolonState_getScrollTop(const SemicolonState *semicolonState);
bytes SemicolonState_getScrollLeft(const SemicolonState *semicolonState);
fn SemicolonState_setScrollTop(SemicolonState *semicolonState, bytes scrollTop);
fn SemicolonState_setScrollLeft(SemicolonState *semicolonState, bytes scrollLeft);
fn SemicolonState_scrollVertical(SemicolonState *semicolonState, int64_t deltaRows);
fn SemicolonState_scrollHorizontal(SemicolonState *semicolonState, int64_t deltaColumns);
fn SemicolonState_scrollToTop(SemicolonState *semicolonState);
fn SemicolonState_scrollToBottom(SemicolonState *semicolonState);

bytes SemicolonState_getRailWidth(const SemicolonState *semicolonState);
bytes SemicolonState_getRailRestoreWidth(const SemicolonState *semicolonState);
fn SemicolonState_setRailWidth(SemicolonState *semicolonState, bytes railWidth);
bool SemicolonState_isRailVisible(const SemicolonState *semicolonState);
fn SemicolonState_setRailVisible(SemicolonState *semicolonState, bool isVisible);
fn SemicolonState_toggleRailVisible(SemicolonState *semicolonState);
bool SemicolonState_isRailCustomized(const SemicolonState *semicolonState);
fn SemicolonState_setRailCustomized(SemicolonState *semicolonState, bool isCustomized);

bool SemicolonState_isSplitterHovered(const SemicolonState *semicolonState);
fn SemicolonState_setSplitterHovered(SemicolonState *semicolonState, bool isHovered);
bool SemicolonState_isSplitterDragging(const SemicolonState *semicolonState);
fn SemicolonState_setSplitterDragging(SemicolonState *semicolonState, bool isDragging);

bool SemicolonState_isDumpMode(const SemicolonState *semicolonState);
fn SemicolonState_setDumpMode(SemicolonState *semicolonState, bool isDumpMode);
bool SemicolonState_isNerdIcons(const SemicolonState *semicolonState);
fn SemicolonState_setNerdIcons(SemicolonState *semicolonState, bool isNerdIcons);
u32 SemicolonState_getThemeAccentColor(const SemicolonState *semicolonState);
fn SemicolonState_setThemeAccentColor(SemicolonState *semicolonState, u32 accentColor);

bool SemicolonState_isMarkdownFile(const SemicolonState *semicolonState);
bool SemicolonState_isMarkdownPreview(const SemicolonState *semicolonState);
fn SemicolonState_setMarkdownPreview(SemicolonState *semicolonState, bool isPreview);
fn SemicolonState_toggleMarkdownPreview(SemicolonState *semicolonState);

bytes SemicolonState_getFileLineCount(const SemicolonState *semicolonState);
bytes SemicolonState_getTabCount(const SemicolonState *semicolonState);
bytes SemicolonState_getActiveTabIndex(const SemicolonState *semicolonState);
const char *SemicolonState_getOpenFilePath(const SemicolonState *semicolonState);
const char *SemicolonState_getOpenFileName(const SemicolonState *semicolonState);
const char *SemicolonState_getRailRootDirectory(const SemicolonState *semicolonState);
const char *SemicolonState_getRailLocationPath(const SemicolonState *semicolonState);
fn SemicolonState_setRailLocationPath(SemicolonState *semicolonState, const char *locationPath);
TerminalPanel *SemicolonState_getRootMainPanel(const SemicolonState *semicolonState);
TerminalTreeView *SemicolonState_getTreeView(const SemicolonState *semicolonState);
TerminalMarkdownView *SemicolonState_getMarkdownView(const SemicolonState *semicolonState);

// General utility helpers
bytes SemicolonUtil_cellsOf(const char *byteSpan, bytes byteLength);
bytes SemicolonUtil_bytesForCells(const char *byteSpan, bytes byteLength, bytes cellTarget);
fn SemicolonUtil_copyCellsEllipsis(const char *sourceString, bytes maximumCells,
                                     char *destinationBuffer, bytes destinationCapacity);
const char *SemicolonUtil_iconUtf8(u32 codepointValue);

#endif
