#include "tui/v2/mainpanel.h"
#include "tui/v2/bottompart.h"
#include "tui/v2/codecanvas.h"
#include "tui/v2/filetree.h"
#include "tui/v2/palette.h"
#include "nio/mem.h"
#include "oop/type.h"
#include "format/terminal_style.h"
#include "annotation/annotation.h"
#include "lifetime/lifetime/platform.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

;;DEFINITION
/** Semicolon v2 top-level MainPanel and application state coordinator.
 * Composes the canonical application frame (header row, file rail, divider seam,
 * code editor canvas and status footer) according to Interactables Preferences. */
;;OVERVIEW
/** ========================================================================
 * CLASS: SemicolonMainPanel (suite/projects/semicolon/src/tui/v2/mainpanel.c)
 * OWNS: root component tree, top-level layout anchoring and SemicolonState lifecycle.
 * BORROWS: SemicolonRail, SemicolonCode and SemicolonFooter factories.
 * CONVENTIONS: Descriptive cameled identifiers, zero -> operators, Memory_alloc.
 * ======================================================================== */

bytes SemicolonUtil_cellsOf(const char *byteSpan, bytes byteLength) {
    if (byteSpan == nullptr || byteLength == 0u)
        return 0u;
    bytes cellCount = 0u;
    bytes byteOffset = 0u;
    while (byteOffset < byteLength) {
        TerminalCanvasDecodedScalar decoded = TerminalCanvas_utf8Decode(byteSpan + byteOffset, byteLength - byteOffset);
        if (decoded.bytes == 0u) {
            ++cellCount;
            ++byteOffset;
            continue;
        }
        cellCount += TerminalCanvas_widthOf(decoded.codepoint);
        byteOffset += decoded.bytes;
    }
    return cellCount;
}

bytes SemicolonUtil_bytesForCells(const char *byteSpan, bytes byteLength, bytes cellTarget) {
    if (byteSpan == nullptr || byteLength == 0u || cellTarget == 0u)
        return 0u;
    bytes consumedCells = 0u;
    bytes byteOffset = 0u;
    while (byteOffset < byteLength && consumedCells < cellTarget) {
        TerminalCanvasDecodedScalar decoded = TerminalCanvas_utf8Decode(byteSpan + byteOffset, byteLength - byteOffset);
        if (decoded.bytes == 0u) {
            ++consumedCells;
            ++byteOffset;
            continue;
        }
        consumedCells += TerminalCanvas_widthOf(decoded.codepoint);
        byteOffset += decoded.bytes;
    }
    return byteOffset;
}

void SemicolonUtil_copyCellsEllipsis(const char *sourceString, bytes maximumCells,
                                     char *destinationBuffer, bytes destinationCapacity) {
    if (sourceString == nullptr || destinationBuffer == nullptr || destinationCapacity == 0u)
        return;
    bytes stringLength = strlen(sourceString);
    if (SemicolonUtil_cellsOf(sourceString, stringLength) <= maximumCells) {
        (void) snprintf(destinationBuffer, destinationCapacity, "%s", sourceString);
        return;
    }
    if (maximumCells == 0u) {
        destinationBuffer[0] = '\0';
        return;
    }

    bytes byteOffset = 0u;
    bytes usedCells = 0u;
    while (byteOffset < stringLength) {
        TerminalCanvasDecodedScalar decoded = TerminalCanvas_utf8Decode(sourceString + byteOffset, stringLength - byteOffset);
        bytes cellWidth = decoded.bytes == 0u ? 1u : TerminalCanvas_widthOf(decoded.codepoint);
        bytes stepBytes = decoded.bytes == 0u ? 1u : decoded.bytes;
        if (usedCells + cellWidth + 1u > maximumCells)
            break;
        usedCells += cellWidth;
        byteOffset += stepBytes;
    }

    if (byteOffset + 3u >= destinationCapacity)
        byteOffset = destinationCapacity > 4u ? destinationCapacity - 4u : 0u;
    memcpy(destinationBuffer, sourceString, byteOffset);
    memcpy(destinationBuffer + byteOffset, "…", 3u);
    destinationBuffer[byteOffset + 3u] = '\0';
}

const char *SemicolonUtil_iconUtf8(u32 codepointValue) {
    static char staticUtf8Buffer[8];
    bytes writeIndex = 0u;
    if (codepointValue < 0x80u) {
        staticUtf8Buffer[writeIndex++] = (char) codepointValue;
    } else if (codepointValue < 0x800u) {
        staticUtf8Buffer[writeIndex++] = (char) (0xC0u | (codepointValue >> 6u));
        staticUtf8Buffer[writeIndex++] = (char) (0x80u | (codepointValue & 0x3Fu));
    } else {
        staticUtf8Buffer[writeIndex++] = (char) (0xE0u | (codepointValue >> 12u));
        staticUtf8Buffer[writeIndex++] = (char) (0x80u | ((codepointValue >> 6u) & 0x3Fu));
        staticUtf8Buffer[writeIndex++] = (char) (0x80u | (codepointValue & 0x3Fu));
    }
    staticUtf8Buffer[writeIndex] = '\0';
    return staticUtf8Buffer;
}

SemicolonState *SemicolonState_create(void) {
    SemicolonState *semicolonState = (SemicolonState*) Memory_alloc(TYPE_BYTE_ARRAY, sizeof(SemicolonState));
    if (semicolonState == nullptr)
        return nullptr;
    memset(semicolonState, 0, sizeof(SemicolonState));

    (*semicolonState).themeAccentColor = SEMICOLON_V2_ACCENT;
    (*semicolonState).isNerdIcons = true;
    (*semicolonState).isDumpMode = false;
    (*semicolonState).terminalWidth = 112u;
    (*semicolonState).terminalHeight = 23u;
    (*semicolonState).railPanelWidth = SEMICOLON_V2_RAIL_DEFAULT_WIDTH;
    (*semicolonState).railRestoreWidth = SEMICOLON_V2_RAIL_DEFAULT_WIDTH;
    (*semicolonState).isRailVisible = true;
    (*semicolonState).isCursorVisible = true;
    (*semicolonState).lineNumberWidth = SEMICOLON_V2_GUTTER_MIN_WIDTH;

    return semicolonState;
}

void SemicolonState_destroy(SemicolonState *semicolonState) {
    if (semicolonState == nullptr)
        return;

    if ((*semicolonState).treeView != nullptr) {
        TerminalTreeView_free((*semicolonState).treeView);
        (*semicolonState).treeView = nullptr;
    }

    if ((*semicolonState).railEntries != nullptr) {
        Memory_free((*semicolonState).railEntries);
        (*semicolonState).railEntries = nullptr;
    }

    if ((*semicolonState).ignorePatterns != nullptr) {
        for (bytes patternIndex = 0u; patternIndex < (*semicolonState).ignorePatternCount; ++patternIndex) {
            if ((*semicolonState).ignorePatterns[patternIndex].patternText != nullptr)
                Memory_free((*semicolonState).ignorePatterns[patternIndex].patternText);
        }
        Memory_free((*semicolonState).ignorePatterns);
        (*semicolonState).ignorePatterns = nullptr;
    }

    if ((*semicolonState).fileLines != nullptr) {
        for (bytes lineIndex = 0u; lineIndex < (*semicolonState).fileLineCount; ++lineIndex) {
            if ((*semicolonState).fileLines[lineIndex] != nullptr)
                Memory_free((*semicolonState).fileLines[lineIndex]);
        }
        Memory_free((*semicolonState).fileLines);
        (*semicolonState).fileLines = nullptr;
    }
    if ((*semicolonState).fileLineLengths != nullptr) {
        Memory_free((*semicolonState).fileLineLengths);
        (*semicolonState).fileLineLengths = nullptr;
    }

    if ((*semicolonState).syntaxHighlighter != nullptr) {
        SemicolonSyntax_free((*semicolonState).syntaxHighlighter);
        (*semicolonState).syntaxHighlighter = nullptr;
    }

    if ((*semicolonState).markdownView != nullptr) {
        TerminalMarkdownView_free((*semicolonState).markdownView);
        (*semicolonState).markdownView = nullptr;
    }

    Memory_free(semicolonState);
}

TerminalPanel *createHeaderPanel(SemicolonState *semicolonState) {
    TerminalPanel *headerBar = TerminalPanel();
    if (headerBar == nullptr)
        return nullptr;
    if (!TerminalPanel_setLayout(headerBar, TERMINAL_PANEL_FLEX)) {
        TerminalPanel_free(headerBar);
        return nullptr;
    }

    TerminalText *gripText = TerminalText("");
    (void) TerminalText_setForegroundColor(gripText, SEMICOLON_V2_MUTED);
    (*semicolonState).headerGripText = gripText;
    (void) TerminalPanel_addText(headerBar, gripText, TerminalAnchor_2(TERMINAL_ANCHOR_BAND_LEFT, 0u));

    TerminalText *projectSpacer = TerminalText("");
    (void) TerminalText_setForegroundColor(projectSpacer, SEMICOLON_V2_TEXT);
    (*semicolonState).headerProjectSpacerText = projectSpacer;
    (*semicolonState).headerProjectSpacerChildId = TerminalPanel_addText(
        headerBar, projectSpacer,
        TerminalAnchor_2(TERMINAL_ANCHOR_BAND_LEFT, (*semicolonState).railPanelWidth));

    if ((*semicolonState).tabCount == 0u && (*semicolonState).openFileName[0] != '\0') {
        (*semicolonState).tabCount = 1u;
        (*semicolonState).activeTabIndex = 0u;
        (void) snprintf((*semicolonState).tabs[0].tabLabel, sizeof (*semicolonState).tabs[0].tabLabel,
                        "%s", (*semicolonState).openFileName);
        (void) snprintf((*semicolonState).tabs[0].tabPath, sizeof (*semicolonState).tabs[0].tabPath,
                        "%s", (*semicolonState).openFilePath);
        (*semicolonState).tabs[0].isPreviewMode = (*semicolonState).isMarkdownFile;
    }

    (*semicolonState).headerPanel = headerBar;
    SemicolonHeader_syncTabs(semicolonState);
    return headerBar;
}

TerminalPanel *createMainPanel(SemicolonState *semicolonState) {
    if (semicolonState == nullptr)
        return nullptr;

    TerminalPanel *rootPanel = TerminalPanel();
    TerminalPanel *headerPanel = createHeaderPanel(semicolonState);
    TerminalPanel *footerPanel = createFooterPanel(semicolonState);
    TerminalPanel *railPanel = createRailPanel(semicolonState);
    TerminalPanel *codePanel = createCodePanel(semicolonState);

    if (rootPanel == nullptr || headerPanel == nullptr || footerPanel == nullptr
        || railPanel == nullptr || codePanel == nullptr) {
        if (rootPanel != nullptr) TerminalPanel_free(rootPanel);
        if (headerPanel != nullptr) TerminalPanel_free(headerPanel);
        if (footerPanel != nullptr) TerminalPanel_free(footerPanel);
        if (railPanel != nullptr) TerminalPanel_free(railPanel);
        if (codePanel != nullptr) TerminalPanel_free(codePanel);
        return nullptr;
    }

    if (!TerminalPanel_setLayout(rootPanel, TERMINAL_PANEL_FLEX)) {
        TerminalPanel_free(rootPanel);
        TerminalPanel_free(headerPanel);
        TerminalPanel_free(footerPanel);
        TerminalPanel_free(railPanel);
        TerminalPanel_free(codePanel);
        return nullptr;
    }

    (*semicolonState).headerChildId = TerminalPanel_addPanel(
        rootPanel, headerPanel, TerminalAnchor_2(TERMINAL_ANCHOR_BAND_TOP, 1u));
    (*semicolonState).footerChildId = TerminalPanel_addPanel(
        rootPanel, footerPanel, TerminalAnchor_2(TERMINAL_ANCHOR_BAND_BOTTOM, 1u));
    (*semicolonState).railChildId = TerminalPanel_addPanel(
        rootPanel, railPanel, TerminalAnchor_2(TERMINAL_ANCHOR_BAND_LEFT, (*semicolonState).railPanelWidth));
    (*semicolonState).codeChildId = TerminalPanel_addPanel(
        rootPanel, codePanel, TerminalAnchor_0());

    if (!(*semicolonState).headerChildId.generation || !(*semicolonState).footerChildId.generation
        || !(*semicolonState).railChildId.generation || !(*semicolonState).codeChildId.generation) {
        TerminalPanel_free(rootPanel);
        return nullptr;
    }

    (*semicolonState).rootMainPanel = rootPanel;
    return rootPanel;
}

void SemicolonMainPanel_setRailWidth(SemicolonState *semicolonState, bytes newRailWidth) {
    if (semicolonState == nullptr || (*semicolonState).rootMainPanel == nullptr)
        return;
    if (newRailWidth < SEMICOLON_V2_RAIL_MIN_WIDTH)
        newRailWidth = SEMICOLON_V2_RAIL_MIN_WIDTH;
    bytes maximumAllowedWidth = (*semicolonState).terminalWidth > 25u ? (*semicolonState).terminalWidth - 25u
                                                                      : SEMICOLON_V2_RAIL_MIN_WIDTH;
    if (newRailWidth > maximumAllowedWidth)
        newRailWidth = maximumAllowedWidth;

    if (!TerminalPanel_setAnchor((*semicolonState).rootMainPanel, (*semicolonState).railChildId,
                                 TerminalAnchor_2(TERMINAL_ANCHOR_BAND_LEFT, newRailWidth)))
        return;

    (*semicolonState).railPanelWidth = newRailWidth;
    (*semicolonState).railRestoreWidth = newRailWidth;

    if ((*semicolonState).headerPanel != nullptr
        && TerminalPanel_isChildId((*semicolonState).headerProjectSpacerChildId)) {
        (void) TerminalPanel_setAnchor((*semicolonState).headerPanel,
                                       (*semicolonState).headerProjectSpacerChildId,
                                       TerminalAnchor_2(TERMINAL_ANCHOR_BAND_LEFT,
                                                        (*semicolonState).railPanelWidth));
    }
    SemicolonHeader_syncTabs(semicolonState);
}

void SemicolonHeader_syncTabs(SemicolonState *semicolonState) {
    if (semicolonState == nullptr || (*semicolonState).headerPanel == nullptr)
        return;

    TerminalPanel *headerBar = (*semicolonState).headerPanel;
    bytes railWidth = (*semicolonState).isRailVisible ? (*semicolonState).railPanelWidth : 0u;
    if (TerminalPanel_isChildId((*semicolonState).headerProjectSpacerChildId)) {
        (void) TerminalPanel_setAnchor(headerBar, (*semicolonState).headerProjectSpacerChildId,
                                       TerminalAnchor_2(TERMINAL_ANCHOR_BAND_LEFT, railWidth));
    }

    // Clean up any retired tabs beyond tabCount
    for (bytes tabIndex = (*semicolonState).tabCount; tabIndex < SEMICOLON_V2_MAX_TABS; ++tabIndex) {
        SemicolonEditorTab *extraTab = &(*semicolonState).tabs[tabIndex];
        if (TerminalPanel_isChildId((*extraTab).tabChildId)) {
            TerminalText *detachedText = TerminalPanel_detachText(headerBar, (*extraTab).tabChildId);
            if (detachedText != nullptr)
                TerminalText_free(detachedText);
            (*extraTab).tabChildId = (TerminalChildId) { 0 };
        } else if ((*extraTab).tabTextView != nullptr) {
            TerminalText_free((*extraTab).tabTextView);
        }
        (*extraTab).tabTextView = nullptr;
        (*extraTab).tabPixelWidth = 0u;
    }

    // Detach all active tabs so their flex slots in headerBar are re-added in strictly consecutive order
    for (bytes tabIndex = 0u; tabIndex < (*semicolonState).tabCount; ++tabIndex) {
        SemicolonEditorTab *tabRecord = &(*semicolonState).tabs[tabIndex];
        if (TerminalPanel_isChildId((*tabRecord).tabChildId)) {
            (void) TerminalPanel_detachText(headerBar, (*tabRecord).tabChildId);
            (*tabRecord).tabChildId = (TerminalChildId) { 0 };
        }
    }

    bytes usedWidth = railWidth;
    bytes totalWidth = (*semicolonState).terminalWidth > 0u ? (*semicolonState).terminalWidth : 80u;

    for (bytes tabIndex = 0u; tabIndex < (*semicolonState).tabCount; ++tabIndex) {
        SemicolonEditorTab *tabRecord = &(*semicolonState).tabs[tabIndex];
        bool isActiveTab = (tabIndex == (*semicolonState).activeTabIndex);
        const char *iconGlyph = SemicolonRail_getFileIcon((*tabRecord).tabLabel, (*semicolonState).isNerdIcons);
        (void) snprintf((*tabRecord).tabDisplay, sizeof (*tabRecord).tabDisplay,
                        "%s %s  \uf00d", iconGlyph, (*tabRecord).tabLabel);
        (void) snprintf((*tabRecord).tabShown, sizeof (*tabRecord).tabShown, "%s", (*tabRecord).tabDisplay);

        TerminalText *tabTextView = (*tabRecord).tabTextView;
        if (tabTextView == nullptr) {
            tabTextView = TerminalText((*tabRecord).tabShown);
            if (tabTextView == nullptr)
                continue;
            (*tabRecord).tabTextView = tabTextView;
        } else {
            (void) TerminalText_setText(tabTextView, (*tabRecord).tabShown, strlen((*tabRecord).tabShown));
        }
        (void) TerminalText_setBrackets(tabTextView, '[', ']');

        bytes measuredCells = TerminalText_measure(tabTextView);
        if (usedWidth + measuredCells > totalWidth) {
            bytes availableCells = totalWidth > usedWidth ? totalWidth - usedWidth : 0u;
            if (availableCells < 5u) {
                (*tabRecord).tabPixelX = usedWidth;
                (*tabRecord).tabPixelWidth = 0u;
                (void) TerminalText_setText(tabTextView, "", 0u);
                (void) TerminalText_setBrackets(tabTextView, 0u, 0u);
                (*tabRecord).tabChildId = TerminalPanel_addText(
                    headerBar, tabTextView, TerminalAnchor_2(TERMINAL_ANCHOR_BAND_LEFT, 0u));
                continue;
            }
            SemicolonUtil_copyCellsEllipsis((*tabRecord).tabDisplay, availableCells - 2u,
                                           (*tabRecord).tabShown, sizeof (*tabRecord).tabShown);
            (void) TerminalText_setText(tabTextView, (*tabRecord).tabShown, strlen((*tabRecord).tabShown));
            measuredCells = TerminalText_measure(tabTextView);
        }

        (*tabRecord).tabPixelX = usedWidth;
        (*tabRecord).tabPixelWidth = measuredCells;

        (*tabRecord).tabChildId = TerminalPanel_addText(
            headerBar, tabTextView, TerminalAnchor_2(TERMINAL_ANCHOR_BAND_LEFT, measuredCells));
        (void) TerminalText_setForegroundColor(tabTextView, isActiveTab ? SEMICOLON_V2_TEXT : SEMICOLON_V2_MUTED);
        (void) TerminalText_setBackgroundColor(tabTextView, isActiveTab ? SEMICOLON_V2_SELECTION : TERMINAL_COLOR_DEFAULT);

        usedWidth += measuredCells;
    }
}

bool SemicolonHeader_handleTabClick(SemicolonState *semicolonState, bytes clickX, TerminalPointerButton pointerButton) {
    if (semicolonState == nullptr)
        return false;
    bytes railWidth = (*semicolonState).isRailVisible ? (*semicolonState).railPanelWidth : 0u;
    if (clickX < railWidth)
        return false;

    for (bytes tabIndex = 0u; tabIndex < (*semicolonState).tabCount; ++tabIndex) {
        SemicolonEditorTab *tabRecord = &(*semicolonState).tabs[tabIndex];
        if ((*tabRecord).tabPixelWidth == 0u || clickX < (*tabRecord).tabPixelX
            || clickX >= (*tabRecord).tabPixelX + (*tabRecord).tabPixelWidth)
            continue;

        if (pointerButton == TERMINAL_BUTTON_MIDDLE
            || (clickX + 3u >= (*tabRecord).tabPixelX + (*tabRecord).tabPixelWidth)) {
            return SemicolonState_closeTab(semicolonState, tabIndex);
        }

        return SemicolonState_switchTab(semicolonState, tabIndex);
    }
    return false;
}

void SemicolonMainPanel_setRailVisible(SemicolonState *semicolonState, bool isVisible) {
    if (semicolonState == nullptr || (*semicolonState).rootMainPanel == nullptr
        || (*semicolonState).isRailVisible == isVisible)
        return;
    if (isVisible) {
        bytes restoredWidth = (*semicolonState).railRestoreWidth >= SEMICOLON_V2_RAIL_MIN_WIDTH
                             ? (*semicolonState).railRestoreWidth : SEMICOLON_V2_RAIL_DEFAULT_WIDTH;
        bytes maximumAllowed = (*semicolonState).terminalWidth > 25u ? (*semicolonState).terminalWidth - 25u : SEMICOLON_V2_RAIL_MIN_WIDTH;
        if (restoredWidth > maximumAllowed)
            restoredWidth = maximumAllowed;
        if (!TerminalPanel_setAnchor((*semicolonState).rootMainPanel, (*semicolonState).railChildId,
                                     TerminalAnchor_2(TERMINAL_ANCHOR_BAND_LEFT, restoredWidth)))
            return;
        (*semicolonState).isRailVisible = true;
        (*semicolonState).railPanelWidth = restoredWidth;
        (*semicolonState).railRestoreWidth = restoredWidth;
    } else {
        if (!TerminalPanel_setAnchor((*semicolonState).rootMainPanel, (*semicolonState).railChildId,
                                     TerminalAnchor_2(TERMINAL_ANCHOR_BAND_LEFT, 0u)))
            return;
        (*semicolonState).isRailVisible = false;
        (*semicolonState).railPanelWidth = 0u;
    }
    if ((*semicolonState).headerPanel != nullptr
        && TerminalPanel_isChildId((*semicolonState).headerProjectSpacerChildId)) {
        (void) TerminalPanel_setAnchor((*semicolonState).headerPanel,
                                       (*semicolonState).headerProjectSpacerChildId,
                                       TerminalAnchor_2(TERMINAL_ANCHOR_BAND_LEFT,
                                                        (*semicolonState).railPanelWidth));
    }
    SemicolonHeader_syncTabs(semicolonState);
}

bool SemicolonState_switchTab(SemicolonState *semicolonState, bytes tabIndex) {
    if (semicolonState == nullptr || tabIndex >= (*semicolonState).tabCount)
        return false;
    (*semicolonState).activeTabIndex = tabIndex;
    const char *targetPath = (*semicolonState).tabs[tabIndex].tabPath;
    if (strcmp((*semicolonState).openFilePath, targetPath) != 0) {
        if (!SemicolonCode_loadFile(semicolonState, targetPath))
            return false;
        (void) SemicolonRail_revealPath(semicolonState, targetPath);
        SemicolonFooter_updateBreadcrumb(semicolonState, targetPath);
    }
    SemicolonHeader_syncTabs(semicolonState);
    SemicolonCache_save(semicolonState);
    return true;
}

bool SemicolonState_closeTab(SemicolonState *semicolonState, bytes tabIndex) {
    if (semicolonState == nullptr || tabIndex >= (*semicolonState).tabCount)
        return false;
    if ((*semicolonState).tabCount <= 1u)
        return true;

    bytes nextActive = (*semicolonState).activeTabIndex;
    if (tabIndex == (*semicolonState).activeTabIndex) {
        nextActive = tabIndex + 1u < (*semicolonState).tabCount ? tabIndex : tabIndex - 1u;
    } else if (tabIndex < (*semicolonState).activeTabIndex) {
        nextActive = (*semicolonState).activeTabIndex - 1u;
    }

    SemicolonEditorTab removedTab = (*semicolonState).tabs[tabIndex];
    if ((*semicolonState).headerPanel != nullptr && TerminalPanel_isChildId(removedTab.tabChildId)) {
        TerminalText *detachedText = TerminalPanel_detachText((*semicolonState).headerPanel, removedTab.tabChildId);
        if (detachedText != nullptr)
            TerminalText_free(detachedText);
    } else if (removedTab.tabTextView != nullptr) {
        TerminalText_free(removedTab.tabTextView);
    }

    if (tabIndex + 1u < (*semicolonState).tabCount) {
        memmove(&(*semicolonState).tabs[tabIndex], &(*semicolonState).tabs[tabIndex + 1u],
                ((*semicolonState).tabCount - tabIndex - 1u) * sizeof(SemicolonEditorTab));
    }
    (*semicolonState).tabCount -= 1u;
    memset(&(*semicolonState).tabs[(*semicolonState).tabCount], 0, sizeof(SemicolonEditorTab));

    bool switchResult = SemicolonState_switchTab(semicolonState, nextActive);
    SemicolonHeader_syncTabs(semicolonState);
    SemicolonCache_save(semicolonState);
    return switchResult;
}

bool SemicolonState_openFile(SemicolonState *semicolonState, const char *filePath) {
    if (semicolonState == nullptr || filePath == nullptr || *filePath == '\0')
        return false;

    for (bytes tabIndex = 0u; tabIndex < (*semicolonState).tabCount; ++tabIndex) {
        if (strcmp((*semicolonState).tabs[tabIndex].tabPath, filePath) == 0) {
            return SemicolonState_switchTab(semicolonState, tabIndex);
        }
    }

    if (!SemicolonCode_loadFile(semicolonState, filePath))
        return false;

    (void) SemicolonRail_revealPath(semicolonState, filePath);
    SemicolonFooter_updateBreadcrumb(semicolonState, filePath);

    if ((*semicolonState).tabCount < SEMICOLON_V2_MAX_TABS) {
        bytes newIndex = (*semicolonState).tabCount;
        (*semicolonState).tabCount += 1u;
        (*semicolonState).activeTabIndex = newIndex;
        (void) snprintf((*semicolonState).tabs[newIndex].tabLabel, sizeof (*semicolonState).tabs[newIndex].tabLabel,
                        "%s", (*semicolonState).openFileName);
        (void) snprintf((*semicolonState).tabs[newIndex].tabPath, sizeof (*semicolonState).tabs[newIndex].tabPath,
                        "%s", filePath);
        (*semicolonState).tabs[newIndex].isPreviewMode = (*semicolonState).isMarkdownFile;
    } else {
        bytes activeIdx = (*semicolonState).activeTabIndex;
        (void) snprintf((*semicolonState).tabs[activeIdx].tabLabel, sizeof (*semicolonState).tabs[activeIdx].tabLabel,
                        "%s", (*semicolonState).openFileName);
        (void) snprintf((*semicolonState).tabs[activeIdx].tabPath, sizeof (*semicolonState).tabs[activeIdx].tabPath,
                        "%s", filePath);
        (*semicolonState).tabs[activeIdx].isPreviewMode = (*semicolonState).isMarkdownFile;
    }

    SemicolonHeader_syncTabs(semicolonState);
    SemicolonCache_save(semicolonState);
    return true;
}

void SemicolonMainPanel_paintSplitter(SemicolonState *semicolonState, TerminalCanvas *terminalCanvas, bool isHovered) {
    if (semicolonState == nullptr || terminalCanvas == nullptr)
        return;
    bytes canvasWidth = TerminalCanvas_getWidth(terminalCanvas);
    bytes canvasHeight = TerminalCanvas_getHeight(terminalCanvas);
    if (canvasHeight < 5u || canvasWidth == 0u)
        return;

    bytes middleRowY = canvasHeight / 2u;
    if (!(*semicolonState).isRailVisible) {
        for (bytes yIndex = middleRowY - 1u; yIndex <= middleRowY + 1u; ++yIndex) {
            TerminalCanvas_text(terminalCanvas, 0u, yIndex, "|", 1u,
                                TerminalPen_3(TERMINAL_STYLE_BOLD, (*semicolonState).themeAccentColor,
                                              TERMINAL_COLOR_DEFAULT));
        }
        return;
    }

    if (!isHovered || (*semicolonState).railPanelWidth == 0u || (*semicolonState).railPanelWidth >= canvasWidth)
        return;

    bytes dividerColumnX = (*semicolonState).railPanelWidth;
    for (bytes yIndex = middleRowY - 1u; yIndex <= middleRowY + 1u && yIndex + 1u < canvasHeight; ++yIndex) {
        TerminalCanvas_text(terminalCanvas, dividerColumnX, yIndex, "|", 1u,
                            TerminalPen_3(TERMINAL_STYLE_NONE, TERMINAL_RGB(0x55, 0x58, 0x62),
                                          TERMINAL_COLOR_DEFAULT));
    }
}

static void SemicolonCache_pathFor(const char *rootDirectory, char *destinationBuffer, bytes destinationCapacity) {
    u64 hashValue = 1469598103934665603ull;
    for (const char *cursorPointer = rootDirectory; *cursorPointer != '\0'; ++cursorPointer) {
        hashValue ^= (unsigned char) *cursorPointer;
        hashValue *= 1099511628211ull;
    }
    const char *stateOverride = getenv("SEMICOLON_STATE");
    if (stateOverride != nullptr && *stateOverride != '\0') {
        (void) snprintf(destinationBuffer, destinationCapacity, "%s/%016llx.state",
                        stateOverride, (unsigned long long) hashValue);
        return;
    }
    const char *xdgState = getenv("XDG_STATE_HOME");
    if (xdgState != nullptr && *xdgState != '\0') {
        (void) snprintf(destinationBuffer, destinationCapacity, "%s/semicolon/%016llx.state",
                        xdgState, (unsigned long long) hashValue);
        return;
    }
    const char *homeDirectory = getenv("HOME");
    if (homeDirectory == nullptr)
        homeDirectory = ".";
#if defined(VEX_PLATFORM_APPLE)
    (void) snprintf(destinationBuffer, destinationCapacity,
                    "%s/Library/Application Support/vexgraph/semicolon/%016llx.state",
                    homeDirectory, (unsigned long long) hashValue);
#else
    (void) snprintf(destinationBuffer, destinationCapacity,
                    "%s/.local/state/vexgraph/semicolon/%016llx.state",
                    homeDirectory, (unsigned long long) hashValue);
#endif
}

static bool SemicolonCache_ensureDirectory(const char *targetPath) {
    char directoryBuffer[1024];
    (void) snprintf(directoryBuffer, sizeof directoryBuffer, "%s", targetPath);
    for (char *slashPointer = directoryBuffer + 1; *slashPointer != '\0'; ++slashPointer) {
        if (*slashPointer != '/')
            continue;
        *slashPointer = '\0';
        if (mkdir(directoryBuffer, 0700) != 0 && errno != EEXIST)
            return false;
        *slashPointer = '/';
    }
    return mkdir(directoryBuffer, 0700) == 0 || errno == EEXIST;
}

void SemicolonCache_load(SemicolonState *semicolonState, const char *rootDirectory) {
    if (semicolonState == nullptr || rootDirectory == nullptr || *rootDirectory == '\0')
        return;
    char stateFilePath[SEMICOLON_V2_MAX_PATH];
    SemicolonCache_pathFor(rootDirectory, stateFilePath, sizeof stateFilePath);
    FILE *fileHandle = fopen(stateFilePath, "r");
    if (fileHandle == nullptr)
        return;

    char lineBuffer[1024];
    bool isSameRoot = false;
    char cachedOpenPath[SEMICOLON_V2_MAX_PATH];
    cachedOpenPath[0] = '\0';
    bytes cachedRailWidth = 0u;
    bool cachedRailHidden = false;
    bool cachedRailCustom = false;
    bool cachedMarkdownPreview = false;
    bool cachedMarkdownValid = false;

    while (fgets(lineBuffer, sizeof lineBuffer, fileHandle) != nullptr) {
        lineBuffer[strcspn(lineBuffer, "\r\n")] = '\0';
        if (strncmp(lineBuffer, "root ", 5) == 0) {
            isSameRoot = (strcmp(lineBuffer + 5, rootDirectory) == 0);
        } else if (strncmp(lineBuffer, "open ", 5) == 0) {
            (void) snprintf(cachedOpenPath, sizeof cachedOpenPath, "%s", lineBuffer + 5);
        } else if (strncmp(lineBuffer, "markdown-preview ", 17) == 0) {
            cachedMarkdownPreview = (strtoul(lineBuffer + 17, nullptr, 10) != 0);
            cachedMarkdownValid = true;
        } else if (strncmp(lineBuffer, "rail ", 5) == 0) {
            cachedRailWidth = (bytes) strtoul(lineBuffer + 5, nullptr, 10);
        } else if (strncmp(lineBuffer, "rail-custom ", 12) == 0) {
            cachedRailCustom = (strtoul(lineBuffer + 12, nullptr, 10) != 0);
        } else if (strncmp(lineBuffer, "hidden ", 7) == 0) {
            cachedRailHidden = (strtoul(lineBuffer + 7, nullptr, 10) != 0);
        }
    }
    (void) fclose(fileHandle);

    if (!isSameRoot)
        return;

    if (cachedRailWidth >= SEMICOLON_V2_RAIL_MIN_WIDTH) {
        (*semicolonState).railPanelWidth = cachedRailWidth;
        (*semicolonState).railRestoreWidth = cachedRailWidth;
        (*semicolonState).isRailWidthCustomized = cachedRailCustom;
    }
    if (cachedRailHidden) {
        (*semicolonState).isRailVisible = false;
        (*semicolonState).railRestoreWidth = cachedRailWidth >= SEMICOLON_V2_RAIL_MIN_WIDTH
            ? cachedRailWidth : SEMICOLON_V2_RAIL_DEFAULT_WIDTH;
        (*semicolonState).railPanelWidth = 0u;
    }
    if (cachedMarkdownValid) {
        (*semicolonState).isMarkdownPreview = cachedMarkdownPreview;
    }
    if (cachedOpenPath[0] != '\0' && access(cachedOpenPath, R_OK) == 0) {
        (void) snprintf((*semicolonState).openFilePath, sizeof (*semicolonState).openFilePath,
                        "%s", cachedOpenPath);
    }
}

void SemicolonCache_save(const SemicolonState *semicolonState) {
    if (semicolonState == nullptr || (*semicolonState).railRootDirectory[0] == '\0')
        return;
    char stateFilePath[SEMICOLON_V2_MAX_PATH];
    SemicolonCache_pathFor((*semicolonState).railRootDirectory, stateFilePath, sizeof stateFilePath);
    char directoryPath[SEMICOLON_V2_MAX_PATH];
    (void) snprintf(directoryPath, sizeof directoryPath, "%s", stateFilePath);
    char *slashPointer = strrchr(directoryPath, '/');
    if (slashPointer != nullptr) {
        *slashPointer = '\0';
        if (!SemicolonCache_ensureDirectory(directoryPath))
            return;
    }
    char temporaryPath[SEMICOLON_V2_MAX_PATH + 8u];
    (void) snprintf(temporaryPath, sizeof temporaryPath, "%s.tmp", stateFilePath);
    FILE *fileHandle = fopen(temporaryPath, "w");
    if (fileHandle == nullptr)
        return;

    fprintf(fileHandle, "root %s\n", (*semicolonState).railRootDirectory);
    fprintf(fileHandle, "open %s\n", (*semicolonState).openFilePath);
    fprintf(fileHandle, "markdown-preview %d\n",
            ((*semicolonState).isMarkdownFile && (*semicolonState).isMarkdownPreview) ? 1 : 0);
    bytes saveRailWidth = (*semicolonState).isRailVisible ? (*semicolonState).railPanelWidth
                                                           : (*semicolonState).railRestoreWidth;
    fprintf(fileHandle, "rail %zu\n", saveRailWidth);
    fprintf(fileHandle, "rail-custom %d\n", (*semicolonState).isRailWidthCustomized ? 1 : 0);
    fprintf(fileHandle, "hidden %d\n", !(*semicolonState).isRailVisible ? 1 : 0);
    fprintf(fileHandle, "accent #%02X%02X%02X\n",
            TERMINAL_COLOR_RED((*semicolonState).themeAccentColor),
            TERMINAL_COLOR_GREEN((*semicolonState).themeAccentColor),
            TERMINAL_COLOR_BLUE((*semicolonState).themeAccentColor));

    bool isFlushed = (fflush(fileHandle) == 0);
    bool isClosed = (fclose(fileHandle) == 0);
    if (!isFlushed || !isClosed || rename(temporaryPath, stateFilePath) != 0) {
        (void) remove(temporaryPath);
    }
}

bytes SemicolonState_getTerminalWidth(const SemicolonState *semicolonState) {
    return semicolonState != nullptr ? (*semicolonState).terminalWidth : 0u;
}

bytes SemicolonState_getTerminalHeight(const SemicolonState *semicolonState) {
    return semicolonState != nullptr ? (*semicolonState).terminalHeight : 0u;
}

void SemicolonState_setTerminalDimensions(SemicolonState *semicolonState, bytes terminalWidth, bytes terminalHeight) {
    if (semicolonState == nullptr)
        return;
    (*semicolonState).terminalWidth = terminalWidth;
    (*semicolonState).terminalHeight = terminalHeight;
}

bytes SemicolonState_getScrollTop(const SemicolonState *semicolonState) {
    if (semicolonState == nullptr)
        return 0u;
    if ((*semicolonState).isMarkdownFile && (*semicolonState).isMarkdownPreview
        && (*semicolonState).markdownView != nullptr) {
        return TerminalMarkdownView_getScroll((*semicolonState).markdownView);
    }
    return (*semicolonState).editorScrollTop;
}

bytes SemicolonState_getScrollLeft(const SemicolonState *semicolonState) {
    if (semicolonState == nullptr)
        return 0u;
    if ((*semicolonState).isMarkdownFile && (*semicolonState).isMarkdownPreview
        && (*semicolonState).markdownView != nullptr) {
        return TerminalMarkdownView_getScrollX((*semicolonState).markdownView);
    }
    return (*semicolonState).editorScrollLeft;
}

void SemicolonState_setScrollTop(SemicolonState *semicolonState, bytes scrollTop) {
    if (semicolonState == nullptr)
        return;
    (*semicolonState).editorScrollTop = scrollTop;
    if ((*semicolonState).isMarkdownFile && (*semicolonState).isMarkdownPreview
        && (*semicolonState).markdownView != nullptr) {
        (void) TerminalMarkdownView_setScroll((*semicolonState).markdownView, scrollTop);
    }
}

void SemicolonState_setScrollLeft(SemicolonState *semicolonState, bytes scrollLeft) {
    if (semicolonState == nullptr)
        return;
    (*semicolonState).editorScrollLeft = scrollLeft;
    if ((*semicolonState).isMarkdownFile && (*semicolonState).isMarkdownPreview
        && (*semicolonState).markdownView != nullptr) {
        (void) TerminalMarkdownView_setScrollX((*semicolonState).markdownView, scrollLeft);
    }
}

void SemicolonState_scrollVertical(SemicolonState *semicolonState, int64_t deltaRows) {
    if (semicolonState == nullptr)
        return;
    if ((*semicolonState).isMarkdownFile && (*semicolonState).isMarkdownPreview
        && (*semicolonState).markdownView != nullptr) {
        (void) TerminalMarkdownView_scrollBy((*semicolonState).markdownView, (long) deltaRows);
        (*semicolonState).editorScrollTop = TerminalMarkdownView_getScroll((*semicolonState).markdownView);
        return;
    }
    if (deltaRows < 0) {
        bytes absRows = (bytes) (-deltaRows);
        if ((*semicolonState).editorScrollTop <= absRows)
            (*semicolonState).editorScrollTop = 0u;
        else
            (*semicolonState).editorScrollTop -= absRows;
    } else {
        bytes absRows = (bytes) deltaRows;
        bytes maxRows = (*semicolonState).fileLineCount > 0u ? (*semicolonState).fileLineCount - 1u : 0u;
        if ((*semicolonState).editorScrollTop + absRows >= maxRows)
            (*semicolonState).editorScrollTop = maxRows;
        else
            (*semicolonState).editorScrollTop += absRows;
    }
}

void SemicolonState_scrollHorizontal(SemicolonState *semicolonState, int64_t deltaColumns) {
    if (semicolonState == nullptr)
        return;
    if ((*semicolonState).isMarkdownFile && (*semicolonState).isMarkdownPreview
        && (*semicolonState).markdownView != nullptr) {
        (void) TerminalMarkdownView_scrollXBy((*semicolonState).markdownView, (long) deltaColumns);
        (*semicolonState).editorScrollLeft = TerminalMarkdownView_getScrollX((*semicolonState).markdownView);
        return;
    }
    if (deltaColumns < 0) {
        bytes absCols = (bytes) (-deltaColumns);
        if ((*semicolonState).editorScrollLeft <= absCols)
            (*semicolonState).editorScrollLeft = 0u;
        else
            (*semicolonState).editorScrollLeft -= absCols;
    } else {
        bytes absCols = (bytes) deltaColumns;
        (*semicolonState).editorScrollLeft += absCols;
    }
}

void SemicolonState_scrollToTop(SemicolonState *semicolonState) {
    if (semicolonState == nullptr)
        return;
    (*semicolonState).editorScrollTop = 0u;
    if ((*semicolonState).isMarkdownFile && (*semicolonState).isMarkdownPreview
        && (*semicolonState).markdownView != nullptr) {
        (void) TerminalMarkdownView_setScroll((*semicolonState).markdownView, 0u);
    }
}

void SemicolonState_scrollToBottom(SemicolonState *semicolonState) {
    if (semicolonState == nullptr)
        return;
    if ((*semicolonState).isMarkdownFile && (*semicolonState).isMarkdownPreview
        && (*semicolonState).markdownView != nullptr) {
        bytes maxScroll = TerminalMarkdownView_maxScroll((*semicolonState).markdownView);
        (void) TerminalMarkdownView_setScroll((*semicolonState).markdownView, maxScroll);
        (*semicolonState).editorScrollTop = maxScroll;
        return;
    }
    bytes maxRows = (*semicolonState).fileLineCount > 0u ? (*semicolonState).fileLineCount - 1u : 0u;
    (*semicolonState).editorScrollTop = maxRows;
}

bytes SemicolonState_getRailWidth(const SemicolonState *semicolonState) {
    return semicolonState != nullptr ? (*semicolonState).railPanelWidth : 0u;
}

bytes SemicolonState_getRailRestoreWidth(const SemicolonState *semicolonState) {
    return semicolonState != nullptr ? (*semicolonState).railRestoreWidth : 0u;
}

void SemicolonState_setRailWidth(SemicolonState *semicolonState, bytes railWidth) {
    if (semicolonState == nullptr)
        return;
    SemicolonMainPanel_setRailWidth(semicolonState, railWidth);
    (*semicolonState).isRailWidthCustomized = true;
    SemicolonCache_save(semicolonState);
}

bool SemicolonState_isRailVisible(const SemicolonState *semicolonState) {
    return semicolonState != nullptr ? (*semicolonState).isRailVisible : false;
}

void SemicolonState_setRailVisible(SemicolonState *semicolonState, bool isVisible) {
    if (semicolonState == nullptr)
        return;
    SemicolonMainPanel_setRailVisible(semicolonState, isVisible);
    SemicolonCache_save(semicolonState);
}

void SemicolonState_toggleRailVisible(SemicolonState *semicolonState) {
    if (semicolonState == nullptr)
        return;
    SemicolonState_setRailVisible(semicolonState, !(*semicolonState).isRailVisible);
}

bool SemicolonState_isRailCustomized(const SemicolonState *semicolonState) {
    return semicolonState != nullptr ? (*semicolonState).isRailWidthCustomized : false;
}

void SemicolonState_setRailCustomized(SemicolonState *semicolonState, bool isCustomized) {
    if (semicolonState == nullptr)
        return;
    (*semicolonState).isRailWidthCustomized = isCustomized;
}

bool SemicolonState_isSplitterHovered(const SemicolonState *semicolonState) {
    return semicolonState != nullptr ? (*semicolonState).isSplitterHovered : false;
}

void SemicolonState_setSplitterHovered(SemicolonState *semicolonState, bool isHovered) {
    if (semicolonState == nullptr)
        return;
    (*semicolonState).isSplitterHovered = isHovered;
}

bool SemicolonState_isSplitterDragging(const SemicolonState *semicolonState) {
    return semicolonState != nullptr ? (*semicolonState).isSplitterDragging : false;
}

void SemicolonState_setSplitterDragging(SemicolonState *semicolonState, bool isDragging) {
    if (semicolonState == nullptr)
        return;
    (*semicolonState).isSplitterDragging = isDragging;
}

bool SemicolonState_isDumpMode(const SemicolonState *semicolonState) {
    return semicolonState != nullptr ? (*semicolonState).isDumpMode : false;
}

void SemicolonState_setDumpMode(SemicolonState *semicolonState, bool isDumpMode) {
    if (semicolonState == nullptr)
        return;
    (*semicolonState).isDumpMode = isDumpMode;
}

bool SemicolonState_isNerdIcons(const SemicolonState *semicolonState) {
    return semicolonState != nullptr ? (*semicolonState).isNerdIcons : false;
}

void SemicolonState_setNerdIcons(SemicolonState *semicolonState, bool isNerdIcons) {
    if (semicolonState == nullptr)
        return;
    (*semicolonState).isNerdIcons = isNerdIcons;
}

u32 SemicolonState_getThemeAccentColor(const SemicolonState *semicolonState) {
    return semicolonState != nullptr ? (*semicolonState).themeAccentColor : 0u;
}

void SemicolonState_setThemeAccentColor(SemicolonState *semicolonState, u32 accentColor) {
    if (semicolonState == nullptr)
        return;
    (*semicolonState).themeAccentColor = accentColor;
}

bool SemicolonState_isMarkdownFile(const SemicolonState *semicolonState) {
    return semicolonState != nullptr ? (*semicolonState).isMarkdownFile : false;
}

bool SemicolonState_isMarkdownPreview(const SemicolonState *semicolonState) {
    return semicolonState != nullptr ? (*semicolonState).isMarkdownPreview : false;
}

void SemicolonState_setMarkdownPreview(SemicolonState *semicolonState, bool isPreview) {
    if (semicolonState == nullptr)
        return;
    (*semicolonState).isMarkdownPreview = isPreview;
    if ((*semicolonState).activeTabIndex < (*semicolonState).tabCount)
        (*semicolonState).tabs[(*semicolonState).activeTabIndex].isPreviewMode = isPreview;
    SemicolonCache_save(semicolonState);
}

void SemicolonState_toggleMarkdownPreview(SemicolonState *semicolonState) {
    if (semicolonState == nullptr)
        return;
    SemicolonState_setMarkdownPreview(semicolonState, !(*semicolonState).isMarkdownPreview);
}

bytes SemicolonState_getFileLineCount(const SemicolonState *semicolonState) {
    return semicolonState != nullptr ? (*semicolonState).fileLineCount : 0u;
}

bytes SemicolonState_getTabCount(const SemicolonState *semicolonState) {
    return semicolonState != nullptr ? (*semicolonState).tabCount : 0u;
}

bytes SemicolonState_getActiveTabIndex(const SemicolonState *semicolonState) {
    return semicolonState != nullptr ? (*semicolonState).activeTabIndex : 0u;
}

const char *SemicolonState_getOpenFilePath(const SemicolonState *semicolonState) {
    return semicolonState != nullptr ? (*semicolonState).openFilePath : "";
}

const char *SemicolonState_getOpenFileName(const SemicolonState *semicolonState) {
    return semicolonState != nullptr ? (*semicolonState).openFileName : "";
}

const char *SemicolonState_getRailRootDirectory(const SemicolonState *semicolonState) {
    return semicolonState != nullptr ? (*semicolonState).railRootDirectory : "";
}

const char *SemicolonState_getRailLocationPath(const SemicolonState *semicolonState) {
    return semicolonState != nullptr ? (*semicolonState).railLocationPath : "";
}

void SemicolonState_setRailLocationPath(SemicolonState *semicolonState, const char *locationPath) {
    if (semicolonState == nullptr || locationPath == nullptr)
        return;
    (void) snprintf((*semicolonState).railLocationPath, sizeof (*semicolonState).railLocationPath, "%s", locationPath);
}

TerminalPanel *SemicolonState_getRootMainPanel(const SemicolonState *semicolonState) {
    return semicolonState != nullptr ? (*semicolonState).rootMainPanel : nullptr;
}

TerminalTreeView *SemicolonState_getTreeView(const SemicolonState *semicolonState) {
    return semicolonState != nullptr ? (*semicolonState).treeView : nullptr;
}

TerminalMarkdownView *SemicolonState_getMarkdownView(const SemicolonState *semicolonState) {
    return semicolonState != nullptr ? (*semicolonState).markdownView : nullptr;
}
