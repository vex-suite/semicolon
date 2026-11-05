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
    size_t tabPixelX;
    size_t tabPixelWidth;
    bool isPreviewMode;
} SemicolonEditorTab;

typedef struct SemicolonState {
    // Project environment paths
    char railRootDirectory[SEMICOLON_V2_MAX_PATH];
    char railLocationPath[SEMICOLON_V2_MAX_PATH];
    char openFilePath[SEMICOLON_V2_MAX_PATH];

    // Styling & mode configuration
    uint32_t themeAccentColor;
    bool isNerdIcons;
    bool isDumpMode;
    size_t terminalWidth;
    size_t terminalHeight;

    // Rail model
    TerminalTreeView *treeView;
    SemicolonRailEntry *railEntries;
    size_t railEntryCount;
    size_t railEntryCapacity;
    TerminalTreeNodeId railRootNodeId;
    size_t railPanelWidth;
    size_t railRestoreWidth;
    bool isRailVisible;
    bool isRailWidthCustomized;
    size_t railScrollRow;
    size_t railColumnOffset;
    size_t railMaxColumn;

    // Gitignore model
    SemicolonIgnorePattern *ignorePatterns;
    size_t ignorePatternCount;
    size_t ignorePatternCapacity;
    struct timespec ignoreFileModifiedTime;
    bool isIgnoreStatValid;

    // File buffer & editor model
    char openFileName[256];
    char **fileLines;
    size_t *fileLineLengths;
    size_t fileLineCount;
    size_t fileLineCapacity;
    size_t cursorLineIndex;
    size_t cursorByteOffset;
    bool isCursorVisible;
    bool isFileDirty;
    size_t editorScrollTop;
    size_t editorScrollLeft;
    size_t lineNumberWidth;
    SemicolonSyntax *syntaxHighlighter;
    bool isSyntaxCurrent;
    bool isMarkdownFile;
    bool isMarkdownPreview;
    TerminalMarkdownView *markdownView;

    // Editor tab strip
    SemicolonEditorTab tabs[SEMICOLON_V2_MAX_TABS];
    size_t tabCount;
    size_t activeTabIndex;

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
    size_t railRowCount;

    // Editor & gutter text widget pool
    TerminalText *gutterRowTexts[SEMICOLON_V2_MAX_ROWS];
    char gutterRowBuffers[SEMICOLON_V2_MAX_ROWS][32];
    TerminalText *codeRowTexts[SEMICOLON_V2_MAX_ROWS];
    size_t codeRowCount;

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
    size_t dragStartRailWidth;
} SemicolonState;

SemicolonState *SemicolonState_create(void);
void SemicolonState_destroy(SemicolonState *semicolonState);
bool SemicolonState_openFile(SemicolonState *semicolonState, const char *filePath);
bool SemicolonState_closeTab(SemicolonState *semicolonState, size_t tabIndex);
bool SemicolonState_switchTab(SemicolonState *semicolonState, size_t tabIndex);

// Session Cache (persisting state across runs)
void SemicolonCache_load(SemicolonState *semicolonState, const char *rootDirectory);
void SemicolonCache_save(const SemicolonState *semicolonState);

// Complete symmetric accessors and mutators (Java Law / Preferences 2.1)
size_t SemicolonState_getTerminalWidth(const SemicolonState *semicolonState);
size_t SemicolonState_getTerminalHeight(const SemicolonState *semicolonState);
void SemicolonState_setTerminalDimensions(SemicolonState *semicolonState, size_t terminalWidth, size_t terminalHeight);

size_t SemicolonState_getScrollTop(const SemicolonState *semicolonState);
size_t SemicolonState_getScrollLeft(const SemicolonState *semicolonState);
void SemicolonState_setScrollTop(SemicolonState *semicolonState, size_t scrollTop);
void SemicolonState_setScrollLeft(SemicolonState *semicolonState, size_t scrollLeft);
void SemicolonState_scrollVertical(SemicolonState *semicolonState, int64_t deltaRows);
void SemicolonState_scrollHorizontal(SemicolonState *semicolonState, int64_t deltaColumns);
void SemicolonState_scrollToTop(SemicolonState *semicolonState);
void SemicolonState_scrollToBottom(SemicolonState *semicolonState);

size_t SemicolonState_getRailWidth(const SemicolonState *semicolonState);
size_t SemicolonState_getRailRestoreWidth(const SemicolonState *semicolonState);
void SemicolonState_setRailWidth(SemicolonState *semicolonState, size_t railWidth);
bool SemicolonState_isRailVisible(const SemicolonState *semicolonState);
void SemicolonState_setRailVisible(SemicolonState *semicolonState, bool isVisible);
void SemicolonState_toggleRailVisible(SemicolonState *semicolonState);
bool SemicolonState_isRailCustomized(const SemicolonState *semicolonState);
void SemicolonState_setRailCustomized(SemicolonState *semicolonState, bool isCustomized);

bool SemicolonState_isSplitterHovered(const SemicolonState *semicolonState);
void SemicolonState_setSplitterHovered(SemicolonState *semicolonState, bool isHovered);
bool SemicolonState_isSplitterDragging(const SemicolonState *semicolonState);
void SemicolonState_setSplitterDragging(SemicolonState *semicolonState, bool isDragging);

bool SemicolonState_isDumpMode(const SemicolonState *semicolonState);
void SemicolonState_setDumpMode(SemicolonState *semicolonState, bool isDumpMode);
bool SemicolonState_isNerdIcons(const SemicolonState *semicolonState);
void SemicolonState_setNerdIcons(SemicolonState *semicolonState, bool isNerdIcons);
uint32_t SemicolonState_getThemeAccentColor(const SemicolonState *semicolonState);
void SemicolonState_setThemeAccentColor(SemicolonState *semicolonState, uint32_t accentColor);

bool SemicolonState_isMarkdownFile(const SemicolonState *semicolonState);
bool SemicolonState_isMarkdownPreview(const SemicolonState *semicolonState);
void SemicolonState_setMarkdownPreview(SemicolonState *semicolonState, bool isPreview);
void SemicolonState_toggleMarkdownPreview(SemicolonState *semicolonState);

size_t SemicolonState_getFileLineCount(const SemicolonState *semicolonState);
size_t SemicolonState_getTabCount(const SemicolonState *semicolonState);
size_t SemicolonState_getActiveTabIndex(const SemicolonState *semicolonState);
const char *SemicolonState_getOpenFilePath(const SemicolonState *semicolonState);
const char *SemicolonState_getOpenFileName(const SemicolonState *semicolonState);
const char *SemicolonState_getRailRootDirectory(const SemicolonState *semicolonState);
const char *SemicolonState_getRailLocationPath(const SemicolonState *semicolonState);
void SemicolonState_setRailLocationPath(SemicolonState *semicolonState, const char *locationPath);
TerminalPanel *SemicolonState_getRootMainPanel(const SemicolonState *semicolonState);
TerminalTreeView *SemicolonState_getTreeView(const SemicolonState *semicolonState);
TerminalMarkdownView *SemicolonState_getMarkdownView(const SemicolonState *semicolonState);

// General utility helpers
size_t SemicolonUtil_cellsOf(const char *byteSpan, size_t byteLength);
size_t SemicolonUtil_bytesForCells(const char *byteSpan, size_t byteLength, size_t cellTarget);
void SemicolonUtil_copyCellsEllipsis(const char *sourceString, size_t maximumCells,
                                     char *destinationBuffer, size_t destinationCapacity);
const char *SemicolonUtil_iconUtf8(uint32_t codepointValue);

#endif
