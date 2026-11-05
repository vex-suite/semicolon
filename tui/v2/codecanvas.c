#include "tui/v2/codecanvas.h"
#include "tui/v2/palette.h"
#include "nio/mem.h"
#include "oop/type.h"
#include "format/terminal_style.h"
#include "annotation/annotation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

;;DEFINITION
/** Semicolon v2 code canvas and text editing pane.
 * Owns the line-number gutter, syntax pen mapping, markdown preview delegation,
 * action chips and cursor cell rendering. */
;;OVERVIEW
/** ========================================================================
 * CLASS: SemicolonCode (suite/projects/semicolon/src/tui/v2/codecanvas.c)
 * OWNS: file line buffers, line lengths, syntax instance and editor row pools.
 * BORROWS: SemicolonState, Determinant TerminalCanvas, SemicolonSyntax, TerminalMarkdownView.
 * CONVENTIONS: Descriptive cameled identifiers, zero -> operators, Memory_alloc.
 * ======================================================================== */

bool SemicolonCode_isMarkdownFile(const char *fileName) {
    if (fileName == nullptr)
        return false;
    const char *extensionPointer = strrchr(fileName, '.');
    return extensionPointer != nullptr && (strcasecmp(extensionPointer, ".md") == 0
                                           || strcasecmp(extensionPointer, ".markdown") == 0);
}

size_t SemicolonCode_lineNumberWidth(size_t lineCount) {
    size_t widthValue = SEMICOLON_V2_GUTTER_MIN_WIDTH;
    for (size_t largestNumber = lineCount; largestNumber >= 10u; largestNumber /= 10u)
        ++widthValue;
    return widthValue;
}

size_t SemicolonCode_visibleRows(size_t terminalHeight) {
    return terminalHeight > 2u ? terminalHeight - 2u : 0u;
}

static void SemicolonCode_freeFileLines(SemicolonState *semicolonState) {
    if (semicolonState == nullptr)
        return;
    if ((*semicolonState).fileLines != nullptr) {
        for (size_t lineIndex = 0u; lineIndex < (*semicolonState).fileLineCount; ++lineIndex) {
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
    (*semicolonState).fileLineCount = 0u;
    (*semicolonState).fileLineCapacity = 0u;
}

static bool SemicolonCode_addLine(SemicolonState *semicolonState, const char *lineText, size_t lineLength) {
    if ((*semicolonState).fileLineCount == (*semicolonState).fileLineCapacity) {
        size_t nextCapacity = (*semicolonState).fileLineCapacity == 0u ? 256u
                                                                       : (*semicolonState).fileLineCapacity * 2u;
        char **grownLines = (char**) Memory_alloc(TYPE_BYTE_ARRAY, nextCapacity * sizeof(char*));
        size_t *grownLengths = (size_t*) Memory_alloc(TYPE_BYTE_ARRAY, nextCapacity * sizeof(size_t));
        if (grownLines == nullptr || grownLengths == nullptr) {
            if (grownLines != nullptr)
                Memory_free(grownLines);
            if (grownLengths != nullptr)
                Memory_free(grownLengths);
            return false;
        }
        if ((*semicolonState).fileLineCount > 0u) {
            memcpy(grownLines, (*semicolonState).fileLines, (*semicolonState).fileLineCount * sizeof(char*));
            memcpy(grownLengths, (*semicolonState).fileLineLengths, (*semicolonState).fileLineCount * sizeof(size_t));
            Memory_free((*semicolonState).fileLines);
            Memory_free((*semicolonState).fileLineLengths);
        }
        (*semicolonState).fileLines = grownLines;
        (*semicolonState).fileLineLengths = grownLengths;
        (*semicolonState).fileLineCapacity = nextCapacity;
    }

    char *allocatedLine = (char*) Memory_alloc(TYPE_BYTE_ARRAY, lineLength + 1u);
    if (allocatedLine == nullptr)
        return false;
    memcpy(allocatedLine, lineText, lineLength);
    allocatedLine[lineLength] = '\0';

    (*semicolonState).fileLines[(*semicolonState).fileLineCount] = allocatedLine;
    (*semicolonState).fileLineLengths[(*semicolonState).fileLineCount] = lineLength;
    (*semicolonState).fileLineCount++;
    return true;
}

bool SemicolonCode_loadFile(SemicolonState *semicolonState, const char *filePath) {
    if (semicolonState == nullptr || filePath == nullptr || *filePath == '\0')
        return false;

    struct stat fileStat;
    if (stat(filePath, &fileStat) != 0 || !S_ISREG(fileStat.st_mode))
        return false;

    FILE *filePointer = fopen(filePath, "rb");
    if (filePointer == nullptr)
        return false;

    SemicolonCode_freeFileLines(semicolonState);

    const char *slashPointer = strrchr(filePath, '/');
    const char *baseName = slashPointer != nullptr ? slashPointer + 1u : filePath;
    (void) snprintf((*semicolonState).openFileName, sizeof (*semicolonState).openFileName, "%s", baseName);
    (void) snprintf((*semicolonState).openFilePath, sizeof (*semicolonState).openFilePath, "%s", filePath);

    char readBuffer[4096];
    char *accumulatedBytes = nullptr;
    size_t totalBytes = 0u;
    size_t readChunk = 0u;
    while ((readChunk = fread(readBuffer, 1u, sizeof readBuffer, filePointer)) > 0u) {
        char *grownBytes = (char*) Memory_alloc(TYPE_BYTE_ARRAY, totalBytes + readChunk + 1u);
        if (grownBytes == nullptr) {
            (void) fclose(filePointer);
            if (accumulatedBytes != nullptr)
                Memory_free(accumulatedBytes);
            return false;
        }
        if (totalBytes > 0u) {
            memcpy(grownBytes, accumulatedBytes, totalBytes);
            Memory_free(accumulatedBytes);
        }
        memcpy(grownBytes + totalBytes, readBuffer, readChunk);
        totalBytes += readChunk;
        grownBytes[totalBytes] = '\0';
        accumulatedBytes = grownBytes;
    }
    (void) fclose(filePointer);

    if (totalBytes > 0u && accumulatedBytes != nullptr) {
        size_t lineStartIndex = 0u;
        for (size_t byteIndex = 0u; byteIndex <= totalBytes; ++byteIndex) {
            if (byteIndex != totalBytes && accumulatedBytes[byteIndex] != '\n')
                continue;
            size_t lineEndIndex = byteIndex;
            if (byteIndex < totalBytes && lineEndIndex > lineStartIndex && accumulatedBytes[lineEndIndex - 1u] == '\r')
                --lineEndIndex;
            if (!SemicolonCode_addLine(semicolonState, accumulatedBytes + lineStartIndex, lineEndIndex - lineStartIndex)) {
                Memory_free(accumulatedBytes);
                SemicolonCode_freeFileLines(semicolonState);
                return false;
            }
            lineStartIndex = byteIndex + 1u;
        }
        Memory_free(accumulatedBytes);
    }

    if ((*semicolonState).fileLineCount == 0u) {
        if (!SemicolonCode_addLine(semicolonState, "", 0u))
            return false;
    }

    (*semicolonState).lineNumberWidth = SemicolonCode_lineNumberWidth((*semicolonState).fileLineCount);
    (*semicolonState).isMarkdownFile = SemicolonCode_isMarkdownFile((*semicolonState).openFileName);
    (*semicolonState).isMarkdownPreview = (*semicolonState).isMarkdownFile;

    if ((*semicolonState).syntaxHighlighter != nullptr) {
        SemicolonSyntax_free((*semicolonState).syntaxHighlighter);
        (*semicolonState).syntaxHighlighter = nullptr;
    }

    if (SemicolonSyntax_acceptsPath(filePath)) {
        (*semicolonState).syntaxHighlighter = SemicolonSyntax();
        if ((*semicolonState).syntaxHighlighter != nullptr) {
            (*semicolonState).isSyntaxCurrent = SemicolonSyntax_setLines(
                (*semicolonState).syntaxHighlighter,
                (const char *const *) (*semicolonState).fileLines,
                (*semicolonState).fileLineLengths,
                (*semicolonState).fileLineCount);
        }
    }

    if ((*semicolonState).markdownView != nullptr) {
        TerminalMarkdownView_free((*semicolonState).markdownView);
        (*semicolonState).markdownView = nullptr;
    }

    if ((*semicolonState).isMarkdownFile) {
        (*semicolonState).markdownView = TerminalMarkdownView();
        if ((*semicolonState).markdownView != nullptr) {
            (void) TerminalMarkdownView_setSource(
                (*semicolonState).markdownView,
                (const char *const *) (*semicolonState).fileLines,
                (*semicolonState).fileLineCount);
            (void) TerminalMarkdownView_setScroll((*semicolonState).markdownView, (*semicolonState).editorScrollTop);
            (void) TerminalMarkdownView_setScrollX((*semicolonState).markdownView, (*semicolonState).editorScrollLeft);
        }
        (void) SemicolonCode_setMarkdownMode(semicolonState, (*semicolonState).isMarkdownPreview);
    }

    return true;
}

void SemicolonCode_syncVisibleRows(SemicolonState *semicolonState, size_t firstLineIndex,
                                   size_t leftColumnIndex, size_t visibleRowCount) {
    if (semicolonState == nullptr)
        return;

    for (size_t rowIndex = 0u; rowIndex < (*semicolonState).codeRowCount; ++rowIndex) {
        TerminalText *gutterTextView = (*semicolonState).gutterRowTexts[rowIndex];
        TerminalText *codeTextView = (*semicolonState).codeRowTexts[rowIndex];
        if (gutterTextView == nullptr || codeTextView == nullptr)
            continue;

        if ((*semicolonState).isMarkdownFile && (*semicolonState).isMarkdownPreview) {
            (void) TerminalText_setText(gutterTextView, "", 0u);
            (void) TerminalText_setText(codeTextView, "", 0u);
            continue;
        }

        size_t currentLineIndex = firstLineIndex + rowIndex;
        if (rowIndex >= visibleRowCount || currentLineIndex >= (*semicolonState).fileLineCount) {
            (void) TerminalText_setText(gutterTextView, "", 0u);
            (void) TerminalText_setText(codeTextView, "", 0u);
            continue;
        }

        int writtenGutter = snprintf((*semicolonState).gutterRowBuffers[rowIndex],
                                     sizeof (*semicolonState).gutterRowBuffers[rowIndex],
                                     "  %*zu.", (int) (*semicolonState).lineNumberWidth, currentLineIndex + 1u);
        if (writtenGutter < 0)
            writtenGutter = 0;

        (void) TerminalText_setText(gutterTextView, (*semicolonState).gutterRowBuffers[rowIndex], (size_t) writtenGutter);

        const char *lineBytes = (*semicolonState).fileLines[currentLineIndex];
        size_t lineLength = (*semicolonState).fileLineLengths[currentLineIndex];
        size_t skipBytes = SemicolonUtil_bytesForCells(lineBytes, lineLength, leftColumnIndex);
        (void) TerminalText_setText(codeTextView, lineBytes + skipBytes, lineLength - skipBytes);
    }
}

void SemicolonCode_paintSyntax(SemicolonState *semicolonState, TerminalCanvas *terminalCanvas,
                               size_t firstLineIndex, size_t leftColumnIndex) {
    if (semicolonState == nullptr || terminalCanvas == nullptr
        || !(*semicolonState).isSyntaxCurrent || (*semicolonState).syntaxHighlighter == nullptr
        || ((*semicolonState).isMarkdownFile && (*semicolonState).isMarkdownPreview))
        return;

    size_t firstRowY = 1u;
    size_t railOffset = (*semicolonState).isRailVisible ? (*semicolonState).railPanelWidth + 1u : 0u;
    size_t codeLeftX = railOffset + ((*semicolonState).lineNumberWidth + 3u) + 2u;
    size_t canvasWidth = TerminalCanvas_getWidth(terminalCanvas);
    size_t canvasHeight = TerminalCanvas_getHeight(terminalCanvas);
    if (codeLeftX >= canvasWidth || firstRowY >= canvasHeight)
        return;

    size_t visibleRowCount = SemicolonCode_visibleRows(canvasHeight);
    if (visibleRowCount > (*semicolonState).codeRowCount)
        visibleRowCount = (*semicolonState).codeRowCount;

    for (size_t rowIndex = 0u; rowIndex < visibleRowCount && firstLineIndex + rowIndex < (*semicolonState).fileLineCount; ++rowIndex) {
        size_t currentLineIndex = firstLineIndex + rowIndex;
        const char *lineText = (*semicolonState).fileLines[currentLineIndex];
        size_t lineLength = (*semicolonState).fileLineLengths[currentLineIndex];
        size_t byteOffset = SemicolonUtil_bytesForCells(lineText, lineLength, leftColumnIndex);
        size_t currentPixelX = codeLeftX;

        while (byteOffset < lineLength && currentPixelX < canvasWidth) {
            TerminalCanvasDecodedScalar decoded = TerminalCanvas_utf8Decode(lineText + byteOffset, lineLength - byteOffset);
            size_t cellWidth = decoded.bytes == 0u ? 1u : TerminalCanvas_widthOf(decoded.codepoint);
            TerminalPen syntaxPen = SemicolonSyntax_penAt((*semicolonState).syntaxHighlighter, currentLineIndex, byteOffset);

            for (size_t cellIndex = 0u; cellIndex < cellWidth && currentPixelX + cellIndex < canvasWidth; ++cellIndex) {
                size_t gridIndex = ((*terminalCanvas).y + firstRowY + rowIndex) * (*terminalCanvas).gridWidth
                                 + (*terminalCanvas).x + currentPixelX + cellIndex;
                if ((*terminalCanvas).fg != nullptr)
                    (*terminalCanvas).fg[gridIndex] = syntaxPen.fg;
                if ((*terminalCanvas).styles != nullptr)
                    (*terminalCanvas).styles[gridIndex] = syntaxPen.style;
            }
            currentPixelX += cellWidth;
            byteOffset += decoded.bytes == 0u ? 1u : decoded.bytes;
        }
    }

    if ((*semicolonState).actionSubPanel != nullptr && canvasWidth > codeLeftX && canvasHeight > firstRowY + 1u) {
        TerminalCanvas actionCanvas = TerminalCanvas_sub(terminalCanvas, codeLeftX, firstRowY,
                                                         canvasWidth - codeLeftX, canvasHeight - firstRowY - 1u);
        TerminalPanel_paint((*semicolonState).actionSubPanel, &actionCanvas);
    }
}

void SemicolonCode_paintMarkdownPreview(SemicolonState *semicolonState, TerminalCanvas *terminalCanvas) {
    if (semicolonState == nullptr || terminalCanvas == nullptr || !(*semicolonState).isMarkdownFile
        || !(*semicolonState).isMarkdownPreview || (*semicolonState).markdownView == nullptr)
        return;

    size_t canvasWidth = TerminalCanvas_getWidth(terminalCanvas);
    size_t canvasHeight = TerminalCanvas_getHeight(terminalCanvas);
    size_t contentX = ((*semicolonState).isRailVisible ? (*semicolonState).railPanelWidth : 0u) + 3u;
    size_t contentY = 1u + 3u;
    if (contentX >= canvasWidth || contentY >= canvasHeight || canvasHeight - contentY < 2u)
        return;

    size_t contentWidth = canvasWidth - contentX;
    size_t contentHeight = canvasHeight - contentY - 1u;
    TerminalCanvas contentCanvas = TerminalCanvas_sub(terminalCanvas, contentX, contentY, contentWidth, contentHeight);

    if (!TerminalMarkdownView_setViewport((*semicolonState).markdownView, contentWidth))
        return;
    (void) TerminalMarkdownView_setVisibleRows((*semicolonState).markdownView, contentHeight);
    TerminalCanvas_fill(&contentCanvas, ' ', TerminalPen());
    TerminalMarkdownView_render((*semicolonState).markdownView, &contentCanvas);
}

void SemicolonCode_paintCursor(SemicolonState *semicolonState, TerminalCanvas *terminalCanvas,
                               size_t firstLineIndex, size_t leftColumnIndex) {
    if (semicolonState == nullptr || terminalCanvas == nullptr || !(*semicolonState).isCursorVisible
        || ((*semicolonState).isMarkdownFile && (*semicolonState).isMarkdownPreview))
        return;
    if ((*semicolonState).cursorLineIndex < firstLineIndex)
        return;

    size_t relativeLineY = (*semicolonState).cursorLineIndex - firstLineIndex;
    if (relativeLineY >= SemicolonCode_visibleRows(TerminalCanvas_getHeight(terminalCanvas)))
        return;

    const char *lineBytes = (*semicolonState).fileLines[(*semicolonState).cursorLineIndex];
    size_t cursorCells = SemicolonUtil_cellsOf(lineBytes, (*semicolonState).cursorByteOffset);
    if (cursorCells < leftColumnIndex)
        return;

    size_t railOffset = (*semicolonState).isRailVisible ? (*semicolonState).railPanelWidth + 1u : 0u;
    size_t codeLeftX = railOffset + ((*semicolonState).lineNumberWidth + 3u) + 2u;
    size_t cursorPixelX = codeLeftX + (cursorCells - leftColumnIndex);
    size_t cursorPixelY = 1u + relativeLineY;

    if (cursorPixelX < TerminalCanvas_getWidth(terminalCanvas) && cursorPixelY < TerminalCanvas_getHeight(terminalCanvas)) {
        size_t gridIndex = ((*terminalCanvas).y + cursorPixelY) * (*terminalCanvas).gridWidth
                         + (*terminalCanvas).x + cursorPixelX;
        if ((*terminalCanvas).styles != nullptr)
            (*terminalCanvas).styles[gridIndex] |= TERMINAL_STYLE_REVERSE;
    }
}

TerminalPanel *createCodePanel(SemicolonState *semicolonState) {
    TerminalPanel *editorPanel = TerminalPanel();
    TerminalPanel *gutterPanel = TerminalPanel();
    TerminalPanel *rowsPanel = TerminalPanel();
    TerminalPanel *actionsPanel = TerminalPanel();
    TerminalPanel *codeAreaPanel = TerminalPanel();
    if (editorPanel == nullptr || gutterPanel == nullptr || rowsPanel == nullptr
        || actionsPanel == nullptr || codeAreaPanel == nullptr) {
        if (editorPanel != nullptr) TerminalPanel_free(editorPanel);
        if (gutterPanel != nullptr) TerminalPanel_free(gutterPanel);
        if (rowsPanel != nullptr) TerminalPanel_free(rowsPanel);
        if (actionsPanel != nullptr) TerminalPanel_free(actionsPanel);
        if (codeAreaPanel != nullptr) TerminalPanel_free(codeAreaPanel);
        return nullptr;
    }

    if (!TerminalPanel_setLayout(editorPanel, TERMINAL_PANEL_FLEX)
        || !TerminalPanel_setLayout(gutterPanel, TERMINAL_PANEL_FLEX)
        || !TerminalPanel_setLayout(rowsPanel, TERMINAL_PANEL_FLEX)
        || !TerminalPanel_setLayout(actionsPanel, TERMINAL_PANEL_FLEX)
        || !TerminalPanel_setLayout(codeAreaPanel, TERMINAL_PANEL_LAYERED)) {
        TerminalPanel_free(editorPanel);
        TerminalPanel_free(gutterPanel);
        TerminalPanel_free(rowsPanel);
        TerminalPanel_free(actionsPanel);
        TerminalPanel_free(codeAreaPanel);
        return nullptr;
    }

    for (size_t rowIndex = 0u; rowIndex < SEMICOLON_V2_MAX_ROWS; ++rowIndex) {
        TerminalText *numberTextView = TerminalText("");
        TerminalText *lineTextView = TerminalText("");
        if (numberTextView == nullptr || lineTextView == nullptr
            || !TerminalPanel_addText(gutterPanel, numberTextView,
                                      TerminalAnchor_2(TERMINAL_ANCHOR_BAND_TOP, 1u)).generation
            || !TerminalPanel_addText(rowsPanel, lineTextView,
                                      TerminalAnchor_2(TERMINAL_ANCHOR_BAND_TOP, 1u)).generation) {
            if (numberTextView != nullptr) TerminalText_free(numberTextView);
            if (lineTextView != nullptr) TerminalText_free(lineTextView);
            TerminalPanel_free(editorPanel);
            TerminalPanel_free(gutterPanel);
            TerminalPanel_free(rowsPanel);
            TerminalPanel_free(actionsPanel);
            TerminalPanel_free(codeAreaPanel);
            return nullptr;
        }
        (void) TerminalText_setForegroundColor(numberTextView, SEMICOLON_V2_GUTTER);
        (void) TerminalText_setForegroundColor(lineTextView, SEMICOLON_V2_TEXT);
        (*semicolonState).gutterRowTexts[rowIndex] = numberTextView;
        (*semicolonState).codeRowTexts[rowIndex] = lineTextView;
    }
    (*semicolonState).codeRowCount = SEMICOLON_V2_MAX_ROWS;

    TerminalPanel *hintRowPanel = TerminalPanel();
    TerminalPanel *modeRowPanel = TerminalPanel();
    if (hintRowPanel == nullptr || modeRowPanel == nullptr
        || !TerminalPanel_setLayout(hintRowPanel, TERMINAL_PANEL_FLEX)
        || !TerminalPanel_setLayout(modeRowPanel, TERMINAL_PANEL_FLEX)) {
        if (hintRowPanel != nullptr) TerminalPanel_free(hintRowPanel);
        if (modeRowPanel != nullptr) TerminalPanel_free(modeRowPanel);
        TerminalPanel_free(editorPanel);
        TerminalPanel_free(gutterPanel);
        TerminalPanel_free(rowsPanel);
        TerminalPanel_free(actionsPanel);
        TerminalPanel_free(codeAreaPanel);
        return nullptr;
    }

    const char *hintLabel = (*semicolonState).isNerdIcons ? "\uf071 off" : "! off";
    TerminalText *hintChip = TerminalText(hintLabel);
    (void) TerminalText_setBrackets(hintChip, '[', ']');
    (void) TerminalText_setForegroundColor(hintChip, SEMICOLON_V2_MUTED);
    (*semicolonState).hintChipText = hintChip;
    (void) TerminalPanel_addText(hintRowPanel, hintChip, TerminalAnchor_2(TERMINAL_ANCHOR_BAND_RIGHT, 9u));

    TerminalText *previewChip = TerminalText((*semicolonState).isMarkdownFile ? " Preview" : "");
    TerminalText *codeChip = TerminalText((*semicolonState).isMarkdownFile ? " Code" : "");
    if ((*semicolonState).isMarkdownFile) {
        (void) TerminalText_setBrackets(previewChip, '[', ']');
        (void) TerminalText_setBrackets(codeChip, '[', ']');
    }
    (void) TerminalText_setForegroundColor(previewChip, SEMICOLON_V2_MUTED);
    (void) TerminalText_setForegroundColor(codeChip, SEMICOLON_V2_MUTED);
    (*semicolonState).previewModeChipText = previewChip;
    (*semicolonState).codeModeChipText = codeChip;

    (void) TerminalPanel_addText(modeRowPanel, previewChip, TerminalAnchor_2(TERMINAL_ANCHOR_BAND_RIGHT, 11u));
    (void) TerminalPanel_addText(modeRowPanel, codeChip, TerminalAnchor_2(TERMINAL_ANCHOR_BAND_RIGHT, 9u));

    (void) TerminalPanel_addPanel(actionsPanel, hintRowPanel, TerminalAnchor_2(TERMINAL_ANCHOR_BAND_TOP, 1u));
    (void) TerminalPanel_addPanel(actionsPanel, modeRowPanel, TerminalAnchor_2(TERMINAL_ANCHOR_BAND_TOP, 1u));

    (void) TerminalPanel_addPanel(codeAreaPanel, rowsPanel, TerminalAnchor_0());
    (void) TerminalPanel_addPanel(codeAreaPanel, actionsPanel, TerminalAnchor_0());

    TerminalText *gapTextView = TerminalText("  ");
    (void) TerminalText_setForegroundColor(gapTextView, SEMICOLON_V2_TEXT);

    size_t gutterWidth = (*semicolonState).lineNumberWidth + 3u;
    (*semicolonState).gutterChildId = TerminalPanel_addPanel(
        editorPanel, gutterPanel, TerminalAnchor_2(TERMINAL_ANCHOR_BAND_LEFT, gutterWidth));
    (void) TerminalPanel_addText(editorPanel, gapTextView, TerminalAnchor_2(TERMINAL_ANCHOR_BAND_LEFT, 2u));
    (void) TerminalPanel_addPanel(editorPanel, codeAreaPanel, TerminalAnchor_0());

    (*semicolonState).gutterSubPanel = gutterPanel;
    (*semicolonState).actionSubPanel = actionsPanel;
    (*semicolonState).editorSubPanel = editorPanel;
    (*semicolonState).codePanel = editorPanel;
    if ((*semicolonState).isMarkdownFile)
        (void) SemicolonCode_setMarkdownMode(semicolonState, (*semicolonState).isMarkdownPreview);
    return editorPanel;
}

bool SemicolonCode_setMarkdownMode(SemicolonState *semicolonState, bool isPreviewMode) {
    if (semicolonState == nullptr || !(*semicolonState).isMarkdownFile)
        return false;
    (*semicolonState).isMarkdownPreview = isPreviewMode;
    if (isPreviewMode && (*semicolonState).markdownView != nullptr) {
        (void) TerminalMarkdownView_setScroll((*semicolonState).markdownView, (*semicolonState).editorScrollTop);
        (void) TerminalMarkdownView_setScrollX((*semicolonState).markdownView, (*semicolonState).editorScrollLeft);
    }
    if ((*semicolonState).codeModeChipText != nullptr) {
        (void) TerminalText_setForegroundColor(
            (*semicolonState).codeModeChipText,
            isPreviewMode ? SEMICOLON_V2_MUTED : SEMICOLON_V2_ACCENT);
        (void) TerminalText_setBackgroundColor(
            (*semicolonState).codeModeChipText,
            isPreviewMode ? TERMINAL_COLOR_DEFAULT : SEMICOLON_V2_SELECTION);
    }
    if ((*semicolonState).previewModeChipText != nullptr) {
        (void) TerminalText_setForegroundColor(
            (*semicolonState).previewModeChipText,
            isPreviewMode ? SEMICOLON_V2_ACCENT : SEMICOLON_V2_MUTED);
        (void) TerminalText_setBackgroundColor(
            (*semicolonState).previewModeChipText,
            isPreviewMode ? SEMICOLON_V2_SELECTION : TERMINAL_COLOR_DEFAULT);
    }
    return true;
}

bool SemicolonCode_handleActionClick(SemicolonState *semicolonState, size_t clickX, size_t clickY) {
    if (semicolonState == nullptr)
        return false;
    size_t totalWidth = (*semicolonState).terminalWidth;
    if (totalWidth == 0u)
        return false;
    if (clickY == 1u) {
        if (totalWidth >= 9u && clickX >= totalWidth - 9u && clickX < totalWidth) {
            return true;
        }
    }
    if (clickY == 2u && (*semicolonState).isMarkdownFile) {
        size_t previewStart = totalWidth >= 11u ? totalWidth - 11u : 0u;
        size_t codeStart = totalWidth >= 20u ? totalWidth - 20u : 0u;
        if (clickX >= codeStart && clickX < previewStart) {
            return SemicolonCode_setMarkdownMode(semicolonState, false);
        }
        if (clickX >= previewStart && clickX < totalWidth) {
            return SemicolonCode_setMarkdownMode(semicolonState, true);
        }
    }
    return false;
}
