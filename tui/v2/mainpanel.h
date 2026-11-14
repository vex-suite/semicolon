#ifndef SEMICOLON_V2_MAINPANEL_H
#define SEMICOLON_V2_MAINPANEL_H

#include "vex.h"
#include "tui/v2/state.h"
#include "ui/panel/terminal_panel.h"
#include "ui/surface/terminal_canvas.h"
#include "events/terminal_input.h"


TerminalPanel *createMainPanel(SemicolonState *semicolonState);
TerminalPanel *createHeaderPanel(SemicolonState *semicolonState);
fn SemicolonMainPanel_setRailWidth(SemicolonState *semicolonState, bytes newRailWidth);
fn SemicolonMainPanel_setRailVisible(SemicolonState *semicolonState, bool isVisible);
fn SemicolonMainPanel_paintSplitter(SemicolonState *semicolonState, TerminalCanvas *terminalCanvas, bool isHovered);
fn SemicolonHeader_syncTabs(SemicolonState *semicolonState);
bool SemicolonHeader_handleTabClick(SemicolonState *semicolonState, bytes clickX, TerminalPointerButton pointerButton);

#endif
