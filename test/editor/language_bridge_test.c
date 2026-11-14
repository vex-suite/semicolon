// Offline host seam: the actual FileText/scene path consumes Language pens.
// Proves edit invalidation, wide/invalid UTF-8 cell mapping, scroll, action-layer
// preservation and caret position without acquiring a tty or approving appearance.
#define main semicolon_program_main
#include "semicolon/main/tui-main.c"
#undef main
#include <assert.h>

int main(int argc, char **argv) {
    assert(argc == 3);
    initTextures();
    gNerdIcons = false;
    (fn) snprintf(gOpenPath, sizeof gOpenPath, "%s", argv[1]);
    FileText file;
    assert(fileLoad(&file, argv[1]));
    assert(file.syntax != nullptr && file.syntaxPending && !file.syntaxCurrent);
    fileSyntaxRefresh(&file);
    assert(file.syntaxCurrent && !file.syntaxPending);
    assert(SemicolonSyntax_penAt(file.syntax, 0u, 0u).fg == 0x008000FFu);
    assert(SemicolonSyntax_penAt(file.syntax, 2u, 0u).fg == 0x808080FFu);
    Scene scene;
    gTerminalWidth = 112;
    sceneBuild(&scene, &file, 10u, 112, 12);
    assert(scene.root != nullptr);
    TerminalCanvas canvas = TerminalCanvas(gCells, gStyles, gFg, gBg, 112u, 12u);
    TerminalCanvas_fill(&canvas, ' ', TerminalPen());
    TerminalPanel_paint(scene.root, &canvas);
    u32 glyphs[112u * 12u];
    memcpy(glyphs, gCells, sizeof glyphs);
    bytes codeLeft = codeLeftColumn(scene.railWidth, scene.numberWidth);
    bytes y = editorFirstRow();
    // Snapshot the genuinely painted run/hint glyph attributes, not a guessed
    // replacement design. They must remain above the long first source line.
    u32 actionFg[112];
    u8 actionStyles[112];
    memcpy(actionFg, gFg + y * 112u, sizeof actionFg);
    memcpy(actionStyles, gStyles + y * 112u, sizeof actionStyles);
    paintEditorSyntax(&canvas, &file, &scene, 0u, 0u);
    assert(memcmp(glyphs, gCells, sizeof glyphs) == 0);
    assert(TerminalCanvas_fgAt(&canvas, codeLeft, y) == 0x008000FFu);
    assert(TerminalCanvas_fgAt(&canvas, codeLeft, y + 2u) == 0x808080FFu);
    assert(TerminalCanvas_glyphAt(&canvas, codeLeft + 1u, y + 3u) == 0x754Cu);
    assert(TerminalCanvas_glyphAt(&canvas, codeLeft + 2u, y + 3u) == 0u);
    assert(TerminalCanvas_fgAt(&canvas, codeLeft + 2u, y + 3u) == 0xA0F5A0FFu);
    assert(TerminalCanvas_fgAt(&canvas, codeLeft + 5u, y + 3u) == 0x9CDCFEFFu);
    assert(TerminalCanvas_fgAt(&canvas, codeLeft + 3u, y + 4u) == 0x008000FFu);
    for (bytes x = 112u - TUI_HINT_CHIP_CELLS - TUI_RUN_CHIP_CELLS; x < 112u; ++x) {
        if (actionFg[x] != COL_TEXT) {
            assert(gFg[y * 112u + x] == actionFg[x]);
            assert(gStyles[y * 112u + x] == actionStyles[x]);
        }
    }
    file.cursorVisible = true;
    file.cursorLine = 0u;
    file.cursorByte = 4u;
    paintEditorCursor(&canvas, &file, 0u, 0u, scene.railWidth, scene.numberWidth);
    assert((TerminalCanvas_styleAt(&canvas, codeLeft + 4u, y) & TERMINAL_STYLE_REVERSE) != 0u);
    assert(editorPlaceCaret(&file, codeLeft + 5u, y, 0u, 0u, codeLeft, &scene));
    assert(file.cursorLine == 0u && file.cursorByte == 5u);

    // Scrolling reuses admitted analysis; the next visible line starts inside a
    // multiline comment. Horizontal offset is source cells, not span-relative.
    SemicolonSyntax *retained = file.syntax;
    sceneSyncRows(&scene, &file, 2u, 2u, 10u);
    TerminalCanvas_fill(&canvas, ' ', TerminalPen());
    TerminalPanel_paint(scene.root, &canvas);
    paintEditorSyntax(&canvas, &file, &scene, 2u, 2u);
    assert(file.syntax == retained && !file.syntaxPending);
    assert(TerminalCanvas_fgAt(&canvas, codeLeft, y) == 0x808080FFu);

    // Removing the opening comment changes a later line's classification only
    // after cold refresh; the intervening stale analysis must never be painted.
    file.cursorLine = 1u;
    file.cursorByte = 0u;
    assert(fileDeleteForward(&file));
    assert(file.syntaxPending && !file.syntaxCurrent);
    fileSyntaxRefresh(&file);
    assert(file.syntaxCurrent);
    assert(SemicolonSyntax_penAt(file.syntax, 2u, 0u).fg == 0x9CDCFEFFu);
    // Insertion, split, backspace/join and direct line refresh invalidate the
    // same cache; they do not depend on timestamps or file-save state.
    assert(fileInsertBytes(&file, 1u, 0u, "/", 1u));
    assert(file.syntaxPending);
    fileSyntaxRefresh(&file);
    assert(SemicolonSyntax_penAt(file.syntax, 2u, 0u).fg == 0x808080FFu);
    assert(fileSplit(&file));
    assert(file.syntaxPending && !file.syntaxCurrent);
    fileSyntaxRefresh(&file);
    assert(fileBackspace(&file));
    fileSyntaxRefresh(&file);
    assert(file.syntaxCurrent);
    // Inject a cold extent refusal without exhausting host memory. A failed
    // replacement disables old ink, preserves glyphs, then recovers on an edit.
    bytes oldLength = file.lineLengths[0];
    file.lineLengths[0] = SIZE_MAX;
    file.syntaxPending = true;
    fileSyntaxRefresh(&file);
    file.lineLengths[0] = oldLength;
    assert(!file.syntaxCurrent && !file.syntaxPending);
    TerminalCanvas_fill(&canvas, ' ', TerminalPen());
    sceneSyncRows(&scene, &file, 0u, 0u, 10u);
    TerminalPanel_paint(scene.root, &canvas);
    u32 oldInk = TerminalCanvas_fgAt(&canvas, codeLeft, y);
    paintEditorSyntax(&canvas, &file, &scene, 0u, 0u);
    assert(TerminalCanvas_fgAt(&canvas, codeLeft, y) == oldInk);
    assert(fileInsertBytes(&file, 0u, 0u, " ", 1u));
    fileSyntaxRefresh(&file);
    assert(file.syntaxCurrent);
    sceneFree(&scene);
    fileFree(&file);
    // Non-C23 files retain the original plain/Markdown behavior.
    assert(fileLoad(&file, argv[2]));
    assert(file.syntax == nullptr && !file.syntaxPending);
    fileSyntaxRefresh(&file);
    fileFree(&file);
    return 0;
}
