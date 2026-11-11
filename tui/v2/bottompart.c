#include "tui/v2/bottompart.h"
#include "tui/v2/palette.h"
#include "ui/text/terminal_breadcrumb.h"
#include "format/terminal_style.h"
#include "annotation/annotation.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

;;DEFINITION
/** Semicolon v2 status footer and breadcrumb bar.
 * Consumes Determinant's TerminalBreadcrumb for bounded path hierarchy display
 * and renders modal badges alongside system chips. */
;;OVERVIEW
/** ========================================================================
 * CLASS: SemicolonFooter (suite/projects/semicolon/src/tui/v2/bottompart.c)
 * OWNS: footer label text objects and status chip updates.
 * BORROWS: SemicolonState, Determinant TerminalPanel, TerminalText, TerminalBreadcrumb.
 * CONVENTIONS: Descriptive cameled identifiers, zero -> operators, Memory_alloc.
 * ======================================================================== */

void SemicolonFooter_updateBreadcrumb(SemicolonState *semicolonState, const char *filePath) {
    if (semicolonState == nullptr || filePath == nullptr)
        return;

    TerminalBreadcrumb breadcrumbHelper = TerminalBreadcrumb_4(
        (*semicolonState).railRootDirectory, filePath, " › ", 44u);
    (void) TerminalBreadcrumb_format(&breadcrumbHelper, (*semicolonState).breadcrumbBuffer,
                                     sizeof (*semicolonState).breadcrumbBuffer);

    if ((*semicolonState).breadcrumbText != nullptr) {
        (void) TerminalText_setText((*semicolonState).breadcrumbText,
                                    (*semicolonState).breadcrumbBuffer,
                                    strlen((*semicolonState).breadcrumbBuffer));
    }
}

void SemicolonFooter_updateClock(SemicolonState *semicolonState) {
    if (semicolonState == nullptr)
        return;
    time_t rawClockTime = time(nullptr);
    struct tm *localCalendarTime = localtime(&rawClockTime);
    if (localCalendarTime != nullptr) {
        (void) snprintf((*semicolonState).timeChipBuffer, sizeof (*semicolonState).timeChipBuffer,
                        "%02d:%02d:%02d",
                        (*localCalendarTime).tm_hour,
                        (*localCalendarTime).tm_min,
                        (*localCalendarTime).tm_sec);
    } else {
        (void) snprintf((*semicolonState).timeChipBuffer, sizeof (*semicolonState).timeChipBuffer, "--:--:--");
    }
    if ((*semicolonState).timeChipText != nullptr) {
        (void) TerminalText_setText((*semicolonState).timeChipText,
                                    (*semicolonState).timeChipBuffer,
                                    strlen((*semicolonState).timeChipBuffer));
    }
}

void SemicolonFooter_updateCursor(SemicolonState *semicolonState, bytes lineNumber, bytes columnNumber) {
    if (semicolonState == nullptr)
        return;
    (void) snprintf((*semicolonState).lineChipBuffer, sizeof (*semicolonState).lineChipBuffer,
                    "ln:%zu:%zu", lineNumber, columnNumber);
    if ((*semicolonState).lineChipText != nullptr) {
        (void) TerminalText_setText((*semicolonState).lineChipText,
                                    (*semicolonState).lineChipBuffer,
                                    strlen((*semicolonState).lineChipBuffer));
    }
}

TerminalPanel *createFooterPanel(SemicolonState *semicolonState) {
    TerminalPanel *footerPanel = TerminalPanel();
    if (footerPanel == nullptr)
        return nullptr;
    if (!TerminalPanel_setLayout(footerPanel, TERMINAL_PANEL_FLEX)) {
        TerminalPanel_free(footerPanel);
        return nullptr;
    }

    TerminalText *codeBadge = TerminalText("();");
    TerminalText *agentsBadge = TerminalText(">w<");
    TerminalText *spacerOne = TerminalText(" ");
    if (codeBadge == nullptr || agentsBadge == nullptr || spacerOne == nullptr) {
        if (codeBadge != nullptr) TerminalText_free(codeBadge);
        if (agentsBadge != nullptr) TerminalText_free(agentsBadge);
        if (spacerOne != nullptr) TerminalText_free(spacerOne);
        TerminalPanel_free(footerPanel);
        return nullptr;
    }

    (void) TerminalText_setBrackets(codeBadge, '[', ']');
    (void) TerminalText_setForegroundColor(codeBadge, SEMICOLON_V2_ACCENT);
    (void) TerminalText_setBackgroundColor(codeBadge, SEMICOLON_V2_SELECTION);

    (void) TerminalText_setBrackets(agentsBadge, '[', ']');
    (void) TerminalText_setForegroundColor(agentsBadge, SEMICOLON_V2_MUTED);

    (void) TerminalText_setForegroundColor(spacerOne, SEMICOLON_V2_TEXT);

    (*semicolonState).codeModeBadgeText = codeBadge;
    (*semicolonState).agentsModeBadgeText = agentsBadge;

    (void) TerminalPanel_addText(footerPanel, codeBadge, TerminalAnchor_2(TERMINAL_ANCHOR_BAND_LEFT, 5u));
    (void) TerminalPanel_addText(footerPanel, agentsBadge, TerminalAnchor_2(TERMINAL_ANCHOR_BAND_LEFT, 5u));
    (void) TerminalPanel_addText(footerPanel, spacerOne, TerminalAnchor_2(TERMINAL_ANCHOR_BAND_LEFT, 1u));

    const char *openPathPointer = (*semicolonState).openFilePath[0] != '\0'
                                ? (*semicolonState).openFilePath
                                : (*semicolonState).openFileName;
    TerminalBreadcrumb initialBreadcrumb = TerminalBreadcrumb_4(
        (*semicolonState).railRootDirectory, openPathPointer, " › ", 44u);
    (void) TerminalBreadcrumb_format(&initialBreadcrumb, (*semicolonState).breadcrumbBuffer,
                                     sizeof (*semicolonState).breadcrumbBuffer);

    TerminalText *crumbText = TerminalText((*semicolonState).breadcrumbBuffer);
    if (crumbText != nullptr) {
        (void) TerminalText_setBrackets(crumbText, '[', ']');
        (void) TerminalText_setForegroundColor(crumbText, SEMICOLON_V2_TEXT);
        (*semicolonState).breadcrumbText = crumbText;
        (void) TerminalPanel_addText(footerPanel, crumbText, TerminalAnchor_2(TERMINAL_ANCHOR_BAND_LEFT, 44u));
    }

    TerminalText *encodingChip = TerminalText("utf-8");
    TerminalText *spacerRightOne = TerminalText(" ");
    (void) snprintf((*semicolonState).timeChipBuffer, sizeof (*semicolonState).timeChipBuffer, "--:--:--");
    TerminalText *timeChip = TerminalText((*semicolonState).timeChipBuffer);
    TerminalText *spacerRightTwo = TerminalText(" ");
    (void) snprintf((*semicolonState).lineChipBuffer, sizeof (*semicolonState).lineChipBuffer, "ln:1:1");
    TerminalText *lineChip = TerminalText((*semicolonState).lineChipBuffer);
    TerminalText *spacerRightThree = TerminalText(" ");

    const char *slashPointer = strrchr((*semicolonState).railRootDirectory, '/');
    const char *rootBaseName = slashPointer != nullptr ? slashPointer + 1u : (*semicolonState).railRootDirectory;
    if (*rootBaseName == '\0')
        rootBaseName = "root";
    (void) snprintf((*semicolonState).projectChipBuffer, sizeof (*semicolonState).projectChipBuffer, "%s", rootBaseName);
    TerminalText *projectChip = TerminalText((*semicolonState).projectChipBuffer);

    if (encodingChip != nullptr) {
        (void) TerminalText_setBrackets(encodingChip, '[', ']');
        (void) TerminalText_setForegroundColor(encodingChip, SEMICOLON_V2_MUTED);
        (*semicolonState).encodingChipText = encodingChip;
        (void) TerminalPanel_addText(footerPanel, encodingChip, TerminalAnchor_2(TERMINAL_ANCHOR_BAND_RIGHT, 7u));
    }
    if (spacerRightOne != nullptr)
        (void) TerminalPanel_addText(footerPanel, spacerRightOne, TerminalAnchor_2(TERMINAL_ANCHOR_BAND_RIGHT, 1u));

    if (timeChip != nullptr) {
        (void) TerminalText_setBrackets(timeChip, '[', ']');
        (void) TerminalText_setForegroundColor(timeChip, SEMICOLON_V2_MUTED);
        (*semicolonState).timeChipText = timeChip;
        (void) TerminalPanel_addText(footerPanel, timeChip, TerminalAnchor_2(TERMINAL_ANCHOR_BAND_RIGHT, 10u));
    }
    if (spacerRightTwo != nullptr)
        (void) TerminalPanel_addText(footerPanel, spacerRightTwo, TerminalAnchor_2(TERMINAL_ANCHOR_BAND_RIGHT, 1u));

    if (lineChip != nullptr) {
        (void) TerminalText_setBrackets(lineChip, '[', ']');
        (void) TerminalText_setForegroundColor(lineChip, SEMICOLON_V2_MUTED);
        (*semicolonState).lineChipText = lineChip;
        (void) TerminalPanel_addText(footerPanel, lineChip, TerminalAnchor_2(TERMINAL_ANCHOR_BAND_RIGHT, 12u));
    }
    if (spacerRightThree != nullptr)
        (void) TerminalPanel_addText(footerPanel, spacerRightThree, TerminalAnchor_2(TERMINAL_ANCHOR_BAND_RIGHT, 1u));

    if (projectChip != nullptr) {
        (void) TerminalText_setBrackets(projectChip, '[', ']');
        (void) TerminalText_setForegroundColor(projectChip, SEMICOLON_V2_MUTED);
        (*semicolonState).projectChipText = projectChip;
        (void) TerminalPanel_addText(footerPanel, projectChip, TerminalAnchor_2(TERMINAL_ANCHOR_BAND_RIGHT, 16u));
    }

    SemicolonFooter_updateClock(semicolonState);
    (*semicolonState).footerPanel = footerPanel;
    return footerPanel;
}
