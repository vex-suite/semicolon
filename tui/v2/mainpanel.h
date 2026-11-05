#ifndef SEMICOLON_V2_MAINPANEL_H
#define SEMICOLON_V2_MAINPANEL_H

#include "tui/v2/state.h"
#include "ui/panel/terminal_panel.h"
#include "ui/surface/terminal_canvas.h"
#include "events/terminal_input.h"

#include <stdbool.h>
#include <stddef.h>

TerminalPanel *createMainPanel(SemicolonState *semicolonState);
TerminalPanel *createHeaderPanel(SemicolonState *semicolonState);
void SemicolonMainPanel_setRailWidth(SemicolonState *semicolonState, size_t newRailWidth);
void SemicolonMainPanel_setRailVisible(SemicolonState *semicolonState, bool isVisible);
void SemicolonMainPanel_paintSplitter(SemicolonState *semicolonState, TerminalCanvas *terminalCanvas, bool isHovered);
void SemicolonHeader_syncTabs(SemicolonState *semicolonState);
bool SemicolonHeader_handleTabClick(SemicolonState *semicolonState, size_t clickX, TerminalPointerButton pointerButton);

#endif
