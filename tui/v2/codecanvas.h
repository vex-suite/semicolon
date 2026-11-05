#ifndef SEMICOLON_V2_CODECANVAS_H
#define SEMICOLON_V2_CODECANVAS_H

#include "tui/v2/state.h"
#include "ui/panel/terminal_panel.h"
#include "ui/surface/terminal_canvas.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

TerminalPanel *createCodePanel(SemicolonState *semicolonState);

bool SemicolonCode_loadFile(SemicolonState *semicolonState, const char *filePath);
void SemicolonCode_syncVisibleRows(SemicolonState *semicolonState, size_t firstLineIndex,
                                   size_t leftColumnIndex, size_t visibleRowCount);
void SemicolonCode_paintSyntax(SemicolonState *semicolonState, TerminalCanvas *terminalCanvas,
                               size_t firstLineIndex, size_t leftColumnIndex);
void SemicolonCode_paintCursor(SemicolonState *semicolonState, TerminalCanvas *terminalCanvas,
                               size_t firstLineIndex, size_t leftColumnIndex);
void SemicolonCode_paintMarkdownPreview(SemicolonState *semicolonState, TerminalCanvas *terminalCanvas);
size_t SemicolonCode_visibleRows(size_t terminalHeight);
size_t SemicolonCode_lineNumberWidth(size_t lineCount);
bool SemicolonCode_isMarkdownFile(const char *fileName);
bool SemicolonCode_setMarkdownMode(SemicolonState *semicolonState, bool isPreviewMode);
bool SemicolonCode_handleActionClick(SemicolonState *semicolonState, size_t clickX, size_t clickY);

#endif
