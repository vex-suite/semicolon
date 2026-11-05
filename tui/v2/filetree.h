#ifndef SEMICOLON_V2_FILETREE_H
#define SEMICOLON_V2_FILETREE_H

#include "tui/v2/state.h"
#include "ui/panel/terminal_panel.h"
#include "ui/surface/terminal_canvas.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

TerminalPanel *createRailPanel(SemicolonState *semicolonState);

bool SemicolonRail_scanDirectory(SemicolonState *semicolonState, const char *rootDirectory);
bool SemicolonRail_revealPath(SemicolonState *semicolonState, const char *filePath);
void SemicolonRail_syncVisibleRows(SemicolonState *semicolonState, size_t visibleRowCount);
void SemicolonRail_paintColors(SemicolonState *semicolonState, TerminalCanvas *terminalCanvas);
const char *SemicolonRail_getFileIcon(const char *fileName, bool isNerdIcons);
uint32_t SemicolonRail_getIconColor(uint32_t glyphCodepoint);
size_t SemicolonRail_visibleRows(size_t terminalHeight);
void SemicolonRail_handlePointerClick(SemicolonState *semicolonState, size_t rowCoordinate);
bool SemicolonRail_readChildren(SemicolonState *semicolonState, TerminalTreeNodeId parentNodeId);
void SemicolonRail_reloadGitignore(SemicolonState *semicolonState);

#endif
