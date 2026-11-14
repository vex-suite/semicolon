#ifndef SEMICOLON_V2_BOTTOMPART_H
#define SEMICOLON_V2_BOTTOMPART_H

#include "vex.h"
#include "tui/v2/state.h"
#include "ui/panel/terminal_panel.h"


TerminalPanel *createFooterPanel(SemicolonState *semicolonState);

fn SemicolonFooter_updateBreadcrumb(SemicolonState *semicolonState, const char *filePath);
fn SemicolonFooter_updateClock(SemicolonState *semicolonState);
fn SemicolonFooter_updateCursor(SemicolonState *semicolonState, bytes lineNumber, bytes columnNumber);

#endif
