#ifndef SEMICOLON_V2_BOTTOMPART_H
#define SEMICOLON_V2_BOTTOMPART_H

#include "tui/v2/state.h"
#include "ui/panel/terminal_panel.h"

#include <stdbool.h>
#include <stddef.h>

TerminalPanel *createFooterPanel(SemicolonState *semicolonState);

void SemicolonFooter_updateBreadcrumb(SemicolonState *semicolonState, const char *filePath);
void SemicolonFooter_updateClock(SemicolonState *semicolonState);
void SemicolonFooter_updateCursor(SemicolonState *semicolonState, size_t lineNumber, size_t columnNumber);

#endif
