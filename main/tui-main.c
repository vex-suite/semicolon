#define XOPEN_SOURCE_EXTENDED 1

#include "vex.h"
#include "ui/panel/terminal_panel.h"
#include "ui/text/terminal_text.h"
#include "ui/panel/terminal_anchor.h"
#include "ui/surface/terminal_canvas.h"
#include "format/terminal_color.h"
#include "format/terminal_style.h"
#include "ui/surface/terminal_surface.h"
#include "ui/tree/terminal_tree_view.h"
#include "events/terminal_input.h"
#include "ui/markdown/terminal_markdown_view.h"
#include "ui/scroll/terminal_scroll_glide.h"
#include "re.h"
#include "oop/oop.h"
#include "editor/semicolon_syntax.h"
#include "icon/icon_catalog.h"

#include <locale.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <fnmatch.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <strings.h>
#include <termios.h>
#include <unistd.h>
#include <ncurses.h>

extern char **environ;

;;DEFINITION
/** The semicolon IDE shell: the design from src/tui/tui_example.txt, drawing a
 * real file on a real terminal, with the rail draggable and the editor scrolling.
 *
 * This file is the HOST, not the toolkit. Determinant owns cells, placement,
 * panels, text and the mouse byte grammar and knows nothing about a tty;
 * acquiring the terminal, owning the alternate screen, deciding which mouse
 * events exist, reading a file and driving the whole thing are this program's
 * job. That is why ncurses is named only here.
 *
 * Two things are worth reading this file for.
 *
 * First, SCROLLING IS FREE BECAUSE TEXT IS ADDRESSABLE. The editor's rows are
 * created once and then re-pointed at different lines each frame with
 * TerminalText_setText. No row is allocated, freed or rebuilt while scrolling,
 * which is the entire argument for a text being an object rather than a string
 * handed to a draw call.
 *
 * Second, THE DIVIDER IS DRAGGED BY MOVING AN ANCHOR. The rail's width is not
 * rebuilt into the tree when the pointer moves; the same layer gets a new
 * extent through TerminalPanel_setAnchor, and the next paint lays out around it.
 * Resizing a pane and scrolling a file are therefore both ordinary state changes,
 * not scene surgery.
 *
 * Nothing is transparent-by-accident: no panel sets a background, so every cell
 * the app does not paint keeps the terminal's own background. That is deliberate
 * — the theme belongs to whoever owns the terminal.
 *
 * The project rail is read from the filesystem, and every visible editor tab
 * owns a real file buffer with its own UTF-8 caret. Selecting a tab restores that
 * buffer; clicking source places the caret. Tabs close from their Nerd-font X or
 * middle-click. Markdown files have a rendered terminal preview and a source
 * toggle. Code view edits UTF-8 rows and attempts a staged save one second after
 * the last edit; a failure keeps the dirty buffer resident. Every mutation is a
 * bounded snapshot transaction: Ctrl-Z restores the pre-edit document, Ctrl-Y
 * reapplies it, and a refused edit leaves both the text and the redo path intact.
 * The console is a
 * separate centre-panel presentation, not source text: a fixed ring keeps the
 * last 10,000 bounded lines, and a finished child leaves a scrollable snapshot
 * until its tab closes. The rail and file buffers outlive that snapshot.
 *
 * C23 code coloring consumes Language through SemicolonSyntax: a copied complete
 * document snapshot is refreshed cold after edits, not during scrolling or cell
 * paint. It changes cell ink/weight only, never source bytes, caret coordinates,
 * glyphs or saved text. .c/.h explicitly select the initial C23-only host mode;
 * identifiers are classified by Language's conservative C23Highlights pass, with
 * no diagnostic or hint integration yet. */
;;OVERVIEW
/** ============================================================================
 * MODULE: semicolon terminal entry (src/main/tui-main.c)
 * ============================================================================
 * OWNED STATE (file-local):
 *   gQuit         signal-safe shutdown request only.
 *   gCells/gStyles/gFg/gBg          static canvas planes, TUI_MAX_W x TUI_MAX_H.
 *   gFrontCells/...                 static last-paint mirror for the surface diff.
 *   gRule/gSpaces/emptyLine         static textures: separators and blank runs.
 *   gGutter[TUI_MAX_ROWS][16]       per-row line-number text buffers.
 *   gLineChip[16]                   the status bar's line-position text buffer.
 *   gSaveLabel[16]                  saved/dirty/error status text.
 *   gOut/gOutLength/gOutFailed       the frame byte buffer (one synchronized write).
 *   gCursorColumn/gCursorRow/gPen    the device's tracked cursor and pen.
 *   gRailLocation[PATH_MAX + 2] —   canonical project location, HOME abbreviated
 *                                   only on a complete path-component match; painted gray beside the pinned root.
 *   gConsoleLines[10000][2048],
 *   gConsoleLengths[10000]        — bounded line ring;
 *                                   head/count/top/follow track retained
 *                                   and viewed output independently of
 *                                   editor scroll, omitted/clipped counts make both
 *                                   limits observable.
 *   gRunPid/gRunOutputFd — supervised child and nonblocking pipe; a finished
 *     child may still have pipe bytes to drain before the console is retired.
 *   gRunStatus/gRunOutcome — bounded, retained waitpid result for the tab and
 *     console heading; an unavailable result is never called exit zero.
 * STRUCT FIELDS:
 *   FileText (the open document, one heap-owned NUL-terminated row per line):
 *     char **lineStorage — owned editable UTF-8 rows.
 *     bytes length — serialized byte count including line endings.
 *     const char **lines — borrowed aliases to rows for rendering and Markdown.
 *     bytes *lineLengths — UTF-8 bytes excluding line endings.
 *     bytes *lineCapacities — allocated bytes per row including NUL.
 *     u8 *lineEndings — NONE/LF/CRLF serialization marker per row.
 *     bytes lineCount, lineCapacity — live rows and growable directory capacity.
 *     bytes longestCells, longestLine — horizontal extent and its row index.
 *     bytes cursorLine, cursorByte — caret position; byte offset is a scalar boundary.
 *     bool cursorVisible — paint the reverse-style caret after keyboard interaction.
 *     u64 lastEditNs — monotonic time of last document mutation.
 *     bool dirty, saveFailed, baselineValid — pending, refused and identity states.
 *     struct stat baseline — original file identity/size/mtime for conflict detection.
 *     char name[64] — breadcrumb display basename.
 *     SemicolonSyntax *syntax — owned copied C23 analysis, travels with the tab.
 *     bool syntaxPending, syntaxCurrent — refresh requested / matches live bytes.
 *     FileHistory history — bounded snapshot undo/redo, owned with this buffer.
 *   FileSnapshot (one owned document state): char **lineStorage, const char **lines,
 *     bytes *lineLengths, *lineCapacities, u8 *lineEndings — full owned row
 *     copies; bytes lineCount/lineCapacity — exact snapshot geometry; bytes
 *     cursorLine/cursorByte — restored caret; u64 revision — document identity;
 *     bool valid — capture completed (a refused capture owns nothing).
 *   FileSnapshotStack: FileSnapshot *items, bytes count/capacity — growable stack,
 *     capped at TUI_HISTORY_LIMIT with the oldest entry dropped.
 *   FileHistory: FileSnapshotStack undo/redo — committed past and future states;
 *     FileSnapshot pending; bool pendingValid — the pre-edit capture awaiting commit
 *     or cancellation; u64 currentRevision/nextRevision/savedRevision — dirty
 *     identity, so undo after save knows the text differs from disk.
 *   TuiRow:
 *     TerminalText *gutter — line number object.
 *     TerminalText *code — source text object.
 *   Scene:
 *     TerminalPanel *root, *header, *rail — retained component tree.
 *     TerminalPanel *editor — borrowed child of centre, for live gutter geometry.
 *     TerminalPanel *codeActions — borrowed code overlay; repainted after ink projection.
 *     TerminalChildId railId, gutterId, editorId — child identities in their
 *       owning panels; editorId collapses while the console owns the centre.
 *     bool editorPanelVisible — last admitted editor anchor mode, changed only
 *       when switching between file and console tabs.
 *     bytes modeWidth — top-row mode band.
 *     TuiRow rows[TUI_MAX_ROWS]; bytes rowCount — fixed visible code-row objects.
 *     TerminalText *lineChip, *saveChip — footer viewport and save-state labels.
 *     bytes railWidth, numberWidth — current layout geometry; initial rail width is
 *       TUI_RAIL_DEFAULT_WIDTH (50 cells).
 *     TerminalText *railRows[TUI_MAX_ROWS]; bytes railRowCount — visible rail rows.
 *     char railText[TUI_MAX_ROWS][TUI_RAIL_TEXT] — storage borrowed by those rows.
 *     TerminalText *breadcrumb, *activeTab — current file identity labels.
 *     TerminalText *languageHints, *runHint — code-mode action hints.
 *     TerminalText *runTab; TerminalChildId runTabId; char runTabText[160],
 *       runTabShown[160] —
 *       persistent backing and closeable header view for the captured child console.
 *     TerminalText *codeMode, *previewMode — icon-and-label Markdown mode controls.
 *     TerminalMarkdownView *markdown — borrowed-source renderer instance.
 *     bool markdownFile, preview — file kind and active presentation mode.
 *     bytes codeModeWidth, previewModeWidth, markdownModeGap — control geometry.
 *     bytes copiedBlock; u64 copiedUntilNs — the fence whose check glyph
 *       is showing, and when that feedback lapses.
 *     char markdownNotice[48]; u64 markdownNoticeUntilNs — transient copy notice.
 *   RailEntry slot record: char label[TUI_RAIL_TEXT] (tree-borrowed name);
 *     char path[1024] (opened path); TerminalTreeNodeId parent (tree parent);
 *     bool loaded (children were read once); bool folder (directory classification).
 *   Interaction: TerminalInput input (mouse decoder); bool grabbing, tracking,
 *     pressed, havePointer (gesture state); bytes trackX, trackLeft, grabX,
 *     grabWidth, pointerX, pointerY (gesture origin and last pointer cell).
 *   Scroll motion: borrowed from Determinant's TerminalScrollGlide
 *     (ui/scroll/terminal_scroll_glide.h); this host owns the file bounds.
 *   DirEntry slot record: char name[TUI_RAIL_TEXT] (sorted entry name);
 *     bool folder (directory classification).
 *   ScrollStep slot record: bool keyed (recognized navigation key);
 *     int rows, columns (viewport delta).
 *   IgnorePattern SLOT RECORD: char *text (owned glob without leading slash or
 *     trailing directory slash); bool negated (last-match re-inclusion); bool
 *     directoryOnly (rule was written with a trailing slash).
 *   Ignore-pattern registry: IgnorePattern *gIgnorePatterns, bytes
 *     gIgnorePatternCount, gIgnorePatternCapacity (growable parsed root rules);
 *     struct timespec gIgnoreModified, bool gIgnoreStatValid, bool gIgnoreFromOpen
 *     (disk revision and current source); char gIgnorePath[1024], u64
 *     gOpenIgnoreHash (source identity and live-buffer revision hash).
 * EditorTab SLOT RECORD:
 *   char path[1024] — opened file identity.
 *   char label[128], display[160], shown[160] — name, Nerd-X label, clipped label.
 *   FileText suspended — owned buffer while this tab is inactive; active buffer is
 *     held at the stable runTerminal FileText address.
 *   bytes x, width — last admitted chrome-row hit rectangle in terminal cells.
 *   bool preview — that tab's Markdown mode.
 *   TerminalText *view; TerminalChildId child — borrowed chrome child identity.
 *   bool gCachedRailCustom, gRailWidthCustomized — only a width chosen by dragging
 *     overrides the 40-cell default; legacy cached defaults are ignored.
 * ICON SETTINGS:
 *    bool gNerdIcons = true — current user setting, overridden by environment/CLI.
 *   EditorTab *gTabs, gTabCount, gTabCapacity — growable open-file tab stack.
 *   gTabFirst / gTabReveal — whole-tab viewport and one-shot selected-tab reveal;
 *     hidden tabs have zero geometry, clipped labels retain their close control.
 *   gRailColumn / gRailMaxColumn — bounded horizontal child-list viewport;
 *     the pinned project/location header never pans with child rows.
 *   bytes gActiveTabIndex — active tab identity in that stack.
 *   static const char *fileIcon(const char *name) — exact build names, then folded
 *     extensions, then generic file; borrowed static UTF-8, no IO or allocation.
 *   static bool setIconMode(const char *mode) — admit nerd/plain or THROW and retain
 *     the previous setting. Environment is read first; explicit CLI overrides it.
 *   static u32 iconColor(u32 glyph) — fixed palette for icon glyphs only;
 *     names do not inherit it.
 *   static bytes railIndent(TerminalTreeRow, bytes) — shared indent geometry.
 *   static fn paintRailColors(Scene *, TerminalCanvas *, ...) — paint icon palette
 *     and orange foreground for root-.gitignore matches after layout. The root
 *     stays on row zero beside its clipped gray location while children scroll;
 *     body row y maps to tree scrollRow + y, independent of editor/tab geometry. 
 *     Child indentation advances two spaces per level (six levels maximum).
 *   static bool visibleDotEntry(name, folder) — admits only .github, .gitignore,
 *     and .gitkeep; other dot entries stay hidden.
 *   static bool ignorePatternAdd(line) / fn ignorePatternsClear/Reload()
 *     parse root .gitignore text, preferring its active editor buffer; disk changes
 *     are detected by mtime.
 *   static bool ignorePatternMatches(pattern, relative) / railEntryIgnored(entry)  
 *     apply ordered component globs, negation, and directory rules to visible rows;
 *     this never consults Git status
 *   Folder open/closed icons are selected from the visible row's expansion state;
 *   plain mode uses [] folders and - files. No automatic font detection is claimed.
 *   Tree labels stay white under the pointer unless root-.gitignore matches them;
 *     matching visible names are orange, while selection background stays.
 * FUNCTION REGISTRY:
 * ----------------------------------------------------------------------------
 * Private helpers:
 *   static fn onSignal(int)
 *   static fn initTextures()
 *   static bool fileLoad(FileText *, const char *)
 *   static fn fileFree(FileText *)
 *   static bool fileSave(FileText *, const char *)   staged atomic replacement + conflict check
 *   static FileSnapshot fileSnapshot(const FileText *) / fn fileSnapshotFree(FileSnapshot *)
 *   static bool fileSnapshotStackReserve(...) / fn fileSnapshotStackPush/Clear/Free(...)
 *   static bool fileHistoryBegin/Commit/Cancel(...) / fn fileHistoryFree/Reset(...)
 *   static fn fileHistoryRestore(FileText *, FileSnapshot *) / bool fileUndo/fileRedo(...)
 *   static bool editorChange(FileText *, bool (*)(FileText *)) / editorInsertText(...)
 *   static bool editorInsertUtf8(FileText *, int) / bytes utf8SequenceLength(int)
 *   static bool fileReserveLines/fileAddLine/fileReserveLine(...)
 *   static bool fileInsertBytes/fileSplit/fileJoinPrevious/fileBackspace/fileDeleteForward(...)
 *   static fn fileMoveLeft/fileMoveRight/fileMoveVertical(...)
 *   static u64 monotonicNowNs() / bool autosave(FileText *, const char *)
 *   static bool writeAll(int, const char *, bytes) / statMatches(stat, stat)
 *   static fn setSaveStatus(Scene *, const FileText *)
 *   static fn keepCursorVisible(FileText *, TerminalScrollGlide *, ...) / paintEditorCursor(...)
 *   static fn fileSyntaxRefresh(FileText *) — cold pending snapshot replacement;
 *     allocation refusal leaves source editable and disables stale ink until next edit.
 *   static fn paintEditorSyntax(canvas,file,scene,top,left) — allocation-free
 *     byte-to-cell pen projection, then restore action chips above source ink.
 *   static bool editorKey(FileText *, int)
 *   static fn editorKey(FileText *, int) / autosave(FileText *, const char *)
 *   static char *workspaceRunSource(const char *) / runBPath(bool)
 *     select the workspace graph for canonical workspace C tests; other files
 *     keep the standalone b.sh adapter. Neither path executes before a Run click.
 *   static bool runSourceHasMain/runEligible/runStart(...) / fn runPoll/runStop/runPaint(...)
 *     admit a possible entry point, supervise bounded child output, then show it
 *     in a separate console tab; admission does not guarantee compilation.
 *   static TerminalText *chip(const char *)
 *   static TerminalText *plain(const char *, u32)
 *   static bool putText(parent, text, anchor) / putPanel(parent, panel, anchor)
 *   static TerminalPanel *buildChromeRow() / buildStatusBar(Scene *, FileText *)
 *   static fn railBuild(const char *)   the project as a tree, one level at a time
 *   static bool railReadChildren(id) / railRevealPath(path)
 *   static unsigned fileSortGroup(const char *) / int compareDirEntries(...)
 *   static fn railClick(y) / railScrollBy(rows)   the rail's pointer contract
 *   static fn sceneSyncRail(Scene *)   point the row pool at the visible rows
  *   static bool openFile(path, node)   swap the editor's file, not the scene
  *   static bool tabReserve/tabAdmit(path, name) / tabIndexForPath(path)
  *   static fn syncTabViews(Scene *) / bool tabClose(index) / bool tabPointerPress(...)
  *     maintain the real-file tab strip and its close/switch hit targets
 *   static bool editorPlaceCaret(file, x, y, top, left, codeLeft, scene)
 *     map a source-cell click to a UTF-8 byte boundary
 *   static fn runAppend(bytes, length) / runPoll/runStop — sanitize and retain
 *     bounded child output, reap without holding the editor or blocking forever
 *   static fn runRecordExit(status) — classify a reaped child as exit code,
 *     terminating signal or unknown, separate from the still-draining pipe
 *   static bytes consoleVisibleRows(height) / consoleScroll(rows, height)
 *     navigate the retained ring by wheel, arrows, Page, Home and End
 *   static fn runPaint(canvas, scene, width, height) — paint the entire centre
 *     from console state, erasing editor gutter, source and Run chips in that tab
 *   static bool isMarkdownFile(name)  folded `.md`/`.markdown` extension gate
 *   static fn sceneMarkdownSource(Scene *, const FileText *)  borrow an admitted
 *     Markdown file and update file-kind controls; default Markdown to preview
 *   static fn sceneSetMarkdownMode(Scene *, bool)  re-point active controls
 *   static bool sceneUpdateNumberWidth(Scene *, const FileText *)
 *     resize the line-number band when the open file crosses a digit boundary
  *   static fn sceneUpdateEdgeRailHandle(Scene *)
  *     keep the top-row spacer aligned with the rail; collapsed grip is canvas-painted
  *   static fn paintRailSplitter(Scene *, canvas, visible)
  *     draw the hover divider grip or the persistent collapsed three-line left grip
  *     at x=railWidth, outside the list's last cell; hit-testing uses that same seam.
 *   static bool sceneMarkdownClick(Scene *, x, width)  map the top-right [code]
 *     then [preview] controls on the editor's first row
 *   static fn sceneRenderMarkdown(Scene *, canvas, railWidth) cold layout if
 *     needed, then hot paint into content three cells after the rail
 *   static MarkdownContent sceneMarkdownContent(...) the one content rect shared by
 *     render, hover and copy, so the three cannot drift apart
 *   static fn sceneMarkdownClearFeedback/ClearNotice/SetNotice(...) transient state
 *   static bool sceneMarkdownCellAt(...) map a pointer cell to a DOCUMENT row/cell
 *   static fn sceneMarkdownUpdateHover(...) move the view's link highlight
 *   static bool sceneMarkdownPress(...) resolve the action, write the REAL clipboard
 *   static bool sceneMarkdownCopyBlock/CopyLink(...) bounded copy through R2 io
 *   static fn sceneMarkdownPaintNotice(...) cold overlay on the content rect
 *   static fn cacheLoad/cacheSave()   the project's session, kept outside the tree
 *   static TerminalPanel *buildRail(Scene *, bytes rowCount)
 *   static TerminalPanel *buildEditor(Scene *, bytes)
 *   static TerminalPanel *buildCentre(Scene *, bytes)
 *   static fn sceneBuild(Scene *, FileText *, bytes)
 *   static fn sceneSyncRows(Scene *, const FileText *, bytes first, bytes left, bytes visible)
  *   static bytes cellsOf/bytesForCells(const char *, bytes, ...)  cell offsets
  *   static const char *fileIcon(const char *) / u32 iconColor(u32)
  *   static bytes railIndent(TerminalTreeRow, bytes)
  *   static fn paintRailColors(Scene *, TerminalCanvas *, bool, bytes, bytes)
 *   static bytes maxScrollTop/maxScrollLeft(clampScroll/applyScroll)
 *   TerminalScrollGlide_isActive/impulse/halt/clamp/step — borrowed scroll
 *     motion from Determinant; this host supplies the current bounds.
 *   static bytes editorFirstRow/editorVisibleRows/codeLeftColumn/codeColumns
  *     codeLeftColumn includes the two-cell leading and trailing number gaps.
 *   static fn sceneSetRailWidth(Scene *, bytes width)
 *   static fn resolveDeviceBackground()   what a fade fades TO
 *   static bool queryDeviceBackground(u32 *) / parseDeviceBackground
 *   static bool parsePackedHex(const char *, u32 *)
 *   static short colorIndex(u32)   packed colour -> 256-cube/basic index
 *   static fn presenterEnter/Leave/Forget/InvalidateFront()
 *     the host's cold device transaction and the front-mirror invalidation
 *   static fn presenterMessage(const char *)  one plain line on a blank grid
 *   static bool blitRun(any , TerminalRun)   absolute ANSI: CUP + SGR + UTF-8
 *   static fn drawChrome(canvas, width, height, railWidth, markdownPreview,
 *     dividerHovered)  paint the single-column preview seam or the code-mode grip
 *   static fn handlePointer(Scene *, Interaction *, int width, int height)
 *   static int dumpFrame(int, int, FileText *)
 *   static fn frameBracket(any , TerminalPresentPhase)  the frame transaction
 *     (synchronized output + one buffered write, no ncurses refresh)
 *   static int runInputProbe()   DEBUG: print what the terminal sends
 *   static int runBackgroundProbe()   DEBUG: what is this terminal's background?
 *   static int runTerminal(FileText *)
 * Entry:
 *   int main(int, char **)   --help / --dump [WxH] / --file PATH / interactive.
 * Contracts:
 *   The interactive path REQUIRES a tty on stdin and stdout and refuses before
 *   ncurses otherwise. It selects the environment's LC_CTYPE before newterm, uses
 *   ncurses for the keyboard and termios only, and writes canvas cells itself as
 *   absolutely addressed ANSI inside one synchronized-output transaction. Raw mode keeps Ctrl-Z
 *   available to the editor as byte 26, so Ctrl-C is read explicitly as byte 3;
 *   a real SIGINT/SIGTERM still requests the same ordinary save-then-exit. Ctrl-C
 *   is the ONLY quit key - a letter key must not quit,
 *   because the terminal is a text editor and letters are text; the terminal is
 *   restored on every normal exit and on SIGINT/SIGTERM (SIGKILL or a crash
 *   cannot promise that).
 *   Mouse reporting is enabled for ANY-MOTION plus SGR encoding, so hover and
 *   drag both arrive; it is disabled again on exit, which hands the mouse back to
 *   the terminal's own selection. Wheel scrolls three lines, a pointer resting on
 *   the top or bottom editor row scrolls one line per motion event, and a press
 *   within one cell of the divider begins a drag that resizes the rail. Code view
 *   inserts printable UTF-8 bytes, splits/joins/deletes lines and autosaves after one
 *   idle second; Markdown preview is read-only. Save conflicts preserve the open buffer.
 *   No network is touched; a program is executed only after an explicit Run
 *   click. Workspace C test files use the project's registered b.sh graph; other
 *   supported files use standalone b.sh, which cannot infer their dependencies.
 *   A `.md` or
 *   `.markdown` file starts in read-only Markdown preview; the top-right
 *   `[preview]`/`[code]` controls switch presentation, and vertical scroll keys
 *   and wheel move the wrapped preview rows. Opening another file updates the
 *   controls and source borrow without rebuilding the scene.
 *   Colour carries REAL alpha: a translucent pen is composited over its cell's
 *   background, and that in turn over the device background, before the device is
 *   handed anything - a terminal has no alpha channel, so this is the only place a
 *   fade can happen. The device background is resolved at startup and is what a
 *   fade actually fades toward; fully opaque artwork passes through untouched.
 * ============================================================================ */

#define TUI_MAX_W 400
#define TUI_MAX_H 200
#define TUI_MAX_ROWS 160
// One rail row's composed text: indent, fold marker, name. Sized so the deepest name a
// 30-cell rail can show still fits rather than being silently cut.
#define TUI_RAIL_TEXT 96
// Where a project's session lives, relative to the home directory. Machine-scoped and
// outside the project on purpose: browsing a checkout must never write into it.
#if defined(VEX_PLATFORM_APPLE)
#define TUI_STATE_DIR "Library/Application Support/vexgraph/semicolon"
#else
#define TUI_STATE_DIR ".local/state/vexgraph/semicolon"
#endif
#define TUI_FILE_READ_CHUNK 65536u
#define TUI_AUTOSAVE_DELAY_NS 1000000000ULL
#define TUI_INITIAL_LINE_CAPACITY 64u
#define TUI_INITIAL_TAB_CAPACITY 4u
// A safety bound, not a promise that every edit is kept forever: the oldest snapshot
// falls off when a document has been edited this many times without closing a tab.
#define TUI_HISTORY_LIMIT 64u
#define TUI_DEFAULT_LINE_CAPACITY 64u
#define TUI_LINE_ENDING_NONE 0u
#define TUI_LINE_ENDING_LF 1u
#define TUI_LINE_ENDING_CRLF 2u
#define TUI_INDENT_SPACES 4u
#define GUTTER_MIN_NUMBER_WIDTH 1u

// The design puts TWO cells between the number field and the first code cell. It is a reserved band in the
// editor's flow rather than padding copied into every line, which keeps the code text a
// borrowed pointer into the file instead of a per-frame copy.
#define TUI_CODE_PAD 2u
#define TUI_MARKDOWN_DIVIDER_OFFSET 1u
#define TUI_MARKDOWN_CONTENT_OFFSET 3u

// A fenced block carries a copy glyph on its first row. The clipboard write is the
// R2 engine's real pasteboard bridge, and the source cap bounds the one engine buffer
// the projection needs; an over-cap block refuses instead of truncating.
#define TUI_MARKDOWN_COPY_MAX_BYTES 65536u
#define TUI_MARKDOWN_LINK_MAX_BYTES 4096u
#define TUI_MARKDOWN_NO_BLOCK ((bytes) -1)
#define TUI_MARKDOWN_NOTICE_NS 1500000000ULL
#define TUI_MARKDOWN_COPIED_NS 1500000000ULL
#define TUI_RUN_CHIP_CELLS 8u
#define TUI_CONSOLE_MAX_LINES 10000u
#define TUI_CONSOLE_LINE_BYTES 2048u
#define TUI_CONSOLE_READS_PER_POLL 16u
#define TUI_HINT_CHIP_CELLS 9u
#define TUI_WARNING_LABEL_CELLS 16u
#define TUI_LINE_CHIP_CELLS 12u
#define TUI_PROJECT_CHIP_CELLS 16u
#define TUI_STATUS_LEFT_CELLS 11u

// The breadcrumb's maximum band. The actual band is computed from the terminal width so
// the footer keeps its chips on an 80-column terminal instead of clipping them.
#define TUI_CRUMB_CELLS 44u
#define TUI_CRUMB_MIN_CELLS 20u
#define TUI_POLL_MS 100
#define TUI_RAIL_DEFAULT_WIDTH 50u
#define TUI_RAIL_MIN 12u
#define TUI_RAIL_MARGIN 24u

// Two clicks on the splitter within this window collapse the rail.
#define TUI_f64_CLICK_NS 350000000ULL

// One wheel notch asks for ONE line, not three. A trackpad quantises a single gesture
// into dozens of notches, so three each was a screen-and-a-half per flick.
#define TUI_SCROLL_ROWS 1u
#define TUI_SCROLL_COLS 1u

// The frame interval while a scroll is in flight. Friction and the snap threshold
// belong to Determinant's TerminalScrollGlide, which owns the motion.
#define TUI_FRAME_MS 16

// Scroll-past padding: how far the view may travel BEYOND the last line and the
// last column, so there is always somewhere to type at the bottom and at the right
// edge instead of the caret being pinned against it.
#define TUI_SCROLL_PAST_BOTTOM 10u
#define TUI_SCROLL_PAST_RIGHT 20u

// Events handled before a paint. A drag floods motion events, so an unbounded
// drain never lets the frame update; a bound keeps the screen live.
#define TUI_EVENTS_PER_FRAME 32u

#define COL_TEXT   TERMINAL_RGB(0xE6, 0xE6, 0xE6)
#define COL_MUTED  TERMINAL_RGB(0x77, 0x77, 0x88)
#define COL_ACCENT gAccentColor
#define COL_ERROR  TERMINAL_RGB(0xFF, 0x6B, 0x6B)
// FADED, not merely dark. A translucent colour lands between itself and whatever is
// behind it, so the gutter recedes on a dark theme and on a light one from ONE value
// instead of two. Alpha 0 is the unset sentinel (inherit), so a fade keeps a non-zero
// alpha - there is deliberately no such thing here as "fully transparent".
#define COL_GUTTER TERMINAL_RGBA(0xAA, 0xAA, 0xC4, 0x7A)
// The shell's one HIGHLIGHT, and it is a tint rather than a block: at 22% the accent
// reads as "this one" over any theme's own background instead of covering it up.
#define COL_SELECT TERMINAL_RGBA(TERMINAL_COLOR_RED(gAccentColor), \
                                 TERMINAL_COLOR_GREEN(gAccentColor), \
                                 TERMINAL_COLOR_BLUE(gAccentColor), 0x38)
// Which tab is the open one. The design lights exactly one tab; naming it keeps that
// from being a bare literal in a loop.
#define TUI_ACTIVE_TAB 3u

// The terminal's OWN background: what shows through every transparent cell, and so what
// a translucent colour is composited against. Resolved at startup (resolveDeviceBackground)
// and this is only the last resort, for a terminal that neither declares nor answers.
// It only ever affects TRANSLUCENT colours - opaque artwork is identical either way.
#define TUI_DEVICE_BG_FALLBACK TERMINAL_RGB(0x1A, 0x1A, 0x1A)
// How long the device background query waits, and how much of the answer is kept. A
// terminal that answers does so in a millisecond or two; one that ignores the query
// never answers at all, so this window is the whole cost of asking.
#define TUI_PROBE_MS 120
#define TUI_PROBE_BYTES 64
static u32 gDeviceBg = TUI_DEVICE_BG_FALLBACK;
static const char *gDeviceBgSource = "the built-in fallback";

static volatile sig_atomic_t gQuit;
static u32 gCells[TUI_MAX_W * TUI_MAX_H];
static u16 gStyles[TUI_MAX_W * TUI_MAX_H];
static u32 gFg[TUI_MAX_W * TUI_MAX_H];
static u32 gBg[TUI_MAX_W * TUI_MAX_H];
static u32 gFrontCells[TUI_MAX_W * TUI_MAX_H];
static u16 gFrontStyles[TUI_MAX_W * TUI_MAX_H];
static u32 gFrontFg[TUI_MAX_W * TUI_MAX_H];
static u32 gFrontBg[TUI_MAX_W * TUI_MAX_H];
static char gGutter[TUI_MAX_ROWS][16];
static char gLineChip[32];
static char gProjectChip[64] = "[vexgraph]";
static char gTimeChip[16] = "[--:--:--]";
static char gWarningText[256];
static char gWarningLabel[16];
static char gWarningLabelText[32];
static char gBreadcrumb[192];
static const char *gEmptyLine = "";
// The editor starts one row below the header. A warning header occupies a second row
// and pushes only the centre down; the rail stays exactly where it was.
static bytes gEditorFirstRow = 1u;
// The live terminal width, so a header or footer built before its first paint can still
// decide how much room it has for a tab strip or a breadcrumb.
static int gTerminalWidth = 80;
// User-selected presentation setting; the terminal cannot report missing font glyphs.
static bool gNerdIcons = true;
static u32 gAccentColor = TERMINAL_RGB(0x7A, 0xC8, 0xFF);

// Encode one BMP codepoint as UTF-8 without consulting the process locale.
// snprintf("%lc") is locale-sensitive: under the C locale it silently produces
// nothing for Private Use glyphs, which blanked every catalog icon. The terminal
// always speaks UTF-8, so the encoder does too.
static const char *iconUtf8(u32 codepoint) {
    static char glyph[8];
    bytes at = 0u;

    if (codepoint < 0x80u) {
        glyph[at++] = (char) codepoint;
    } else if (codepoint < 0x800u) {
        glyph[at++] = (char) (0xC0u | (codepoint >> 6));
        glyph[at++] = (char) (0x80u | (codepoint & 0x3Fu));
    } else {
        glyph[at++] = (char) (0xE0u | (codepoint >> 12));
        glyph[at++] = (char) (0x80u | ((codepoint >> 6) & 0x3Fu));
        glyph[at++] = (char) (0x80u | (codepoint & 0x3Fu));
    }
   
    glyph[at] = '\0';
    return glyph;
}

// One filename vocabulary for the rail and tabs. Nerd Fonts v3 BMP glyphs avoid
// supplementary-plane wchar_t assumptions. Unknown types remain ordinary files.
static const char *fileIcon(const char *name) {
    if (!gNerdIcons)
        return "-";
    for (bytes i = 0; i < SEMICOLON_NAMED_ICON_COUNT; ++i)
        if (strcasecmp(name, SEMICOLON_NAMED_ICONS[i].name) == 0)
            return iconUtf8(SEMICOLON_NAMED_ICONS[i].glyph);
    const char *extension = strrchr(name, '.');
    if (extension == nullptr)
        return "\uf15c";
    const char *suffix = extension[0] == '.' ? extension + 1 : extension;
    for (bytes i = 0; i < SEMICOLON_ICON_CATALOG_COUNT; ++i) {
        const char *cursor = SEMICOLON_ICON_CATALOG[i].extensions;
        while (*cursor != '\0') {
            while (*cursor == ' ')
                ++cursor;
            const char *end = strchr(cursor, ' ');
            bytes length = end == nullptr ? strlen(cursor) : (bytes) (end - cursor);
            if (strlen(suffix) == length && strncasecmp(cursor, suffix, length) == 0)
                return iconUtf8(SEMICOLON_ICON_CATALOG[i].glyph);
            cursor = end == nullptr ? cursor + length : end;
        }
    }
    return "\uf15c";
}

// INTENTIONAL(vex): the icon palette follows vex's requested language colours,
// not upstream nvim-web-devicons defaults (notably Java, Python, C++, JSON and MD).
// Defaults are presentation policy; icon glyph identity is independent of colour.
static u32 iconColor(u32 glyph) {
    for (bytes i = 0; i < SEMICOLON_NAMED_ICON_COUNT; ++i)
        if (SEMICOLON_NAMED_ICONS[i].glyph == glyph)
            return (SEMICOLON_NAMED_ICONS[i].color << 8) | 0xffu;
    for (bytes i = 0; i < SEMICOLON_UI_ICON_COUNT; ++i)
        if (SEMICOLON_UI_ICONS[i].glyph == glyph)
            return (SEMICOLON_UI_ICONS[i].color << 8) | 0xffu;
    for (bytes i = 0; i < SEMICOLON_ICON_CATALOG_COUNT; ++i)
        if (SEMICOLON_ICON_CATALOG[i].glyph == glyph)
            return (SEMICOLON_ICON_CATALOG[i].color << 8) | 0xffu;
    switch (glyph) {
        case 0xf07b: case 0xf07c: return 0xC58A52FFu; // folder brown
        case 0xe738: return 0xF08A32FFu; // Java orange
        case 0xe74e: return 0xF0D746FFu; // JavaScript yellow
        case 0xe648:
             return 0x75C84DFFu; // C# green
        case 0xe73c:
             return 0x28C5B0FFu; // Python turquoise
        case 0xe628:
        case 0xe7ba:
             return 0x3399FFFFu; // TypeScript/React blue
        case 0xe68a: return 0x769BCEFFu; // R gray-blue
        case 0xe61e: case 0xf1dc: return 0xA88AF2FFu; // C/header gray-purple
        case 0xe61d: return 0xC678DDFFu; // C++ purple
        case 0xe69e: return 0xAB78E3FFu; // Fortran purple
        case 0xe60b: return 0xC792EAFFu; // JSON lavender
        case 0xe73e: return 0x5CA9FFFFu; // Markdown blue
        case 0xe627: return 0x56B6C2FFu; // Go cyan
        case 0xe7a8: return 0xDEA584FFu; // Rust warm neutral
        case 0xe755: case 0xe736: return 0xE99545FFu; // Swift/HTML orange
        case 0xe749: return 0xB18BDDFFu; // CSS purple
        case 0xf489: return 0x79B86AFFu; // shell green
        case 0xe615: return 0x9993B5FFu; // configuration
        case 0xf425: return 0xC58A52FFu; // build
        default: return 0xA0A0AAFFu; // generic files, images and databases
    }
}

static bool setIconMode(const char *mode) {
    if (strcmp(mode, "nerd") != 0 && strcmp(mode, "plain") != 0) {
        THROW("semicolon: icons must be nerd or plain");
        return false;
    }
    gNerdIcons = strcmp(mode, "nerd") == 0;
    return true;
}

static int showIconCatalog() {
    (fn) setlocale(LC_ALL, "");
    puts("Semicolon supported icon catalog (Nerd Fonts glyphs; colors are RGB)");
    printf("%-18s %-30s %-8s %s\n", "FORMAT", "EXTENSIONS", "GLYPH", "COLOR");
    puts("--------------------------------------------------------------------------");
    printf("%-18s %-30s \033[38;2;197;138;82m%lc\033[0m   #C58A52\n",
           "Folder", "directories", (wchar_t) 0xf07b);
    printf("%-18s %-30s \033[38;2;197;138;82m%lc\033[0m   #C58A52\n",
           "Open folder", "expanded directories", (wchar_t) 0xf07c);
    for (bytes i = 0; i < SEMICOLON_ICON_CATALOG_COUNT; ++i) {
        const SemicolonIconEntry *entry = &SEMICOLON_ICON_CATALOG[i];
        unsigned red = ((*entry).color >> 16) & 0xffu;
        unsigned green = ((*entry).color >> 8) & 0xffu;
        unsigned blue = (*entry).color & 0xffu;
        printf("%-18s %-30s \033[38;2;%u;%u;%um%lc\033[0m   #%06X\n",
               (*entry).format, (*entry).extensions, red, green, blue,
               (wchar_t) (*entry).glyph, (*entry).color);
    }
    puts("\nSpecial filenames and workspace UI:");
    for (bytes i = 0; i < SEMICOLON_NAMED_ICON_COUNT; ++i) {
        const SemicolonNamedIcon *entry = &SEMICOLON_NAMED_ICONS[i];
        printf("%-18s %-30s \033[38;2;%u;%u;%um%lc\033[0m   #%06X\n",
               (*entry).name, (*entry).description,
               ((*entry).color >> 16) & 0xffu, ((*entry).color >> 8) & 0xffu,
               (*entry).color & 0xffu, (wchar_t) (*entry).glyph, (*entry).color);
    }
    puts("Unknown extensions use the generic file icon; fonts must provide these glyphs.");
    puts("\nIDE UI icon inventory (design vocabulary, not all implemented controls):");
    for (bytes i = 0; i < SEMICOLON_UI_ICON_COUNT; ++i) {
        const SemicolonNamedIcon *entry = &SEMICOLON_UI_ICONS[i];
        printf("%-18s %-30s \033[38;2;%u;%u;%um%lc\033[0m   #%06X\n",
               (*entry).name, (*entry).description,
               ((*entry).color >> 16) & 0xffu, ((*entry).color >> 8) & 0xffu,
               (*entry).color & 0xffu, (wchar_t) (*entry).glyph, (*entry).color);
    }
    return 0;
}

static bool isMarkdownFile(const char *name) {
    if (name == nullptr)
        return false;
    const char *extension = strrchr(name, '.');
    return extension != nullptr
        && (strcasecmp(extension, ".md") == 0 || strcasecmp(extension, ".markdown") == 0);
}

// ---------------------------------------------------------------- file text

// A document snapshot is the undo/redo currency: every stored line is an owned copy,
// so a history entry can never be invalidated by a later edit. The arrays mirror
// FileText's row layout, and lineCapacity is exactly lineCount: the next structural
// edit reserves a larger directory through the ordinary fileReserveLines path.
typedef struct FileSnapshot {
    char **lineStorage;
    const char **lines;
    bytes *lineLengths;
    bytes *lineCapacities;
    u8 *lineEndings;
    bytes lineCount;
    bytes lineCapacity;
    bytes cursorLine;
    bytes cursorByte;
    u64 revision;
    bool valid;
} FileSnapshot;

typedef struct FileSnapshotStack {
    FileSnapshot *items;
    bytes count;
    bytes capacity;
} FileSnapshotStack;

// A bounded stack pair. pending is the pre-edit state captured by fileHistoryBegin;
// it is committed only after the edit actually changes the document, so a refused
// backspace at byte zero leaves both the text and the redo stack untouched.
typedef struct FileHistory {
    FileSnapshotStack undo;
    FileSnapshotStack redo;
    FileSnapshot pending;
    bool pendingValid;
    u64 currentRevision;
    u64 nextRevision;
    u64 savedRevision;
} FileHistory;

typedef struct FileText {
    char **lineStorage;      // individually owned editable UTF-8 lines
    bytes length;
    const char **lines;      // aliases lineStorage for existing renderer/Markdown seam
    bytes *lineLengths;     // bytes per line, excluding line endings
    bytes *lineCapacities;  // allocated bytes per line, including NUL
    u8 *lineEndings;    // NONE, LF or CRLF after each line
    bytes lineCount;
    bytes lineCapacity;
    bytes longestCells;     // widest line in CELLS, so horizontal scroll has a bound
    bytes longestLine;
    bytes cursorLine;
    bytes cursorByte;        // byte offset on a UTF-8 scalar boundary
    bool cursorVisible;
    u64 lastEditNs;
    bool dirty;
    bool saveFailed;
    bool baselineValid;
    struct stat baseline;
    char name[64];           // display name for the breadcrumb
    SemicolonSyntax *syntax;
    bool syntaxPending;
    bool syntaxCurrent;
    FileHistory history;     // bounded snapshot undo/redo, owned with this buffer
} FileText;

// SLOT RECORD: EditorTab
typedef struct EditorTab {
    char path[1024];
    char label[128];
    char display[160];
    char shown[160];
    FileText suspended;
    bytes x;
    bytes width;
    bool preview;
    TerminalText *view;
    TerminalChildId child;
} EditorTab;

typedef struct TuiRow {
    TerminalText *gutter;
    TerminalText *code;
} TuiRow;

typedef struct Scene {
    TerminalPanel *root;
    TerminalPanel *header;
    TerminalPanel *rail;
    TerminalPanel *centre;
    TerminalPanel *editor;      // borrowed from centre
    TerminalPanel *codeActions; // borrowed from editor's code layer
    TerminalChildId railId;
    TerminalChildId gutterId;
    TerminalChildId editorId;
    bool editorPanelVisible;
    TerminalChildId projectTextId;
    TerminalChildId gripId;
    TerminalChildId warningId;
    bytes headerLeftWidth;
    bool railVisible;
    bool edgeRailHandle;
    bytes railRestoreWidth;
    bool warningVisible;
    TuiRow rows[TUI_MAX_ROWS];
    bytes rowCount;
    TerminalText *lineChip;
    TerminalText *projectChip;
    TerminalText *timeChip;
    TerminalText *gripChip;
    TerminalText *warningText;
    TerminalText *warningLabel;
    bool gripHovered;
    bytes railWidth;
    bytes numberWidth;
    // The rail's rows. One Text per VISIBLE row, created once and re-pointed, so folding
    // the tree and scrolling it are both allocation-free - exactly the trick the editor
    // rows use, and the reason the row count is the panel's height rather than the
    // tree's size.
    TerminalText *railRows[TUI_MAX_ROWS];
    bytes railRowCount;
    char railText[TUI_MAX_ROWS][TUI_RAIL_TEXT];
    // Re-pointed when the open file changes, so the header cannot describe a file the
    // editor is not showing.
    TerminalText *breadcrumb;
    TerminalText *activeTab;
    TerminalText *languageHints;
    TerminalText *runHint;
    TerminalText *runTab;
    TerminalChildId runTabId;
    char runTabText[160];
    char runTabShown[160];
    TerminalText *codeMode;
    TerminalText *previewMode;
    TerminalMarkdownView *markdown;
    bool markdownFile;
    bool preview;
    bytes codeModeWidth;
    bytes previewModeWidth;
    bytes markdownModeGap;
    // The fence whose check glyph is showing, and when that feedback lapses. The
    // view owns the glyph; this is the application's timer, cleared whenever the
    // content moves so a check can never describe a block the reader scrolled away.
    bytes copiedBlock;
    u64 copiedUntilNs;
    char markdownNotice[48];
    u64 markdownNoticeUntilNs;
} Scene;

static bytes editorFirstRow();
static bytes editorVisibleRows(int height);
static bytes railVisibleRows(int height);
static bytes codeLeftColumn(bytes railWidth, bytes numberWidth);
static bytes codeColumns(bytes railWidth, bytes numberWidth, int width);
static bytes maxScrollTop(const FileText *file, bytes visible);
static bytes maxScrollLeft(const FileText *file, bytes codeWidth);
static const char *projectRootName();
static char gOpenPath[1024];
static bool parsePackedHex(const char *text, u32 *color);
static bool sceneSetMarkdownMode(Scene *scene, bool preview);
static fn syncTabViews(Scene *scene);
static fn tabScrollBy(bool forward);
static bool runEligible(const char *path, const FileText *file);
static fn syncRunControls(Scene *scene, const char *path, const FileText *file);
static bool runStart(const char *path, const FileText *file);
static fn runPoll();
static fn runStop();
static fn runPaint(TerminalCanvas *canvas, const Scene *scene, bytes width, bytes height);

// Defined beside the Markdown renderer; declared here because opening a file resets the
// transient hover, check and notice before those helpers exist in the translation unit.
static fn sceneMarkdownClearFeedback(Scene *scene);
static fn sceneMarkdownSetNotice(Scene *scene, const char *text);
// Defined beside the scene builders; setSaveStatus reports a refused save through it.
static fn sceneSetWarning(Scene *scene, const char *text, const char *label);
static bool sceneSetRailVisible(Scene *scene, bool visible);
static fn sceneSetGripHover(Scene *scene, bool hovered);

static fn sceneMarkdownSource(Scene *scene, const FileText *file) {
    sceneMarkdownClearFeedback(scene);
    (*scene).markdownFile = isMarkdownFile((*file).name);
    if ((*scene).markdown == nullptr)
        (*scene).markdown = TerminalMarkdownView();
    if ((*scene).markdown == nullptr)
        return;
    (fn) TerminalMarkdownView_setSource((*scene).markdown,
                                          (*scene).markdownFile ? (*file).lines : nullptr,
                                          (*scene).markdownFile ? (*file).lineCount : 0u);
    (*scene).preview = (*scene).markdownFile;
    if ((*scene).codeMode != nullptr) {
        (fn) TerminalText_setForegroundColor((*scene).codeMode,
                                               (*scene).preview ? COL_MUTED : COL_ACCENT);
        (fn) TerminalText_setBackgroundColor((*scene).codeMode,
                                               (*scene).preview ? TERMINAL_COLOR_DEFAULT : COL_SELECT);
    }
    if ((*scene).previewMode != nullptr) {
        (fn) TerminalText_setForegroundColor((*scene).previewMode,
                                               (*scene).preview ? COL_ACCENT : COL_MUTED);
        (fn) TerminalText_setBackgroundColor((*scene).previewMode,
                                               (*scene).preview ? COL_SELECT : TERMINAL_COLOR_DEFAULT);
    }
    syncRunControls(scene, gOpenPath, file);
    if ((*scene).codeMode != nullptr) {
        const char *code = (*scene).markdownFile ? "[ Code]" : "";
        (fn) TerminalText_setText((*scene).codeMode, code, strlen(code));
    }
    if ((*scene).previewMode != nullptr) {
        const char *preview = (*scene).markdownFile ? "[ Preview]" : "";
        (fn) TerminalText_setText((*scene).previewMode, preview, strlen(preview));
    }
}

static fn setSaveStatus(Scene *scene, const FileText *file) {
    // The footer's project chip names the project the session is based on, and carries
    // the save state as one trailing mark rather than a second chip the design does not
    // have. The root's name is stable, so the chip does not wander as files change.
    const char *root = projectRootName();
    (fn) snprintf(gProjectChip, sizeof gProjectChip, "[%s%s]", root,
                    (*file).saveFailed ? "!" : (*file).dirty ? "*" : "");
    if ((*scene).projectChip != nullptr) {
        (fn) TerminalText_setText((*scene).projectChip, gProjectChip, strlen(gProjectChip));
        (fn) TerminalText_setForegroundColor((*scene).projectChip,
                                               (*file).saveFailed ? COL_ERROR
                                               : (*file).dirty ? COL_ACCENT : COL_MUTED);
    }
    // A refused save is the one condition the shell must shout about: the buffer is
    // still only in memory. The warning is cleared when the save finally lands.
    if ((*file).saveFailed && !(*scene).warningVisible)
        sceneSetWarning(scene, "Save failed; changes are only in memory", "save");
    else if (!(*file).saveFailed && strcmp(gWarningLabel, "save") == 0)
        sceneSetWarning(scene, "", "");
}

// The live clock is one second of resolution, so it is re-formatted only when the
// displayed text actually changes. It is still a per-frame call, but it is a localtime
// read and a string compare, not an allocation or a terminal write.
static fn updateClock(Scene *scene) {
    if (scene == nullptr || (*scene).timeChip == nullptr)
        return;
    time_t now = time(nullptr);
    struct tm local;
    if (localtime_r(&now, &local) == nullptr)
        return;
    char text[16];
    (fn) snprintf(text, sizeof text, "[%02d:%02d:%02d]",
                    local.tm_hour, local.tm_min, local.tm_sec);
    if (strcmp(text, gTimeChip) == 0)
        return;
    (fn) snprintf(gTimeChip, sizeof gTimeChip, "%s", text);
    (fn) TerminalText_setText((*scene).timeChip, gTimeChip, strlen(gTimeChip));
}

// SLOT RECORD: RailEntry
typedef struct RailEntry {
    char label[TUI_RAIL_TEXT];      // borrowed by the tree view, so it lives here
    char path[1024];                // absolute, for reading and for opening
    TerminalTreeNodeId parent;
    bool loaded;                    // children have been read (exactly once)
    bool folder;
} RailEntry;

static TerminalTreeView *gRail;
static RailEntry *gRailEntries;
static bytes gRailEntryCount;
static bytes gRailEntryCapacity;
static char gRailRoot[1024];
static char gRailLocation[PATH_MAX + 2u]; // canonical location; ~/... when under HOME

// The project's display name: the last component of the root, never the whole path.
// A root of "/" has no component, so it keeps the slash rather than becoming empty.
static const char *projectRootName() {
    if (gRailRoot[0] == '\0')
        return "project";
    const char *name = strrchr(gRailRoot, '/');
    name = name == nullptr ? gRailRoot : name + 1u;
    return *name == '\0' ? "/" : name;
}

static TerminalTreeNodeId gRailRootNode;
static char gOpenPath[1024];
static FileText *gOpenFile;         // the editor's file, owned by runTerminal
static Scene *gScene;               // the live scene, or null outside an interactive run
static bool gResetView;             // a newly opened file starts at its top-left
static EditorTab *gTabs;
static bytes gTabCount;
static bytes gTabCapacity;
static bytes gTabFirst; // first admitted tab in the horizontal viewport
static bool gTabReveal; // explicit selection/open requests visibility once
static bytes gRailColumn; // horizontal child-list viewport, in terminal cells
static bytes gRailMaxColumn; // recomputed from admitted visible tree labels
static bytes gActiveTabIndex;
static char gCachedOpen[1024];      // restored from the project's session
static bool gCachedMarkdownPreview;
static bool gCachedMarkdownModeValid;
static bytes gCachedRail;
static bool gCachedRailValid;       // the session actually recorded a rail width
static bool gCachedRailCustom;      // stored width came from a user's divider drag
static bool gRailWidthCustomized;
static bool gCachedRailHidden;      // it recorded the text-based (rail-hidden) mode
static bool gAccentOverride;
static bool gRunEligible;
static bool gRunSelected;
static bool gRunTabVisible;
static bytes gConsoleHead;
static bytes gConsoleCount;
static bytes gConsoleTop;
static bytes gConsoleOmitted;
static bytes gConsoleClipped;
static bool gConsoleFollow = true;
static bool gConsoleLineClipped;
static bytes gConsoleLengths[TUI_CONSOLE_MAX_LINES];
static char gConsoleLines[TUI_CONSOLE_MAX_LINES][TUI_CONSOLE_LINE_BYTES];
static pid_t gRunPid = -1;
static int gRunOutputFd = -1;
static bytes gRunTabX;
static bytes gRunTabWidth;
static char gRunLabel[160];
static char gRunStatus[64] = "not run";
static char gRunOutcome[64] = "not run";
static unsigned gRunEscapeState;

static bool runSourceHasMain(const FileText *file) {
    if (file == nullptr || (*file).length == SIZE_MAX)
        return false;
    if ((*file).lineCount > SIZE_MAX - (*file).length - 1u)
        return false;
    bytes sourceCapacity = (*file).length + (*file).lineCount + 1u;
    char *source = malloc(sourceCapacity);
    if (source == nullptr)
        return false;
    bytes used = 0u;
    for (bytes line = 0; line < (*file).lineCount; ++line) {
        bytes length = (*file).lineLengths[line];
        if (used >= sourceCapacity || length >= sourceCapacity - used)
            break;
        memcpy(source + used, (*file).lines[line], length);
        used += length;
        source[used++] = '\n';
    }
    source[used] = '\0';
    bool found = false;
    for (bytes at = 0; at < used && !found;) {
            if (source[at] == '/' && source[at + 1u] == '/') {
            while (at < used && source[at] != '\n')
                ++at;
        } else if (source[at] == '/' && source[at + 1u] == '*') {
            at += 2u;
            while (at + 1u < used && !(source[at] == '*' && source[at + 1u] == '/'))
                ++at;
            at = at + 1u < used ? at + 2u : used;
        } else if (source[at] == '"' || source[at] == '\'') {
            char quote = source[at++];
            while (at < used && source[at] != quote) {
                if (source[at] == '\\' && at + 1u < used)
                    at += 2u;
                else
                    ++at;
            }
            if (at < used)
                ++at;
        } else if ((source[at] >= 'A' && source[at] <= 'Z')
                   || (source[at] >= 'a' && source[at] <= 'z') || source[at] == '_') {
            bytes start = at++;
            while (at < used && ((source[at] >= 'A' && source[at] <= 'Z')
                   || (source[at] >= 'a' && source[at] <= 'z')
                   || (source[at] >= '0' && source[at] <= '9') || source[at] == '_'))
                ++at;
            if (at - start != 4u || memcmp(source + start, "main", 4u) != 0)
                continue;
            bytes next = at;
            while (next < used && (source[next] == ' ' || source[next] == '\t'
                   || source[next] == '\r' || source[next] == '\n'))
                ++next;
            if (next == used || source[next++] != '(')
                continue;
            bytes depth = 1u;
            while (next < used && depth != 0u) {
                if (source[next] == '"' || source[next] == '\'') {
                    char quote = source[next++];
                    while (next < used && source[next] != quote) {
                        if (source[next] == '\\' && next + 1u < used)
                            next += 2u;
                        else
                            ++next;
                    }
                    if (next < used)
                        ++next;
                } else if (source[next] == '(') {
                    ++depth;
                    ++next;
                } else if (source[next] == ')') {
                    --depth;
                    ++next;
                } else {
                    ++next;
                }
            }
            while (next < used && (source[next] == ' ' || source[next] == '\t'
                   || source[next] == '\r' || source[next] == '\n'))
                ++next;
            found = depth == 0u && next < used && source[next] == '{';
        } else {
            ++at;
        }
    }
    free(source);
    return found;
}

static bool runEligible(const char *path, const FileText *file) {
    if (path == nullptr || file == nullptr)
        return false;
    const char *extension = strrchr(path, '.');
    if (extension == nullptr)
        return false;
    if (strcmp(extension, ".c") == 0 || strcmp(extension, ".m") == 0)
        return runSourceHasMain(file);
    return strcmp(extension, ".sh") == 0 || strcmp(extension, ".py") == 0
        || strcmp(extension, ".js") == 0 || strcmp(extension, ".mjs") == 0
        || strcmp(extension, ".cjs") == 0 || strcmp(extension, ".lua") == 0
        || strcmp(extension, ".php") == 0 || strcmp(extension, ".go") == 0
        || strcmp(extension, ".rs") == 0;
}

static fn syncRunControls(Scene *scene, const char *path, const FileText *file) {
    if (scene == nullptr)
        return;
    gRunEligible = !(*scene).markdownFile && runEligible(path, file);
    if ((*scene).runHint == nullptr)
        return;
    bool running = gRunPid > 0;
    const char *hint = gRunEligible ? (running ? "■ stop" : "▶ run") : "";
    (fn) TerminalText_setText((*scene).runHint, hint, strlen(hint));
    (fn) TerminalText_setBrackets((*scene).runHint, gRunEligible ? '[' : 0u,
                                    gRunEligible ? ']' : 0u);
    (fn) TerminalText_setForegroundColor((*scene).runHint,
                                            running ? COL_ERROR : TERMINAL_RGB(0x70, 0xC9, 0x83));
    if ((*scene).languageHints != nullptr) {
        const char *hintState = gNerdIcons ? "\uf071 off" : "! off";
        (fn) TerminalText_setText((*scene).languageHints, hintState, strlen(hintState));
        (fn) TerminalText_setBrackets((*scene).languageHints, '[', ']');
        (fn) TerminalText_setForegroundColor((*scene).languageHints, COL_MUTED);
    }
}

// A C test in THIS checkout borrows its registered graph, not standalone b.sh's
// single-source compiler. Resolve the file first so a relative path or a symlink
// cannot accidentally select the graph by spelling alone. Other projects retain
// the ordinary standalone adapter; a main() token is not dependency discovery.
static char *workspaceRunSource(const char *path) {
    const char *marker = strstr(__FILE__, "/semicolon/main/tui-main.c");
    if (path == nullptr || marker == nullptr)
        return nullptr;
    char *source = realpath(path, nullptr);
    if (source == nullptr)
        return nullptr;
    bytes rootLength = (bytes) (marker - __FILE__);
    bytes sourceLength = strlen(source);
    bool owned = sourceLength > rootLength + sizeof "/test/" - 1u
              && strncmp(source, __FILE__, rootLength) == 0
              && strncmp(source + rootLength, "/test/", sizeof "/test/" - 1u) == 0;
    if (owned)
        return source;
    free(source);
    return nullptr;
}

static char *runBPath(bool workspaceGraph) {
    const char *override = getenv("SEMICOLON_B_PATH");
    if (override != nullptr && *override != '\0')
        return strdup(override);
    const char *marker = strstr(__FILE__, "/semicolon/main/tui-main.c");
    if (marker != nullptr) {
        bytes workspaceLength = (bytes) (marker - __FILE__);
        const char *tool = workspaceGraph ? "/tools/b.sh" : "/suite/personal/b.sh/b.sh";
        bytes length = workspaceLength + strlen(tool) + 1u;
        char *path = malloc(length);
        if (path != nullptr) {
            (fn) snprintf(path, length, "%.*s%s", (int) workspaceLength, __FILE__, tool);
            if (access(path, X_OK) == 0)
                return path;
            free(path);
        }
    }
    if (workspaceGraph) {
        THROW("semicolon: workspace b.sh is unavailable; refusing standalone compilation");
        return nullptr;
    }
    return strdup("b.sh");
}

static bool runStart(const char *path, const FileText *file) {
    if (path == nullptr || file == nullptr || !runEligible(path, file))
        return false;
    if (gRunPid > 0 || gRunOutputFd >= 0) {
        gRunSelected = true;
        gTabReveal = true;
        return true;
    }
    int outputPipe[2];
    if (pipe(outputPipe) != 0) {
        THROW("semicolon: cannot create b.sh console pipe: %s", strerror(errno));
        return false;
    }
    int nullInput = open("/dev/null", O_RDONLY);
    if (nullInput < 0) {
        (fn) close(outputPipe[0]);
        (fn) close(outputPipe[1]);
        THROW("semicolon: cannot open child stdin: %s", strerror(errno));
        return false;
    }
    bool compiled = strcasecmp(strrchr(path, '.') + 1u, "c") == 0
                 || strcasecmp(strrchr(path, '.') + 1u, "m") == 0;
    char *workspaceSource = compiled && strcasecmp(strrchr(path, '.'), ".c") == 0
                          ? workspaceRunSource(path) : nullptr;
    char *binary = runBPath(workspaceSource != nullptr);
    if (binary == nullptr) {
        free(workspaceSource);
        (fn) close(nullInput);
        (fn) close(outputPipe[0]);
        (fn) close(outputPipe[1]);
        return false;
    }
    char *arguments[] = { binary, "run", compiled ? "exec" : "instance",
                          (char*) path, nullptr };
    if (workspaceSource != nullptr) {
        arguments[2] = workspaceSource;
        arguments[3] = nullptr;
    }
    posix_spawn_file_actions_t actions;
    int error = posix_spawn_file_actions_init(&actions);
    if (error == 0)
        error = posix_spawn_file_actions_adddup2(&actions, nullInput, STDIN_FILENO);
    if (error == 0)
        error = posix_spawn_file_actions_adddup2(&actions, outputPipe[1], STDOUT_FILENO);
    if (error == 0)
        error = posix_spawn_file_actions_adddup2(&actions, outputPipe[1], STDERR_FILENO);
    if (error == 0)
        error = posix_spawn_file_actions_addclose(&actions, outputPipe[0]);
    if (error == 0)
        error = posix_spawn_file_actions_addclose(&actions, outputPipe[1]);
    if (error == 0)
        error = posix_spawn_file_actions_addclose(&actions, nullInput);
#if defined(VEX_PLATFORM_APPLE)
    if (error == 0)
        error = posix_spawn_file_actions_addchdir_np(&actions, gRailRoot);
#endif
    pid_t child = -1;
    if (error == 0) {
        if (strchr(binary, '/') != nullptr)
            error = posix_spawn(&child, binary, &actions, nullptr, arguments, environ);
        else
            error = posix_spawnp(&child, binary, &actions, nullptr, arguments, environ);
    }
    (fn) posix_spawn_file_actions_destroy(&actions);
    free(binary);
    free(workspaceSource);
    (fn) close(nullInput);
    (fn) close(outputPipe[1]);
    if (error != 0) {
        (fn) close(outputPipe[0]);
        THROW("semicolon: b.sh could not start: %s", strerror(error));
        return false;
    }
    int flags = fcntl(outputPipe[0], F_GETFL, 0);
    if (flags >= 0)
        (fn) fcntl(outputPipe[0], F_SETFL, flags | O_NONBLOCK);
    gRunPid = child;
    (fn) snprintf(gRunStatus, sizeof gRunStatus, "running");
    (fn) snprintf(gRunOutcome, sizeof gRunOutcome, "running");
    gRunOutputFd = outputPipe[0];
    gConsoleHead = 0u;
    gConsoleCount = 1u;
    gConsoleTop = 0u;
    gConsoleOmitted = 0u;
    gConsoleClipped = 0u;
    gConsoleFollow = true;
    gConsoleLineClipped = false;
    gConsoleLengths[0] = 0u;
    gRunEscapeState = 0u;
    gRunTabVisible = true;
    gRunSelected = true;
    gTabReveal = true;
    const char *name = strrchr(path, '/');
    name = name == nullptr ? path : name + 1u;
    (fn) snprintf(gRunLabel, sizeof gRunLabel, "%s", name);
    if (gScene != nullptr) {
        syncRunControls(gScene, path, file);
        syncTabViews(gScene);
    }
    return true;
}

static fn runAppend(const char *data, bytes length) {
    for (bytes index = 0; index < length; ++index) {
        unsigned char byte = (unsigned char) data[index];
        if (gRunEscapeState == 1u) {
            gRunEscapeState = byte == '[' ? 2u : byte == ']' ? 3u : 0u;
            continue;
        }
        if (gRunEscapeState == 2u) {
            if (byte >= '@' && byte <= '~')
                gRunEscapeState = 0u;
            continue;
        }
        if (gRunEscapeState == 3u) {
            if (byte == '\a')
                gRunEscapeState = 0u;
            else if (byte == 0x1bu)
                gRunEscapeState = 4u;
            continue;
        }
        if (gRunEscapeState == 4u) {
            gRunEscapeState = byte == '\\' ? 0u : 3u;
            continue;
        }
        if (byte == 0x1bu) {
            gRunEscapeState = 1u;
            continue;
        }
        if (byte < 0x20u && byte != '\n' && byte != '\t' && byte != '\r')
            continue;
        if (byte == '\n' || byte == '\r') {
            if (gConsoleCount == TUI_CONSOLE_MAX_LINES) {
                gConsoleHead = (gConsoleHead + 1u) % TUI_CONSOLE_MAX_LINES;
                if (gConsoleOmitted < SIZE_MAX)
                    ++gConsoleOmitted;
                if (!gConsoleFollow && gConsoleTop != 0u)
                    --gConsoleTop;
            } else {
                ++gConsoleCount;
            }
            bytes tail = (gConsoleHead + gConsoleCount - 1u) % TUI_CONSOLE_MAX_LINES;
            gConsoleLengths[tail] = 0u;
            gConsoleLineClipped = false;
            continue;
        }
        bytes tail = (gConsoleHead + gConsoleCount - 1u) % TUI_CONSOLE_MAX_LINES;
        if (gConsoleLengths[tail] < TUI_CONSOLE_LINE_BYTES) {
            gConsoleLines[tail][gConsoleLengths[tail]++] = byte == '\t' ? ' ' : (char) byte;
        } else if (!gConsoleLineClipped) {
            gConsoleLineClipped = true;
            if (gConsoleClipped < SIZE_MAX)
                ++gConsoleClipped;
        }
    }
}

static fn runRecordExit(int status) {
    if (WIFEXITED(status)) {
        (fn) snprintf(gRunStatus, sizeof gRunStatus, "finished (exit %d)",
                        WEXITSTATUS(status));
        (fn) snprintf(gRunOutcome, sizeof gRunOutcome, "exit %d", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        (fn) snprintf(gRunStatus, sizeof gRunStatus, "finished (signal %d)",
                        WTERMSIG(status));
        (fn) snprintf(gRunOutcome, sizeof gRunOutcome, "signal %d", WTERMSIG(status));
    } else {
        (fn) snprintf(gRunStatus, sizeof gRunStatus, "finished (status unknown)");
        (fn) snprintf(gRunOutcome, sizeof gRunOutcome, "status unknown");
    }
}

static fn runPoll() {
    if (gRunOutputFd >= 0) {
        char buffer[4096];
        for (bytes reads = 0u; reads < TUI_CONSOLE_READS_PER_POLL; ++reads) {
            ssize_t amount = read(gRunOutputFd, buffer, sizeof buffer);
            if (amount > 0) {
                runAppend(buffer, (bytes) amount);
                continue;
            }
            if (amount == 0) {
                (fn) close(gRunOutputFd);
                gRunOutputFd = -1;
            }
            break;
        }
    }
    if (gRunPid > 0) {
        int status = 0;
        pid_t result = waitpid(gRunPid, &status, WNOHANG);
        if (result == gRunPid || (result < 0 && errno != EINTR)) {
            if (result == gRunPid)
                runRecordExit(status);
            else {
                (fn) snprintf(gRunStatus, sizeof gRunStatus, "finished (status unknown)");
                (fn) snprintf(gRunOutcome, sizeof gRunOutcome, "status unknown");
            }
            gRunPid = -1;
            if (gScene != nullptr) {
                syncRunControls(gScene, gOpenPath, gOpenFile);
                syncTabViews(gScene);
            }
        }
    }
}

static fn runStop() {
    if (gRunPid <= 0)
        return;
    (fn) kill(gRunPid, SIGTERM);
    struct timespec pause = { .tv_sec = 0, .tv_nsec = 1000000L };
    for (bytes attempt = 0; attempt < 100u; ++attempt) {
        int status = 0;
        if (waitpid(gRunPid, &status, WNOHANG) == gRunPid) {
            runRecordExit(status);
            gRunPid = -1;
            break;
        }
        (fn) nanosleep(&pause, nullptr);
    }
    if (gRunPid > 0) {
        (fn) kill(gRunPid, SIGKILL);
        for (bytes attempt = 0; attempt < 100u; ++attempt) {
            int status = 0;
            pid_t result = waitpid(gRunPid, &status, WNOHANG);
            if (result == gRunPid || (result < 0 && errno == ECHILD)) {
                if (result == gRunPid)
                    runRecordExit(status);
                else {
                    (fn) snprintf(gRunStatus, sizeof gRunStatus, "finished (status unknown)");
                    (fn) snprintf(gRunOutcome, sizeof gRunOutcome, "status unknown");
                }
                gRunPid = -1;
                break;
            }
            (fn) nanosleep(&pause, nullptr);
        }
    }
    if (gRunOutputFd >= 0) {
        (fn) close(gRunOutputFd);
        gRunOutputFd = -1;
    }
}

static bytes consoleVisibleRows(bytes height) {
    bytes start = editorFirstRow() + 1u;
    return height > start + 1u ? height - start - 1u : 0u;
}

static fn consoleScroll(long rows, bytes height) {
    bytes visible = consoleVisibleRows(height);
    bytes maximum = gConsoleCount > visible ? gConsoleCount - visible : 0u;
    bytes top = gConsoleFollow || gConsoleTop > maximum ? maximum : gConsoleTop;
    if (rows < 0) {
        bytes move = (bytes) -(rows + 1) + 1u;
        top = move > top ? 0u : top - move;
    } else if ((bytes) rows > maximum - top) {
        top = maximum;
    } else {
        top += (bytes) rows;
    }
    gConsoleTop = top;
    gConsoleFollow = top == maximum;
}

static fn runPaint(TerminalCanvas *canvas, const Scene *scene, bytes width, bytes height) {
    if (!gRunSelected || canvas == nullptr || scene == nullptr)
        return;
    bytes x = (*scene).railWidth;
    bytes y = editorFirstRow();
    if (x >= width || y + 1u >= height)
        return;
    TerminalCanvas area = TerminalCanvas_sub(canvas, x, y, width - x, height - y - 1u);
    TerminalCanvas_fill(&area, ' ', TerminalPen());
    bytes visible = TerminalCanvas_getHeight(&area) - 1u;
    bytes maximum = gConsoleCount > visible ? gConsoleCount - visible : 0u;
    bytes first = gConsoleFollow ? maximum : gConsoleTop;
    if (first > maximum)
        first = maximum;
    TerminalPen pen = TerminalPen_3(TERMINAL_STYLE_NONE, COL_TEXT,
                                    TERMINAL_COLOR_DEFAULT);
    char header[256];
    int count = snprintf(header, sizeof header, "%s %zu/%u omitted %zu clipped %zu",
                         gRunOutcome, gConsoleCount,
                         TUI_CONSOLE_MAX_LINES, gConsoleOmitted, gConsoleClipped);
    if (count > 0)
        TerminalCanvas_text(&area, 0u, 0u, header,
                            (bytes) count < sizeof header ? (bytes) count : sizeof header - 1u,
                            TerminalPen_3(TERMINAL_STYLE_BOLD, COL_MUTED,
                                          TERMINAL_COLOR_DEFAULT));
    for (bytes row = 0u; row < visible && first + row < gConsoleCount; ++row) {
        bytes slot = (gConsoleHead + first + row) % TUI_CONSOLE_MAX_LINES;
        if (gConsoleLengths[slot] != 0u)
            TerminalCanvas_text(&area, 0u, row + 1u, gConsoleLines[slot],
                                gConsoleLengths[slot], pen);
    }
}

typedef struct Interaction {
    TerminalInput input;
    bool grabbing;          // the divider is held
    bool dividerHovered;    // the short divider grip is under the pointer
    bool tracking;          // the bottom editor rows' horizontal slider is held
    bytes trackX;          // pointer cell where the tracking drag began
    bytes trackLeft;       // horizontal offset when the tracking drag began
    bytes grabX;           // pointer cell where the grab began
    bytes grabWidth;       // rail width when the grab began
    bool gripPress;         // the press began on the hamburger
    bool grabMoved;         // the hamburger press became a drag
    u64 lastDividerClickNs;
    bool haveLastDividerClick;
    bool pressed;           // any button is currently down
    bool havePointer;
    bytes pointerX;
    bytes pointerY;
    bool mouseCapture;              // suffix bytes are still arriving
    u64 mouseCaptureUntilNs;   // and this is when a stalled report is abandoned
    bool mouseCaptureDetect;         // KEY_MOUSE fallback has not exposed its encoding yet
} Interaction;

// Scroll motion lives in Determinant's TerminalScrollGlide; keepCursorVisible and
// the frame loop borrow it and supply the file's current bounds.

static fn onSignal(int signalNumber) {
    (fn) signalNumber;
    gQuit = 1;
}

static fn initTextures() {
    memset(gLineChip, 0, sizeof gLineChip);
    (fn) snprintf(gLineChip, sizeof gLineChip, "[ln:1:1]");
    (fn) snprintf(gTimeChip, sizeof gTimeChip, "[--:--:--]");
    gWarningText[0] = '\0';
    gWarningLabel[0] = '\0';
}

// Cells a byte span occupies, using the canvas's own locale-free width rule so a
// wide glyph counts two and the horizontal offset lands on a cell boundary.
static bytes cellsOf(const char *data, bytes length) {
    bytes cells = 0;
    bytes at = 0;
    while (at < length) {
        TerminalCanvasDecodedScalar decoded = TerminalCanvas_utf8Decode(data + at, length - at);
        if (decoded.bytes == 0) {
            ++cells;
            ++at;
            continue;
        }
        cells += TerminalCanvas_widthOf(decoded.codepoint);
        at += decoded.bytes;
    }
    return cells;
}

// Byte offset of a CELL offset inside a line. Cutting on a byte offset would split
// a UTF-8 sequence in half and render two pieces of one glyph.
static bytes bytesForCells(const char *data, bytes length, bytes cells) {
    bytes consumed = 0;
    bytes at = 0;
    while (at < length && consumed < cells) {
        TerminalCanvasDecodedScalar decoded = TerminalCanvas_utf8Decode(data + at, length - at);
        if (decoded.bytes == 0) {
            ++consumed;
            ++at;
            continue;
        }
        consumed += TerminalCanvas_widthOf(decoded.codepoint);
        at += decoded.bytes;
    }
    return at;
}

static fn fileSnapshotFree(FileSnapshot *snapshot) {
    if (snapshot == nullptr)
        return;
    if ((*snapshot).lineStorage != nullptr) {
        for (bytes i = 0; i < (*snapshot).lineCount; ++i)
            free((*snapshot).lineStorage[i]);
    }
    free((*snapshot).lineStorage);
    free((*snapshot).lines);
    free((*snapshot).lineLengths);
    free((*snapshot).lineCapacities);
    free((*snapshot).lineEndings);
    *snapshot = (FileSnapshot) {0};
}

// Copy every row and the caret. A snapshot is deliberately a full document: undo is a
// cold user action, and an owned copy is the one representation that cannot alias a
// line the next edit is about to realloc or move.
static FileSnapshot fileSnapshot(const FileText *file) {
    FileSnapshot snapshot = {0};
    if (file == nullptr || (*file).lineCount == 0)
        return snapshot;
    bytes count = (*file).lineCount;
    if (count > SIZE_MAX / sizeof(char*) || count > SIZE_MAX / sizeof(bytes)
        || count > SIZE_MAX / sizeof(u8))
        return snapshot;
    snapshot.lineStorage = calloc(count, sizeof *snapshot.lineStorage);
    snapshot.lines = malloc(count * sizeof *snapshot.lines);
    snapshot.lineLengths = malloc(count * sizeof *snapshot.lineLengths);
    snapshot.lineCapacities = malloc(count * sizeof *snapshot.lineCapacities);
    snapshot.lineEndings = malloc(count * sizeof *snapshot.lineEndings);
    if (snapshot.lineStorage == nullptr || snapshot.lines == nullptr
        || snapshot.lineLengths == nullptr || snapshot.lineCapacities == nullptr
        || snapshot.lineEndings == nullptr) {
        fileSnapshotFree(&snapshot);
        return (FileSnapshot) {0};
    }
    snapshot.lineCount = count;
    snapshot.lineCapacity = count;
    snapshot.cursorLine = (*file).cursorLine;
    snapshot.cursorByte = (*file).cursorByte;
    const FileHistory *history = &(*file).history;
    snapshot.revision = (*history).currentRevision;
    for (bytes i = 0; i < count; ++i) {
        bytes length = (*file).lineLengths[i];
        if (length == SIZE_MAX) {
            fileSnapshotFree(&snapshot);
            return (FileSnapshot) {0};
        }
        char *copy = malloc(length + 1u);
        if (copy == nullptr) {
            fileSnapshotFree(&snapshot);
            return (FileSnapshot) {0};
        }
        if (length != 0)
            memcpy(copy, (*file).lineStorage[i], length);
        copy[length] = '\0';
        snapshot.lineStorage[i] = copy;
        snapshot.lines[i] = copy;
        snapshot.lineLengths[i] = length;
        snapshot.lineCapacities[i] = length + 1u;
        snapshot.lineEndings[i] = (*file).lineEndings[i];
    }
    snapshot.valid = true;
    return snapshot;
}

static fn fileSnapshotStackClear(FileSnapshotStack *stack) {
    if (stack == nullptr)
        return;
    for (bytes i = 0; i < (*stack).count; ++i)
        fileSnapshotFree(&(*stack).items[i]);
    (*stack).count = 0;
}

static fn fileSnapshotStackFree(FileSnapshotStack *stack) {
    if (stack == nullptr)
        return;
    fileSnapshotStackClear(stack);
    free((*stack).items);
    *stack = (FileSnapshotStack) {0};
}

static bool fileSnapshotStackReserve(FileSnapshotStack *stack, bytes needed) {
    if (stack == nullptr || needed <= (*stack).capacity)
        return true;
    bytes next = (*stack).capacity == 0 ? 8u : (*stack).capacity;
    while (next < needed) {
        if (next > SIZE_MAX / 2u)
            return false;
        next *= 2u;
    }
    if (next > SIZE_MAX / sizeof(FileSnapshot))
        return false;
    FileSnapshot *grown = realloc((*stack).items, next * sizeof *grown);
    if (grown == nullptr)
        return false;
    (*stack).items = grown;
    (*stack).capacity = next;
    return true;
}

// The caller reserves room first. A full history drops its oldest entry rather than
// refusing the edit the user just made.
static fn fileSnapshotStackPush(FileSnapshotStack *stack, FileSnapshot snapshot) {
    if (stack == nullptr) {
        fileSnapshotFree(&snapshot);
        return;
    }
    if ((*stack).count >= TUI_HISTORY_LIMIT) {
        fileSnapshotFree(&(*stack).items[0]);
        memmove((*stack).items, (*stack).items + 1u,
                ((*stack).count - 1u) * sizeof *(*stack).items);
        --(*stack).count;
    }
    (*stack).items[(*stack).count++] = snapshot;
}

static fn fileHistoryFree(FileHistory *history) {
    if (history == nullptr)
        return;
    fileSnapshotStackFree(&(*history).undo);
    fileSnapshotStackFree(&(*history).redo);
    fileSnapshotFree(&(*history).pending);
    *history = (FileHistory) {0};
}

static fn fileHistoryReset(FileHistory *history) {
    fileHistoryFree(history);
    (*history).currentRevision = 1u;
    (*history).nextRevision = 1u;
    (*history).savedRevision = 1u;
}

// Capture the pre-edit state and make sure the undo stack can accept it. The snapshot
// is committed only after a real change, so a no-op keystroke never eats the redo path.
static bool fileHistoryBegin(FileText *file) {
    if (file == nullptr)
        return false;
    FileHistory *history = &(*file).history;
    FileSnapshotStack *undo = &(*history).undo;
    if ((*history).pendingValid)
        return false;
    if ((*undo).count < TUI_HISTORY_LIMIT
        && !fileSnapshotStackReserve(undo, (*undo).count + 1u))
        return false;
    FileSnapshot snapshot = fileSnapshot(file);
    if (!snapshot.valid)
        return false;
    (*history).pending = snapshot;
    (*history).pendingValid = true;
    return true;
}

static fn fileHistoryCommit(FileText *file) {
    if (file == nullptr)
        return;
    FileHistory *history = &(*file).history;
    if (!(*history).pendingValid)
        return;
    fileSnapshotStackPush(&(*history).undo, (*history).pending);
    (*history).pending = (FileSnapshot) {0};
    (*history).pendingValid = false;
    fileSnapshotStackClear(&(*history).redo);
    (*history).currentRevision = ++(*history).nextRevision;
    (*file).dirty = (*history).currentRevision != (*history).savedRevision;
}

static fn fileHistoryCancel(FileText *file) {
    if (file == nullptr)
        return;
    FileHistory *history = &(*file).history;
    if (!(*history).pendingValid)
        return;
    fileSnapshotFree(&(*history).pending);
    (*history).pending = (FileSnapshot) {0};
    (*history).pendingValid = false;
}

static fn fileFree(FileText *file) {
    SemicolonSyntax_free((*file).syntax);
    for (bytes i = 0; i < (*file).lineCount; ++i)
        free((*file).lineStorage[i]);
    free((*file).lineStorage);
    free((*file).lines);
    free((*file).lineLengths);
    free((*file).lineCapacities);
    free((*file).lineEndings);
    fileHistoryFree(&(*file).history);
    *file = (FileText) {0};
}

static bool tabReserve(bytes needed) {
    if (needed <= gTabCapacity)
        return true;
    bytes next = gTabCapacity == 0u ? TUI_INITIAL_TAB_CAPACITY : gTabCapacity;
    while (next < needed) {
        if (next > SIZE_MAX / 2u)
            return false;
        next *= 2u;
    }
    if (next > SIZE_MAX / sizeof(EditorTab))
        return false;
    EditorTab *grown = realloc(gTabs, next * sizeof(EditorTab));
    if (grown == nullptr)
        return false;
    memset(grown + gTabCapacity, 0, (next - gTabCapacity) * sizeof(EditorTab));
    gTabs = grown;
    gTabCapacity = next;
    return true;
}

static bytes tabIndexForPath(const char *path) {
    if (path == nullptr)
        return SIZE_MAX;
    for (bytes index = 0; index < gTabCount; ++index)
        if (strcmp(gTabs[index].path, path) == 0)
            return index;
    return SIZE_MAX;
}

static bool tabAdmit(const char *path, const char *name) {
    if (path == nullptr || name == nullptr || strlen(path) >= sizeof gTabs[0].path
        || strlen(name) >= sizeof gTabs[0].label || !tabReserve(gTabCount + 1u))
        return false;
    EditorTab *tab = &gTabs[gTabCount];
    (fn) snprintf((*tab).path, sizeof (*tab).path, "%s", path);
    (fn) snprintf((*tab).label, sizeof (*tab).label, "%s", name);
    ++gTabCount;
    return true;
}

static bool fileReserveLines(FileText *file, bytes needed) {
    if (needed <= (*file).lineCapacity)
        return true;
    bytes next = (*file).lineCapacity == 0 ? TUI_INITIAL_LINE_CAPACITY : (*file).lineCapacity;
    while (next < needed) {
        if (next > SIZE_MAX / 2u)
            return false;
        next *= 2u;
    }
    if (next > SIZE_MAX / sizeof(char*) || next > SIZE_MAX / sizeof(bytes))
        return false;
    char **storage = malloc(next * sizeof *storage);
    const char **lines = malloc(next * sizeof *lines);
    bytes *lengths = malloc(next * sizeof *lengths);
    bytes *capacities = malloc(next * sizeof *capacities);
    u8 *endings = malloc(next * sizeof *endings);
    if (storage == nullptr || lines == nullptr || lengths == nullptr
        || capacities == nullptr || endings == nullptr) {
        free(storage);
        free(lines);
        free(lengths);
        free(capacities);
        free(endings);
        return false;
    }
    if ((*file).lineCount != 0) {
        memcpy(storage, (*file).lineStorage, (*file).lineCount * sizeof *storage);
        memcpy(lines, (*file).lines, (*file).lineCount * sizeof *lines);
        memcpy(lengths, (*file).lineLengths, (*file).lineCount * sizeof *lengths);
        memcpy(capacities, (*file).lineCapacities, (*file).lineCount * sizeof *capacities);
        memcpy(endings, (*file).lineEndings, (*file).lineCount * sizeof *endings);
    }
    free((*file).lineStorage);
    free((*file).lines);
    free((*file).lineLengths);
    free((*file).lineCapacities);
    free((*file).lineEndings);
    (*file).lineStorage = storage;
    (*file).lines = lines;
    (*file).lineLengths = lengths;
    (*file).lineCapacities = capacities;
    (*file).lineEndings = endings;
    (*file).lineCapacity = next;
    return true;
}

static bool fileAddLine(FileText *file, const char *data, bytes length, u8 ending) {
    if (length == SIZE_MAX || !fileReserveLines(file, (*file).lineCount + 1u))
        return false;
    char *line = malloc(length + 1u);
    if (line == nullptr)
        return false;
    if (length != 0)
        memcpy(line, data, length);
    line[length] = '\0';
    bytes index = (*file).lineCount++;
    (*file).lineStorage[index] = line;
    (*file).lines[index] = line;
    (*file).lineLengths[index] = length;
    (*file).lineCapacities[index] = length + 1u;
    (*file).lineEndings[index] = ending;
    return true;
}

// Admit the complete file before publishing a view. The editor retains source bytes
// exactly; rendering is a projection and never rewrites tabs or control bytes.
static bool statMatches(const struct stat *left, const struct stat *right);

static bool fileLoad(FileText *file, const char *path) {
    *file = (FileText) {0};
    const char *slash = strrchr(path, '/');
    const char *base = slash != nullptr ? slash + 1 : path;
    (fn) snprintf((*file).name, sizeof (*file).name, "%s", base);
    struct stat initial;
    if (lstat(path, &initial) != 0 || !S_ISREG(initial.st_mode))
        return false;
    FILE *handle = fopen(path, "rb");
    if (handle == nullptr)
        return false;
    char *data = nullptr;
    bytes length = 0;
    bytes capacity = 0;
    bool readOk = true;
    for (;;) {
        if (capacity - length < TUI_FILE_READ_CHUNK) {
            bytes need = length + TUI_FILE_READ_CHUNK;
            if (need < length || need == SIZE_MAX) {
                readOk = false;
                break;
            }
            bytes next = capacity == 0 ? TUI_FILE_READ_CHUNK : capacity;
            while (next < need) {
                if (next > SIZE_MAX / 2u) {
                    readOk = false;
                    break;
                }
                next *= 2u;
            }
            if (!readOk)
                break;
            char *grown = realloc(data, next + 1u);
            if (grown == nullptr) {
                readOk = false;
                break;
            }
            data = grown;
            capacity = next;
        }
        bytes got = fread(data + length, 1, capacity - length, handle);
        length += got;
        if (got == 0) {
            if (ferror(handle))
                readOk = false;
            break;
        }
    }
    int closeResult = fclose(handle);
    if (!readOk || closeResult != 0) {
        free(data);
        return false;
    }
    bytes start = 0;
    for (bytes at = 0; at <= length; ++at) {
        if (at != length && data[at] != '\n')
            continue;
        bytes end = at;
        u8 ending = at == length ? TUI_LINE_ENDING_NONE : TUI_LINE_ENDING_LF;
        if (at < length && end > start && data[end - 1u] == '\r') {
            --end;
            ending = TUI_LINE_ENDING_CRLF;
        }
        if (!fileAddLine(file, data != nullptr ? data + start : "", end - start, ending)) {
            free(data);
            fileFree(file);
            return false;
        }
        start = at + 1u;
    }
    free(data);
    if ((*file).lineCount == 0 && !fileAddLine(file, "", 0, TUI_LINE_ENDING_NONE)) {
        fileFree(file);
        return false;
    }
    (*file).length = length;
    if (stat(path, &(*file).baseline) != 0 || !statMatches(&initial, &(*file).baseline)) {
        fileFree(file);
        return false;
    }
    (*file).baselineValid = true;
    bytes longest = 0;
    for (bytes i = 0; i < (*file).lineCount; ++i) {
        bytes cells = cellsOf((*file).lines[i], (*file).lineLengths[i]);
        if (cells > longest) {
            longest = cells;
            (*file).longestLine = i;
        }
    }
    (*file).longestCells = longest;
    if (SemicolonSyntax_acceptsPath(path)) {
        (*file).syntax = SemicolonSyntax();
        (*file).syntaxPending = (*file).syntax != nullptr;
    }
    fileHistoryReset(&(*file).history);
    return true;
}

// Only document mutation requests analysis; a rejected replacement never paints
// yesterday's spans over today's source. The next edit permits a fresh attempt.
static fn fileSyntaxRefresh(FileText *file) {
    if (!(*file).syntaxPending)
        return;
    (*file).syntaxPending = false;
    (*file).syntaxCurrent = SemicolonSyntax_setLines((*file).syntax, (*file).lines,
                                                    (*file).lineLengths, (*file).lineCount);
}

static u64 monotonicNowNs() {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (u64) now.tv_sec * 1000000000ULL + (u64) now.tv_nsec;
}

static bool fileReserveLine(FileText *file, bytes line, bytes needed) {
    if (line >= (*file).lineCount || needed == SIZE_MAX)
        return false;
    bytes capacity = (*file).lineCapacities[line];
    if (needed + 1u <= capacity)
        return true;
    bytes next = capacity == 0 ? TUI_DEFAULT_LINE_CAPACITY : capacity;
    while (next < needed + 1u) {
        if (next > SIZE_MAX / 2u)
            return false;
        next *= 2u;
    }
    char *grown = realloc((*file).lineStorage[line], next);
    if (grown == nullptr)
        return false;
    (*file).lineStorage[line] = grown;
    (*file).lines[line] = grown;
    (*file).lineCapacities[line] = next;
    return true;
}

static fn fileRefreshLengthAndWidth(FileText *file) {
    (*file).syntaxPending = (*file).syntax != nullptr;
    (*file).syntaxCurrent = false;
    (*file).length = 0;
    (*file).longestCells = 0;
    (*file).longestLine = 0;
    for (bytes i = 0; i < (*file).lineCount; ++i) {
        bytes ending = (*file).lineEndings[i] == TUI_LINE_ENDING_CRLF ? 2u
                      : (*file).lineEndings[i] == TUI_LINE_ENDING_LF ? 1u : 0u;
        if ((*file).lineLengths[i] <= SIZE_MAX - ending)
            (*file).length += (*file).lineLengths[i] + ending;
        bytes cells = cellsOf((*file).lines[i], (*file).lineLengths[i]);
        if (cells > (*file).longestCells) {
            (*file).longestCells = cells;
            (*file).longestLine = i;
        }
    }
}

// Move a snapshot's owned rows into the live document. The snapshot is zeroed because
// FileText now owns its arrays; freeing it again would d64-free every line.
static fn fileHistoryRestore(FileText *file, FileSnapshot *snapshot) {
    if (file == nullptr || snapshot == nullptr || !(*snapshot).valid)
        return;
    for (bytes i = 0; i < (*file).lineCount; ++i)
        free((*file).lineStorage[i]);
    free((*file).lineStorage);
    free((*file).lines);
    free((*file).lineLengths);
    free((*file).lineCapacities);
    free((*file).lineEndings);
    (*file).lineStorage = (*snapshot).lineStorage;
    (*file).lines = (*snapshot).lines;
    (*file).lineLengths = (*snapshot).lineLengths;
    (*file).lineCapacities = (*snapshot).lineCapacities;
    (*file).lineEndings = (*snapshot).lineEndings;
    (*file).lineCount = (*snapshot).lineCount;
    (*file).lineCapacity = (*snapshot).lineCapacity;
    (*file).cursorLine = (*snapshot).cursorLine;
    (*file).cursorByte = (*snapshot).cursorByte;
    FileHistory *history = &(*file).history;
    (*history).currentRevision = (*snapshot).revision;
    (*file).cursorVisible = true;
    (*file).saveFailed = false;
    (*file).lastEditNs = monotonicNowNs();
    (*file).dirty = (*history).currentRevision != (*history).savedRevision;
    *snapshot = (FileSnapshot) {0};
    fileRefreshLengthAndWidth(file);
}

static bool fileUndo(FileText *file) {
    if (file == nullptr)
        return false;
    FileHistory *history = &(*file).history;
    FileSnapshotStack *undo = &(*history).undo;
    FileSnapshotStack *redo = &(*history).redo;
    if ((*undo).count == 0)
        return false;
    if ((*redo).count < TUI_HISTORY_LIMIT
        && !fileSnapshotStackReserve(redo, (*redo).count + 1u))
        return false;
    FileSnapshot current = fileSnapshot(file);
    if (!current.valid)
        return false;
    FileSnapshot restore = (*undo).items[--(*undo).count];
    (*undo).items[(*undo).count] = (FileSnapshot) {0};
    fileSnapshotStackPush(redo, current);
    fileHistoryRestore(file, &restore);
    return true;
}

static bool fileRedo(FileText *file) {
    if (file == nullptr)
        return false;
    FileHistory *history = &(*file).history;
    FileSnapshotStack *undo = &(*history).undo;
    FileSnapshotStack *redo = &(*history).redo;
    if ((*redo).count == 0)
        return false;
    if ((*undo).count < TUI_HISTORY_LIMIT
        && !fileSnapshotStackReserve(undo, (*undo).count + 1u))
        return false;
    FileSnapshot current = fileSnapshot(file);
    if (!current.valid)
        return false;
    FileSnapshot restore = (*redo).items[--(*redo).count];
    (*redo).items[(*redo).count] = (FileSnapshot) {0};
    fileSnapshotStackPush(undo, current);
    fileHistoryRestore(file, &restore);
    return true;
}

static fn fileRefreshEditedWidth(FileText *file, bytes line, bytes oldWidth) {
    (*file).syntaxPending = (*file).syntax != nullptr;
    (*file).syntaxCurrent = false;
    bytes width = cellsOf((*file).lines[line], (*file).lineLengths[line]);
    if (line == (*file).longestLine && width < oldWidth) {
        (*file).longestCells = 0;
        (*file).longestLine = 0;
        for (bytes i = 0; i < (*file).lineCount; ++i) {
            bytes candidate = cellsOf((*file).lines[i], (*file).lineLengths[i]);
            if (candidate > (*file).longestCells) {
                (*file).longestCells = candidate;
                (*file).longestLine = i;
            }
        }
    } else if (width >= (*file).longestCells) {
        (*file).longestCells = width;
        (*file).longestLine = line;
    }
}

static bool fileInsertBytes(FileText *file, bytes line, bytes at,
                            const char *data, bytes count) {
    if (line >= (*file).lineCount || at > (*file).lineLengths[line]
        || count > SIZE_MAX - (*file).lineLengths[line])
        return false;
    if (count == 0)
        return true;
    bytes oldLength = (*file).lineLengths[line];
    if (count > SIZE_MAX - (*file).length)
        return false;
    bytes oldWidth = cellsOf((*file).lines[line], oldLength);
    if (!fileReserveLine(file, line, oldLength + count))
        return false;
    char *text = (*file).lineStorage[line];
    memmove(text + at + count, text + at, oldLength - at + 1u);
    memcpy(text + at, data, count);
    (*file).lineLengths[line] = oldLength + count;
    (*file).length += count;
    (*file).cursorLine = line;
    (*file).cursorByte = at + count;
    (*file).dirty = true;
    (*file).saveFailed = false;
    (*file).lastEditNs = monotonicNowNs();
    fileRefreshEditedWidth(file, line, oldWidth);
    return true;
}

static bool fileInsertRow(FileText *file, bytes index, char *text, bytes length,
                          bytes capacity, u8 ending) {
    if (index > (*file).lineCount || !fileReserveLines(file, (*file).lineCount + 1u))
        return false;
    bytes move = (*file).lineCount - index;
    memmove((*file).lineStorage + index + 1u, (*file).lineStorage + index, move * sizeof *(*file).lineStorage);
    memmove((*file).lines + index + 1u, (*file).lines + index, move * sizeof *(*file).lines);
    memmove((*file).lineLengths + index + 1u, (*file).lineLengths + index, move * sizeof *(*file).lineLengths);
    memmove((*file).lineCapacities + index + 1u, (*file).lineCapacities + index,
            move * sizeof *(*file).lineCapacities);
    memmove((*file).lineEndings + index + 1u, (*file).lineEndings + index, move * sizeof *(*file).lineEndings);
    (*file).lineStorage[index] = text;
    (*file).lines[index] = text;
    (*file).lineLengths[index] = length;
    (*file).lineCapacities[index] = capacity;
    (*file).lineEndings[index] = ending;
    ++(*file).lineCount;
    return true;
}

static bool fileSplit(FileText *file) {
    bytes line = (*file).cursorLine;
    bytes at = (*file).cursorByte;
    if (line >= (*file).lineCount || at > (*file).lineLengths[line])
        return false;
    bytes oldLength = (*file).lineLengths[line];
    bytes tailLength = oldLength - at;
    char *tail = malloc(tailLength + 1u);
    if (tail == nullptr)
        return false;
    memcpy(tail, (*file).lineStorage[line] + at, tailLength);
    tail[tailLength] = '\0';
    bytes tailCapacity = tailLength + 1u;
    u8 oldEnding = (*file).lineEndings[line];
    u8 newEnding = oldEnding == TUI_LINE_ENDING_NONE ? TUI_LINE_ENDING_LF : oldEnding;
    if (!fileReserveLines(file, (*file).lineCount + 1u)) {
        free(tail);
        return false;
    }
    if (oldLength - at + 1u > (*file).lineCapacities[line]) {
        free(tail);
        return false;
    }
    (*file).lineStorage[line][at] = '\0';
    (*file).lineLengths[line] = at;
    (*file).lineEndings[line] = newEnding;
    if (!fileInsertRow(file, line + 1u, tail, tailLength, tailCapacity, oldEnding)) {
        free(tail);
        return false;
    }
    (*file).cursorLine = line + 1u;
    (*file).cursorByte = 0;
    (*file).dirty = true;
    (*file).saveFailed = false;
    (*file).lastEditNs = monotonicNowNs();
    fileRefreshLengthAndWidth(file);
    return true;
}

static bool fileJoinPrevious(FileText *file) {
    bytes line = (*file).cursorLine;
    if (line == 0 || line >= (*file).lineCount)
        return false;
    bytes previous = line - 1u;
    bytes prefix = (*file).lineLengths[previous];
    bytes suffix = (*file).lineLengths[line];
    if (suffix > SIZE_MAX - prefix || !fileReserveLine(file, previous, prefix + suffix))
        return false;
    memcpy((*file).lineStorage[previous] + prefix, (*file).lineStorage[line], suffix + 1u);
    (*file).lineLengths[previous] = prefix + suffix;
    (*file).lineEndings[previous] = (*file).lineEndings[line];
    free((*file).lineStorage[line]);
    memmove((*file).lineStorage + line, (*file).lineStorage + line + 1u,
            ((*file).lineCount - line - 1u) * sizeof *(*file).lineStorage);
    memmove((*file).lines + line, (*file).lines + line + 1u,
            ((*file).lineCount - line - 1u) * sizeof *(*file).lines);
    memmove((*file).lineLengths + line, (*file).lineLengths + line + 1u,
            ((*file).lineCount - line - 1u) * sizeof *(*file).lineLengths);
    memmove((*file).lineCapacities + line, (*file).lineCapacities + line + 1u,
            ((*file).lineCount - line - 1u) * sizeof *(*file).lineCapacities);
    memmove((*file).lineEndings + line, (*file).lineEndings + line + 1u,
            ((*file).lineCount - line - 1u) * sizeof *(*file).lineEndings);
    --(*file).lineCount;
    (*file).cursorLine = previous;
    (*file).cursorByte = prefix;
    (*file).dirty = true;
    (*file).saveFailed = false;
    (*file).lastEditNs = monotonicNowNs();
    fileRefreshLengthAndWidth(file);
    return true;
}

static bool writeAll(int descriptor, const char *data, bytes length) {
    bytes written = 0;
    while (written < length) {
        ssize_t count = write(descriptor, data + written, length - written);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        written += (bytes) count;
    }
    return true;
}

static struct timespec statModified(const struct stat *info) {
#if defined(VEX_PLATFORM_APPLE)
    return (*info).st_mtimespec;
#else
    return (*info).st_mtim;
#endif
}

static bool statMatches(const struct stat *left, const struct stat *right) {
    return (*left).st_dev == (*right).st_dev && (*left).st_ino == (*right).st_ino
        && (*left).st_size == (*right).st_size && (*left).st_mtime == (*right).st_mtime
        && statModified(left).tv_nsec == statModified(right).tv_nsec;
}

static bool fileSave(FileText *file, const char *path) {
    if (file == nullptr || path == nullptr || path[0] == '\0' || !(*file).baselineValid)
        return false;
    struct stat current;
    struct stat linkInfo;
    if (lstat(path, &linkInfo) != 0 || !S_ISREG(linkInfo.st_mode) || linkInfo.st_nlink != 1
        || stat(path, &current) != 0 || !statMatches(&(*file).baseline, &current)) {
        THROW("semicolon: save refused because the source changed or is not a single regular file");
        return false;
    }
    bytes pathLength = strlen(path);
    static const char suffix[] = ".semicolon-save-XXXXXX";
    if (pathLength > SIZE_MAX - sizeof suffix || pathLength + sizeof suffix > 1200u) {
        THROW("semicolon: save path is too long");
        return false;
    }
    char temporary[1200];
    memcpy(temporary, path, pathLength);
    memcpy(temporary + pathLength, suffix, sizeof suffix);
    int descriptor = mkstemp(temporary);
    if (descriptor < 0) {
        THROW("semicolon: cannot create staged save file");
        return false;
    }
    bool ok = fchmod(descriptor, current.st_mode & 07777) == 0;
    for (bytes i = 0; ok && i < (*file).lineCount; ++i) {
        ok = writeAll(descriptor, (*file).lines[i], (*file).lineLengths[i]);
        if (ok && (*file).lineEndings[i] == TUI_LINE_ENDING_LF)
            ok = writeAll(descriptor, "\n", 1u);
        else if (ok && (*file).lineEndings[i] == TUI_LINE_ENDING_CRLF)
            ok = writeAll(descriptor, "\r\n", 2u);
    }
    if (ok)
        ok = fsync(descriptor) == 0;
    int closeResult = close(descriptor);
    if (closeResult != 0)
        ok = false;
    if (ok) {
        // Recheck immediately before publication. This is conflict detection for
        // ordinary concurrent edits, not hostile-writer race protection.
        struct stat latest;
        ok = stat(path, &latest) == 0 && statMatches(&(*file).baseline, &latest);
    }
    if (ok)
        ok = rename(temporary, path) == 0;
    if (!ok) {
        int savedErrno = errno;
        (fn) unlink(temporary);
        errno = savedErrno;
        THROW("semicolon: staged save failed; edits remain unsaved");
        return false;
    }
    if (stat(path, &(*file).baseline) != 0) {
        (*file).baselineValid = false;
        THROW("semicolon: save published but baseline refresh failed");
        return false;
    }
    (*file).baselineValid = true;
    FileHistory *history = &(*file).history;
    (*history).savedRevision = (*history).currentRevision;
    (*file).dirty = false;
    (*file).saveFailed = false;
    return true;
}

static bytes previousScalar(const char *text, bytes at) {
    if (at == 0)
        return 0;
    --at;
    while (at > 0 && ((unsigned char) text[at] & 0xC0u) == 0x80u)
        --at;
    return at;
}

static bytes nextScalar(const char *text, bytes length, bytes at) {
    if (at >= length)
        return length;
    TerminalCanvasDecodedScalar decoded = TerminalCanvas_utf8Decode(text + at, length - at);
    return at + (decoded.bytes == 0 ? 1u : decoded.bytes);
}

static bool fileBackspace(FileText *file) {
    bytes line = (*file).cursorLine;
    if (line >= (*file).lineCount)
        return false;
    if ((*file).cursorByte == 0)
        return fileJoinPrevious(file);
    bytes previous = previousScalar((*file).lineStorage[line], (*file).cursorByte);
    bytes count = (*file).cursorByte - previous;
    bytes oldWidth = cellsOf((*file).lines[line], (*file).lineLengths[line]);
    memmove((*file).lineStorage[line] + previous, (*file).lineStorage[line] + (*file).cursorByte,
            (*file).lineLengths[line] - (*file).cursorByte + 1u);
    (*file).lineLengths[line] -= count;
    (*file).length -= count;
    (*file).cursorByte = previous;
    (*file).dirty = true;
    (*file).saveFailed = false;
    (*file).lastEditNs = monotonicNowNs();
    fileRefreshEditedWidth(file, line, oldWidth);
    return true;
}

static bool fileDeleteForward(FileText *file) {
    bytes line = (*file).cursorLine;
    if (line >= (*file).lineCount)
        return false;
    if ((*file).cursorByte == (*file).lineLengths[line]) {
        if (line + 1u >= (*file).lineCount)
            return false;
        bytes firstLength = (*file).lineLengths[line];
        bytes secondLength = (*file).lineLengths[line + 1u];
        bytes separatorLength = (*file).lineEndings[line] == TUI_LINE_ENDING_CRLF ? 2u
                               : (*file).lineEndings[line] == TUI_LINE_ENDING_LF ? 1u : 0u;
        if (secondLength > SIZE_MAX - firstLength
            || !fileReserveLine(file, line, firstLength + secondLength))
            return false;
        memcpy((*file).lineStorage[line] + firstLength, (*file).lineStorage[line + 1u], secondLength + 1u);
        (*file).lineLengths[line] = firstLength + secondLength;
        (*file).lineEndings[line] = (*file).lineEndings[line + 1u];
        free((*file).lineStorage[line + 1u]);
        memmove((*file).lineStorage + line + 1u, (*file).lineStorage + line + 2u,
                ((*file).lineCount - line - 2u) * sizeof *(*file).lineStorage);
        memmove((*file).lines + line + 1u, (*file).lines + line + 2u,
                ((*file).lineCount - line - 2u) * sizeof *(*file).lines);
        memmove((*file).lineLengths + line + 1u, (*file).lineLengths + line + 2u,
                ((*file).lineCount - line - 2u) * sizeof *(*file).lineLengths);
        memmove((*file).lineCapacities + line + 1u, (*file).lineCapacities + line + 2u,
                ((*file).lineCount - line - 2u) * sizeof *(*file).lineCapacities);
        memmove((*file).lineEndings + line + 1u, (*file).lineEndings + line + 2u,
                ((*file).lineCount - line - 2u) * sizeof *(*file).lineEndings);
        --(*file).lineCount;
        (*file).length -= separatorLength;
    } else {
        bytes end = nextScalar((*file).lineStorage[line], (*file).lineLengths[line], (*file).cursorByte);
        bytes removed = end - (*file).cursorByte;
        bytes oldWidth = cellsOf((*file).lines[line], (*file).lineLengths[line]);
        memmove((*file).lineStorage[line] + (*file).cursorByte, (*file).lineStorage[line] + end,
                (*file).lineLengths[line] - end + 1u);
        (*file).lineLengths[line] -= removed;
        (*file).length -= removed;
        (*file).dirty = true;
        (*file).saveFailed = false;
        (*file).lastEditNs = monotonicNowNs();
        fileRefreshEditedWidth(file, line, oldWidth);
        return true;
    }
    (*file).dirty = true;
    (*file).saveFailed = false;
    (*file).lastEditNs = monotonicNowNs();
    fileRefreshLengthAndWidth(file);
    return true;
}

static fn fileMoveLeft(FileText *file) {
    if ((*file).cursorLine >= (*file).lineCount)
        return;
    if ((*file).cursorByte != 0) {
        (*file).cursorByte = previousScalar((*file).lineStorage[(*file).cursorLine], (*file).cursorByte);
    } else if ((*file).cursorLine != 0) {
        --(*file).cursorLine;
        (*file).cursorByte = (*file).lineLengths[(*file).cursorLine];
    }
}

static fn fileMoveRight(FileText *file) {
    if ((*file).cursorLine >= (*file).lineCount)
        return;
    bytes length = (*file).lineLengths[(*file).cursorLine];
    if ((*file).cursorByte < length) {
        (*file).cursorByte = nextScalar((*file).lineStorage[(*file).cursorLine], length, (*file).cursorByte);
    } else if ((*file).cursorLine + 1u < (*file).lineCount) {
        ++(*file).cursorLine;
        (*file).cursorByte = 0;
    }
}

static fn fileMoveVertical(FileText *file, int delta) {
    if (delta < 0 && (*file).cursorLine > 0)
        --(*file).cursorLine;
    else if (delta > 0 && (*file).cursorLine + 1u < (*file).lineCount)
        ++(*file).cursorLine;
    if ((*file).cursorLine < (*file).lineCount && (*file).cursorByte > (*file).lineLengths[(*file).cursorLine])
        (*file).cursorByte = (*file).lineLengths[(*file).cursorLine];
}

static bool autosave(FileText *file, const char *path) {
    if (!(*file).dirty || (*file).saveFailed)
        return true;
    u64 now = monotonicNowNs();
    if (now == 0 || now < (*file).lastEditNs || now - (*file).lastEditNs < TUI_AUTOSAVE_DELAY_NS)
        return true;
    if (fileSave(file, path))
        return true;
    (*file).saveFailed = true;
    return false;
}


// ---------------------------------------------------------------- scene

static TerminalText *chip(const char *label) {
    TerminalText *text = TerminalText(label);
    if (text == nullptr)
        return nullptr;
    (fn) TerminalText_setBrackets(text, '[', ']');
    return text;
}

static TerminalText *plain(const char *label, u32 color) {
    TerminalText *text = TerminalText(label);
    if (text == nullptr)
        return nullptr;
    (fn) TerminalText_setForegroundColor(text, color);
    return text;
}

static bool putText(TerminalPanel *parent, TerminalText *text, TerminalAnchor anchor) {
    if (text == nullptr)
        return false;
    if (!TerminalPanel_isChildId(TerminalPanel_addText(parent, text, anchor))) {
        TerminalText_free(text);
        return false;
    }
    return true;
}

static bool putPanel(TerminalPanel *parent, TerminalPanel *child, TerminalAnchor anchor) {
    if (child == nullptr)
        return false;
    if (!TerminalPanel_isChildId(TerminalPanel_addPanel(parent, child, anchor))) {
        TerminalPanel_free(child);
        return false;
    }
    return true;
}

// Fit a UTF-8 string into a CELL budget, ending with one ellipsis when it does not fit.
// Truncating on bytes would split a scalar; truncating silently would make a short name
// indistinguishable from a clipped one, so the ellipsis is the contract.
static fn copyCellsEllipsis(const char *source, bytes maxCells, char *out, bytes size) {
    if (out == nullptr || size == 0u)
        return;
    if (source == nullptr) {
        out[0] = '\0';
        return;
    }
    bytes length = strlen(source);
    if (cellsOf(source, length) <= maxCells) {
        (fn) snprintf(out, size, "%s", source);
        return;
    }
    if (maxCells == 0u) {
        out[0] = '\0';
        return;
    }
    bytes at = 0u;
    bytes used = 0u;
    while (at < length) {
        TerminalCanvasDecodedScalar decoded =
            TerminalCanvas_utf8Decode(source + at, length - at);
        bytes width = decoded.bytes == 0 ? 1u : TerminalCanvas_widthOf(decoded.codepoint);
        bytes step = decoded.bytes == 0 ? 1u : decoded.bytes;
        if (used + width + 1u > maxCells)
            break;
        used += width;
        at += step;
    }
    if (at + 3u >= size)
        at = size > 4u ? size - 4u : 0u;
    memcpy(out, source, at);
    memcpy(out + at, "…", 3u);
    out[at + 3u] = '\0';
}

static TerminalPanel *buildChromeRow(Scene *scene) {
    TerminalPanel *bar = TerminalPanel();
    if (bar == nullptr)
        return nullptr;
    // The project root is painted into the rail's top row after layout; reserve its
    // width here so file tabs still begin at the editor seam.
    TerminalText *grip = plain("", COL_MUTED);
    (*scene).gripChip = grip;
    (*scene).gripId = TerminalPanel_addText(
        bar, grip, TerminalAnchor_2(TERMINAL_ANCHOR_BAND_LEFT, 0));
    if (!TerminalPanel_isChildId((*scene).gripId)) {
        TerminalPanel_free(bar);
        return nullptr;
    }
    TerminalText *projectSpacer = plain("", COL_TEXT);
    bytes spacerCells = (*scene).headerLeftWidth;
    (*scene).projectTextId = TerminalPanel_addText(
        bar, projectSpacer, TerminalAnchor_2(TERMINAL_ANCHOR_BAND_LEFT, spacerCells));
    if (!TerminalPanel_isChildId((*scene).projectTextId)) {
        TerminalText_free(projectSpacer);
        TerminalPanel_free(bar);
        return nullptr;
    }
    (*scene).header = bar;
    // Each visible tab is backed by an admitted file; the strip never invents editor tabs.
    bytes used = (*scene).headerLeftWidth;
    bytes width = gTerminalWidth > 0 ? (bytes) gTerminalWidth : 80u;
    for (bytes i = 0; i < gTabCount; ++i) {
        EditorTab *record = &gTabs[i];
        (*record).view = nullptr;
        bool open = i == gActiveTabIndex;
        (fn) snprintf((*record).display, sizeof (*record).display, "%s %s  \uf00d",
                        fileIcon((*record).label), (*record).label);
        (fn) snprintf((*record).shown, sizeof (*record).shown, "%s", (*record).display);
        const char *shown = (*record).shown;
        TerminalText *tab = chip(shown);
        if (tab == nullptr) {
            TerminalPanel_free(bar);
            return nullptr;
        }
        if (open)
            (*scene).activeTab = tab;
        bytes cells = TerminalText_measure(tab);
        if (used + cells > width) {
            bytes available = width > used ? width - used : 0u;
            if (available < 5u) {
                TerminalText_free(tab);
                break;
            }
            copyCellsEllipsis(shown, available - 2u, (*record).shown,
                              sizeof (*record).shown);
            (fn) TerminalText_setText(tab, (*record).shown, strlen((*record).shown));
            cells = TerminalText_measure(tab);
        }
        (*record).x = used;
        (*record).width = cells;
        (*record).view = tab;
        (fn) TerminalText_setForegroundColor(tab, open ? COL_TEXT : COL_MUTED);
        if (open)
            (fn) TerminalText_setBackgroundColor(tab, COL_SELECT);
        (*record).child = TerminalPanel_addText(
            bar, tab, TerminalAnchor_2(TERMINAL_ANCHOR_BAND_LEFT, cells));
        if (!TerminalPanel_isChildId((*record).child)) {
            TerminalText_free(tab);
            TerminalPanel_free(bar);
            return nullptr;
        }
        used += cells;
        if (used >= width)
            break;
    }
    (fn) TerminalPanel_setForegroundColor(bar, COL_TEXT);
    return bar;
}

// Defined with the rest of the rail: the header names the file on screen, so it needs
// the project root and the open path.
static fn breadcrumbFor(const char *path, char *out, bytes size);

static TerminalPanel *buildStatusBar(Scene *scene, const FileText *file, int width) {
    TerminalPanel *bar = TerminalPanel();
    if (bar == nullptr)
        return nullptr;
    // NO RULE and NO DASH RUNS. The footer is the two modes, the breadcrumb, then the
    // project/position/time/encoding chips, with transparent cells between them. The
    // empty cells are what keeps the whole floor draggable without drawing a line.
    TerminalText *codeMode = chip("();");
    (fn) TerminalText_setForegroundColor(codeMode, COL_ACCENT);
    (fn) TerminalText_setBackgroundColor(codeMode, COL_SELECT);
    if (!putText(bar, codeMode, TerminalAnchor_2(TERMINAL_ANCHOR_BAND_LEFT, 5))) {
        TerminalPanel_free(bar);
        return nullptr;
    }
    TerminalText *agentsMode = chip(">w<");
    (fn) TerminalText_setForegroundColor(agentsMode, COL_MUTED);
    if (!putText(bar, agentsMode, TerminalAnchor_2(TERMINAL_ANCHOR_BAND_LEFT, 5))
        || !putText(bar, plain(" ", COL_TEXT), TerminalAnchor_2(TERMINAL_ANCHOR_BAND_LEFT, 1))) {
        TerminalPanel_free(bar);
        return nullptr;
    }
    // The right side is fixed so the breadcrumb can take whatever is left: at 80 columns
    // it still keeps every chip, and at 112 it grows to its designed maximum.
    bytes rightCells = 7u + 1u + 10u + 1u + TUI_LINE_CHIP_CELLS + 1u + TUI_PROJECT_CHIP_CELLS;
    bytes available = width > (int) (TUI_STATUS_LEFT_CELLS + rightCells)
                     ? (bytes) width - TUI_STATUS_LEFT_CELLS - rightCells : 0u;
    bytes crumbCells = available < TUI_CRUMB_MIN_CELLS ? TUI_CRUMB_MIN_CELLS : available;
    if (crumbCells > TUI_CRUMB_CELLS)
        crumbCells = TUI_CRUMB_CELLS;
    // The chip BORROWS this string for the life of the tree, so it must outlive this
    // function: a stack buffer here renders as garbage. The crumb describes the file on
    // screen - a header naming a different file is a lie, not a placeholder.
    breadcrumbFor(gOpenPath[0] != '\0' ? gOpenPath : (*file).name, gBreadcrumb,
                  sizeof gBreadcrumb);
    TerminalText *crumb = chip(gBreadcrumb);
    (*scene).breadcrumb = crumb;
    (fn) TerminalText_setForegroundColor(crumb, COL_TEXT);
    if (!putText(bar, crumb, TerminalAnchor_2(TERMINAL_ANCHOR_BAND_LEFT, crumbCells))) {
        TerminalPanel_free(bar);
        return nullptr;
    }
    // Placed from the right edge inward: encoding, clock, position, project. The order is
    // the design's, and each extent is fixed so the row never reflows as the numbers grow.
    TerminalText *encoding = chip("utf-8");
    (fn) TerminalText_setForegroundColor(encoding, COL_MUTED);
    if (!putText(bar, encoding, TerminalAnchor_2(TERMINAL_ANCHOR_BAND_RIGHT, 7))
        || !putText(bar, plain(" ", COL_TEXT), TerminalAnchor_2(TERMINAL_ANCHOR_BAND_RIGHT, 1))) {
        TerminalPanel_free(bar);
        return nullptr;
    }
    (fn) snprintf(gTimeChip, sizeof gTimeChip, "[--:--:--]");
    TerminalText *clockChip = TerminalText_1(gTimeChip);
    (fn) TerminalText_setForegroundColor(clockChip, COL_MUTED);
    (*scene).timeChip = clockChip;
    if (!putText(bar, clockChip, TerminalAnchor_2(TERMINAL_ANCHOR_BAND_RIGHT, 10))
        || !putText(bar, plain(" ", COL_TEXT), TerminalAnchor_2(TERMINAL_ANCHOR_BAND_RIGHT, 1))) {
        TerminalPanel_free(bar);
        return nullptr;
    }
    (fn) snprintf(gLineChip, sizeof gLineChip, "[ln:1:1]");
    TerminalText *lineChip = TerminalText_1(gLineChip);
    (fn) TerminalText_setForegroundColor(lineChip, COL_MUTED);
    (*scene).lineChip = lineChip;
    if (!putText(bar, lineChip, TerminalAnchor_2(TERMINAL_ANCHOR_BAND_RIGHT,
                                                 TUI_LINE_CHIP_CELLS))
        || !putText(bar, plain(" ", COL_TEXT), TerminalAnchor_2(TERMINAL_ANCHOR_BAND_RIGHT, 1))) {
        TerminalPanel_free(bar);
        return nullptr;
    }
    (fn) snprintf(gProjectChip, sizeof gProjectChip, "[%s]", projectRootName());
    TerminalText *projectChip = TerminalText_1(gProjectChip);
    (fn) TerminalText_setForegroundColor(projectChip, COL_MUTED);
    (*scene).projectChip = projectChip;
    if (!putText(bar, projectChip, TerminalAnchor_2(TERMINAL_ANCHOR_BAND_RIGHT,
                                                    TUI_PROJECT_CHIP_CELLS))) {
        TerminalPanel_free(bar);
        return nullptr;
    }
    (fn) TerminalPanel_setForegroundColor(bar, COL_TEXT);
    return bar;
}

// ---------------------------------------------------------------- the rail's tree
//
// The rail IS the project. It is read ONE DIRECTORY AT A TIME, when a folder opens, so
// starting in a big checkout costs the listing of its top level and nothing else: no
// recursive walk, no watch, no index to go stale. A symlink loop cannot hang it either,
// because following one only adds another directory that is read when it is opened.
//
// The tree BORROWS every label, so each name must live as long as its node. It lives in
// RailEntry, indexed by (id - 1): ids are handed out in order and nodes are never
// removed, which makes that index stable for the life of the process.

// One listing, sorted before insertion. readdir order is not a contract, and a tree that
// reshuffles between runs is not a tree.
// SLOT RECORD: DirEntry
typedef struct DirEntry {
    char name[TUI_RAIL_TEXT];
    bool folder;
} DirEntry;

// SLOT RECORD: IgnorePattern
typedef struct IgnorePattern {
    char *text;
    bool negated;
    bool directoryOnly;
    bool anchored;
    bool hasSlash;
} IgnorePattern;

static IgnorePattern *gIgnorePatterns;
static bytes gIgnorePatternCount;
static bytes gIgnorePatternCapacity;
static struct timespec gIgnoreModified;
static bool gIgnoreStatValid;
static bool gIgnoreFromOpen;
static char gIgnorePath[1024];
static u64 gOpenIgnoreHash;

static bool visibleDotEntry(const char *name, bool folder) {
    return (folder && strcmp(name, ".github") == 0)
        || (!folder && (strcmp(name, ".gitignore") == 0 || strcmp(name, ".gitkeep") == 0));
}

static fn ignorePatternsClear() {
    for (bytes index = 0; index < gIgnorePatternCount; ++index)
        free(gIgnorePatterns[index].text);
    gIgnorePatternCount = 0u;
}

static bool ignorePatternAdd(const char *line) {
    while (*line == ' ' || *line == '\t')
        ++line;
    bytes length = strlen(line);
    while (length != 0u && (line[length - 1u] == ' ' || line[length - 1u] == '\t'))
        --length;
    if (length == 0u || line[0] == '#')
        return true;
    bool negated = line[0] == '!';
    if (negated) {
        ++line;
        --length;
    }
    if (length == 0u)
        return true;
    bool directoryOnly = line[length - 1u] == '/';
    if (directoryOnly)
        --length;
    bytes start = line[0] == '/' ? 1u : 0u;
    bool anchored = start != 0u;
    bool hasSlash = memchr(line + start, '/', length - start) != nullptr;
    if (length <= start)
        return true;
    if (gIgnorePatternCount == gIgnorePatternCapacity) {
        bytes next = gIgnorePatternCapacity == 0u ? 16u : gIgnorePatternCapacity * 2u;
        if (next < gIgnorePatternCapacity || next > SIZE_MAX / sizeof(IgnorePattern))
            return false;
        IgnorePattern *grown = realloc(gIgnorePatterns, next * sizeof(IgnorePattern));
        if (grown == nullptr)
            return false;
        gIgnorePatterns = grown;
        gIgnorePatternCapacity = next;
    }
    char *pattern = malloc(length - start + 1u);
    if (pattern == nullptr)
        return false;
    memcpy(pattern, line + start, length - start);
    pattern[length - start] = '\0';
    gIgnorePatterns[gIgnorePatternCount++] = (IgnorePattern) {
        .text = pattern, .negated = negated, .directoryOnly = directoryOnly,
        .anchored = anchored, .hasSlash = hasSlash
    };
    return true;
}

static fn ignorePatternsReload() {
    char path[sizeof gIgnorePath];
    (fn) snprintf(path, sizeof path, "%s/.gitignore", gRailRoot);
    struct stat info;
    if (gOpenFile != nullptr && strcmp(gOpenPath, path) == 0) {
        u64 hash = UINT64_C(1469598103934665603);
        for (bytes lineIndex = 0; lineIndex < (*gOpenFile).lineCount; ++lineIndex)
            for (bytes byte = 0; byte < (*gOpenFile).lineLengths[lineIndex]; ++byte) {
                hash ^= (unsigned char) (*gOpenFile).lines[lineIndex][byte];
                hash *= UINT64_C(1099511628211);
            }
        if (!gIgnoreFromOpen || strcmp(gIgnorePath, path) != 0 || hash != gOpenIgnoreHash) {
            ignorePatternsClear();
            (fn) snprintf(gIgnorePath, sizeof gIgnorePath, "%s", path);
            gIgnoreStatValid = true;
            gIgnoreFromOpen = true;
            gOpenIgnoreHash = hash;
            for (bytes lineIndex = 0; lineIndex < (*gOpenFile).lineCount; ++lineIndex)
                (fn) ignorePatternAdd((*gOpenFile).lines[lineIndex]);
        }
        return;
    }
    bool exists = stat(path, &info) == 0 && S_ISREG(info.st_mode);
    if ((exists && !gIgnoreFromOpen && gIgnoreStatValid && strcmp(path, gIgnorePath) == 0
         && info.st_mtime == gIgnoreModified.tv_sec
         && statModified(&info).tv_nsec == gIgnoreModified.tv_nsec)
        || (!exists && !gIgnoreStatValid && strcmp(path, gIgnorePath) == 0))
        return;
    ignorePatternsClear();
    gIgnoreFromOpen = false;
    (fn) snprintf(gIgnorePath, sizeof gIgnorePath, "%s", path);
    gIgnoreStatValid = exists;
    if (!exists)
        return;
    gIgnoreModified = statModified(&info);
    gOpenIgnoreHash = 0u;
    FILE *file = fopen(path, "rb");
    if (file == nullptr)
        return;
    char *line = nullptr;
    bytes capacity = 0u;
    while (getline(&line, &capacity, file) >= 0) {
        bytes length = strlen(line);
        while (length != 0u && (line[length - 1u] == '\n' || line[length - 1u] == '\r'))
            line[--length] = '\0';
        if (!ignorePatternAdd(line))
            break;
    }
    free(line);
    (fn) fclose(file);
}

static bool ignorePatternMatches(const IgnorePattern *pattern, const char *relative) {
    if ((*pattern).directoryOnly) {
        const char *slash = relative;
        while ((slash = strchr(slash, '/')) != nullptr) {
            bytes prefixLength = (bytes) (slash - relative);
            char prefix[1024];
            if (prefixLength < sizeof prefix) {
                memcpy(prefix, relative, prefixLength);
                prefix[prefixLength] = '\0';
                if (fnmatch((*pattern).text, prefix, FNM_PATHNAME) == 0)
                    return true;
            }
            ++slash;
        }
    }
    if ((*pattern).anchored || (*pattern).hasSlash)
        return fnmatch((*pattern).text, relative, FNM_PATHNAME) == 0;
    const char *at = relative;
    while (*at != '\0') {
        const char *slash = strchr(at, '/');
        bytes length = slash == nullptr ? strlen(at) : (bytes) (slash - at);
        char component[TUI_RAIL_TEXT];
        if (length < sizeof component) {
            memcpy(component, at, length);
            component[length] = '\0';
            if (fnmatch((*pattern).text, component, FNM_PATHNAME) == 0)
                return true;
        }
        if (slash == nullptr)
            break;
        at = slash + 1u;
    }
    return fnmatch((*pattern).text, relative, FNM_PATHNAME) == 0;
}

static bool railEntryIgnored(const RailEntry *entry) {
    if (entry == nullptr || strcmp((*entry).path, gRailRoot) == 0)
        return false;
    bytes rootLength = strlen(gRailRoot);
    if (strncmp((*entry).path, gRailRoot, rootLength) != 0
        || (*entry).path[rootLength] != '/')
        return false;
    const char *relative = (*entry).path + rootLength + 1u;
    bool ignored = false;
    for (bytes index = 0; index < gIgnorePatternCount; ++index) {
        IgnorePattern *pattern = &gIgnorePatterns[index];
        if (!ignorePatternMatches(pattern, relative))
            continue;
        if ((*pattern).directoryOnly && !(*entry).folder && !strchr(relative, '/'))
            continue;
        ignored = !(*pattern).negated;
    }
    return ignored;
}

static unsigned fileSortGroup(const char *name) {
    const char *extension = strrchr(name, '.');
    if (extension != nullptr && strcasecmp(extension, ".c") == 0)
        return 0u;
    if (extension != nullptr && strcasecmp(extension, ".h") == 0)
        return 1u;
    return 2u;
}

static int compareDirEntries(const_any left, const_any right) {
    const DirEntry *a = left;
    const DirEntry *b = right;
    if ((*a).folder != (*b).folder)
        return (*a).folder ? -1 : 1;              // folders first, like every browser
    if (!(*a).folder) {
        unsigned groupA = fileSortGroup((*a).name);
        unsigned groupB = fileSortGroup((*b).name);
        if (groupA != groupB)
            return groupA < groupB ? -1 : 1;
    }
    int folded = strcasecmp((*a).name, (*b).name);
    return folded != 0 ? folded : strcmp((*a).name, (*b).name);
}

static RailEntry *railEntryFor(TerminalTreeNodeId id) {
    if (id == TERMINAL_TREE_NO_NODE || (bytes) id > gRailEntryCount)
        return nullptr;
    return &gRailEntries[(bytes) id - 1u];
}

static bool railReserveEntry() {
    if (gRailEntryCount < gRailEntryCapacity)
        return true;
    bytes next = gRailEntryCapacity == 0 ? 32u : gRailEntryCapacity * 2u;
    RailEntry *grown = realloc(gRailEntries, next * sizeof(RailEntry));
    if (grown == nullptr)
        return false;
    gRailEntries = grown;
    gRailEntryCapacity = next;
    return true;
}

// Add a node and give it a home for its borrowed label. The tree is given an empty label
// first and re-pointed afterwards, because the entry array may move: handing it the
// address of a temporary would leave the tree reading freed memory.
static TerminalTreeNodeId railAddNode(TerminalTreeNodeId parent, const char *name,
                                      const char *path, bool folder) {
    if (gRail == nullptr || !railReserveEntry())
        return TERMINAL_TREE_NO_NODE;
    TerminalTreeNodeId id = TerminalTreeView_addChild(gRail, parent, "", folder);
    if (id == TERMINAL_TREE_NO_NODE)
        return TERMINAL_TREE_NO_NODE;
    RailEntry *entry = &gRailEntries[gRailEntryCount];
    (fn) snprintf((*entry).label, sizeof (*entry).label, "%s", name);
    (fn) snprintf((*entry).path, sizeof (*entry).path, "%s", path);
    (*entry).parent = parent;
    (*entry).loaded = false;
    (*entry).folder = folder;
    ++gRailEntryCount;
    (fn) TerminalTreeView_setLabel(gRail, id, (*entry).label);
    return id;
}

// Read one directory, once. A read that fails is remembered as "loaded": retrying on
// every frame would hammer the same unreadable path for no gain.
static bool railReadChildren(TerminalTreeNodeId id) {
    RailEntry *entry = railEntryFor(id);
    if (entry == nullptr)
        return false;
    if ((*entry).loaded)
        return true;
    (*entry).loaded = true;
    char parentPath[1024];
    (fn) snprintf(parentPath, sizeof parentPath, "%s", (*entry).path);
    DIR *directory = opendir(parentPath);
    if (directory == nullptr)
        return false;
    DirEntry *found = nullptr;
    bytes count = 0;
    bytes capacity = 0;
    struct dirent *item = nullptr;
    while ((item = readdir(directory)) != nullptr) {
        if (strcmp((*item).d_name, ".") == 0 || strcmp((*item).d_name, "..") == 0)
            continue;
        if (strlen((*item).d_name) >= TUI_RAIL_TEXT)
            continue;
        char child[1024];
        (fn) snprintf(child, sizeof child, "%s/%s", parentPath, (*item).d_name);
        struct stat info;
        if (stat(child, &info) != 0)
            continue;
        bool folder = S_ISDIR(info.st_mode);
        if ((*item).d_name[0] == '.' && !visibleDotEntry((*item).d_name, folder))
            continue;
        if (!folder && !S_ISREG(info.st_mode))
            continue;                             // no fifos, no devices, no sockets
        if (count == capacity) {
            bytes next = capacity == 0 ? 32u : capacity * 2u;
            DirEntry *grown = realloc(found, next * sizeof(DirEntry));
            if (grown == nullptr)
                break;
            found = grown;
            capacity = next;
        }
        (fn) snprintf(found[count].name, sizeof found[count].name, "%s", (*item).d_name);
        found[count].folder = folder;
        ++count;
    }
    (fn) closedir(directory);
    if (count > 1u)
        qsort(found, count, sizeof(DirEntry), compareDirEntries);
    // The entry array may move as children are added, so nothing taken above is used here.
    for (bytes index = 0; index < count; ++index) {
        char child[1024];
        (fn) snprintf(child, sizeof child, "%s/%s", parentPath, found[index].name);
        (fn) railAddNode(id, found[index].name, child, found[index].folder);
    }
    free(found);
    return true;
}

static TerminalTreeNodeId railFindChild(TerminalTreeNodeId parent, const char *name) {
    for (bytes index = 0; index < gRailEntryCount; ++index)
        if (gRailEntries[index].parent == parent && strcmp(gRailEntries[index].label, name) == 0)
            return (TerminalTreeNodeId) (index + 1u);
    return TERMINAL_TREE_NO_NODE;
}

// Open the path to one node by walking it from the root, reading as it goes. Returns the
// node, or NO_NODE when the path is outside this project.
static TerminalTreeNodeId railRevealPath(const char *path) {
    if (gRail == nullptr || path == nullptr || gRailRootNode == TERMINAL_TREE_NO_NODE)
        return TERMINAL_TREE_NO_NODE;
    bytes rootLength = strlen(gRailRoot);
    if (rootLength == 0 || strncmp(path, gRailRoot, rootLength) != 0 || path[rootLength] != '/')
        return TERMINAL_TREE_NO_NODE;
    TerminalTreeNodeId current = gRailRootNode;
    const char *at = path + rootLength + 1u;
    while (*at != '\0') {
        char component[TUI_RAIL_TEXT];
        bytes length = 0;
        while (at[length] != '\0' && at[length] != '/' && length + 1u < sizeof component)
            ++length;
        memcpy(component, at, length);
        component[length] = '\0';
        at += length;
        if (*at == '/')
            ++at;
        (fn) railReadChildren(current);
        TerminalTreeNodeId child = railFindChild(current, component);
        if (child == TERMINAL_TREE_NO_NODE)
            return TERMINAL_TREE_NO_NODE;
        current = child;
        if (*at != '\0')
            (fn) TerminalTreeView_setExpanded(gRail, current, true);
    }
    return current;
}

// The breadcrumb keeps BOTH ends of the path: the checkout and its first child, then the
// last directory and the file, with an ellipsis standing in for whatever is omitted. A
// file deep inside the workspace therefore reads `vexgraph > suite > … > discord > file.h`
// - neither the checkout nor the directory that owns the file is hidden. A shallow path
// has no middle to omit, so it is written out whole. The crumb band is fixed
// (TUI_CRUMB_CELLS); a crumb that outgrows it is clipped by that band, never allowed to
// run under the status chips.
static fn breadcrumbFor(const char *path, char *out, bytes size) {
    if (path == nullptr || out == nullptr || size == 0u)
        return;
    bytes rootLength = strlen(gRailRoot);
    bool inside = rootLength != 0 && strncmp(path, gRailRoot, rootLength) == 0
                  && path[rootLength] == '/';
    const char *relative = inside ? path + rootLength + 1u : path;
    // One pass over the components records the first two and the last two; those are all
    // the crumb can show, so no component array is needed. `last[0]` trails one segment
    // behind `last[1]`, which is what keeps the final directory beside the file name.
    const char *first[2] = { nullptr, nullptr };
    const char *last[2] = { nullptr, nullptr };
    bytes firstLength[2] = { 0, 0 };
    bytes lastLength[2] = { 0, 0 };
    bytes count = 0;
    const char *at = relative;
    while (*at != '\0') {
        const char *end = strchr(at, '/');
        bytes length = end == nullptr ? strlen(at) : (bytes) (end - at);
        if (length != 0) {
            if (count < 2) {
                first[count] = at;
                firstLength[count] = length;
            }
            last[0] = last[1];
            lastLength[0] = lastLength[1];
            last[1] = at;
            lastLength[1] = length;
            ++count;
        }
        if (end == nullptr)
            break;
        at = end + 1;
    }
    // A file opened from OUTSIDE the project - the default file, or an explicit --file -
    // is shown as outside it. Prefixing it with the project's name would claim a path that
    // does not exist, and a header that lies about where a file is, is worse than a short
    // one.
    const char *rootName = strrchr(gRailRoot, '/');
    rootName = rootName == nullptr ? gRailRoot : rootName + 1u;
    if (!inside) {
        if (count >= 2) {
            (fn) snprintf(out, size, "… > %.*s > %.*s",
                            (int) lastLength[0], last[0],
                            (int) lastLength[1], last[1]);
        } else if (count == 1) {
            (fn) snprintf(out, size, "… > %.*s", (int) lastLength[1], last[1]);
        } else {
            (fn) snprintf(out, size, "…");
        }
        return;
    }
    if (count == 0) {
        (fn) snprintf(out, size, "%s", rootName);
        return;
    }
    if (count <= 4) {
        // No middle to omit: write the whole relative path with spaced separators.
        // Mixing `>` and `/` in the same string reads as two different things.
        char spaced[512];
        bytes written = 0;
        for (const char *source = relative; *source != '\0' && written + 4u < sizeof spaced;
             ++source) {
            if (*source == '/') {
                spaced[written++] = ' ';
                spaced[written++] = '>';
                spaced[written++] = ' ';
            } else {
                spaced[written++] = *source;
            }
        }
        spaced[written] = '\0';
        (fn) snprintf(out, size, "%s > %s", rootName, spaced);
        return;
    }
    // Spaced separators, because the crumb is read as words rather than parsed as a path.
    // The design also shows a trailing `::symbol`; nothing here resolves a symbol yet, so
    // the crumb stops at the file rather than inventing one.
    (fn) snprintf(out, size, "%s > %.*s > … > %.*s > %.*s",
                    rootName,
                    (int) firstLength[0], first[0],
                    (int) lastLength[0], last[0],
                    (int) lastLength[1], last[1]);
}

// Open a file in the editor. The OLD file stays open when the new one cannot be read: a
// failed click must not blank the view and throw away the scroll position for nothing.
static bool openFile(const char *path, TerminalTreeNodeId node) {
    if (gOpenFile == nullptr || path == nullptr)
        return false;
    bytes target = tabIndexForPath(path);
    bool existing = target != SIZE_MAX;
    bool created = false;
    FileText opened = {0};
    if (target == SIZE_MAX) {
        if (!fileLoad(&opened, path))
            return false;
        if (!tabAdmit(path, opened.name)) {
            fileFree(&opened);
            return false;
        }
        target = gTabCount - 1u;
        created = true;
        gTabs[target].preview = isMarkdownFile(opened.name);
    }
    if ((*gOpenFile).dirty && !fileSave(gOpenFile, gOpenPath)) {
        (*gOpenFile).saveFailed = true;
        if (gScene != nullptr)
            setSaveStatus(gScene, gOpenFile);
        if (opened.lineStorage != nullptr)
            fileFree(&opened);
        if (created) {
            --gTabCount;
            gTabs[gTabCount] = (EditorTab) {0};
        }
        return false;
    }
    if (target != gActiveTabIndex && gActiveTabIndex < gTabCount) {
        gTabs[gActiveTabIndex].suspended = *gOpenFile;
        if (gScene != nullptr)
            gTabs[gActiveTabIndex].preview = (*gScene).preview;
    }
    if (target == gActiveTabIndex) {
        // A repeated click on the selected tree row must not replace its live buffer.
    } else if (gTabs[target].suspended.lineStorage != nullptr) {
        *gOpenFile = gTabs[target].suspended;
        gTabs[target].suspended = (FileText) {0};
    } else {
        *gOpenFile = opened;
        opened = (FileText) {0};
    }
    gActiveTabIndex = target;
    gTabReveal = true;
    (fn) snprintf(gOpenPath, sizeof gOpenPath, "%s", path);
    if (gRail != nullptr) {
        (fn) TerminalTreeView_setSelected(gRail, node);
        (fn) TerminalTreeView_reveal(gRail, node);
    }
    gResetView = !existing;
    if (gScene != nullptr) {
        sceneMarkdownSource(gScene, gOpenFile);
        if (gTabs[target].preview && (*gScene).markdownFile)
            (fn) sceneSetMarkdownMode(gScene, true);
        else if ((*gScene).markdownFile)
            (fn) sceneSetMarkdownMode(gScene, false);
        breadcrumbFor(path, gBreadcrumb, sizeof gBreadcrumb);
        if ((*gScene).breadcrumb != nullptr)
            (fn) TerminalText_setText((*gScene).breadcrumb, gBreadcrumb, strlen(gBreadcrumb));
        syncTabViews(gScene);
    }
    if (opened.lineStorage != nullptr)
        fileFree(&opened);
    return true;
}

static fn syncTabViews(Scene *scene) {
    if (scene == nullptr || (*scene).header == nullptr)
        return;
    // File tabs opened after the console must precede it in both paint and hit
    // order. Detach only for cold admission, not ordinary viewport refreshes.
    bool appendRunTab = false;
    for (bytes index = 0u; index < gTabCount && (*scene).runTab != nullptr; ++index)
        if (gTabs[index].view == nullptr) {
            (*scene).runTab = TerminalPanel_detachText((*scene).header, (*scene).runTabId);
            appendRunTab = (*scene).runTab != nullptr;
            break;
        }
    bytes used = (*scene).headerLeftWidth;
    bytes width = gTerminalWidth > 0 ? (bytes) gTerminalWidth : 80u;
    bytes totalTabs = gTabCount + (gRunTabVisible ? 1u : 0u);
    if (gTabFirst >= totalTabs)
        gTabFirst = totalTabs > 0u ? totalTabs - 1u : 0u;
    if (gTabReveal) {
        bytes selected = gRunSelected && gRunTabVisible ? gTabCount : gActiveTabIndex;
        if (selected < gTabFirst)
            gTabFirst = selected;
        bytes available = width > used ? width - used : 0u;
        bytes needed = 0u;
        for (bytes index = gTabFirst; index <= selected && index < totalTabs; ++index) {
            if (index < gTabCount)
                needed += cellsOf(gTabs[index].label, strlen(gTabs[index].label))
                        + cellsOf(fileIcon(gTabs[index].label), strlen(fileIcon(gTabs[index].label)))
                        + 6u;
            else
                needed += strlen(gRunStatus) + cellsOf(gRunLabel, strlen(gRunLabel)) + 6u;
        }
        while (needed > available && gTabFirst < selected) {
            EditorTab *first = &gTabs[gTabFirst];
            needed -= cellsOf((*first).label, strlen((*first).label))
                    + cellsOf(fileIcon((*first).label), strlen(fileIcon((*first).label))) + 6u;
            ++gTabFirst;
        }
        gTabReveal = false;
    }
    (*scene).activeTab = nullptr;
    for (bytes index = 0; index < gTabCount; ++index) {
        EditorTab *record = &gTabs[index];
        TerminalText *view = (*record).view;
        (fn) snprintf((*record).display, sizeof (*record).display, "%s %s  \uf00d",
                        fileIcon((*record).label), (*record).label);
        if (view == nullptr) {
            view = chip((*record).display);
            if (view == nullptr)
                continue;
        } else {
            (fn) TerminalText_setText(view, (*record).display, strlen((*record).display));
        }
        if (index < gTabFirst || used >= width || width - used < 5u) {
            (*record).x = used;
            (*record).width = 0u;
            if ((*record).view == nullptr)
                TerminalText_free(view);
            else {
                (fn) TerminalText_setText(view, "", 0u);
                (fn) TerminalText_setBrackets(view, 0u, 0u);
                (fn) TerminalPanel_setAnchor((*scene).header, (*record).child,
                                               TerminalAnchor_2(TERMINAL_ANCHOR_BAND_LEFT, 0u));
            }
            continue;
        }
        (fn) TerminalText_setBrackets(view, '[', ']');
        bytes cells = TerminalText_measure(view);
        if (used + cells > width) {
            bytes available = width > used ? width - used : 0u;
            if (available < 5u) {
                if ((*record).view == nullptr) {
                    TerminalText_free(view);
                } else {
                    (fn) TerminalText_setText(view, "", 0u);
                    (fn) TerminalText_setBrackets(view, 0u, 0u);
                    (fn) TerminalPanel_setAnchor((*scene).header, (*record).child,
                                                   TerminalAnchor_2(TERMINAL_ANCHOR_BAND_LEFT, 0u));
                }
                (*record).x = used;
                (*record).width = 0u;
                continue;
            }
            copyCellsEllipsis((*record).display, available - 4u, (*record).shown,
                              sizeof (*record).shown);
            bytes shownLength = strlen((*record).shown);
            (fn) snprintf((*record).shown + shownLength, sizeof (*record).shown - shownLength,
                            " \uf00d");
            (fn) TerminalText_setText(view, (*record).shown, strlen((*record).shown));
            cells = TerminalText_measure(view);
        } else {
            (fn) snprintf((*record).shown, sizeof (*record).shown, "%s", (*record).display);
        }
        (*record).x = used;
        (*record).width = cells;
        if ((*record).view == nullptr) {
            (*record).child = TerminalPanel_addText(
                (*scene).header, view, TerminalAnchor_2(TERMINAL_ANCHOR_BAND_LEFT, cells));
            if (!TerminalPanel_isChildId((*record).child)) {
                TerminalText_free(view);
                (*record).width = 0u;
                continue;
            }
            (*record).view = view;
        } else {
            (fn) TerminalPanel_setAnchor((*scene).header, (*record).child,
                                           TerminalAnchor_2(TERMINAL_ANCHOR_BAND_LEFT, cells));
        }
        bool active = !gRunSelected && index == gActiveTabIndex;
        (fn) TerminalText_setForegroundColor(view, active ? COL_TEXT : COL_MUTED);
        (fn) TerminalText_setBackgroundColor(view, active ? COL_SELECT
                                                            : TERMINAL_COLOR_DEFAULT);
        if (active)
            (*scene).activeTab = view;
        used += cells;
    }
    if (gRunTabVisible) {
        if (appendRunTab)
            (*scene).runTabId = TerminalPanel_addText((*scene).header, (*scene).runTab,
                                                   TerminalAnchor_2(TERMINAL_ANCHOR_BAND_LEFT, 0u));
        (fn) snprintf((*scene).runTabText, sizeof (*scene).runTabText,
                        "%s %s  \uf00d", gRunStatus, gRunLabel);
        if ((*scene).runTab == nullptr) {
            (*scene).runTab = chip((*scene).runTabText);
            if ((*scene).runTab != nullptr) {
                (*scene).runTabId = TerminalPanel_addText(
                    (*scene).header, (*scene).runTab,
                    TerminalAnchor_2(TERMINAL_ANCHOR_BAND_LEFT, 1u));
                if (!TerminalPanel_isChildId((*scene).runTabId)) {
                    TerminalText_free((*scene).runTab);
                    (*scene).runTab = nullptr;
                }
            }
        } else {
            (fn) TerminalText_setText((*scene).runTab, (*scene).runTabText,
                                        strlen((*scene).runTabText));
        }
        if ((*scene).runTab != nullptr) {
            bytes available = width > used ? width - used : 0u;
            if (gTabCount < gTabFirst || available < 5u) {
                gRunTabWidth = 0u;
                (fn) TerminalText_setText((*scene).runTab, "", 0u);
                (fn) TerminalText_setBrackets((*scene).runTab, 0u, 0u);
                (fn) TerminalPanel_setAnchor((*scene).header, (*scene).runTabId,
                                               TerminalAnchor_2(TERMINAL_ANCHOR_BAND_LEFT, 0u));
                return;
            }
            (fn) TerminalText_setBrackets((*scene).runTab, '[', ']');
            bytes cells = TerminalText_measure((*scene).runTab);
            if (cells > available && available >= 5u) {
                copyCellsEllipsis((*scene).runTabText, available - 4u,
                                  (*scene).runTabShown, sizeof (*scene).runTabShown);
                bytes shownLength = strlen((*scene).runTabShown);
                (fn) snprintf((*scene).runTabShown + shownLength,
                                sizeof (*scene).runTabShown - shownLength, " \uf00d");
                (fn) TerminalText_setText((*scene).runTab, (*scene).runTabShown,
                                            strlen((*scene).runTabShown));
                cells = TerminalText_measure((*scene).runTab);
            }
            gRunTabX = used;
            gRunTabWidth = cells <= available ? cells : 0u;
            (fn) TerminalPanel_setAnchor((*scene).header, (*scene).runTabId,
                                           TerminalAnchor_2(TERMINAL_ANCHOR_BAND_LEFT, cells));
            (fn) TerminalText_setForegroundColor((*scene).runTab,
                                                    gRunSelected ? COL_TEXT : COL_MUTED);
            (fn) TerminalText_setBackgroundColor((*scene).runTab,
                                                    gRunSelected ? COL_SELECT
                                                                 : TERMINAL_COLOR_DEFAULT);
        }
    } else {
        gRunTabWidth = 0u;
    }
}

// Wheel navigation advances whole tabs, keeping close controls attached to their
// labels. The final tab is still reachable even when one title exceeds the bar.
static fn tabScrollBy(bool forward) {
    bytes total = gTabCount + (gRunTabVisible ? 1u : 0u);
    if (forward && gTabFirst + 1u < total)
        ++gTabFirst;
    else if (!forward && gTabFirst > 0u)
        --gTabFirst;
    gTabReveal = false;
    syncTabViews(gScene);
}

static bool tabClose(bytes index) {
    if (index >= gTabCount || gTabCount <= 1u || gOpenFile == nullptr)
        return false;
    if (index == gActiveTabIndex) {
        bytes next = index + 1u < gTabCount ? index + 1u : index - 1u;
        TerminalTreeNodeId node = railRevealPath(gTabs[next].path);
        if (!openFile(gTabs[next].path, node))
            return false;
    }
    EditorTab removed = gTabs[index];
    if (removed.view != nullptr && TerminalPanel_isChildId(removed.child)) {
        TerminalText *view = TerminalPanel_detachText((*gScene).header, removed.child);
        TerminalText_free(view);
    }
    fileFree(&removed.suspended);
    if (index + 1u < gTabCount)
        memmove(&gTabs[index], &gTabs[index + 1u],
                (gTabCount - index - 1u) * sizeof(EditorTab));
    --gTabCount;
    gTabs[gTabCount] = (EditorTab) {0};
    if (index < gActiveTabIndex)
        --gActiveTabIndex;
    if (index < gTabFirst)
        --gTabFirst;
    gTabReveal = true;
    syncTabViews(gScene);
    return true;
}

static bool tabPointerPress(bytes x, bytes y, TerminalPointerButton button) {
    if (y != 0u)
        return false;
    for (bytes index = 0; index < gTabCount; ++index) {
        EditorTab *tab = &gTabs[index];
        if ((*tab).width == 0u || x < (*tab).x || x >= (*tab).x + (*tab).width)
            continue;
        if (button == TERMINAL_BUTTON_MIDDLE || x + 2u >= (*tab).x + (*tab).width)
            return tabClose(index);
        if (index == gActiveTabIndex)
        {
            gRunSelected = false;
            gTabReveal = true;
            syncTabViews(gScene);
            return true;
        }
        TerminalTreeNodeId node = railRevealPath((*tab).path);
        gRunSelected = false;
        return openFile((*tab).path, node);
    }
    if (gRunTabWidth != 0u && x >= gRunTabX && x < gRunTabX + gRunTabWidth) {
        if (button == TERMINAL_BUTTON_MIDDLE || x + 2u >= gRunTabX + gRunTabWidth) {
            runStop();
            if (gScene != nullptr && (*gScene).runTab != nullptr) {
                (fn) TerminalPanel_detachText((*gScene).header, (*gScene).runTabId);
                TerminalText_free((*gScene).runTab);
                (*gScene).runTab = nullptr;
                (*gScene).runTabId = (TerminalChildId) {0};
            }
            gRunTabVisible = false;
            gRunTabWidth = 0u;
            gRunSelected = false;
            if (gScene != nullptr)
                syncTabViews(gScene);
            return true;
        }
        gRunSelected = true;
        gTabReveal = true;
        if (gScene != nullptr)
            syncTabViews(gScene);
        return true;
    }
    return false;
}

static bool editorPlaceCaret(FileText *file, bytes x, bytes y, bytes top, bytes left,
                             bytes codeLeft, const Scene *scene) {
    if (file == nullptr || scene == nullptr)
        return false;
    // Source rows begin at the editor anchor. Controls overlay those rows; they
    // do not reserve an extra row in either plain or Markdown Code mode.
    bytes first = editorFirstRow();
    if ((*scene).preview || y < first || x < codeLeft)
        return false;
    bytes line = top + y - first;
    if (line >= (*file).lineCount)
        return false;
    bytes cell = left + x - codeLeft;
    if (cell > cellsOf((*file).lines[line], (*file).lineLengths[line]))
        cell = cellsOf((*file).lines[line], (*file).lineLengths[line]);
    (*file).cursorLine = line;
    (*file).cursorByte = bytesForCells((*file).lines[line], (*file).lineLengths[line], cell);
    (*file).cursorVisible = true;
    return true;
}

// The project's session, kept OUTSIDE the project. Writing state into the checkout would
// litter every repository the shell is pointed at, so it lives in the machine's state
// directory, one file per root, named by a hash of that root and carrying the root inside
// so a collision is caught instead of restoring somebody else's session. SEMICOLON_STATE
// overrides the directory, which is also how the owner test avoids touching a real one.
static fn cachePathFor(const char *root, char *out, bytes size) {
    u64 hash = 1469598103934665603ull;
    for (const char *at = root; *at != '\0'; ++at) {
        hash ^= (unsigned char) *at;
        hash *= 1099511628211ull;
    }
    const char *override = getenv("SEMICOLON_STATE");
    if (override != nullptr && *override != '\0') {
        (fn) snprintf(out, size, "%s/%016llx.state", override, (unsigned long long) hash);
        return;
    }
    const char *xdg = getenv("XDG_STATE_HOME");
    if (xdg != nullptr && *xdg != '\0') {
        (fn) snprintf(out, size, "%s/semicolon/%016llx.state", xdg,
                        (unsigned long long) hash);
        return;
    }
    const char *home = getenv("HOME");
    if (home == nullptr)
        home = ".";
    (fn) snprintf(out, size, "%s/%s/%016llx.state", home, TUI_STATE_DIR,
                    (unsigned long long) hash);
}

// mkdir -p, without a shell: a state directory that does not exist yet is not an error.
static bool ensureDirectory(const char *path) {
    char buffer[1024];
    (fn) snprintf(buffer, sizeof buffer, "%s", path);
    for (char *at = buffer + 1; *at != '\0'; ++at) {
        if (*at != '/')
            continue;
        *at = '\0';
        if (mkdir(buffer, 0700) != 0 && errno != EEXIST)
            return false;
        *at = '/';
    }
    return mkdir(buffer, 0700) == 0 || errno == EEXIST;
}

static fn cacheLoad(const char *root) {
    gCachedOpen[0] = '\0';
    gCachedMarkdownPreview = false;
    gCachedMarkdownModeValid = false;
    gCachedRail = 0;
    gCachedRailValid = false;
    gCachedRailCustom = false;
    gRailWidthCustomized = false;
    gCachedRailHidden = false;
    if (!gAccentOverride)
        gAccentColor = TERMINAL_RGB(0x7A, 0xC8, 0xFF);
    char path[1024];
    cachePathFor(root, path, sizeof path);
    FILE *handle = fopen(path, "r");
    if (handle == nullptr)
        return;
    char line[1300];
    bool sameRoot = false;
    char opened[1024];
    opened[0] = '\0';
    u32 cachedAccent = 0u;
    bool cachedAccentValid = false;
    bytes width = 0;
    while (fgets(line, sizeof line, handle) != nullptr) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, "root ", 5) == 0)
            sameRoot = strcmp(line + 5, root) == 0;
        else if (strncmp(line, "open ", 5) == 0)
            (fn) snprintf(opened, sizeof opened, "%s", line + 5);
        else if (strncmp(line, "markdown-preview ", 17) == 0) {
            gCachedMarkdownPreview = strtoul(line + 17, nullptr, 10) != 0;
            gCachedMarkdownModeValid = true;
        }
        else if (strncmp(line, "rail ", 5) == 0) {
            width = (bytes) strtoul(line + 5, nullptr, 10);
            gCachedRailValid = true;
        } else if (strncmp(line, "rail-custom ", 12) == 0) {
            gCachedRailCustom = strtoul(line + 12, nullptr, 10) != 0;
            gRailWidthCustomized = gCachedRailCustom;
        } else if (strncmp(line, "hidden ", 7) == 0) {
            gCachedRailHidden = strtoul(line + 7, nullptr, 10) != 0;
        } else if (strncmp(line, "accent ", 7) == 0 && !gAccentOverride) {
            cachedAccentValid = parsePackedHex(line + 7, &cachedAccent);
        }
    }
    (fn) fclose(handle);
    if (!sameRoot)
        return;                                   // a hash collision, or a reused directory
    if (opened[0] != '\0')
        (fn) snprintf(gCachedOpen, sizeof gCachedOpen, "%s", opened);
    gCachedRail = width;
    if (cachedAccentValid && !gAccentOverride)
        gAccentColor = cachedAccent;
}

// Staged and renamed, so an interrupted write cannot leave a half session that the next
// run would believe.
static fn cacheSave() {
    if (gRailRoot[0] == '\0')
        return;
    char path[1024];
    cachePathFor(gRailRoot, path, sizeof path);
    char directory[1024];
    (fn) snprintf(directory, sizeof directory, "%s", path);
    char *slash = strrchr(directory, '/');
    if (slash != nullptr) {
        *slash = '\0';
        if (!ensureDirectory(directory))
            return;
    }
    char staged[1100];
    (fn) snprintf(staged, sizeof staged, "%s.tmp", path);
    FILE *handle = fopen(staged, "w");
    if (handle == nullptr)
        return;
    fprintf(handle, "root %s\n", gRailRoot);
    fprintf(handle, "open %s\n", gOpenPath);
    fprintf(handle, "markdown-preview %d\n", gScene != nullptr && (*gScene).markdownFile
                                                   && (*gScene).preview ? 1 : 0);
    // A hidden rail still remembers the width it had, so the text-based mode is a state
    // the next run can come back from rather than a one-way trip.
    fprintf(handle, "rail %zu\n", gScene != nullptr
                                  ? ((*gScene).railVisible ? (*gScene).railWidth
                                                         : (*gScene).railRestoreWidth)
                                  : 0u);
    fprintf(handle, "rail-custom %d\n", gRailWidthCustomized ? 1 : 0);
    fprintf(handle, "hidden %d\n", gScene != nullptr && !(*gScene).railVisible ? 1 : 0);
    fprintf(handle, "accent #%02X%02X%02X\n", TERMINAL_COLOR_RED(gAccentColor),
            TERMINAL_COLOR_GREEN(gAccentColor), TERMINAL_COLOR_BLUE(gAccentColor));
    bool ok = fflush(handle) == 0;
    ok = fclose(handle) == 0 && ok;
    if (!ok || rename(staged, path) != 0)
        (fn) remove(staged);
}

// The file to open when nothing else names one: the root's first regular file in sorted
// order. Deterministic, so two runs on the same project open the same thing.
static bool firstFileIn(const char *root, char *out, bytes size) {
    DIR *directory = opendir(root);
    if (directory == nullptr)
        return false;
    char best[TUI_RAIL_TEXT];
    best[0] = '\0';
    struct dirent *item = nullptr;
    while ((item = readdir(directory)) != nullptr) {
        if ((*item).d_name[0] == '.' || strlen((*item).d_name) >= TUI_RAIL_TEXT)
            continue;
        char child[1024];
        (fn) snprintf(child, sizeof child, "%s/%s", root, (*item).d_name);
        struct stat info;
        if (stat(child, &info) != 0 || !S_ISREG(info.st_mode))
            continue;
        if (best[0] == '\0' || strcasecmp((*item).d_name, best) < 0)
            (fn) snprintf(best, sizeof best, "%s", (*item).d_name);
    }
    (fn) closedir(directory);
    if (best[0] == '\0')
        return false;
    (fn) snprintf(out, size, "%s/%s", root, best);
    return true;
}

// Build the rail at startup: one root node, its top level read, and the open file
// revealed down the tree. Nothing recursive happens here - the rest of the project is
// read when a folder is opened.
static fn railBuild(const char *root) {
    (fn) snprintf(gRailRoot, sizeof gRailRoot, "%s", root);
    gRail = TerminalTreeView();
    if (gRail == nullptr)
        return;
    const char *name = strrchr(root, '/');
    name = name == nullptr ? root : name + 1u;
    if (*name == '\0')
        name = "/";
    gRailRootNode = railAddNode(TERMINAL_TREE_NO_NODE, name, root, true);
    if (gRailRootNode == TERMINAL_TREE_NO_NODE)
        return;
    (fn) railReadChildren(gRailRootNode);
    (fn) TerminalTreeView_setExpanded(gRail, gRailRootNode, true);
}

// Compose one rail row: indentation, the fold marker, and the name. Presentation lives
// here and structure lives in the tree, which is why the tree never had to know what a
// folder looks like.
static bytes railIndent(TerminalTreeRow row) {
    // Preserve room for a disclosure marker, icon and useful filename at depth.
    bytes deepest = 6u;
    bytes levels = row.depth + 1u;
    // Two cells per nesting level keep parent/child structure legible; bounded
    // indentation still leaves filename room in a deeply nested tree.
    return (levels > deepest ? deepest : levels) * 2u;
}

static fn railFormat(char *out, bytes size, TerminalTreeRow row, bytes width) {
    // The rail sits UNDER the frame's left border, which is painted over the first cell of
    // every row - so the margin is not decoration, it is what keeps the tree off the
    // border. The design's rows have always carried it. One extra level of indent is
    // reserved for that margin; beyond that the indent stops growing so a deep node still
    // shows a name instead of nothing but spaces.
    bytes at = 0;
    bytes levels = railIndent(row);
    for (bytes level = 0; level < levels && at + 2u < size; ++level) {
        out[at++] = ' ';
    }
    // Disclosure shapes stay Unicode through the wide-character curses device boundary.
    if (row.folder)
        (fn) snprintf(out + at, size - at, "%s %s %s", row.expanded ? "⌄" : "›",
                         gNerdIcons ? (row.expanded ? "\uf07c" : "\uf07b") : "[]",
                         row.label);
    else
        (fn) snprintf(out + at, size - at, "  %s %s", fileIcon(row.label), row.label);
    // Truncate on a CELL boundary and say so with one ellipsis: a row that silently loses
    // its tail cannot be told apart from a row with a shorter name.
    bytes available = width > 2u ? width - 2u : 0u;
    bytes length = strlen(out);
    bytes skip = bytesForCells(out, length, gRailColumn);
    memmove(out, out + skip, length - skip + 1u);
    length -= skip;
    if (cellsOf(out, length) > available) {
        bytes keep = available > 1u ? available - 1u : 0u;
        length = bytesForCells(out, length, keep);
        if (length + 3u < size) {
            memcpy(out + length, "…", 3u);
            length += 3u;
            out[length] = '\0';
        }
    }
}

// Point every rail row at the tree row it shows. One Text per VISIBLE row is what makes
// folding and scrolling allocation-free: the pool is the panel's height, so a row with
// nothing to show shows nothing.
static fn sceneSyncRail(Scene *scene, bytes visible) {
    if (gRail == nullptr)
        return;
    ignorePatternsReload();
    // The viewport is what the panel can actually SHOW, not the size of the row pool: a
    // viewport as tall as the pool would let nothing scroll, because everything would
    // already fit.
    // The pinned root lives in the chrome. The viewport reserves that tree row so
    // scrolling and the last child still agree on the number of visible body rows.
    (fn) TerminalTreeView_setViewportRows(gRail, visible + 1u);
    // `first` and `shown` are the TREE's own coordinates; `visible` above is how many rows
    // the panel has to give it. Two different questions, so two different names.
    bytes first = TerminalTreeView_scrollRow(gRail);
    bytes shown = TerminalTreeView_visibleCount(gRail);
    gRailMaxColumn = 0u;
    for (bytes index = 1u; index < shown; ++index) {
        TerminalTreeRow record = TerminalTreeView_rowAt(gRail, index);
        bytes extent = railIndent(record) + 3u
                      + cellsOf(fileIcon(record.label), strlen(fileIcon(record.label)))
                      + cellsOf(record.label, strlen(record.label));
        bytes available = (*scene).railWidth > 2u ? (*scene).railWidth - 2u : 0u;
        if (extent > available && extent - available > gRailMaxColumn)
            gRailMaxColumn = extent - available;
    }
    if (gRailColumn > gRailMaxColumn)
        gRailColumn = gRailMaxColumn;
    for (bytes index = 0; index < (*scene).railRowCount; ++index) {
        TerminalText *row = (*scene).railRows[index];
        if (row == nullptr)
            continue;
        if (first + index + 1u >= shown) {
            (fn) TerminalText_setText(row, gEmptyLine, 0);
            (fn) TerminalText_setBackgroundColor(row, TERMINAL_COLOR_DEFAULT);
            continue;
        }
        TerminalTreeRow record = TerminalTreeView_rowAt(gRail, first + index + 1u);
        bytes contentWidth = (*scene).railWidth;
        railFormat((*scene).railText[index], sizeof (*scene).railText[index], record,
                   contentWidth);
        bytes rowLength = strlen((*scene).railText[index]);
        bytes rowCells = cellsOf((*scene).railText[index], rowLength);
        while (rowCells < contentWidth && rowLength + 1u < sizeof (*scene).railText[index]) {
            (*scene).railText[index][rowLength++] = ' ';
            ++rowCells;
        }
        (*scene).railText[index][rowLength] = '\0';
        (fn) TerminalText_setText(row, (*scene).railText[index],
                                    rowLength);
        (fn) TerminalText_setForegroundColor(row, 0xFFFFFFFFu);
        // The open file is the selection, and the tint is the one highlight in the shell.
        (fn) TerminalText_setBackgroundColor(row, record.selected ? COL_SELECT
                                                                   : TERMINAL_COLOR_DEFAULT);
    }
}

// Colour only the icon/name cells AFTER normal tree layout. This is a presentation
// pass over admitted cells, not ANSI inside text and not a second layout owner.
// Geometry comes from railIndent/railFormat; clipping and selection backgrounds
// survive because only the owned foreground plane changes. No allocation or IO.
static fn paintRailColors(Scene *scene, TerminalCanvas *canvas,
                            bool havePointer, bytes pointerX, bytes pointerY) {
    (fn) havePointer;
    (fn) pointerX;
    (fn) pointerY;
    bytes width = TerminalCanvas_getWidth(canvas);
    bytes height = TerminalCanvas_getHeight(canvas);
    bytes railEnd = (*scene).railWidth < width ? (*scene).railWidth : width;
    if ((*scene).railVisible && railEnd > 0u && TerminalTreeView_visibleCount(gRail) > 0u) {
        TerminalTreeRow root = TerminalTreeView_rowAt(gRail, 0u);
        char title[TUI_RAIL_TEXT];
        char clipped[TUI_RAIL_TEXT];
        (fn) snprintf(title, sizeof title, " %s %s %s",
                        root.expanded ? "⌄" : "›",
                        gNerdIcons ? (root.expanded ? "\uf07c" : "\uf07b") : "[]",
                        root.label);
        copyCellsEllipsis(title, railEnd / 2u, clipped, sizeof clipped);
        bytes titleCells = cellsOf(clipped, strlen(clipped));
        TerminalCanvas_text(canvas, 0u, 0u, clipped, strlen(clipped),
                            TerminalPen_3(TERMINAL_STYLE_NONE, COL_TEXT,
                                          TERMINAL_COLOR_DEFAULT));
        if (gNerdIcons && railEnd > 3u)
            gFg[3u] = iconColor(TerminalCanvas_glyphAt(canvas, 3u, 0u));
        if (gRailLocation[0] != '\0' && railEnd > titleCells + 1u) {
            char location[PATH_MAX + 4u];
            copyCellsEllipsis(gRailLocation, railEnd - titleCells - 1u,
                              location, sizeof location);
            TerminalCanvas_text(canvas, titleCells + 1u, 0u, location, strlen(location),
                                TerminalPen_3(TERMINAL_STYLE_NONE, TERMINAL_RGB(0x88, 0x88, 0x88),
                                              TERMINAL_COLOR_DEFAULT));
        }
    }
    bytes first = TerminalTreeView_scrollRow(gRail);
    bytes count = TerminalTreeView_visibleCount(gRail);
    for (bytes index = 0; index < (*scene).railRowCount && index + 2u < height
                           && first + index + 1u < count; ++index) {
        TerminalTreeRow row = TerminalTreeView_rowAt(gRail, first + index + 1u);
        bytes iconX = railIndent(row) + 2u;
        bytes y = index + 1u;
        RailEntry *entry = railEntryFor(row.id);
        if (gNerdIcons && iconX >= gRailColumn && iconX - gRailColumn < railEnd) {
            bytes x = iconX - gRailColumn;
            gFg[y * width + x] = iconColor(TerminalCanvas_glyphAt(canvas, x, y));
        }
        if (railEntryIgnored(entry)) {
            bytes nameX = iconX + 2u;
            nameX = nameX > gRailColumn ? nameX - gRailColumn : 0u;
            for (bytes x = nameX; x < railEnd; ++x)
                gFg[y * width + x] = TERMINAL_RGB(0xFF, 0xB8, 0x6C);
        }
    }
    // The tab strip is a literal row with bracketed labels; only private-use icon
    // cells receive the palette, never the label, bracket, close mark or mode chips.
    if (gNerdIcons)
        for (bytes x = (*scene).railWidth; x < width; ++x) {
            u32 glyph = TerminalCanvas_glyphAt(canvas, x, 0);
            if (glyph >= 0xe000u && glyph <= 0xf8ffu)
                gFg[x] = iconColor(glyph);
        }
}

// The divider is invisible at rest. Hover reveals only a three-cell grip at its
// vertical midpoint, keeping the tree free of an always-on line.
static fn paintRailSplitter(const Scene *scene, TerminalCanvas *canvas, bool visible) {
    if (scene == nullptr || canvas == nullptr)
        return;
    bytes width = TerminalCanvas_getWidth(canvas);
    bytes height = TerminalCanvas_getHeight(canvas);
    if (height < 5u || width == 0u)
        return;
    bytes middle = height / 2u;
    if (!(*scene).railVisible) {
        for (bytes y = middle - 1u; y <= middle + 1u; ++y)
            TerminalCanvas_text(canvas, 0u, y, "|", 1u,
                                TerminalPen_3(TERMINAL_STYLE_BOLD, gAccentColor,
                                              TERMINAL_COLOR_DEFAULT));
        return;
    }
    if (!visible || (*scene).railWidth == 0u || (*scene).railWidth >= width)
        return;
    bytes x = (*scene).railWidth;
    for (bytes y = middle - 1u; y <= middle + 1u && y + 1u < height; ++y)
        TerminalCanvas_text(canvas, x, y, "|", 1u,
                            TerminalPen_3(TERMINAL_STYLE_NONE,
                                          TERMINAL_RGB(0x55, 0x58, 0x62),
                                          TERMINAL_COLOR_DEFAULT));
}

static TerminalPanel *buildRail(Scene *scene, bytes rowCount) {
    TerminalPanel *rail = TerminalPanel();
    if (rail == nullptr)
        return nullptr;
    if (rowCount > TUI_MAX_ROWS)
        rowCount = TUI_MAX_ROWS;
    for (bytes i = 0; i < rowCount; ++i) {
        // Every row starts EMPTY and is pointed at a tree row by sceneSyncRail. The pool
        // is the panel's height, not the tree's size, which is what keeps a fold and a
        // scroll free of allocation: a row that has nothing to show shows nothing.
        TerminalText *row = plain(gEmptyLine, COL_TEXT);
        if (row == nullptr || !putText(rail, row, TerminalAnchor_2(TERMINAL_ANCHOR_BAND_TOP, 1))) {
            TerminalPanel_free(rail);
            return nullptr;
        }
        (*scene).railRows[i] = row;
    }
    (*scene).railRowCount = rowCount;
    (fn) TerminalPanel_setForegroundColor(rail, COL_TEXT);
    return rail;
}

// The editor: a gutter column, then the code column whose first row also carries
// the right-aligned action chips. Every row is a text that gets re-pointed rather
// than rebuilt, which is what makes scrolling allocation-free.
static TerminalPanel *buildEditor(Scene *scene, bytes rowCount) {
    TerminalPanel *editor = TerminalPanel();
    TerminalPanel *gutter = TerminalPanel();
    TerminalPanel *code = TerminalPanel();
    TerminalPanel *rows = TerminalPanel();
    TerminalPanel *actions = TerminalPanel();
    if (editor == nullptr || gutter == nullptr || code == nullptr || rows == nullptr || actions == nullptr) {
        TerminalPanel_free(editor);
        TerminalPanel_free(gutter);
        TerminalPanel_free(code);
        TerminalPanel_free(rows);
        TerminalPanel_free(actions);
        return nullptr;
    }
    for (bytes i = 0; i < rowCount; ++i) {
        TerminalText *number = plain(gEmptyLine, COL_GUTTER);
        TerminalText *line = plain(gEmptyLine, COL_TEXT);
        if (!putText(gutter, number, TerminalAnchor_2(TERMINAL_ANCHOR_BAND_TOP, 1))
            || !putText(rows, line, TerminalAnchor_2(TERMINAL_ANCHOR_BAND_TOP, 1))) {
            TerminalPanel_free(editor);
            TerminalPanel_free(gutter);
            TerminalPanel_free(code);
            TerminalPanel_free(rows);
            TerminalPanel_free(actions);
            return nullptr;
        }
        (*scene).rows[i].gutter = number;
        (*scene).rows[i].code = line;
    }
    (*scene).rowCount = rowCount;
    // Three control rows sit above the content and are a LAYERED sibling of the code
    // rows, so they paint over the first three rows without consuming any of them:
    //   row 0  [▶ run][⚠ off]
    //   row 1  [ Code][ Preview]              (Markdown only)
    // The Markdown content itself starts on row 3 (sceneMarkdownContent), which is why
    // a preview never sits under its own controls.
    //
    // Each row is its own FLEX band. That matters: a BAND_RIGHT chip RESERVES its
    // slice for the next sibling, so one shared FLEX panel would push a later row's
    // chip left by every empty chip on the rows above. Separate bands keep each row
    // flush with the editor's right edge.
    (*scene).languageHints = chip("");
    (*scene).runHint = chip("");
    (*scene).codeMode = TerminalText((*scene).markdownFile ? "[ Code]" : "");
    (*scene).previewMode = TerminalText((*scene).markdownFile ? "[ Preview]" : "");
    (*scene).codeModeWidth = 9u;
    (*scene).previewModeWidth = 11u;
    (*scene).markdownModeGap = 0u;
    // During the initial scene build gOpenFile is not assigned yet. The caller's
    // admitted file is supplied by sceneBuild after the controls have been wired.
    (fn) TerminalText_setForegroundColor((*scene).codeMode, COL_MUTED);
    (fn) TerminalText_setForegroundColor((*scene).previewMode, COL_MUTED);
    TerminalPanel *hintRow = TerminalPanel();
    TerminalPanel *modeRow = TerminalPanel();
    if ((*scene).languageHints == nullptr || (*scene).runHint == nullptr || (*scene).codeMode == nullptr
        || (*scene).previewMode == nullptr || hintRow == nullptr || modeRow == nullptr) {
        TerminalPanel_free(hintRow);
        TerminalPanel_free(modeRow);
        TerminalPanel_free(editor);
        TerminalPanel_free(gutter);
        TerminalPanel_free(code);
        TerminalPanel_free(rows);
        TerminalPanel_free(actions);
        return nullptr;
    }
    // First row: the run/stop action sits beside the gray inlay status.
    if (!putText(hintRow, (*scene).languageHints,
                 TerminalAnchor_2(TERMINAL_ANCHOR_BAND_RIGHT, TUI_HINT_CHIP_CELLS))
        || !putText(hintRow, (*scene).runHint,
                    TerminalAnchor_2(TERMINAL_ANCHOR_BAND_RIGHT, TUI_RUN_CHIP_CELLS))
        // Second row: preview at the far right, then code.
        || !putText(modeRow, (*scene).previewMode,
                    TerminalAnchor_2(TERMINAL_ANCHOR_BAND_RIGHT, (*scene).previewModeWidth))
        || !putText(modeRow, (*scene).codeMode,
                    TerminalAnchor_2(TERMINAL_ANCHOR_BAND_RIGHT, (*scene).codeModeWidth))
        ) {
        TerminalPanel_free(hintRow);
        TerminalPanel_free(modeRow);
        TerminalPanel_free(editor);
        TerminalPanel_free(gutter);
        TerminalPanel_free(code);
        TerminalPanel_free(rows);
        TerminalPanel_free(actions);
        return nullptr;
    }
    if (!putPanel(actions, hintRow, TerminalAnchor_2(TERMINAL_ANCHOR_BAND_TOP, 1))) {
        TerminalPanel_free(hintRow);
        TerminalPanel_free(modeRow);
        TerminalPanel_free(editor);
        TerminalPanel_free(gutter);
        TerminalPanel_free(code);
        TerminalPanel_free(rows);
        TerminalPanel_free(actions);
        return nullptr;
    }
    hintRow = nullptr;                              // actions owns it now
    if (!putPanel(actions, modeRow, TerminalAnchor_2(TERMINAL_ANCHOR_BAND_TOP, 1))) {
        TerminalPanel_free(modeRow);
        TerminalPanel_free(editor);
        TerminalPanel_free(gutter);
        TerminalPanel_free(code);
        TerminalPanel_free(rows);
        TerminalPanel_free(actions);
        return nullptr;
    }
    modeRow = nullptr;
    (fn) TerminalPanel_setForegroundColor(actions, COL_MUTED);
    if (!putPanel(code, rows, TerminalAnchor_0()) || !putPanel(code, actions, TerminalAnchor_0())) {
        TerminalPanel_free(editor);
        TerminalPanel_free(gutter);
        TerminalPanel_free(code);
        return nullptr;
    }
    (fn) TerminalPanel_setLayout(code, TERMINAL_PANEL_LAYERED);
    (*scene).codeActions = actions;
    // The gutter is two spaces, a right-aligned per-file number, and a period; the reserved
    // two-cell gap after it keeps code aligned without rendering a divider at rest.
    TerminalText *gap = plain("  ", COL_TEXT);
    (*scene).gutterId = TerminalPanel_addPanel(
        editor, gutter,
        TerminalAnchor_2(TERMINAL_ANCHOR_BAND_LEFT, (*scene).numberWidth + 2u + 1u));
    if (!TerminalPanel_isChildId((*scene).gutterId)) {
        TerminalPanel_free(gutter);
        TerminalText_free(gap);
        TerminalPanel_free(editor);
        return nullptr;
    }
    gutter = nullptr;
    if (!putText(editor, gap, TerminalAnchor_2(TERMINAL_ANCHOR_BAND_LEFT, TUI_CODE_PAD))
        || !putPanel(editor, code, TerminalAnchor_0())) {
        TerminalPanel_free(editor);
        return nullptr;
    }
    (*scene).editor = editor;
    return editor;
}

// The conditional warning header. It is a BAND_TOP child of the centre whose extent is
// 0 when there is nothing to say, so the editor's first row is unchanged in the normal
// case. The panel's own children are placed from the right first (the closing brace and
// the label), then the opening brace and the message take the rest, which lets the
// message absorb a rail resize without rebuilding the tree.
static TerminalPanel *buildWarningHeader(Scene *scene) {
    TerminalPanel *warning = TerminalPanel();
    if (warning == nullptr)
        return nullptr;
    TerminalText *close = plain("}", COL_ACCENT);
    TerminalText *label = chip("");
    TerminalText *open = plain("{", COL_ACCENT);
    TerminalText *message = plain(gWarningText, COL_TEXT);
    (*scene).warningLabel = label;
    (*scene).warningText = message;
    (fn) TerminalText_setForegroundColor(label, COL_MUTED);
    if (close == nullptr || label == nullptr || open == nullptr || message == nullptr
        || !putText(warning, close, TerminalAnchor_2(TERMINAL_ANCHOR_BAND_RIGHT, 1))
        || !putText(warning, label, TerminalAnchor_2(TERMINAL_ANCHOR_BAND_RIGHT,
                                                     TUI_WARNING_LABEL_CELLS))
        || !putText(warning, open, TerminalAnchor_2(TERMINAL_ANCHOR_BAND_LEFT, 1))
        || !putText(warning, message, TerminalAnchor_1(TERMINAL_ANCHOR_BAND_LEFT))) {
        TerminalPanel_free(warning);
        return nullptr;
    }
    (fn) TerminalPanel_setForegroundColor(warning, COL_TEXT);
    return warning;
}

// The centre is the warning header (zero rows when silent) over the editor. The tab
// strip lives in the chrome row, which is the compaction.
static TerminalPanel *buildCentre(Scene *scene, bytes rowCount) {
    TerminalPanel *centre = TerminalPanel();
    TerminalPanel *warning = buildWarningHeader(scene);
    TerminalPanel *editor = buildEditor(scene, rowCount);
    if (centre == nullptr || warning == nullptr || editor == nullptr) {
        TerminalPanel_free(centre);
        TerminalPanel_free(warning);
        TerminalPanel_free(editor);
        return nullptr;
    }
    (*scene).warningId = TerminalPanel_addPanel(
        centre, warning, TerminalAnchor_2(TERMINAL_ANCHOR_BAND_TOP,
                                          (*scene).warningVisible ? 1u : 0u));
    (*scene).editorId = TerminalPanel_addPanel(centre, editor, TerminalAnchor_0());
    (*scene).editorPanelVisible = true;
    if (!TerminalPanel_isChildId((*scene).warningId)
        || !TerminalPanel_isChildId((*scene).editorId)) {
        TerminalPanel_free(centre);
        return nullptr;
    }
    (*scene).centre = centre;
    return centre;
}

static fn sceneSyncRows(Scene *scene, const FileText *file, bytes first, bytes left,
                          bytes visible);
static fn sceneRenderMarkdown(Scene *scene, TerminalCanvas *canvas, bytes railWidth);

static bytes lineNumberWidth(bytes lineCount) {
    bytes width = GUTTER_MIN_NUMBER_WIDTH;
    for (bytes largest = lineCount; largest >= 10u; largest /= 10u)
        ++width;
    return width;
}

// The open file may cross a power-of-ten boundary while being edited. Resize the
// retained gutter in place so the period stays inside its band and code keeps a stable
// left edge for the new number width. Shrinking after deletions follows the same rule.
static bool sceneUpdateNumberWidth(Scene *scene, const FileText *file) {
    if (scene == nullptr || file == nullptr)
        return false;
    bytes width = lineNumberWidth((*file).lineCount);
    if (width == (*scene).numberWidth)
        return true;
    if ((*scene).editor == nullptr || !TerminalPanel_isChildId((*scene).gutterId)
        || !TerminalPanel_setAnchor((*scene).editor, (*scene).gutterId,
                                    TerminalAnchor_2(TERMINAL_ANCHOR_BAND_LEFT,
                                                    width + 2u + 1u))) {
        THROW("semicolon: could not update line-number gutter width");
        return false;
    }
    (*scene).numberWidth = width;
    return true;
}

// Build the whole tree. Every child is built first and handed over afterwards, so
// a refusal is detected before anything is partially wired. A failure here ends
// the process with a diagnostic, which is why the not-yet-handed-over locals are
// simply reclaimed by exit rather than untangled.
static fn sceneBuild(Scene *scene, const FileText *file, bytes rowCount, int width, int height) {
    *scene = (Scene) {0};
    (*scene).railWidth = TUI_RAIL_DEFAULT_WIDTH;
    (*scene).headerLeftWidth = TUI_RAIL_DEFAULT_WIDTH;
    (*scene).railRestoreWidth = TUI_RAIL_DEFAULT_WIDTH;
    (*scene).railVisible = true;
    (*scene).numberWidth = lineNumberWidth((*file).lineCount);
    gEditorFirstRow = 1u;
    sceneMarkdownSource(scene, file);
    TerminalPanel *title = buildChromeRow(scene);
    TerminalPanel *status = buildStatusBar(scene, file, width);
    TerminalPanel *rail = buildRail(scene, railVisibleRows(height));
    TerminalPanel *centre = buildCentre(scene, rowCount);
    syncRunControls(scene, gOpenPath, file);
    (*scene).root = TerminalPanel();
    if (title == nullptr || status == nullptr || rail == nullptr || centre == nullptr
        || (*scene).root == nullptr) {
        TerminalPanel_free((*scene).root);
        TerminalPanel_free(title);
        TerminalPanel_free(status);
        TerminalPanel_free(rail);
        TerminalPanel_free(centre);
        *scene = (Scene) {0};
        return;
    }
    if (!putPanel((*scene).root, title, TerminalAnchor_2(TERMINAL_ANCHOR_BAND_TOP, 1))
        || !putPanel((*scene).root, status, TerminalAnchor_2(TERMINAL_ANCHOR_BAND_BOTTOM, 1))) {
        TerminalPanel_free((*scene).root);
        TerminalPanel_free(rail);
        TerminalPanel_free(centre);
        *scene = (Scene) {0};
        return;
    }
    (*scene).rail = rail;
    (*scene).railId = TerminalPanel_addPanel((*scene).root, rail,
                                          TerminalAnchor_2(TERMINAL_ANCHOR_BAND_LEFT, (*scene).railWidth));
    if (!TerminalPanel_isChildId((*scene).railId)) {
        TerminalPanel_free((*scene).root);
        TerminalPanel_free(rail);
        TerminalPanel_free(centre);
        *scene = (Scene) {0};
        return;
    }
    if (!putPanel((*scene).root, centre, TerminalAnchor_0())) {
        TerminalPanel_free((*scene).root);
        *scene = (Scene) {0};
        return;
    }
    sceneSyncRows(scene, file, 0, 0, editorVisibleRows(height));
}

// Point every visible row at the line it should show. No allocation, no tree
// change: this is the whole of scrolling.
static fn sceneSyncRows(Scene *scene, const FileText *file, bytes first, bytes left,
                          bytes visible) {
    for (bytes i = 0; i < (*scene).rowCount; ++i) {
        TuiRow *row = &(*scene).rows[i];
        if ((*scene).markdownFile && (*scene).preview) {
            (fn) TerminalText_setText((*row).gutter, gEmptyLine, 0);
            (fn) TerminalText_setText((*row).code, gEmptyLine, 0);
            continue;
        }
        if (i >= visible || first + i >= (*file).lineCount) {
            // Past the end: the gutter goes blank with the code, so the scroll-past
            // space reads as empty rather than as a continuation of the last line.
            (fn) TerminalText_setText((*row).gutter, gEmptyLine, 0);
            (fn) TerminalText_setText((*row).code, gEmptyLine, 0);
            continue;
        }
        bytes line = first + i;
        // Match every number to the largest line number in this file. Two leading spaces
        // follow the invisible rail seam; the period is adjacent to the number.
        int written = snprintf(gGutter[i], sizeof gGutter[i], "  %*zu.",
                               (int) (*scene).numberWidth, line + 1);
        if (written < 0)
            written = 0;
        (fn) TerminalText_setText((*row).gutter, gGutter[i], (bytes) written);
        const char *lineText = (*file).lines[line];
        bytes length = (*file).lineLengths[line];
        bytes skip = bytesForCells(lineText, length, left);
        (fn) TerminalText_setText((*row).code, lineText + skip, length - skip);
    }
    if ((*scene).lineChip != nullptr) {
        // The position chip is the CURSOR, not the viewport: line first, then the
        // cursor's cell column inside that line. It is the one footer field that
        // answers "where am I typing?" rather than "what am I looking at?".
        bytes line = (*file).cursorLine < (*file).lineCount ? (*file).cursorLine + 1u : 1u;
        bytes column = 1u;
        if ((*file).cursorLine < (*file).lineCount) {
            bytes at = (*file).cursorByte;
            bytes lineLength = (*file).lineLengths[(*file).cursorLine];
            if (at > lineLength)
                at = lineLength;
            column = cellsOf((*file).lines[(*file).cursorLine], at) + 1u;
        }
        (fn) snprintf(gLineChip, sizeof gLineChip, "[ln:%zu:%zu]", line, column);
        (fn) TerminalText_setText((*scene).lineChip, gLineChip, strlen(gLineChip));
    }
}

// A rail resize moves the rail band and the blank tab spacer together, keeping tabs
// aligned with the editor while the project root remains the first tree row.
static fn sceneSetRailWidth(Scene *scene, bytes width) {
    if (scene == nullptr || (*scene).root == nullptr)
        return;
    if (width < TUI_RAIL_MIN)
        width = TUI_RAIL_MIN;
    bytes maxWidth = gTerminalWidth > (int) (TUI_RAIL_MIN + TUI_RAIL_MARGIN)
                    ? (bytes) gTerminalWidth - TUI_RAIL_MARGIN : TUI_RAIL_MIN;
    if (width > maxWidth)
        width = maxWidth;
    if ((*scene).railVisible && width == (*scene).railWidth)
        return;
    bytes oldWidth = (*scene).railWidth;
    if ((*scene).railVisible
        && !TerminalPanel_setAnchor((*scene).root, (*scene).railId,
                                    TerminalAnchor_2(TERMINAL_ANCHOR_BAND_LEFT, width)))
        return;
    bytes titleCells = width;
    if ((*scene).header != nullptr && TerminalPanel_isChildId((*scene).projectTextId)
        && !TerminalPanel_setAnchor((*scene).header, (*scene).projectTextId,
                                    TerminalAnchor_2(TERMINAL_ANCHOR_BAND_LEFT, titleCells))) {
        if ((*scene).railVisible)
            (fn) TerminalPanel_setAnchor((*scene).root, (*scene).railId,
                                           TerminalAnchor_2(TERMINAL_ANCHOR_BAND_LEFT, oldWidth));
        return;
    }
    (*scene).headerLeftWidth = width;
    (*scene).railRestoreWidth = width;
    if ((*scene).railVisible)
        (*scene).railWidth = width;
    // The content rectangle moved with the divider, so any hover chip is at a stale
    // coordinate now.
    sceneMarkdownClearFeedback(scene);
}

static fn sceneUpdateEdgeRailHandle(Scene *scene) {
    if (scene == nullptr)
        return;
    (*scene).edgeRailHandle = !(*scene).railVisible;
    bytes handleCells = 0u;
    if ((*scene).gripChip != nullptr) {
        (fn) TerminalText_setText((*scene).gripChip, "", 0u);
        (fn) TerminalText_setForegroundColor((*scene).gripChip,
                                               (*scene).edgeRailHandle ? COL_ACCENT : COL_MUTED);
    }
    if ((*scene).header != nullptr && TerminalPanel_isChildId((*scene).gripId))
        (fn) TerminalPanel_setAnchor((*scene).header, (*scene).gripId,
                                       TerminalAnchor_2(TERMINAL_ANCHOR_BAND_LEFT,
                                                        handleCells));
    (*scene).headerLeftWidth = (*scene).railVisible ? (*scene).railWidth : handleCells;
    if ((*scene).header != nullptr && TerminalPanel_isChildId((*scene).projectTextId)) {
        bytes spacerCells = (*scene).headerLeftWidth > handleCells
                           ? (*scene).headerLeftWidth - handleCells : 0u;
        (fn) TerminalPanel_setAnchor((*scene).header, (*scene).projectTextId,
                                       TerminalAnchor_2(TERMINAL_ANCHOR_BAND_LEFT,
                                                        spacerCells));
    }
}

// Show or hide the file rail. A hidden rail exposes its three-cell left-edge grip.
static bool sceneSetRailVisible(Scene *scene, bool visible) {
    if (scene == nullptr || (*scene).root == nullptr || (*scene).railVisible == visible)
        return false;
    if (visible) {
        bytes width = (*scene).railRestoreWidth >= TUI_RAIL_MIN
                     ? (*scene).railRestoreWidth : TUI_RAIL_DEFAULT_WIDTH;
        bytes maxWidth = gTerminalWidth > (int) (TUI_RAIL_MIN + TUI_RAIL_MARGIN)
                        ? (bytes) gTerminalWidth - TUI_RAIL_MARGIN : TUI_RAIL_MIN;
        if (width > maxWidth)
            width = maxWidth;
        if (!TerminalPanel_setAnchor((*scene).root, (*scene).railId,
                                     TerminalAnchor_2(TERMINAL_ANCHOR_BAND_LEFT, width)))
            return false;
        (*scene).railVisible = true;
        (*scene).railWidth = width;
        (*scene).railRestoreWidth = width;
    } else {
        if (!TerminalPanel_setAnchor((*scene).root, (*scene).railId,
                                     TerminalAnchor_2(TERMINAL_ANCHOR_BAND_LEFT, 0)))
            return false;
        (*scene).railVisible = false;
        (*scene).railWidth = 0;
    }
    sceneUpdateEdgeRailHandle(scene);
    sceneMarkdownClearFeedback(scene);
    return true;
}

static fn sceneSetGripHover(Scene *scene, bool hovered) {
    if (scene == nullptr || (*scene).gripHovered == hovered)
        return;
    (*scene).gripHovered = hovered;
    if ((*scene).gripChip == nullptr)
        return;
    (fn) TerminalText_setStyle((*scene).gripChip,
                                 hovered ? TERMINAL_STYLE_BOLD : TERMINAL_STYLE_NONE);
    (fn) TerminalText_setForegroundColor((*scene).gripChip, hovered ? COL_ACCENT : COL_MUTED);
}

// The warning is a real row only when it has something to say. Showing it moves the
// centre's first row down by one; the rail never moves, because the folders stay where
// the user left them. The message is fitted to the centre's width so the label and the
// closing brace always survive.
static fn sceneSetWarning(Scene *scene, const char *text, const char *label) {
    if (scene == nullptr)
        return;
    const char *message = text == nullptr ? "" : text;
    const char *tag = label == nullptr ? "" : label;
    bool visible = message[0] != '\0';
    bytes rail = (*scene).railVisible ? (*scene).railWidth : 0u;
    bytes centreWidth = gTerminalWidth > (int) rail ? (bytes) gTerminalWidth - rail : 0u;
    bytes messageCells = centreWidth > 1u + TUI_WARNING_LABEL_CELLS + 1u
                        ? centreWidth - 1u - TUI_WARNING_LABEL_CELLS - 1u : 0u;
    char fitted[256];
    copyCellsEllipsis(message, messageCells, fitted, sizeof fitted);
    (fn) snprintf(gWarningText, sizeof gWarningText, "%s", fitted);
    (fn) snprintf(gWarningLabel, sizeof gWarningLabel, "%s", tag);
    if ((*scene).warningText != nullptr)
        (fn) TerminalText_setText((*scene).warningText, gWarningText, strlen(gWarningText));
    if ((*scene).warningLabel != nullptr) {
        if (tag[0] != '\0')
            (fn) snprintf(gWarningLabelText, sizeof gWarningLabelText, "[%s]", tag);
        else
            gWarningLabelText[0] = '\0';
        (fn) TerminalText_setText((*scene).warningLabel, gWarningLabelText,
                                    strlen(gWarningLabelText));
        (fn) TerminalText_setBrackets((*scene).warningLabel, 0u, 0u);
    }
    if ((*scene).centre != nullptr && TerminalPanel_isChildId((*scene).warningId))
        (fn) TerminalPanel_setAnchor((*scene).centre, (*scene).warningId,
                                       TerminalAnchor_2(TERMINAL_ANCHOR_BAND_TOP,
                                                        visible ? 1u : 0u));
    (*scene).warningVisible = visible;
    gEditorFirstRow = visible ? 2u : 1u;
    sceneMarkdownClearFeedback(scene);
}

static fn sceneFree(Scene *scene) {
    TerminalPanel_free((*scene).root);   // frees every child, text and panel alike
    TerminalMarkdownView_free((*scene).markdown);
    (*scene).root = nullptr;
    (*scene).markdown = nullptr;
}

static bool sceneSetMarkdownMode(Scene *scene, bool preview) {
    if (scene == nullptr || !(*scene).markdownFile || (*scene).markdown == nullptr)
        return false;
    if (preview && gOpenFile != nullptr
        && !TerminalMarkdownView_setSource((*scene).markdown, (*gOpenFile).lines,
                                            (*gOpenFile).lineCount))
        return false;
    (*scene).preview = preview;
    if (gActiveTabIndex < gTabCount)
        gTabs[gActiveTabIndex].preview = preview;
    (fn) TerminalText_setForegroundColor((*scene).codeMode, preview ? COL_MUTED : COL_ACCENT);
    (fn) TerminalText_setForegroundColor((*scene).previewMode, preview ? COL_ACCENT : COL_MUTED);
    (fn) TerminalText_setBackgroundColor((*scene).codeMode, preview ? TERMINAL_COLOR_DEFAULT : COL_SELECT);
    (fn) TerminalText_setBackgroundColor((*scene).previewMode, preview ? COL_SELECT : TERMINAL_COLOR_DEFAULT);
    if (preview) {
        (fn) TerminalMarkdownView_setScroll((*scene).markdown, 0);
        (fn) TerminalMarkdownView_setScrollX((*scene).markdown, 0);
    }
    sceneMarkdownClearFeedback(scene);
    return true;
}

static bool sceneMarkdownClick(Scene *scene, bytes x, bytes y, bytes width) {
    if (scene == nullptr || !(*scene).markdownFile || (*scene).codeMode == nullptr
        || (*scene).previewMode == nullptr || y != editorFirstRow() + 1u
        || width < (*scene).codeModeWidth + (*scene).previewModeWidth
                   + (*scene).markdownModeGap)
        return false;
    bytes first = width - (*scene).codeModeWidth - (*scene).previewModeWidth
                 - (*scene).markdownModeGap;
    if (x >= first && x < first + (*scene).codeModeWidth)
        return sceneSetMarkdownMode(scene, false);
    bytes previewStart = first + (*scene).codeModeWidth + (*scene).markdownModeGap;
    if (x >= previewStart && x < previewStart + (*scene).previewModeWidth)
        return sceneSetMarkdownMode(scene, true);
    return false;
}

static bool runControlHit(const Scene *scene, bytes x, bytes y, bytes width) {
    if (scene == nullptr || !gRunEligible || y != editorFirstRow()
        || width < TUI_HINT_CHIP_CELLS + TUI_RUN_CHIP_CELLS)
        return false;
    bytes first = width - TUI_HINT_CHIP_CELLS - TUI_RUN_CHIP_CELLS;
    return x >= first && x < first + TUI_RUN_CHIP_CELLS;
}

// VALUE RECORD: MarkdownContent (no behaviour).
// The one Markdown content rectangle: one row under the editor header, one cell right
// of the rail divider, and above the frame's bottom border. Rendering, hover mapping and
// overlay painting all resolve through this record so their geometry cannot drift apart.
typedef struct MarkdownContent {
    bytes x;
    bytes y;
    bytes width;
    bytes height;
} MarkdownContent;

static MarkdownContent sceneMarkdownContent(bytes railWidth, bytes canvasWidth,
                                            bytes canvasHeight) {
    MarkdownContent content = {0};
    bytes x = railWidth + TUI_MARKDOWN_CONTENT_OFFSET;
    bytes y = editorFirstRow() + 3u;
    if (x >= canvasWidth || y >= canvasHeight || canvasHeight - y < 2u)
        return content;
    content.x = x;
    content.y = y;
    content.width = canvasWidth - x;
    content.height = canvasHeight - y - 1u;
    return content;
}

static fn sceneMarkdownClearNotice(Scene *scene) {
    if (scene == nullptr)
        return;
    (*scene).markdownNotice[0] = '\0';
    (*scene).markdownNoticeUntilNs = 0u;
}

static fn sceneMarkdownClearCopied(Scene *scene) {
    if (scene == nullptr)
        return;
    if ((*scene).markdown != nullptr && (*scene).copiedBlock != TUI_MARKDOWN_NO_BLOCK)
        (fn) TerminalMarkdownView_setCopied((*scene).markdown, (*scene).copiedBlock, false);
    (*scene).copiedBlock = TUI_MARKDOWN_NO_BLOCK;
    (*scene).copiedUntilNs = 0u;
}

static fn sceneMarkdownClearFeedback(Scene *scene) {
    if (scene == nullptr)
        return;
    sceneMarkdownClearCopied(scene);
    if ((*scene).markdown != nullptr)
        TerminalMarkdownView_clearHover((*scene).markdown);
    sceneMarkdownClearNotice(scene);
}

static fn sceneMarkdownSetNotice(Scene *scene, const char *text) {
    if (scene == nullptr || text == nullptr)
        return;
    (fn) snprintf((*scene).markdownNotice, sizeof (*scene).markdownNotice, "%s", text);
    (*scene).markdownNoticeUntilNs = monotonicNowNs() + TUI_MARKDOWN_NOTICE_NS;
}

// Translate a pointer cell into the DOCUMENT row/cell the view indexes. Both the hover
// and the press resolve through the same content rectangle the renderer paints, so a
// rail drag, a resize, a scroll or a mode switch cannot leave a stale coordinate. A
// table row carries its own horizontal pan; prose rows report zero, matching what the
// renderer actually drew under the pointer.
static bool sceneMarkdownCellAt(Scene *scene, bytes pointerX, bytes pointerY,
                                bytes canvasWidth, bytes canvasHeight,
                                bytes *row, bytes *column) {
    if (scene == nullptr || (*scene).markdown == nullptr
        || !(*scene).markdownFile || !(*scene).preview)
        return false;
    MarkdownContent content = sceneMarkdownContent((*scene).railWidth, canvasWidth, canvasHeight);
    if (content.width == 0u || content.height == 0u)
        return false;
    if (pointerX < content.x || pointerX >= content.x + content.width
        || pointerY < content.y || pointerY >= content.y + content.height)
        return false;
    bytes documentRow = TerminalMarkdownView_getScroll((*scene).markdown)
                       + (pointerY - content.y);
    if (row != nullptr)
        *row = documentRow;
    if (column != nullptr)
        *column = TerminalMarkdownView_rowScrollX((*scene).markdown, documentRow)
                + (pointerX - content.x);
    return true;
}

// The view owns the link-highlight state; the application only says where the pointer
// is. Leaving the content rect clears it, which is what keeps a highlight from
// surviving a rail drag or a mode switch.
static fn sceneMarkdownUpdateHover(Scene *scene, bytes pointerX, bytes pointerY,
                                     bytes canvasWidth, bytes canvasHeight) {
    if (scene == nullptr || (*scene).markdown == nullptr)
        return;
    bytes row = 0u;
    bytes column = 0u;
    if (!sceneMarkdownCellAt(scene, pointerX, pointerY, canvasWidth, canvasHeight,
                             &row, &column)) {
        TerminalMarkdownView_clearHover((*scene).markdown);
        return;
    }
    (fn) TerminalMarkdownView_setHover((*scene).markdown, row, column);
}

// Copy a fence through the REAL clipboard. The view projects the newline-separated
// source into ONE engine block, the R2 io seam writes it to the pasteboard, and the view
// paints the check for that same block until the feedback window closes. No OSC 52, no
// base64 and no second heap; an over-cap block refuses instead of truncating.
static bool sceneMarkdownCopyBlock(Scene *scene, bytes block) {
    if (scene == nullptr || (*scene).markdown == nullptr)
        return false;
    char *text = Memory_alloc(TYPE_BYTE_ARRAY, TUI_MARKDOWN_COPY_MAX_BYTES);
    if (text == nullptr) {
        sceneMarkdownSetNotice(scene, "Copy failed: no memory");
        return false;
    }
    bytes written = 0u;
    bool projected = TerminalMarkdownView_copyBlock((*scene).markdown, block, text,
                                                    TUI_MARKDOWN_COPY_MAX_BYTES, &written);
    if (!projected || written == 0u) {
        Memory_free(text);
        sceneMarkdownSetNotice(scene, projected ? "Code block is empty"
                                                : "Code block too large to copy");
        return false;
    }
    bool stored = Clipboard_setText(text);
    Memory_free(text);
    if (!stored) {
        sceneMarkdownSetNotice(scene, "Copy failed: clipboard write");
        return false;
    }
    sceneMarkdownClearCopied(scene);
    (fn) TerminalMarkdownView_setCopied((*scene).markdown, block, true);
    (*scene).copiedBlock = block;
    (*scene).copiedUntilNs = monotonicNowNs() + TUI_MARKDOWN_COPIED_NS;
    sceneMarkdownSetNotice(scene, "Copied to clipboard");
    return true;
}

// A preview can describe untrusted text, so a link press copies its target and says so
// rather than handing an arbitrary URI to a desktop opener. Control bytes are refused;
// the pasteboard ABI needs a NUL-terminated copy, which the engine allocator owns.
static bool sceneMarkdownCopyLink(Scene *scene, TerminalMarkdownAction action) {
    if (scene == nullptr || action.target == nullptr || action.targetLength == 0u
        || action.targetLength >= TUI_MARKDOWN_LINK_MAX_BYTES)
        return false;
    for (bytes index = 0u; index < action.targetLength; ++index)
        if ((unsigned char) action.target[index] < 0x20u)
            return false;
    char *target = Memory_alloc(TYPE_BYTE_ARRAY, action.targetLength + 1u);
    if (target == nullptr) {
        sceneMarkdownSetNotice(scene, "Copy failed: no memory");
        return false;
    }
    memcpy(target, action.target, action.targetLength);
    target[action.targetLength] = '\0';
    bool stored = Clipboard_setText(target);
    Memory_free(target);
    sceneMarkdownSetNotice(scene, stored ? "Link copied to clipboard"
                                         : "Copy failed: clipboard write");
    return stored;
}

// Resolve a press against the view's action table. True means the press was a Markdown
// interaction even when the action refused, so it cannot fall through and start a divider
// drag. The view retains no handler: this tier performs the clipboard write.
static bool sceneMarkdownPress(Scene *scene, bytes pointerX, bytes pointerY,
                               bytes canvasWidth, bytes canvasHeight) {
    if (scene == nullptr || (*scene).markdown == nullptr)
        return false;
    bytes row = 0u;
    bytes column = 0u;
    if (!sceneMarkdownCellAt(scene, pointerX, pointerY, canvasWidth, canvasHeight,
                             &row, &column))
        return false;
    TerminalMarkdownAction action =
        TerminalMarkdownView_actionAt((*scene).markdown, row, column);
    switch (action.kind) {
        case TERMINAL_MARKDOWN_ACTION_COPY_CODE:
            (fn) sceneMarkdownCopyBlock(scene, action.block);
            return true;
        case TERMINAL_MARKDOWN_ACTION_LINK:
            (fn) sceneMarkdownCopyLink(scene, action);
            return true;
        case TERMINAL_MARKDOWN_ACTION_NONE:
        default:
            return false;
    }
}

static TerminalPen sceneMarkdownOverlayPen() {
    return TerminalPen_3(TERMINAL_STYLE_BOLD, TERMINAL_RGB(0x1A, 0x1A, 0x1A),
                         TERMINAL_RGB(0xE6, 0xE6, 0xE6));
}

// The notice sits on the content rectangle's bottom-right cell. A dynamic notice that
// nothing displays is a silent failure, so it is painted over the document rather than
// written raw to stdout where the managed screen would hide it.
static fn sceneMarkdownPaintNotice(Scene *scene, TerminalCanvas *content) {
    if (scene == nullptr || content == nullptr || (*scene).markdownNotice[0] == '\0')
        return;
    bytes width = TerminalCanvas_getWidth(content);
    bytes height = TerminalCanvas_getHeight(content);
    bytes length = strlen((*scene).markdownNotice);
    if (width == 0u || height == 0u || length > width)
        return;
    TerminalCanvas_text(content, width - length, height - 1u, (*scene).markdownNotice, length,
                        sceneMarkdownOverlayPen());
}

static fn sceneRenderMarkdown(Scene *scene, TerminalCanvas *canvas, bytes railWidth) {
    if (scene == nullptr || canvas == nullptr || !(*scene).markdownFile || !(*scene).preview
        || (*scene).markdown == nullptr)
        return;
    bytes width = TerminalCanvas_getWidth(canvas);
    bytes height = TerminalCanvas_getHeight(canvas);
    MarkdownContent contentRect = sceneMarkdownContent(railWidth, width, height);
    if (contentRect.width == 0u || contentRect.height == 0u)
        return;
    TerminalCanvas content = TerminalCanvas_sub(canvas, contentRect.x, contentRect.y,
                                                contentRect.width, contentRect.height);
    if (!TerminalMarkdownView_setViewport((*scene).markdown, TerminalCanvas_getWidth(&content)))
        return;
    (fn) TerminalMarkdownView_setVisibleRows((*scene).markdown,
                                               TerminalCanvas_getHeight(&content));
    TerminalCanvas_fill(&content, ' ', TerminalPen());
    TerminalMarkdownView_render((*scene).markdown, &content);
    sceneMarkdownPaintNotice(scene, &content);
}

// ---------------------------------------------------------------- the presenter
//
// The host paints its OWN grid. A TerminalSurface hands over only the cells that
// differ from the last frame, and this writer turns them into absolutely addressed
// ANSI: a cursor position, one pen change, then the glyphs of that run. Nothing
// here asks ncurses to interpret a frame, which is the whole point - ncurses may
// express a one-row shift as DECSTBM plus `rin`, repeat `─` runs with `rep`, erase
// with `ech`, or insert/delete lines, and a terminal that advertises one of those
// and then handles it differently leaves the physical screen disagreeing with the
// grid. The app composed the cells; the app writes them. The device decides
// nothing.
//
// This is deferred rendering in the only form a cell terminal allows: the frame is
// composed into the grid first, the diff against the last paint is collected
// second, and the bytes are written once, inside a synchronized-output bracket,
// third. A run is usually a whole row on a repaint, so a row costs one cursor move
// and its glyphs instead of a per-cell ncurses call.
static int hexDigitValue(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// SEMICOLON_BG is written the way the rest of the ecosystem writes colours: #RRGGBB
// (opaque) or #RRGGBBAA. Anything else is refused rather than guessed at, because a
// silently mis-parsed background would fade every colour in the shell toward a value
// that was never asked for.
static bool parsePackedHex(const char *text, u32 *color) {
    if (text == nullptr)
        return false;
    if (*text == '#')
        ++text;
    else if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        text += 2;
    u32 value = 0u;
    bytes digits = 0;
    while (digits < 8u) {
        int digit = hexDigitValue(text[digits]);
        if (digit < 0)
            break;
        value = value * 16u + (u32) digit;
        ++digits;
    }
    if ((digits != 6u && digits != 8u) || text[digits] != '\0')
        return false;
    *color = digits == 6u ? ((value << 8) | 0xFFu) : value;
    return true;
}

// The device answers the OSC 11 query with an X11 colour: `rgb:RRRR/GGGG/BBBB` (16 bits
// per channel) or the short `rgb:RR/GG/BB`. Both scale to the same high Byte, so only
// the first two hex digits of each component are read and the two forms need no special
// case. The reply is scanned rather than indexed because the framing around it varies
// (BEL or ST, with or without an ESC that ncurses has already consumed).
static bool parseDeviceBackground(const char *text, bytes length, u32 *color) {
    bytes at = 0;
    while (at + 4u <= length
           && !(text[at] == 'r' && text[at + 1] == 'g' && text[at + 2] == 'b' && text[at + 3] == ':'))
        ++at;
    if (at + 4u > length)
        return false;
    at += 4u;
    u32 channels[3] = { 0u, 0u, 0u };
    for (bytes channel = 0; channel < 3u; ++channel) {
        int high = -1;
        int low = -1;
        bytes digits = 0;
        while (at < length && digits < 4u) {
            int value = hexDigitValue(text[at]);
            if (value < 0)
                break;
            if (digits == 0u)
                high = value;
            else if (digits == 1u)
                low = value;
            ++digits;
            ++at;
        }
        if (digits < 2u)
            return false;
        channels[channel] = (u32) (high * 16 + low);
        if (at < length && text[at] == '/')
            ++at;
    }
    *color = TERMINAL_RGB(channels[0], channels[1], channels[2]);
    return true;
}

// Ask the device for its background (OSC 11) and read the answer inside ONE bounded
// window. A terminal that does not answer - and plenty ignore the query outright - is
// not an error: it means the value stays whatever was already resolved. Nothing here is
// load-bearing for rendering, so nothing waits longer than TUI_PROBE_MS for it.
static bool queryDeviceBackground(u32 *color) {
    static const char request[] = "\x1b]11;?\x1b\\";
    if (write(STDOUT_FILENO, request, sizeof request - 1u) < 0)
        return false;
    char reply[TUI_PROBE_BYTES];
    bytes length = 0;
    timeout(TUI_PROBE_MS);
    int key = 0;
    while (length + 1u < sizeof reply && (key = getch()) != ERR)
        reply[length++] = (char) (key & 0xFF);
    reply[length] = '\0';
    return parseDeviceBackground(reply, length, color);
}

// Decide what is behind every transparent cell: what a fade fades TO. Stated beats
// guessed, so an explicit SEMICOLON_BG wins; otherwise the device is asked; otherwise
// the fallback stands. The caller owns the screen state (this reads bytes from it, so
// it must run with the terminal already in raw mode and before the event loop starts).
static fn resolveDeviceBackground() {
    const char *configured = getenv("SEMICOLON_BG");
    if (configured != nullptr && *configured != '\0') {
        u32 declared = 0u;
        if (!parsePackedHex(configured, &declared)) {
            THROW("semicolon: SEMICOLON_BG '%s' is not #RRGGBB; using the fallback", configured);
        } else {
            gDeviceBg = declared;
            gDeviceBgSource = "SEMICOLON_BG";
            return;
        }
    }
    u32 answered = 0u;
    if (queryDeviceBackground(&answered)) {
        gDeviceBg = answered;
        gDeviceBgSource = "the terminal answered OSC 11";
        return;
    }
    gDeviceBgSource = "the built-in fallback (this terminal did not answer)";
}

static short colorIndex(u32 color) {
    if (!TERMINAL_COLOR_IS_SET(color))
        return -1;
    int r = TERMINAL_COLOR_RED(color);
    int g = TERMINAL_COLOR_GREEN(color);
    int b = TERMINAL_COLOR_BLUE(color);
    if (COLORS >= 256) {
        int cr = (r * 5 + 127) / 255;
        int cg = (g * 5 + 127) / 255;
        int cb = (b * 5 + 127) / 255;
        return (short) (16 + 36 * cr + 6 * cg + cb);
    }
    int luminance = (r * 30 + g * 59 + b * 11) / 100;
    if (luminance < 64)
        return COLOR_BLACK;
    if (luminance < 128)
        return COLOR_BLUE;
    if (luminance < 200)
        return COLOR_CYAN;
    return COLOR_WHITE;
}

// The bytes one frame is built in. A full 400x200 repaint of wide text is far
// smaller than this, but a pathological frame may outgrow it: the buffer then
// flushes INSIDE the synchronized-output bracket, which still publishes the frame
// as one update, so the bound never changes what the device sees - only how many
// write calls it takes.
#define TUI_OUT_CAPACITY 262144u
#define TUI_PEN_CAPACITY 64u

static char gOut[TUI_OUT_CAPACITY];
static bytes gOutLength;
static bool gOutFailed;
static bytes gCursorColumn = (bytes) -1;   // (bytes) -1 = the device cursor is unknown
static bytes gCursorRow = (bytes) -1;
static char gPen[TUI_PEN_CAPACITY];
static bytes gPenLength;
static bool gPenValid;
static bool gDeviceColour;
static bytes gPresentWidth;
// A resize blanked the device outside the surface's knowledge, so the next present
// must repaint every cell - even when the clamped geometry did not change (a
// terminal growing past TUI_MAX_W is the case that would otherwise leave a blank
// screen behind a diff that believes it is already there).
static bool gRepaintAll;

static fn presenterFlush() {
    if (gOutLength == 0u)
        return;
    if (!writeAll(STDOUT_FILENO, gOut, gOutLength))
        gOutFailed = true;
    gOutLength = 0u;
}

// Append without ever growing past the static buffer: a full buffer is written out
// in place, so the frame costs a bounded amount of memory and no allocation at all.
static fn presenterAppend(const char *data, bytes length) {
    while (length != 0u) {
        bytes room = sizeof gOut - gOutLength;
        if (room == 0u) {
            presenterFlush();
            room = sizeof gOut;
        }
        bytes count = length < room ? length : room;
        memcpy(gOut + gOutLength, data, count);
        gOutLength += count;
        data += count;
        length -= count;
    }
}

static fn presenterText(const char *text) {
    presenterAppend(text, strlen(text));
}

static fn presenterByte(char byte) {
    presenterAppend(&byte, 1u);
}

// One colour parameter, prefixed with the separator it needs: `;38;5;N` for the
// 256-cell cube, `;3N`/`;4N` for a basic eight-colour device. An unset colour is
// written as nothing, so the reset at the front of the pen leaves the device's own
// default in place.
static bytes penColour(char *params, bytes capacity, bytes at, bool foreground,
                        u32 colour) {
    if (!gDeviceColour || !TERMINAL_COLOR_IS_SET(colour))
        return at;
    short index = colorIndex(colour);
    if (index < 0)
        return at;
    int written;
    if (COLORS >= 256)
        written = snprintf(params + at, capacity - at, ";%d8;5;%d",
                           foreground ? 3 : 4, (int) index);
    else
        written = snprintf(params + at, capacity - at, ";%d",
                           (int) (foreground ? 30 : 40) + index);
    if (written <= 0 || (bytes) written >= capacity - at)
        return at;
    return at + (bytes) written;
}

// A cell's pen as SGR parameters: `0` first, so no previous cell's weight,
// decoration or colour survives into this one, then only what this cell sets.
static bytes penParams(char *params, bytes capacity, u16 style, u32 fg,
                        u32 bg) {
    int written = snprintf(params, capacity, "0%s%s%s%s%s",
                           (style & TERMINAL_STYLE_BOLD) ? ";1" : "",
                           (style & (TERMINAL_STYLE_DIM | TERMINAL_STYLE_MUTED)) ? ";2" : "",
                           (style & TERMINAL_STYLE_ITALIC) ? ";3" : "",
                           (style & TERMINAL_STYLE_UNDERLINE) ? ";4" : "",
                           (style & TERMINAL_STYLE_REVERSE) ? ";7" : "");
    if (written <= 0)
        return 0u;
    bytes at = (bytes) written < capacity ? (bytes) written : capacity;
    at = penColour(params, capacity, at, true, fg);
    at = penColour(params, capacity, at, false, bg);
    return at;
}

// A pen is emitted only when it differs from the one already on the device: a run
// shares one pen most of the time, and a row of source text usually costs one SGR.
static fn presenterPen(u16 style, u32 fg, u32 bg) {
    char params[TUI_PEN_CAPACITY];
    bytes length = penParams(params, sizeof params, style, fg, bg);
    if (gPenValid && length == gPenLength && memcmp(gPen, params, length) == 0)
        return;
    memcpy(gPen, params, length);
    gPenLength = length;
    gPenValid = true;
    presenterText("\033[");
    presenterAppend(params, length);
    presenterByte('m');
}

// Absolute cursor addressing, and only when the cursor is not already there. A
// repaint moves once per row; a one-cell hover costs one move inside the row.
static fn presenterMove(bytes column, bytes row) {
    if (gCursorColumn == column && gCursorRow == row)
        return;
    char sequence[32];
    int written = snprintf(sequence, sizeof sequence, "\033[%zu;%zuH", row + 1u, column + 1u);
    if (written > 0)
        presenterAppend(sequence, (bytes) written);
    gCursorColumn = column;
    gCursorRow = row;
}

static bytes utf8Encode(u32 glyph, char *out) {
    if (glyph < 0x80u) {
        out[0] = (char) glyph;
        return 1u;
    }
    if (glyph < 0x800u) {
        out[0] = (char) (0xC0u | (glyph >> 6));
        out[1] = (char) (0x80u | (glyph & 0x3Fu));
        return 2u;
    }
    if (glyph < 0x10000u) {
        out[0] = (char) (0xE0u | (glyph >> 12));
        out[1] = (char) (0x80u | ((glyph >> 6) & 0x3Fu));
        out[2] = (char) (0x80u | (glyph & 0x3Fu));
        return 3u;
    }
    out[0] = (char) (0xF0u | (glyph >> 18));
    out[1] = (char) (0x80u | ((glyph >> 12) & 0x3Fu));
    out[2] = (char) (0x80u | ((glyph >> 6) & 0x3Fu));
    out[3] = (char) (0x80u | (glyph & 0x3Fu));
    return 4u;
}

// The frame sink. A run is same-row cells; each cell is addressed absolutely, so a
// wide glyph can never be split by a diff boundary and no cell depends on where the
// previous one left the cursor.
static bool blitRun(any context, TerminalRun run) {
    (fn) context;
    for (bytes i = 0; i < run.length; ++i) {
        u32 glyph = run.cells != nullptr ? run.cells[i] : ' ';
        if (glyph == 0u) {
            // The second cell of a wide glyph. Its first cell advanced the device two
            // columns, so the track is right only when it already points past this
            // cell; otherwise the first half was painted by an earlier run (or not at
            // all) and where the device sits cannot be read from a continuation.
            if (gCursorRow != run.y || gCursorColumn != run.x + i + 1u)
                gCursorColumn = (bytes) -1;
            continue;
        }
        if (glyph > 0x10FFFFu || (glyph >= 0xD800u && glyph <= 0xDFFFu))
            glyph = '?';
        u16 style = run.styles != nullptr ? run.styles[i] : 0u;
        u32 fg = run.fg != nullptr ? run.fg[i] : TERMINAL_COLOR_DEFAULT;
        u32 bg = run.bg != nullptr ? run.bg[i] : TERMINAL_COLOR_DEFAULT;
        // Resolve the cell against the device before mapping it, because SGR sets ONE
        // colour and a fade has to be composed by US. The background plane goes over
        // the device background; the ink then goes over THAT.
        //
        // An unset background is left unset and the device paints its own, which keeps
        // a wrong backdrop guess a subtle tint instead of repainting the whole screen.
        // An unset foreground is left unset too: it means "the device's default ink",
        // and compositing it would paint the glyph in the backdrop colour and make the
        // text silently vanish - the difference between faded ink and invisible ink.
        u32 cellBg = TERMINAL_COLOR_IS_SET(bg)
                              ? TerminalColor_over(bg, gDeviceBg)
                              : TERMINAL_COLOR_DEFAULT;
        u32 backdrop = TERMINAL_COLOR_IS_SET(cellBg) ? cellBg : gDeviceBg;
        u32 cellFg = TERMINAL_COLOR_IS_SET(fg)
                              ? TerminalColor_over(fg, backdrop)
                              : TERMINAL_COLOR_DEFAULT;
        bytes column = run.x + i;
        bytes width = TerminalCanvas_widthOf(glyph);
        if (width == 0u)
            width = 1u;
        if (gPresentWidth != 0u && column + width > gPresentWidth) {
            // A wide glyph with no room for its second cell is clipped to a blank
            // rather than allowed to wrap the last column onto the next row.
            glyph = ' ';
            width = 1u;
        }
        presenterMove(column, run.y);
        presenterPen(style, cellFg, cellBg);
        char encoded[4];
        bytes length = utf8Encode(glyph, encoded);
        presenterAppend(encoded, length);
        gCursorColumn = column + width;
        gCursorRow = run.y;
    }
    return !gOutFailed;
}

// Forget the device cursor and pen. Called whenever something outside a run moved
// them - a clear, a resize, the first paint - never in the middle of one.
static fn presenterForget() {
    gCursorColumn = (bytes) -1;
    gCursorRow = (bytes) -1;
    gPenLength = 0u;
    gPenValid = false;
}

// Declare that the device holds nothing this grid describes: a value no canvas can
// produce makes every cell differ, so the next present repaints the whole screen.
// Used on the first bind, and again after a resize blanked the device behind the
// surface's back.
static fn presenterInvalidateFront() {
    for (bytes i = 0; i < TUI_MAX_W * TUI_MAX_H; ++i) {
        gFrontCells[i] = 0xFFFFFFFFu;
        gFrontStyles[i] = 0xFFFFu;
        gFrontFg[i] = 0xFFFFFFFFu;
        gFrontBg[i] = 0xFFFFFFFFu;
    }
}

// The cold device transaction: take the alternate screen, stop the device from
// wrapping the last column onto the next row, hide the cursor and start from a
// blank grid. The host returns the screen, autowrap and the cursor on exit.
static fn presenterEnter() {
    gDeviceColour = has_colors();
    gOutLength = 0u;
    gOutFailed = false;
    presenterForget();
    // `CSI r` releases any scroll region the device was left with (a previous
    // program, or a terminal that never restored one): the host addresses every cell
    // absolutely, so a margin can only turn a future scroll into a wrong one.
    presenterText("\033[?1049h\033[?7l\033[?25l\033[H\033[2J\033[r");
    presenterFlush();
}

static fn presenterLeave() {
    // The pen is reset before the screen is handed back, so a host without an
    // alternate screen does not leave the user's shell in our last colour.
    presenterText("\033[0m\033[?7h\033[?25h\033[?1049l");
    presenterFlush();
    presenterForget();
    (fn) fflush(stdout);
}

// A blank screen with one line of plain text, outside any frame. Only the
// too-small refusal uses it: there is no scene to compose at one cell wide.
static fn presenterMessage(const char *text) {
    presenterForget();
    presenterText("\033[H\033[2J");
    presenterPen(TERMINAL_STYLE_NONE, TERMINAL_COLOR_DEFAULT, TERMINAL_COLOR_DEFAULT);
    presenterText(text);
    presenterFlush();
}

// Row 0 is the chrome (title, undo/redo and the tab strip together); the warning header,
// when present, owns the next row, and the last row is the status bar. Everything
// between is editor, so the first editor row is 1 normally and 2 under a warning.
static bytes editorFirstRow() { return gEditorFirstRow; }

static bytes editorRowCount(int height) {
    if (height < (int) (editorFirstRow() + 2u))
        return 0;
    return (bytes) height - editorFirstRow() - 1u;
}

// The rail lives between the chrome and the status bar and never moves for a warning:
// folders stay where the user left them while the code/text shifts down.
static bytes railVisibleRows(int height) {
    if (height < 3)
        return 0;
    return (bytes) height - 2u;
}

// The scene owns at most TUI_MAX_ROWS row texts, so a very tall terminal shows
// the rows it has rather than reaching past them.
// The code area's columns, derived from the same layout the anchors produce: the
// centre starts at the rail seam, the number field follows the file's largest line,
// and the period and two-cell code gap are reserved before code begins.
static bytes codeLeftColumn(bytes railWidth, bytes numberWidth) {
    return railWidth + numberWidth + 2u + 1u + TUI_CODE_PAD;
}

static bytes codeColumns(bytes railWidth, bytes numberWidth, int width) {
    if (width <= 0 || (bytes) width <= codeLeftColumn(railWidth, numberWidth))
        return 0;
    return (bytes) width - codeLeftColumn(railWidth, numberWidth);
}

// Preserve the existing glyph projection, including malformed-byte replacement,
// wide-cell clipping and exact source caret geometry. Only ink/weight changes.
static fn paintEditorSyntax(TerminalCanvas *canvas, const FileText *file,
                               const Scene *scene, bytes top, bytes left) {
    if (!(*file).syntaxCurrent || (*scene).preview)
        return;
    bytes first = editorFirstRow();
    bytes codeLeft = codeLeftColumn((*scene).railWidth, (*scene).numberWidth);
    bytes width = TerminalCanvas_getWidth(canvas);
    bytes height = TerminalCanvas_getHeight(canvas);
    if (codeLeft >= width || first >= height)
        return;
    bytes visible = editorVisibleRows((int) height);
    if (visible > (*scene).rowCount)
        visible = (*scene).rowCount;
    for (bytes row = 0u; row < visible && top + row < (*file).lineCount; ++row) {
        bytes line = top + row;
        const char *text = (*file).lines[line];
        bytes length = (*file).lineLengths[line];
        bytes at = bytesForCells(text, length, left);
        bytes x = codeLeft;
        while (at < length && x < width) {
            TerminalCanvasDecodedScalar scalar = TerminalCanvas_utf8Decode(text + at, length - at);
            bytes cells = scalar.bytes == 0u ? 1u : TerminalCanvas_widthOf(scalar.codepoint);
            TerminalPen pen = SemicolonSyntax_penAt((*file).syntax, line, at);
            for (bytes cell = 0u; cell < cells && x + cell < width; ++cell) {
                bytes index = ((*canvas).y + first + row) * (*canvas).gridWidth
                             + (*canvas).x + x + cell;
                if ((*canvas).fg != nullptr)
                    (*canvas).fg[index] = pen.fg;
                if ((*canvas).styles != nullptr)
                    (*canvas).styles[index] = pen.style;
            }
            x += cells;
            at += scalar.bytes == 0u ? 1u : scalar.bytes;
        }
    }
    // The existing action layer intentionally covers source on the first rows.
    // Restore it through its real layout rather than guessing chip hit rectangles.
    TerminalCanvas code = TerminalCanvas_sub(canvas, codeLeft, first, width - codeLeft,
                                            height - first - 1u);
    TerminalPanel_paint((*scene).codeActions, &code);
}

static fn keepCursorVisible(const FileText *file, TerminalScrollGlide *glide,
                              bytes visible, bytes columns, const Scene *scene) {
    bytes codeWidth = codeColumns((*scene).railWidth, (*scene).numberWidth, (int) columns);
    if (visible == 0 || codeWidth == 0 || (*file).lineCount == 0)
        return;
    bytes maxTop = maxScrollTop(file, visible);
    bytes maxLeft = maxScrollLeft(file, codeWidth);
    bytes rowTarget = (bytes) TerminalScrollGlide_getRowTarget(glide);
    if ((*file).cursorLine < rowTarget)
        rowTarget = (*file).cursorLine;
    else if ((*file).cursorLine >= rowTarget + visible)
        rowTarget = (*file).cursorLine - visible + 1u;
    bytes cursorCells = cellsOf((*file).lines[(*file).cursorLine], (*file).cursorByte);
    bytes columnTarget = (bytes) TerminalScrollGlide_getColumnTarget(glide);
    if (cursorCells < columnTarget)
        columnTarget = cursorCells;
    else if (cursorCells >= columnTarget + codeWidth)
        columnTarget = cursorCells - codeWidth + 1u;
    TerminalScrollGlide_setTarget(glide, rowTarget, columnTarget, maxTop, maxLeft);
}

static fn paintEditorCursor(TerminalCanvas *canvas, const FileText *file,
                              bytes top, bytes left, bytes railWidth,
                              bytes numberWidth) {
    if (!(*file).cursorVisible || (*file).cursorLine < top
        || (*file).cursorLine - top >= editorVisibleRows((int) (*canvas).gridHeight))
        return;
    bytes cells = cellsOf((*file).lines[(*file).cursorLine], (*file).cursorByte);
    if (cells < left)
        return;
    bytes x = codeLeftColumn(railWidth, numberWidth) + cells - left;
    bytes y = editorFirstRow() + (*file).cursorLine - top;
    if (x >= (*canvas).gridWidth || y >= (*canvas).gridHeight)
        return;
    bytes index = y * (*canvas).gridWidth + x;
    if ((*canvas).styles != nullptr)
        (*canvas).styles[index] |= TERMINAL_STYLE_REVERSE;
}

// Every mutating key goes through one transaction: snapshot, change, then either
// commit the pre-edit state or throw it away when the operation refused. A failed
// snapshot refuses the edit before any source byte moves.
static bool editorChange(FileText *file, bool (*operation)(FileText*)) {
    if (operation == nullptr || !fileHistoryBegin(file))
        return false;
    if (!operation(file)) {
        fileHistoryCancel(file);
        return false;
    }
    fileHistoryCommit(file);
    return true;
}

static bool editorInsertText(FileText *file, const char *data, bytes count) {
    if (!fileHistoryBegin(file))
        return false;
    if (!fileInsertBytes(file, (*file).cursorLine, (*file).cursorByte, data, count)) {
        fileHistoryCancel(file);
        return false;
    }
    fileHistoryCommit(file);
    return true;
}

static bool utf8Continuation(int key) {
    return key >= 0x80 && key <= 0xBF;
}

static bytes utf8SequenceLength(int lead) {
    if (lead >= 0xC2 && lead <= 0xDF)
        return 2u;
    if (lead >= 0xE0 && lead <= 0xEF)
        return 3u;
    if (lead >= 0xF0 && lead <= 0xF4)
        return 4u;
    return 1u;
}

// ncurses in this build hands over one BYTE at a time, not one scalar. A multibyte
// UTF-8 character must therefore become one history transaction, or undo would leave
// a truncated sequence behind. Gather the continuation bytes that are already queued;
// an incomplete sequence is still inserted once rather than silently discarded.
static bool editorInsertUtf8(FileText *file, int key) {
    unsigned char raw[4];
    raw[0] = (unsigned char) key;
    bytes length = utf8SequenceLength(key);
    bytes got = 1u;
    if (length > 1u) {
        timeout(1);
        while (got < length) {
            int next = getch();
            if (!utf8Continuation(next)) {
                if (next != ERR)
                    (fn) ungetch(next);
                break;
            }
            raw[got++] = (unsigned char) next;
        }
        timeout(0);
    }
    (*file).cursorVisible = true;
    return editorInsertText(file, (const char*) raw, got);
}

static bool editorKey(FileText *file, int key) {
    // Ctrl-Z / Ctrl-Y. The host runs raw() so the tty does not turn Ctrl-Z into
    // SIGTSTP and discard the byte before ncurses can see it.
    if (key == 26)
        return fileUndo(file);
    if (key == 25)
        return fileRedo(file);
    if (key == KEY_LEFT) {
        fileMoveLeft(file);
        (*file).cursorVisible = true;
        return true;
    }
    if (key == KEY_RIGHT) {
        fileMoveRight(file);
        (*file).cursorVisible = true;
        return true;
    }
    if (key == KEY_UP) {
        fileMoveVertical(file, -1);
        (*file).cursorVisible = true;
        return true;
    }
    if (key == KEY_DOWN) {
        fileMoveVertical(file, 1);
        (*file).cursorVisible = true;
        return true;
    }
    bytes line = (*file).cursorLine;
    if (line >= (*file).lineCount)
        return false;
    if (key == KEY_HOME) {
        (*file).cursorByte = 0;
        (*file).cursorVisible = true;
        return true;
    }
    if (key == KEY_END) {
        (*file).cursorByte = (*file).lineLengths[line];
        (*file).cursorVisible = true;
        return true;
    }
    if (key == KEY_BACKSPACE || key == 127 || key == 8) {
        (*file).cursorVisible = true;
        return editorChange(file, fileBackspace);
    }
    if (key == KEY_DC) {
        (*file).cursorVisible = true;
        return editorChange(file, fileDeleteForward);
    }
    if (key == '\n' || key == '\r') {
        (*file).cursorVisible = true;
        return editorChange(file, fileSplit);
    }
    if (key == '\t') {
        static const char spaces[TUI_INDENT_SPACES] = { ' ', ' ', ' ', ' ' };
        (*file).cursorVisible = true;
        return editorInsertText(file, spaces, sizeof spaces);
    }
    if (key >= 128 && key <= 255) {
        (*file).cursorVisible = true;
        return editorInsertUtf8(file, key);
    }
    if (key >= 32 && key != 127) {
        char byte = (char) key;
        (*file).cursorVisible = true;
        return editorInsertText(file, &byte, 1u);
    }
    return false;
}

static bytes editorVisibleRows(int height) {
    bytes rows = editorRowCount(height);
    return rows > TUI_MAX_ROWS ? TUI_MAX_ROWS : rows;
}

// One click, one decision: a folder opens or closes, a file opens in the editor. The row
// is resolved by the tree from the viewport offset, so the rail's scroll is already
// accounted for and a click past the last row is not a click on anything.
static fn railClick(bytes y) {
    // The root occupies the chrome's rail slice; body offsets skip its tree row.
    if (gRail == nullptr)
        return;
    TerminalTreeNodeId id = y == 0u ? gRailRootNode
                                  : TerminalTreeView_nodeAtOffset(gRail, y);
    if (id == TERMINAL_TREE_NO_NODE)
        return;
    if (TerminalTreeView_isFolder(gRail, id)) {
        if (TerminalTreeView_isExpanded(gRail, id)) {
            (fn) TerminalTreeView_setExpanded(gRail, id, false);
            return;
        }
        // Read BEFORE opening, so the rows exist by the time they become visible.
        (fn) railReadChildren(id);
        (fn) TerminalTreeView_setExpanded(gRail, id, true);
        return;
    }
    RailEntry *entry = railEntryFor(id);
    if (entry != nullptr)
        (fn) openFile((*entry).path, id);
}

static fn railScrollBy(long rows) {
    if (gRail != nullptr)
        (fn) TerminalTreeView_scrollBy(gRail, rows);
}

#define TUI_MOUSE_SGR (KEY_MAX + 1)
#define TUI_MOUSE_X10 (KEY_MAX + 2)
// A mouse report is one contiguous escape sequence, but ncurses hands us the
// introducer as soon as it matches the prefix. The suffix can therefore still be
// in flight when the next read happens; under timeout(0) it would be left in the
// queue and typed into the editor as ordinary digits. Wait briefly for each suffix
// byte, and if the terminal stalls mid-report, keep feeding the remaining bytes to
// the decoder for this long instead of treating them as text.
#define TUI_MOUSE_SUFFIX_MS 25
#define TUI_MOUSE_CAPTURE_NS 5000000000ULL

// One normalized key-scroll step, as a value (the Value Return Law: no destination
// parameters).
//
// SLOT RECORD: ScrollStep
typedef struct ScrollStep {
    bool keyed;    // the key was a scroll key at all
    int rows;      // vertical cells
    int columns;   // horizontal cells
} ScrollStep;

// Arrow keys are the point: the horizontal wheel is a pair most terminals never
// send, Shift+wheel is routinely eaten by the terminal's own scrollback, and a
// one-column drag edge is unhittable. The arrows exist on every keyboard.
static ScrollStep scrollStepFor(int key) {
    switch (key) {
        case KEY_UP: return (ScrollStep) { true, -1, 0 };
        case KEY_DOWN: return (ScrollStep) { true, 1, 0 };
        case KEY_LEFT: return (ScrollStep) { true, 0, -(int) TUI_SCROLL_COLS };
        case KEY_RIGHT: return (ScrollStep) { true, 0, (int) TUI_SCROLL_COLS };
        case KEY_SLEFT: return (ScrollStep) { true, 0, -(int) TUI_SCROLL_COLS };
        case KEY_SRIGHT: return (ScrollStep) { true, 0, (int) TUI_SCROLL_COLS };
        case KEY_PPAGE: return (ScrollStep) { true, -((int) TUI_SCROLL_ROWS * 4), 0 };
        case KEY_NPAGE: return (ScrollStep) { true, (int) TUI_SCROLL_ROWS * 4, 0 };
        case KEY_HOME: return (ScrollStep) { true, -1000000, 0 };
        case KEY_END: return (ScrollStep) { true, 1000000, 0 };
        default: return (ScrollStep) { false, 0, 0 };
    }
}

// ncurses matches the mouse introducer itself (it is registered with define_key),
// so the bytes it consumed are re-injected before the suffix is read. This keeps
// ncurses as the KEY decoder and Determinant as the MOUSE decoder, which is the
// layering the toolkit's lawbook asks for.
//
// The suffix must not be read with timeout(0): every byte would race the input
// queue, and the bytes that lose the race would be typed into the editor. Wait a
// slice for them here; the caller keeps a capture window for the stragglers.
static bool readMouseSuffix(TerminalInput *input, bool sgr, bool blocking) {
    static const char *sgrPrefix = "\x1b[<";
    static const char *x10Prefix = "\x1b[M";
    const char *prefix = sgr ? sgrPrefix : x10Prefix;
    for (bytes i = 0; prefix[i] != '\0'; ++i)
        (fn) TerminalInput_feed(input, (unsigned char) prefix[i]);
    if (!blocking)
        timeout(TUI_MOUSE_SUFFIX_MS);
    bool ready = false;
    for (bytes i = 0; i < 24; ++i) {
        int byte = getch();
        if (byte == ERR)
            break;
        if (TerminalInput_feed(input, byte)) {
            ready = true;
            break;
        }
        if (!sgr && i == 2)
            break;   // X10 is exactly three bytes; anything else is malformed
    }
    if (!blocking)
        timeout(0);
    return ready;
}

static fn beginMouseSequence(TerminalInput *input, bool sgr) {
    static const char *sgrPrefix = "\x1b[<";
    static const char *x10Prefix = "\x1b[M";
    const char *prefix = sgr ? sgrPrefix : x10Prefix;
    for (bytes i = 0; prefix[i] != '\0'; ++i)
        (fn) TerminalInput_feed(input, (unsigned char) prefix[i]);
}

static TerminalPointerEvent ncursesMouseEvent() {
    MEVENT mouse;
    if (getmouse(&mouse) != OK || mouse.x < 0 || mouse.y < 0)
        return (TerminalPointerEvent) {0};
    TerminalPointerEvent event = {
        .kind = TERMINAL_POINTER_NONE,
        .button = TERMINAL_BUTTON_NONE,
        .x = (bytes) mouse.x,
        .y = (bytes) mouse.y,
        .shift = (mouse.bstate & BUTTON_SHIFT) != 0,
        .meta = (mouse.bstate & BUTTON_ALT) != 0,
        .ctrl = (mouse.bstate & BUTTON_CTRL) != 0
    };
    if ((mouse.bstate & BUTTON4_PRESSED) != 0) {
        event.kind = TERMINAL_POINTER_WHEEL_UP;
#if defined(BUTTON5_PRESSED)
    } else if ((mouse.bstate & BUTTON5_PRESSED) != 0) {
        event.kind = TERMINAL_POINTER_WHEEL_DOWN;
#endif
#if defined(BUTTON6_PRESSED)
    } else if ((mouse.bstate & BUTTON6_PRESSED) != 0) {
        event.kind = TERMINAL_POINTER_WHEEL_LEFT;
#endif
#if defined(BUTTON7_PRESSED)
    } else if ((mouse.bstate & BUTTON7_PRESSED) != 0) {
        event.kind = TERMINAL_POINTER_WHEEL_RIGHT;
#endif
    } else if ((mouse.bstate & (BUTTON1_RELEASED | BUTTON2_RELEASED | BUTTON3_RELEASED)) != 0) {
        event.kind = TERMINAL_POINTER_RELEASE;
        if ((mouse.bstate & BUTTON1_RELEASED) != 0)
            event.button = TERMINAL_BUTTON_LEFT;
        else if ((mouse.bstate & BUTTON2_RELEASED) != 0)
            event.button = TERMINAL_BUTTON_MIDDLE;
        else
            event.button = TERMINAL_BUTTON_RIGHT;
    } else if ((mouse.bstate & (BUTTON1_PRESSED | BUTTON1_CLICKED
                                | BUTTON1_DOUBLE_CLICKED | BUTTON1_TRIPLE_CLICKED)) != 0) {
        event.kind = TERMINAL_POINTER_PRESS;
        event.button = TERMINAL_BUTTON_LEFT;
    } else if ((mouse.bstate & (BUTTON2_PRESSED | BUTTON2_CLICKED
                                | BUTTON2_DOUBLE_CLICKED | BUTTON2_TRIPLE_CLICKED)) != 0) {
        event.kind = TERMINAL_POINTER_PRESS;
        event.button = TERMINAL_BUTTON_MIDDLE;
    } else if ((mouse.bstate & (BUTTON3_PRESSED | BUTTON3_CLICKED
                                | BUTTON3_DOUBLE_CLICKED | BUTTON3_TRIPLE_CLICKED)) != 0) {
        event.kind = TERMINAL_POINTER_PRESS;
        event.button = TERMINAL_BUTTON_RIGHT;
    } else if ((mouse.bstate & REPORT_MOUSE_POSITION) != 0) {
        event.kind = TERMINAL_POINTER_MOVE;
    }
    return event;
}

static bool isMouseContinuation(const TerminalInput *input, int key) {
    if (input == nullptr || key < 0 || key > 255)
        return false;
    TerminalInputState state = TerminalInput_getState(input);
    unsigned char byte = (unsigned char) key;
    if (state == TERMINAL_INPUT_SGR_BUTTON || state == TERMINAL_INPUT_SGR_X
        || state == TERMINAL_INPUT_SGR_Y)
        return (byte >= '0' && byte <= '9') || byte == ';' || byte == 'M' || byte == 'm';
    if (state == TERMINAL_INPUT_X10)
        return byte >= 32u;
    return false;
}

// The frame transaction, mapped onto the terminal.
//
// DEC private mode 2026 ("synchronized output") asks the device to hold an update and
// apply it as ONE step, which is the terminal's counterpart of a compositor
// transaction: a divider drag or a resize repaints a great many cells, and without this
// the device is free to show the middle of that work. Terminals that do not know the
// mode ignore it, so this needs no probe and no capability gate.
//
// The whole frame is ONE write: the opening bracket, every changed run, and the
// closing bracket sit in the same buffer, so the close can never arrive ahead of the
// content it is supposed to bracket. The buffer is flushed even when a sink refused
// part-way, because a device left inside a half-finished transaction is a device
// left frozen.
static fn frameBracket(any frameContext, TerminalPresentPhase phase) {
    (fn) frameContext;
    if (phase == TERMINAL_PRESENT_BEGIN) {
        // Anything ncurses itself still has buffered (the keypad-mode string is the
        // one example) belongs to the device before our cells do.
        (fn) fflush(stdout);
        presenterText("\033[?2026h");
        return;
    }
    presenterText("\033[?2026l");
    presenterFlush();
}

static bool mouseReporting(bool enabled) {
    return fputs(enabled ? "\033[?1003h\033[?1006h" : "\033[?1003l\033[?1006l", stdout) >= 0
        && fflush(stdout) == 0;
}

// Vertical bound: the last line may sit anywhere down to the last row, and
// TUI_SCROLL_PAST_BOTTOM lines of empty space follow it, so there is always
// somewhere below the end to work.
static bytes maxScrollTop(const FileText *file, bytes visible) {
    bytes total = (*file).lineCount + TUI_SCROLL_PAST_BOTTOM;
    return total > visible ? total - visible : 0;
}

// Horizontal bound: the longest line may sit anywhere left of the last column, and
// TUI_SCROLL_PAST_RIGHT columns of empty space follow it.
static bytes maxScrollLeft(const FileText *file, bytes codeWidth) {
    bytes total = (*file).longestCells + TUI_SCROLL_PAST_RIGHT;
    return total > codeWidth ? total - codeWidth : 0;
}

static bytes clampScroll(bytes value, bytes maximum) {
    return value > maximum ? maximum : value;
}

// The tracking line: the bottom editor row is one long horizontal slider. A drag on
// it maps the pointer's position straight onto the offset, so dragging to either end
// reaches that end of the document rather than nudging by a step. Clicking there does
// nothing at all - press and drag are separable gestures, and only the drag pans.
static bytes applyScroll(bytes value, int delta, bytes maximum) {
    if (delta < 0) {
        bytes amount = (bytes) (-delta);
        return amount > value ? 0 : value - amount;
    }
    return clampScroll(value + (bytes) delta, maximum);
}

;;TEST
static int dumpFrame(int width, int height, FileText *file) {
    if (width < 1 || height < 1 || width > TUI_MAX_W || height > TUI_MAX_H) {
        fprintf(stderr, "[vex] semicolon: dump size %dx%d out of range (max %dx%d)\n",
                width, height, TUI_MAX_W, TUI_MAX_H);
        return 1;
    }
    fileSyntaxRefresh(file);
    Scene scene;
    gTerminalWidth = width;
    sceneBuild(&scene, file, TUI_MAX_ROWS, width, height);
    // The dump renders the same scene, so it needs the same project: a rail that is empty
    // here would be a different frame from the one the shell shows.
    railBuild(gRailRoot);
    (fn) railRevealPath(gOpenPath);
    setSaveStatus(&scene, file);
    updateClock(&scene);
    sceneSyncRail(&scene, railVisibleRows(height));
    if (scene.root == nullptr) {
        fprintf(stderr, "[vex] semicolon: scene construction failed\n");
        return 1;
    }
    sceneSyncRows(&scene, file, 0, 0, editorVisibleRows(height));
    TerminalCanvas canvas = TerminalCanvas(gCells, gStyles, gFg, gBg, (bytes) width, (bytes) height);
    TerminalCanvas_fill(&canvas, ' ', TerminalPen());
    TerminalPanel_paint(scene.root, &canvas);
    paintEditorSyntax(&canvas, file, &scene, 0u, 0u);
    sceneRenderMarkdown(&scene, &canvas, scene.railWidth);
    paintRailColors(&scene, &canvas, false, 0, 0);
    for (int y = 0; y < height; ++y) {
        TerminalCanvasTextRow row = TerminalCanvas_copyRow(&canvas, (bytes) y);
        fwrite(row.data, 1, row.bytes, stdout);
        fputc('\n', stdout);
    }
    sceneFree(&scene);
    return 0;
}

// A diagnostic that prints what the terminal ACTUALLY sends. Whether a trackpad
// emits a horizontal wheel (buttons 66/67) is a property of the terminal emulator,
// not of this program, so arguing about it is pointless: swipe and read the answer.
;;VALIDATOR
static int runInputProbe() {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        THROW("semicolon --input-probe requires a terminal");
        return 1;
    }
    if (setlocale(LC_CTYPE, "") == nullptr
        && setlocale(LC_CTYPE, "UTF-8") == nullptr
        && setlocale(LC_CTYPE, "en_US.UTF-8") == nullptr
        && setlocale(LC_CTYPE, "C.UTF-8") == nullptr) {
        THROW("semicolon: cannot select the environment character locale");
        return 1;
    }
    SCREEN *screen = newterm(nullptr, stdout, stdin);
    if (screen == nullptr)
        return 1;
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
    (fn) cbreak();
    (fn) noecho();
    (fn) curs_set(0);
    (fn) keypad(stdscr, true);
    set_escdelay(25);
    (fn) mousemask(0, nullptr);
    (fn) define_key("\033[<", TUI_MOUSE_SGR);
    (fn) define_key("\033[M", TUI_MOUSE_X10);
    (fn) define_key("\033[A", KEY_UP);
    (fn) define_key("\033[B", KEY_DOWN);
    (fn) define_key("\033[C", KEY_RIGHT);
    (fn) define_key("\033[D", KEY_LEFT);
    (fn) mouseReporting(true);
    erase();
    mvaddstr(0, 0, "input probe: wheel, swipe sideways, drag, press keys. Ctrl-C quits.");
    mvaddstr(1, 0, "a horizontal swipe shows wheel-left / wheel-right if the terminal sends it.");
    refresh();
    TerminalInput input = TerminalInput_0();
    int line = 3;
    while (!gQuit) {
        int key = getch();
        if (key == ERR)
            continue;
        bool havePointer = false;
        if (key == TUI_MOUSE_SGR)
            havePointer = readMouseSuffix(&input, true, true);
        else if (key == TUI_MOUSE_X10)
            havePointer = readMouseSuffix(&input, false, true);
        if (line >= LINES - 1)
            line = 3;
        if (havePointer) {
            TerminalPointerEvent event = TerminalInput_getEvent(&input);
            const char *kind = "?";
            switch (event.kind) {
                case TERMINAL_POINTER_MOVE: kind = "move"; break;
                case TERMINAL_POINTER_DRAG: kind = "drag"; break;
                case TERMINAL_POINTER_PRESS: kind = "press"; break;
                case TERMINAL_POINTER_RELEASE: kind = "release"; break;
                case TERMINAL_POINTER_WHEEL_UP: kind = "wheel-up"; break;
                case TERMINAL_POINTER_WHEEL_DOWN: kind = "wheel-down"; break;
                case TERMINAL_POINTER_WHEEL_LEFT: kind = "wheel-left"; break;
                case TERMINAL_POINTER_WHEEL_RIGHT: kind = "wheel-right"; break;
                default: break;
            }
            mvprintw(line++, 0, "%-12s x=%-4zu y=%-4zu button=%d %s%s%s        ",
                     kind, event.x, event.y, (int) event.button,
                     event.shift ? "shift " : "", event.meta ? "meta " : "",
                     event.ctrl ? "ctrl" : "");
        } else {
            mvprintw(line++, 0, "key %-5d (no mouse event)        ", key);
        }
        refresh();
    }
    (fn) mouseReporting(false);
    runStop();
    (fn) curs_set(1);
    (fn) endwin();
    (fn) delscreen(screen);
    return 0;
}

// A diagnostic that answers one question with the terminal's own words: what colour is
// your background? Every fade in the shell is composited against that value, so when a
// fade looks wrong, this is the number to read first and SEMICOLON_BG is the fix.
;;VALIDATOR
static int runBackgroundProbe() {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        THROW("semicolon --bg-probe requires a terminal");
        return 1;
    }
    SCREEN *screen = newterm(nullptr, stdout, stdin);
    if (screen == nullptr) {
        THROW("semicolon: terminal initialization failed");
        return 1;
    }
    (fn) cbreak();
    (fn) noecho();
    (fn) notimeout(stdscr, false);
    set_escdelay(25);
    resolveDeviceBackground();
    endwin();
    delscreen(screen);
    printf("device background: #%02X%02X%02X\n",
           (unsigned) TERMINAL_COLOR_RED(gDeviceBg), (unsigned) TERMINAL_COLOR_GREEN(gDeviceBg),
           (unsigned) TERMINAL_COLOR_BLUE(gDeviceBg));
    printf("source: %s\n", gDeviceBgSource);
    printf("set SEMICOLON_BG=#RRGGBB to override it.\n");
    fflush(stdout);
    return 0;
}

static int runTerminal(FileText *file) {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        THROW("semicolon requires terminal stdin and stdout (use --dump for a plain render)");
        return 1;
    }
    if (setlocale(LC_CTYPE, "") == nullptr
        && setlocale(LC_CTYPE, "UTF-8") == nullptr
        && setlocale(LC_CTYPE, "en_US.UTF-8") == nullptr
        && setlocale(LC_CTYPE, "C.UTF-8") == nullptr) {
        THROW("semicolon: cannot select the environment character locale");
        return 1;
    }
    SCREEN *screen = newterm(nullptr, stdout, stdin);
    if (screen == nullptr) {
        THROW("semicolon: terminal initialization failed");
        return 1;
    }
    if (has_colors()) {
        start_color();
        use_default_colors();
    }
    // Raw mode, not cbreak: Ctrl-Z has to arrive as byte 26 so the editor can undo
    // instead of the line discipline suspending the whole shell. Ctrl-C is therefore
    // handled explicitly as byte 3 in the event loop; SIGINT still covers a real
    // process signal sent by someone else.
    (fn) raw();
    (fn) noecho();
    (fn) nonl();
    (fn) curs_set(0);
    (fn) mousemask(0, nullptr);   // ncurses' own mouse ABI is not used here
    // keypad decodes the arrow and page keys; without it a keypress is just bytes
    // and there is no way to scroll from the keyboard at all.
    (fn) keypad(stdscr, true);
    (fn) notimeout(stdscr, false);
    set_escdelay(25);
    // Bind the two mouse introducers so ncurses hands them over as one code instead
    // of trying to match the whole sequence against terminfo.
    (fn) define_key("\033[<", TUI_MOUSE_SGR);
    (fn) define_key("\033[M", TUI_MOUSE_X10);
    // The cursor keys need binding too. A terminal in its normal mode sends CSI
    // (ESC [ C), but many terminfo entries only list the application-mode SS3 form
    // (ESC O C), so ncurses failed to match the arrows it actually receives and they
    // fell through as raw bytes. Both forms are bound to the same key codes.
    (fn) define_key("\033[A", KEY_UP);
    (fn) define_key("\033[B", KEY_DOWN);
    (fn) define_key("\033[C", KEY_RIGHT);
    (fn) define_key("\033[D", KEY_LEFT);
    (fn) define_key("\033OA", KEY_UP);
    (fn) define_key("\033OB", KEY_DOWN);
    (fn) define_key("\033OC", KEY_RIGHT);
    (fn) define_key("\033OD", KEY_LEFT);
    (fn) define_key("\033[H", KEY_HOME);
    (fn) define_key("\033[F", KEY_END);
    (fn) define_key("\033[1~", KEY_HOME);
    (fn) define_key("\033[4~", KEY_END);
    // Before the event loop owns the input stream: this reads the terminal's reply, so
    // it has to happen while nothing else is watching the same bytes.
    resolveDeviceBackground();
    timeout(TUI_POLL_MS);
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
    (fn) mouseReporting(true);
    // The device transaction is the host's from here on: the alternate screen,
    // autowrap off, the cursor hidden and a blank grid. ncurses keeps the keyboard
    // and the termios mode; it never sees a cell again.
    presenterEnter();
    // The size is read from the tty every pass, so ncurses has no reason to resize
    // its own (never painted) window on a signal - and resizing it would mark it
    // touched, which is the one situation where getch would paint a screen we own.
    (fn) signal(SIGWINCH, SIG_IGN);

    // The grid size is the tty's, read directly: ncurses caches a size it only
    // refreshes on its own resize path, and this host decides its geometry itself.
    struct winsize startSize;
    int rawRows = 24;
    int rawColumns = 80;
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &startSize) == 0 && startSize.ws_row > 0
        && startSize.ws_col > 0) {
        rawRows = (int) startSize.ws_row;
        rawColumns = (int) startSize.ws_col;
    }
    int height = rawRows;
    int width = rawColumns;
    if (width > TUI_MAX_W)
        width = TUI_MAX_W;
    if (height > TUI_MAX_H)
        height = TUI_MAX_H;
    Scene scene;
    gTerminalWidth = width;
    sceneBuild(&scene, file, TUI_MAX_ROWS, width, height);
    gScene = &scene;
    gOpenFile = file;
    if (gCachedMarkdownModeValid && isMarkdownFile((*file).name))
        (fn) sceneSetMarkdownMode(&scene, gCachedMarkdownPreview);
    setSaveStatus(&scene, file);
    updateClock(&scene);
    // Only a user-adjusted width overrides the new 40-cell default. Legacy session widths
    // were written even for defaults, so their old small value is intentionally ignored.
    if (gCachedRailCustom && gCachedRailValid && gCachedRail >= TUI_RAIL_MIN
        && gCachedRail + TUI_RAIL_MARGIN < (bytes) width)
        sceneSetRailWidth(&scene, gCachedRail);
    if (gCachedRailHidden)
        (fn) sceneSetRailVisible(&scene, false);
    // The project, one level at a time, then the open file revealed down it.
    railBuild(gRailRoot);
    TerminalTreeNodeId opening = railRevealPath(gOpenPath);
    if (opening != TERMINAL_TREE_NO_NODE)
        (fn) TerminalTreeView_setSelected(gRail, opening);
    bool ok = scene.root != nullptr;
    Interaction interaction = { .input = TerminalInput_0() };
    TerminalSurface surface = TerminalSurface_0();
    ok = ok && TerminalSurface_setFrame(&surface, frameBracket, nullptr);
    TerminalScrollGlide glide = TerminalScrollGlide();
    // The UNCLAMPED tty size, so a terminal wider than the grid does not look like a
    // resize on every single pass.
    int knownRows = rawRows;
    int knownColumns = rawColumns;
    if (!ok)
        THROW("semicolon: scene construction failed");

    while (ok) {
        runPoll();
        if (gQuit) {
            if ((*file).dirty && !fileSave(file, gOpenPath)) {
                (*file).saveFailed = true;
                gQuit = 0;
                setSaveStatus(&scene, file);
            } else {
                break;
            }
        }
        // The size is read from the tty directly. It is applied ONLY when it actually
        // changed, and a resize is the host's own transaction: blank the device, forget
        // where its cursor is, and let the geometry change below rebind the surface
        // against a mirror that differs everywhere, so the whole grid is repainted.
        struct winsize terminalSize;
        if (ioctl(STDIN_FILENO, TIOCGWINSZ, &terminalSize) == 0 && terminalSize.ws_row > 0
            && terminalSize.ws_col > 0
            && ((int) terminalSize.ws_row != knownRows || (int) terminalSize.ws_col != knownColumns)) {
            knownRows = (int) terminalSize.ws_row;
            knownColumns = (int) terminalSize.ws_col;
            gRepaintAll = true;
            presenterForget();
            presenterText("\033[H\033[2J\033[r");
            presenterFlush();
        }
        int rows = knownRows;
        int columns = knownColumns;
        if (columns > TUI_MAX_W)
            columns = TUI_MAX_W;
        if (rows > TUI_MAX_H)
            rows = TUI_MAX_H;
        width = columns;
        height = rows;
        gTerminalWidth = width;
        if ((*file).dirty && !(*file).saveFailed && autosave(file, gOpenPath))
            setSaveStatus(&scene, file);
        else if ((*file).saveFailed)
            setSaveStatus(&scene, file);
        if (!sceneUpdateNumberWidth(&scene, file)) {
            ok = false;
            break;
        }
        bytes visible = editorVisibleRows(height);
        bytes codeLeft = codeLeftColumn(scene.railWidth, scene.numberWidth);
        bytes codeRight = width > 0 ? (bytes) width - 2u : 0;
        bytes maxTop = maxScrollTop(file, visible);
        bytes maxLeft = maxScrollLeft(file, codeColumns(scene.railWidth, scene.numberWidth, width));
        TerminalScrollGlide_clamp(&glide, maxTop, maxLeft);
        if (visible == 0 || codeRight <= codeLeft) {
            presenterMessage("terminal too small");
            timeout(TUI_POLL_MS);
            (fn) getch();
            continue;
        }
        // All input arrives here: keys, and the mouse bytes the decoder turns into
        // normalized events.
        // One read may wait up to the poll timeout, so an idle shell sleeps
        // instead of spinning. Every later read in this frame is NON-BLOCKING:
        // with a timeout still set, the read that finds the buffer empty would
        // stall the paint by the whole timeout, which is exactly the lag a drag
        // exposes.
        // 60fps while a scroll is in flight, and a long sleep when nothing is moving.
        // Present-on-demand is preserved: an idle shell still emits nothing at all.
        int waitMs = TerminalScrollGlide_isActive(&glide) ? TUI_FRAME_MS : TUI_POLL_MS;
        if (gRunPid > 0 && waitMs > 50)
            waitMs = 50;
        if ((*file).dirty && !(*file).saveFailed) {
            u64 now = monotonicNowNs();
            u64 elapsed = now >= (*file).lastEditNs ? now - (*file).lastEditNs : 0;
            u64 remaining = elapsed < TUI_AUTOSAVE_DELAY_NS
                               ? TUI_AUTOSAVE_DELAY_NS - elapsed : 0;
            int deadlineMs = (int) ((remaining + 999999u) / 1000000u);
            if (deadlineMs < waitMs)
                waitMs = deadlineMs;
            if (waitMs < 1)
                waitMs = 1;
        }
        // A live copy notice or check glyph is the only other timed paint: wake exactly
        // when it must disappear instead of spinning or lingering until the next input.
        u64 feedbackUntil = scene.markdownNoticeUntilNs;
        if (scene.copiedBlock != TUI_MARKDOWN_NO_BLOCK && scene.copiedUntilNs > feedbackUntil)
            feedbackUntil = scene.copiedUntilNs;
        if (feedbackUntil != 0u) {
            u64 now = monotonicNowNs();
            u64 remaining = now < feedbackUntil ? feedbackUntil - now : 0u;
            int deadlineMs = (int) ((remaining + 999999u) / 1000000u);
            if (deadlineMs < waitMs)
                waitMs = deadlineMs;
            if (waitMs < 1)
                waitMs = 1;
        }
        timeout(waitMs);
        int key = getch();
        timeout(0);
        if (key == ERR && interaction.mouseCapture
            && monotonicNowNs() > interaction.mouseCaptureUntilNs) {
            interaction.mouseCapture = false;
            interaction.mouseCaptureDetect = false;
            TerminalInput_reset(&interaction.input);
        }
        bytes handled = 0;
        while (key != ERR && handled < TUI_EVENTS_PER_FRAME) {
            ++handled;
            // Ctrl-C is the only way out; raw mode delivers it as byte 3 rather than
            // SIGINT, and there is deliberately no letter shortcut because a keystroke
            // is input and 'q' is a legitimate thing to want to type.
            if (key == 3) {
                gQuit = 1;
                break;
            }

            bool havePointer = false;
            bool captured = false;
            TerminalPointerEvent directPointer = {0};
            if (interaction.mouseCapture) {
                u64 now = monotonicNowNs();
                bool continuation = false;
                if (interaction.mouseCaptureDetect && key >= 0 && key <= 255
                    && now <= interaction.mouseCaptureUntilNs) {
                    bool sgr = key >= '0' && key <= '9';
                    beginMouseSequence(&interaction.input, sgr);
                    interaction.mouseCaptureDetect = false;
                    continuation = true;
                } else if (!interaction.mouseCaptureDetect
                           && now <= interaction.mouseCaptureUntilNs) {
                    continuation = isMouseContinuation(&interaction.input, key);
                }
                if (continuation) {
                    captured = true;
                    if (TerminalInput_feed(&interaction.input, key)) {
                        interaction.mouseCapture = false;
                        havePointer = true;
                    } else if (!TerminalInput_isPending(&interaction.input)) {
                        interaction.mouseCapture = false;
                    }
                } else {
                    interaction.mouseCapture = false;
                    interaction.mouseCaptureDetect = false;
                    TerminalInput_reset(&interaction.input);
                }
            }
            if (!captured && key == KEY_RESIZE) {
                (fn) clear();
                sceneMarkdownClearFeedback(&scene);
            } else if (!captured && key == TUI_MOUSE_SGR) {
                havePointer = readMouseSuffix(&interaction.input, true, false);
                interaction.mouseCaptureDetect = false;
                if (!havePointer && TerminalInput_isPending(&interaction.input)) {
                    interaction.mouseCapture = true;
                    interaction.mouseCaptureUntilNs = monotonicNowNs() + TUI_MOUSE_CAPTURE_NS;
                }
            } else if (!captured && key == TUI_MOUSE_X10) {
                havePointer = readMouseSuffix(&interaction.input, false, false);
                interaction.mouseCaptureDetect = false;
                if (!havePointer && TerminalInput_isPending(&interaction.input)) {
                    interaction.mouseCapture = true;
                    interaction.mouseCaptureDetect = false;
                    interaction.mouseCaptureUntilNs = monotonicNowNs() + TUI_MOUSE_CAPTURE_NS;
                }
            } else if (!captured && key == KEY_MOUSE) {
                directPointer = ncursesMouseEvent();
                havePointer = directPointer.kind != TERMINAL_POINTER_NONE;
                if (!havePointer) {
                    int suffix = getch();
                    if (suffix == ERR) {
                        interaction.mouseCapture = true;
                        interaction.mouseCaptureDetect = true;
                        interaction.mouseCaptureUntilNs = monotonicNowNs() + TUI_MOUSE_CAPTURE_NS;
                    } else if (suffix >= 0 && suffix <= 255) {
                        bool sgr = suffix >= '0' && suffix <= '9';
                        beginMouseSequence(&interaction.input, sgr);
                        if (TerminalInput_feed(&interaction.input, suffix)) {
                            havePointer = true;
                        } else if (TerminalInput_isPending(&interaction.input)) {
                            interaction.mouseCapture = true;
                            interaction.mouseCaptureDetect = false;
                            interaction.mouseCaptureUntilNs = monotonicNowNs() + TUI_MOUSE_CAPTURE_NS;
                        }
                    } else if (suffix != ERR) {
                        (fn) ungetch(suffix);
                    }
                }
            } else if (!captured && key == 19 && !scene.preview) {
                (*file).saveFailed = false;
                if (!fileSave(file, gOpenPath))
                    (*file).saveFailed = true;
                setSaveStatus(&scene, file);
            } else if (!captured) {
                // Deliberately NOT fed to TerminalInput: it owns "ESC [", so an arrow
                // it did not recognise would be swallowed as a malformed mouse
                // sequence instead of reaching the scroll step below.

                ScrollStep step = scrollStepFor(key);
                if (gRunSelected && step.keyed) {
                    if (key == KEY_HOME)
                        consoleScroll(-LONG_MAX, (bytes) height);
                    else if (key == KEY_END)
                        consoleScroll(LONG_MAX, (bytes) height);
                    else if (step.rows != 0)
                        consoleScroll(step.rows, (bytes) height);
                } else if (gRunSelected) {
                    // No editor input while the console tab owns this panel.
                } else if (scene.preview && step.keyed) {
                    if (step.rows != 0) {
                        (fn) TerminalMarkdownView_scrollBy(scene.markdown, step.rows);
                        sceneMarkdownClearFeedback(&scene);
                    }
                    if (step.columns != 0) {
                        (fn) TerminalMarkdownView_scrollXBy(scene.markdown, step.columns);
                        sceneMarkdownClearFeedback(&scene);
                    }
                } else if (key == KEY_PPAGE || key == KEY_NPAGE) {
                    TerminalScrollGlide_impulse(&glide, step.rows, step.columns, maxTop, maxLeft);
                } else if (!gRunSelected && !scene.preview && editorKey(file, key)) {
                    keepCursorVisible(file, &glide, visible, (bytes) width, &scene);
                }
            }
            if (havePointer) {
                TerminalPointerEvent event = directPointer.kind != TERMINAL_POINTER_NONE
                    ? directPointer : TerminalInput_getEvent(&interaction.input);
                interaction.havePointer = true;
                interaction.pointerX = event.x;
                interaction.pointerY = event.y;
                // The rail's seam is just beyond the last list cell when visible. When
                // the rail is hidden there is no rail target at all; the editor owns the
                // full width; the project root is the first tree row when visible.
                bytes railEdge = scene.railVisible ? scene.railWidth : 0u;
                bytes seam = railEdge;
                interaction.dividerHovered = scene.railVisible && event.x == seam
                    && event.y > 0u && event.y + 1u < (bytes) height;
                switch (event.kind) {
                    case TERMINAL_POINTER_WHEEL_UP:
                    case TERMINAL_POINTER_WHEEL_DOWN: {
                        bool down = event.kind == TERMINAL_POINTER_WHEEL_DOWN;
                        if (event.y == 0u && event.x >= railEdge) {
                            tabScrollBy(down);
                            break;
                        }
                        // A wheel over the RAIL scrolls the rail, not the editor: the
                        // pointer says which list the user means, so no mode is needed.
                        if (scene.railVisible && event.x < railEdge) {
                            if (event.shift || event.meta || event.ctrl) {
                                if (down)
                                    gRailColumn += gRailMaxColumn - gRailColumn < TUI_SCROLL_COLS
                                        ? gRailMaxColumn - gRailColumn : TUI_SCROLL_COLS;
                                else
                                    gRailColumn -= gRailColumn < TUI_SCROLL_COLS
                                        ? gRailColumn : TUI_SCROLL_COLS;
                            } else
                                railScrollBy(down ? 1 : -1);
                            break;
                        }
                        if (gRunSelected && event.x >= railEdge) {
                            consoleScroll(down ? 3 : -3, (bytes) height);
                            break;
                        }
                        if (scene.preview && event.x >= codeLeft) {
                            if (event.shift || event.meta || event.ctrl) {
                                (fn) TerminalMarkdownView_scrollXBy(scene.markdown,
                                    down ? (long) TUI_SCROLL_COLS : -(long) TUI_SCROLL_COLS);
                            } else {
                                (fn) TerminalMarkdownView_scrollBy(scene.markdown, down ? 1 : -1);
                            }
                            sceneMarkdownClearFeedback(&scene);
                            break;
                        }
                        // ANY modified wheel pans sideways, because terminals differ
                        // over which modifier they let through: Shift is the one most
                        // often stolen for the terminal's own scrollback, while
                        // Alt/Option usually is not. Accepting all three means a
                        // trackpad reaches the columns on whichever modifier the
                        // emulator happens to forward - and a modifier is the user
                        // asking for something other than a plain scroll.
                        // A modified wheel drives the horizontal axis: terminals differ
                        // over which modifier they forward, so all three count.
                        long amount = down ? 1 : -1;
                        if (event.shift || event.meta || event.ctrl)
                            TerminalScrollGlide_impulse(&glide, 0, amount * (long) TUI_SCROLL_COLS, maxTop, maxLeft);
                        else
                            TerminalScrollGlide_impulse(&glide, amount * (long) TUI_SCROLL_ROWS, 0, maxTop, maxLeft);
                        break;
                    }
                    case TERMINAL_POINTER_WHEEL_LEFT:
                    case TERMINAL_POINTER_WHEEL_RIGHT: {
                        bool right = event.kind == TERMINAL_POINTER_WHEEL_RIGHT;
                        if (event.y == 0u && event.x >= railEdge) {
                            tabScrollBy(right);
                            break;
                        }
                        if (scene.railVisible && event.x < railEdge) {
                            if (right)
                                gRailColumn += gRailMaxColumn - gRailColumn < TUI_SCROLL_COLS
                                    ? gRailMaxColumn - gRailColumn : TUI_SCROLL_COLS;
                            else
                                gRailColumn -= gRailColumn < TUI_SCROLL_COLS
                                    ? gRailColumn : TUI_SCROLL_COLS;
                            break;
                        }
                        if (gRunSelected)
                            break;
                        if (scene.preview && event.x >= codeLeft) {
                            (fn) TerminalMarkdownView_scrollXBy(scene.markdown,
                                right ? (long) TUI_SCROLL_COLS : -(long) TUI_SCROLL_COLS);
                            sceneMarkdownClearFeedback(&scene);
                            break;
                        }
                        TerminalScrollGlide_impulse(&glide, 0, right ? (long) TUI_SCROLL_COLS
                                                     : -(long) TUI_SCROLL_COLS, maxTop, maxLeft);
                        break;
                    }
                    case TERMINAL_POINTER_PRESS:
                    {
                        interaction.pressed = true;
                        // CATCH IT: a press ends any scroll in flight, which is what makes
                        // momentum safe to have - you can always stop it by touching.
                        TerminalScrollGlide_halt(&glide);
                        if (runControlHit(&scene, event.x, event.y, (bytes) width)) {
                            if (gRunPid > 0)
                                runStop();
                            else
                                (fn) runStart(gOpenPath, file);
                            syncRunControls(&scene, gOpenPath, file);
                            syncTabViews(&scene);
                            break;
                        }
                        if (tabPointerPress(event.x, event.y, event.button))
                            break;
                        if (gRunSelected)
                            break;
                        // A press on a link or a fence's copy glyph is handled before
                        // the divider and tracking-line targets; content rows never
                        // overlap those, and a refused action still consumes the click.
                        if (scene.markdownFile && scene.preview
                            && sceneMarkdownPress(&scene, event.x, event.y,
                                                  (bytes) width, (bytes) height))
                            break;
                        // Markdown mode controls share the second row.
                        if (scene.markdownFile
                            && sceneMarkdownClick(&scene, event.x, event.y, (bytes) width))
                            break;
                        // The collapsed three-cell grip opens the file list and can start
                        // a width drag from the saved edge position.
                        bytes gripMiddle = (bytes) height / 2u;
                        bool collapsedGrip = !scene.railVisible && event.x == 0u
                            && event.y + 1u >= gripMiddle && event.y <= gripMiddle + 1u;
                        if (collapsedGrip) {
                            interaction.gripPress = true;
                            interaction.grabMoved = false;
                            interaction.grabX = event.x;
                            interaction.grabWidth = scene.railRestoreWidth >= TUI_RAIL_MIN
                                                  ? scene.railRestoreWidth
                                                  : TUI_RAIL_DEFAULT_WIDTH;
                            if (!scene.railVisible && sceneSetRailVisible(&scene, true)) {
                                codeLeft = codeLeftColumn(scene.railWidth, scene.numberWidth);
                                maxLeft = maxScrollLeft(file, codeColumns(scene.railWidth,
                                                                         scene.numberWidth,
                                                                         columns));
                                TerminalScrollGlide_clamp(&glide, maxTop, maxLeft);
                            }
                            break;
                        }
                        // A press inside the rail is a file-browser click. Checked before
                        // the divider because the divider is its own edge: a rail row
                        // beside it must still open.
                        if (scene.railVisible && event.x < seam) {
                            railClick(event.y);
                            break;
                        }
                        // The tracking line is the bottom TWO rows: the last editor row
                        // and the frame's bottom border, so the whole floor of the
                        // window is draggable. It wins there; the divider is still
                        // grabbable on every row above.
                        if (event.y + 1u >= (bytes) height - 1u) {
                            interaction.tracking = true;
                            interaction.trackX = event.x;
                            interaction.trackLeft = TerminalScrollGlide_getPosition(&glide).column;
                            break;
                        }
                        TerminalScrollGlidePosition pointerPosition =
                            TerminalScrollGlide_getPosition(&glide);
                        if (!gRunSelected && editorPlaceCaret(file, event.x, event.y,
                                             pointerPosition.row,
                                             pointerPosition.column,
                                             codeLeft, &scene))
                            break;
                        // Three cells of tolerance, and the rail follows the pointer
                        // RELATIVE to where the grab began. Resizing to the absolute
                        // pointer cell made the divider jump whenever the grab
                        // landed a cell off centre.
                        if (scene.railVisible && event.x == seam) {
                            interaction.grabbing = true;
                            interaction.dividerHovered = true;
                            interaction.grabX = event.x;
                            interaction.grabWidth = scene.railWidth;
                        }
                        break;
                    }
                    case TERMINAL_POINTER_RELEASE: {
                        bool wasGrip = interaction.gripPress;
                        bool moved = interaction.grabMoved;
                        bool wasDivider = interaction.grabbing;
                        interaction.gripPress = false;
                        interaction.grabMoved = false;
                        if (wasGrip && !moved && !scene.railVisible) {
                            (fn) sceneSetRailVisible(&scene, true);
                            codeLeft = codeLeftColumn(scene.railWidth, scene.numberWidth);
                            maxLeft = maxScrollLeft(file, codeColumns(scene.railWidth,
                                                                     scene.numberWidth,
                                                                     columns));
                            TerminalScrollGlide_clamp(&glide, maxTop, maxLeft);
                        }
                        if (wasDivider && !moved) {
                            u64 now = monotonicNowNs();
                            bool f64Click = interaction.haveLastDividerClick
                                            && now >= interaction.lastDividerClickNs
                                            && now - interaction.lastDividerClickNs
                                               <= TUI_f64_CLICK_NS;
                            if (f64Click) {
                                interaction.haveLastDividerClick = false;
                                (fn) sceneSetRailVisible(&scene, false);
                                codeLeft = codeLeftColumn(scene.railWidth, scene.numberWidth);
                                maxLeft = maxScrollLeft(file, codeColumns(scene.railWidth,
                                                                         scene.numberWidth,
                                                                         columns));
                                TerminalScrollGlide_clamp(&glide, maxTop, maxLeft);
                            } else {
                                interaction.haveLastDividerClick = true;
                                interaction.lastDividerClickNs = now;
                            }
                        }
                        interaction.grabbing = false;
                        interaction.dividerHovered = scene.railVisible
                            && event.x == scene.railWidth;
                        interaction.tracking = false;
                        interaction.pressed = false;
                        break;
                    }
                    case TERMINAL_POINTER_DRAG: {
                        if (interaction.gripPress) {
                            long moved = (long) event.x - (long) interaction.grabX;
                            if (moved != 0) {
                                interaction.grabMoved = true;
                                if (!scene.railVisible)
                                    (fn) sceneSetRailVisible(&scene, true);
                                long wanted = (long) interaction.grabWidth + moved;
                                if (wanted >= (long) TUI_RAIL_MIN
                                    && wanted <= (long) columns - (long) TUI_RAIL_MARGIN) {
                                    gRailWidthCustomized = true;
                                    sceneSetRailWidth(&scene, (bytes) wanted);
                                    codeLeft = codeLeftColumn(scene.railWidth, scene.numberWidth);
                                    maxLeft = maxScrollLeft(file, codeColumns(scene.railWidth,
                                                                             scene.numberWidth,
                                                                             columns));
                                    TerminalScrollGlide_clamp(&glide, maxTop, maxLeft);
                                }
                            }
                            break;
                        }
                        if (interaction.tracking) {
                            // RELATIVE, by the pointer's own delta. Mapping the absolute
                            // pointer position onto the offset flung the view to
                            // wherever the press landed; a drag has to move the content
                            // by as much as the finger moved, so the point under the
                            // finger stays under the finger.
                            bytes wanted = applyScroll(interaction.trackLeft,
                                                        (int) event.x - (int) interaction.trackX,
                                                        maxLeft);
                            TerminalScrollGlide_setColumnPosition(&glide, wanted, maxLeft);
                            break;
                        }
                        if (interaction.grabbing) {
                            long moved = (long) event.x - (long) interaction.grabX;
                            long wanted = (long) interaction.grabWidth + moved;
                            if (moved != 0)
                                interaction.grabMoved = true;
                            if (event.x <= 1u || wanted < (long) TUI_RAIL_MIN) {
                                if (scene.railVisible)
                                    (fn) sceneSetRailVisible(&scene, false);
                                codeLeft = codeLeftColumn(scene.railWidth, scene.numberWidth);
                                maxLeft = maxScrollLeft(file, codeColumns(scene.railWidth,
                                                                         scene.numberWidth,
                                                                         columns));
                                TerminalScrollGlide_clamp(&glide, maxTop, maxLeft);
                            } else if (wanted >= (long) TUI_RAIL_MIN
                                && wanted <= (long) columns - (long) TUI_RAIL_MARGIN) {
                                if (!scene.railVisible)
                                    (fn) sceneSetRailVisible(&scene, true);
                                gRailWidthCustomized = true;
                                sceneSetRailWidth(&scene, (bytes) wanted);
                                // The code area moved with the rail, so its bounds did.
                                codeLeft = codeLeftColumn(scene.railWidth, scene.numberWidth);
                                maxLeft = maxScrollLeft(file, codeColumns(scene.railWidth,
                                                                         scene.numberWidth,
                                                                         columns));
                                TerminalScrollGlide_clamp(&glide, maxTop, maxLeft);
                            }
                        }
                        // Edge autoscroll belongs to a DRAG, not to hovering: acting
                        // on plain motion made the view scroll whenever the pointer
                        // passed over the first or last editor row.
                        // Both bottom rows belong to the tracking line, so the vertical
                        // edge zone sits two rows above them.
                        if (event.y == editorFirstRow())
                            TerminalScrollGlide_impulse(&glide, -1, 0, maxTop, maxLeft);
                        else if (event.y == height - 3u)
                            TerminalScrollGlide_impulse(&glide, 1, 0, maxTop, maxLeft);
                        if (event.x <= codeLeft)
                            TerminalScrollGlide_impulse(&glide, 0, -1, maxTop, maxLeft);
                        else if (event.x >= codeRight)
                            TerminalScrollGlide_impulse(&glide, 0, 1, maxTop, maxLeft);
                        break;
                    }
                    case TERMINAL_POINTER_MOVE:
                        {
                            bytes gripMiddle = (bytes) height / 2u;
                            bool collapsedGrip = !scene.railVisible && event.x == 0u
                                && event.y + 1u >= gripMiddle
                                && event.y <= gripMiddle + 1u;
                            sceneSetGripHover(&scene, collapsedGrip);
                        }
                        interaction.dividerHovered = scene.railVisible
                            && event.x == scene.railWidth
                            && event.y > 0u && event.y + 1u < (bytes) height;
                        if (scene.markdownFile && scene.preview)
                            sceneMarkdownUpdateHover(&scene, event.x, event.y,
                                                     (bytes) width, (bytes) height);
                        break;
                    default:
                        break;
                }
            }
            key = handled < TUI_EVENTS_PER_FRAME ? getch() : ERR;
        }

        // A notice or a check glyph is feedback, not retained state: once its window
        // closes it is gone before the frame paints, so no later present resurrects it.
        if (scene.markdownNotice[0] != '\0'
            && monotonicNowNs() >= scene.markdownNoticeUntilNs)
            sceneMarkdownClearNotice(&scene);
        if (scene.copiedBlock != TUI_MARKDOWN_NO_BLOCK
            && monotonicNowNs() >= scene.copiedUntilNs)
            sceneMarkdownClearCopied(&scene);

        runPoll();
        TerminalCanvas back = TerminalCanvas(gCells, gStyles, gFg, gBg, (bytes) width, (bytes) height);
        TerminalCanvas front = TerminalCanvas(gFrontCells, gFrontStyles, gFrontFg, gFrontBg,
                                             (bytes) width, (bytes) height);
        TerminalCanvas_fill(&back, ' ', TerminalPen());
        bool needsBind = !TerminalSurface_isBound(&surface)
            || TerminalSurface_getBack(&surface).gridWidth != (bytes) width
            || TerminalSurface_getBack(&surface).gridHeight != (bytes) height;
        if (needsBind || gRepaintAll) {
            // A mirror that differs from every cell there is: the device behind it is
            // brand new or was just cleared, so nothing it holds is still described by
            // an old front grid.
            presenterInvalidateFront();
            gRepaintAll = false;
        }
        if (needsBind && !TerminalSurface_bind(&surface, &back, &front, blitRun, nullptr)) {
            ok = false;
            break;
        }
        gPresentWidth = (bytes) width;
        // Cover a fraction of what is left. The position is a whole number of cells, so
        // the fractional glide is rounded at the moment it is drawn.
        // A newly opened file starts at its top-left, and the glide is dropped with it:
        // momentum carried over from the previous file would scroll the new one away.
        if (gResetView) {
            bytes rowTarget = 0u;
            bytes columnTarget = 0u;
            if (!scene.preview && (*file).lineCount != 0u) {
                if ((*file).cursorLine >= visible)
                    rowTarget = (*file).cursorLine - visible + 1u;
                bytes cursorCells = cellsOf((*file).lines[(*file).cursorLine],
                                             (*file).lineLengths[(*file).cursorLine]);
                bytes codeWidth = codeColumns(scene.railWidth, scene.numberWidth, width);
                if (cursorCells >= codeWidth)
                    columnTarget = cursorCells - codeWidth + 1u;
            }
            TerminalScrollGlide_setPosition(&glide, rowTarget, columnTarget, maxTop, maxLeft);
            gResetView = false;
        }
        TerminalScrollGlidePosition scrollPosition = TerminalScrollGlide_step(&glide);
        bytes scrollTop = scrollPosition.row;
        bytes scrollLeft = scrollPosition.column;
        fileSyntaxRefresh(file);
        sceneSyncRows(&scene, file, scrollTop, scrollLeft, visible);
        if (scene.editorPanelVisible == gRunSelected) {
            if (!TerminalPanel_setAnchor(scene.centre, scene.editorId,
                gRunSelected ? TerminalAnchor_2(TERMINAL_ANCHOR_BAND_TOP, 0u)
                             : TerminalAnchor_0())) {
                ok = false;
                break;
            }
            scene.editorPanelVisible = !gRunSelected;
        }
        sceneSyncRail(&scene, railVisibleRows(height));
        updateClock(&scene);
        TerminalPanel_paint(scene.root, &back);
        if (!gRunSelected)
            paintEditorSyntax(&back, file, &scene, scrollTop, scrollLeft);
        if (!gRunSelected)
            sceneRenderMarkdown(&scene, &back, scene.railWidth);
        if (!scene.preview && !gRunSelected)
            paintEditorCursor(&back, file, scrollTop, scrollLeft,
                              scene.railWidth, scene.numberWidth);
        runPaint(&back, &scene, (bytes) width, (bytes) height);
        paintRailColors(&scene, &back, interaction.havePointer,
                         interaction.pointerX, interaction.pointerY);
        paintRailSplitter(&scene, &back,
                          interaction.dividerHovered || interaction.grabbing);
        // The surface diffs this grid against the last one it handed over and writes
        // only what changed, so a frame that types, hovers or ticks the clock costs a
        // few runs and an idle frame costs nothing at all.
        TerminalPresentResult presented = TerminalSurface_present(&surface);
        if (!presented.ok)
            ok = false;
        if (!ok)
            break;
    }
    (fn) mouseReporting(false);
    // The session is written on the way out, so the next run in this project comes back
    // to the same file and the same divider.
    cacheSave();
    gScene = nullptr;
    gOpenFile = nullptr;
    // Hand the device back in the order it was taken: autowrap, cursor, alternate
    // screen. endwin still restores termios and the terminal modes around this.
    presenterLeave();
    (fn) curs_set(1);
    (fn) endwin();
    (fn) delscreen(screen);
    sceneFree(&scene);
    if (!ok)
        THROW("semicolon: terminal operation failed; terminal restored");
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    initTextures();
    const char *configuredAccent = getenv("SEMICOLON_ACCENT");
    if (configuredAccent != nullptr && *configuredAccent != '\0') {
        if (!parsePackedHex(configuredAccent, &gAccentColor)) {
            THROW("semicolon: SEMICOLON_ACCENT must be #RRGGBB or #RRGGBBAA");
            return 1;
        }
        gAccentOverride = true;
    }
    const char *iconMode = getenv("SEMICOLON_ICONS");
    if (iconMode != nullptr && !setIconMode(iconMode))
        return 1;
    bool dump = false;
    bool fileGiven = false;
    int width = 0;
    int height = 0;
    char path[1024];
    path[0] = '\0';
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "icons") == 0)
            return showIconCatalog();
        if (strcmp(argv[i], "--help") == 0) {
            puts("semicolon [--dump [WxH]] [--file PATH] [--accent #RRGGBB] [--help]");
            puts("  icons         list supported icons and their terminal colors");
            puts("  no arguments  draw the IDE shell on this terminal (Ctrl-C quits)");
            puts("  --root PATH   browse this project (default: the current directory)");
            puts("  --icons MODE  nerd (default) or plain; overrides SEMICOLON_ICONS");
            puts("  --accent HEX  set and save the shared Markdown/tree accent (#RRGGBB)");
            puts("  --file PATH   open PATH in the editor (default: this project's session,"
                 " else its first file)");
            puts("  --dump        print one frame as plain text instead of using a terminal");
            puts("  --input-probe print the mouse and key events this terminal actually sends");
            puts("  --bg-probe    print the background colour this terminal reports (fades use it)");
            return 0;
        }
        if (strcmp(argv[i], "--icons") == 0 && i + 1 < argc) {
            if (!setIconMode(argv[++i]))
                return 1;
            continue;
        }
        if (strcmp(argv[i], "--accent") == 0 && i + 1 < argc) {
            if (!parsePackedHex(argv[++i], &gAccentColor)) {
                THROW("semicolon: --accent must be #RRGGBB or #RRGGBBAA");
                return 1;
            }
            gAccentOverride = true;
            continue;
        }
        if (strcmp(argv[i], "--dump") == 0) {
            dump = true;
            if (i + 1 < argc && strchr(argv[i + 1], 'x') != nullptr) {
                char *end = nullptr;
                long parsedWidth = strtol(argv[++i], &end, 10);
                char *heightEnd = nullptr;
                long parsedHeight = end != nullptr ? strtol(end + 1, &heightEnd, 10) : 0;
                if (end == nullptr || *end != 'x' || heightEnd == nullptr || *heightEnd != '\0'
                    || parsedWidth < 1 || parsedHeight < 1) {
                    THROW("semicolon: --dump size must look like 103x23");
                    return 1;
                }
                width = (int) parsedWidth;
                height = (int) parsedHeight;
            }
            continue;
        }
        if (strcmp(argv[i], "--input-probe") == 0) {
            initTextures();
            return runInputProbe();
        }
        if (strcmp(argv[i], "--bg-probe") == 0)
            return runBackgroundProbe();
        if (strcmp(argv[i], "--root") == 0 && i + 1 < argc) {
            (fn) snprintf(gRailRoot, sizeof gRailRoot, "%s", argv[++i]);
            continue;
        }
        if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
            (fn) snprintf(path, sizeof path, "%s", argv[++i]);
            fileGiven = true;
            continue;
        }
        THROW("semicolon: unknown argument");
        return 1;
    }
    // The project is where the shell was started, unless one was named. That is what
    // makes "cd anywhere && semicolon" mean the project you are standing in.
    if (gRailRoot[0] == '\0') {
        if (getcwd(gRailRoot, sizeof gRailRoot) == nullptr) {
            THROW("semicolon: cannot determine the current directory; pass --root");
            return 1;
        }
    }
    bytes rootLength = strlen(gRailRoot);
    while (rootLength > 1u && gRailRoot[rootLength - 1u] == '/')
        gRailRoot[--rootLength] = '\0';
    char absoluteRoot[PATH_MAX];
    if (realpath(gRailRoot, absoluteRoot) == nullptr) {
        THROW("semicolon: cannot resolve project location %s", gRailRoot);
        return 1;
    }
    if (gRailRoot[0] != '/') {
        if (strlen(absoluteRoot) >= sizeof gRailRoot) {
            THROW("semicolon: resolved project path exceeds %zu bytes", sizeof gRailRoot - 1u);
            return 1;
        }
        memcpy(gRailRoot, absoluteRoot, strlen(absoluteRoot) + 1u);
    }
    const char *home = getenv("HOME");
    bytes homeLength = home == nullptr ? 0u : strlen(home);
    if (homeLength > 0u && strncmp(absoluteRoot, home, homeLength) == 0
        && (absoluteRoot[homeLength] == '/' || absoluteRoot[homeLength] == '\0'))
        (fn) snprintf(gRailLocation, sizeof gRailLocation, "~%s", absoluteRoot + homeLength);
    else
        (fn) snprintf(gRailLocation, sizeof gRailLocation, "%s", absoluteRoot);
    cacheLoad(gRailRoot);
    if (!fileGiven) {
        // The project's own session first, then its first file. A cached path that is no
        // longer readable is simply not used, so a deleted file is not a startup failure.
        if (gCachedOpen[0] != '\0' && access(gCachedOpen, R_OK) == 0)
            (fn) snprintf(path, sizeof path, "%s", gCachedOpen);
        else if (!firstFileIn(gRailRoot, path, sizeof path)) {
            THROW("semicolon: no file to open in %s; pass --file", gRailRoot);
            return 1;
        }
    }
    (fn) snprintf(gOpenPath, sizeof gOpenPath, "%s", path);
    FileText file;
    if (!fileLoad(&file, path)) {
        fprintf(stderr, "[vex] semicolon: cannot read %s\n", path);
        return 1;
    }
    // The active tab names the file that is on screen. Set here, once, so the dumped frame
    // and the interactive shell cannot disagree about which file they are showing.
    if (!tabAdmit(path, file.name)) {
        fileFree(&file);
        THROW("semicolon: could not allocate an editor tab for %s", path);
        return 1;
    }
    gActiveTabIndex = 0u;
    int status = 0;
    if (dump) {
        if (width < 1)
            width = 103;
        if (height < 1)
            height = 23;
        status = dumpFrame(width, height, &file);
    } else {
        status = runTerminal(&file);
    }
    fileFree(&file);
    for (bytes index = 0u; index < gTabCount; ++index)
        fileFree(&gTabs[index].suspended);
    free(gTabs);
    return status;
}

