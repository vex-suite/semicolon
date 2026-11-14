#ifndef SEMICOLON_V2_FILETREE_H
#define SEMICOLON_V2_FILETREE_H

#include "vex.h"
#include "tui/v2/state.h"
#include "ui/panel/terminal_panel.h"
#include "ui/surface/terminal_canvas.h"


TerminalPanel *createRailPanel(SemicolonState *semicolonState);

bool SemicolonRail_scanDirectory(SemicolonState *semicolonState, const char *rootDirectory);
bool SemicolonRail_revealPath(SemicolonState *semicolonState, const char *filePath);
fn SemicolonRail_syncVisibleRows(SemicolonState *semicolonState, bytes visibleRowCount);
fn SemicolonRail_paintColors(SemicolonState *semicolonState, TerminalCanvas *terminalCanvas);
const char *SemicolonRail_getFileIcon(const char *fileName, bool isNerdIcons);
u32 SemicolonRail_getIconColor(u32 glyphCodepoint);
bytes SemicolonRail_visibleRows(bytes terminalHeight);
fn SemicolonRail_handlePointerClick(SemicolonState *semicolonState, bytes rowCoordinate);
bool SemicolonRail_readChildren(SemicolonState *semicolonState, TerminalTreeNodeId parentNodeId);
fn SemicolonRail_reloadGitignore(SemicolonState *semicolonState);

#endif
