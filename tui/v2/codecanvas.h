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
void SemicolonCode_syncVisibleRows(SemicolonState *semicolonState, bytes firstLineIndex,
                                   bytes leftColumnIndex, bytes visibleRowCount);
void SemicolonCode_paintSyntax(SemicolonState *semicolonState, TerminalCanvas *terminalCanvas,
                               bytes firstLineIndex, bytes leftColumnIndex);
void SemicolonCode_paintCursor(SemicolonState *semicolonState, TerminalCanvas *terminalCanvas,
                               bytes firstLineIndex, bytes leftColumnIndex);
void SemicolonCode_paintMarkdownPreview(SemicolonState *semicolonState, TerminalCanvas *terminalCanvas);
bytes SemicolonCode_visibleRows(bytes terminalHeight);
bytes SemicolonCode_lineNumberWidth(bytes lineCount);
bool SemicolonCode_isMarkdownFile(const char *fileName);
bool SemicolonCode_setMarkdownMode(SemicolonState *semicolonState, bool isPreviewMode);
bool SemicolonCode_handleActionClick(SemicolonState *semicolonState, bytes clickX, bytes clickY);

#endif
