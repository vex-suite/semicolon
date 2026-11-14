#include "editor/semicolon_syntax.h"
#include "adapters/c/c23_highlights.h"
#include "annotation/annotation.h"
#include "exception/throw.h"
#include <stdlib.h>
#include <string.h>

;;DEFINITION
/** SemicolonSyntax is the R6 consumer bridge between Language's conservative C23
 * highlight pass and Determinant pens. It copies editor rows into one immutable
 * normalized snapshot so multiline comments are classified as a document, never
 * independent lines. Only cold document replacement analyses/allocates. Hot byte
 * projections binary search Language's ordered spans; scroll and caret movement
 * need no new analysis. Palette choices belong here, not inside either R3 owner.
 * Language's function/type/variable/macro and preprocessor-directive distinction,
 * including the string-role header path after #include, is a local lexical
 * heuristic, not parsing, diagnostics or symbol resolution. */

;;OVERVIEW
/** ============================================================================
 * CLASS: SemicolonSyntax (editor/semicolon_syntax.c)
 * STRUCT FIELDS (Mirroring this opaque class)
 *   char *source                  owned immutable normalized source
 *   bytes length                 source bytes, excluding NUL
 *   bytes *starts                owned line-start offsets plus end sentinel
 *   bytes lineCount              initialized row count
 *   C23Highlights *highlights     owned ordered Language semantic-ish spans
 *   TerminalPen pens[SYNTAX_ROLE_IDENTIFIER + 1u] current role palette
 *   bytes refusals               storage refusals in this owner's lifetime
 * FUNCTION REGISTRY
 *   SemicolonSyntax *SemicolonSyntax_0(), SemicolonSyntax(), zero()
 *   fn SemicolonSyntax_free(SemicolonSyntax *self)
 *   bool SemicolonSyntax_setLines(self,const char *const *lines,
 *                                const bytes *lengths,bytes count)
 *   bool SemicolonSyntax_acceptsPath(const char *path)
 *   bool SemicolonSyntax_setPen(self,SyntaxRole role,TerminalPen pen)
 *   TerminalPen SemicolonSyntax_getPen(const self,SyntaxRole role)
 *   TerminalPen SemicolonSyntax_penAt(const self,bytes line,bytes byte)
 *   bytes SemicolonSyntax_getLineCount/getSpanCount/exhaustionCount(const self)
 *   TerminalProjection SemicolonSyntax_toString/toStringStruct(const self)
 * PRIVATE FUNCTIONS
 *   refuse(self,request)           report first storage refusal; count every one
 * CONTRACT
 *   External serialization; no borrowed input survives setLines. Configuration
 *   and failed replacement preserve all previously admitted bytes and colors.
 *   Counts/owned geometry change only through admission/lifecycle, not setters
 *   that forge initialized storage (Conflict Triage Law managed accessor seam).
 *   No public source projection: terminal controls remain the host's sanitized
 *   glyphs. INVALID and unclassified bytes use ordinary text, not error styling.
 *   Capacity is growable with checked arithmetic; refusal count resets on new
 *   construction, not successful replacement. Null queries return safe defaults.
 * ============================================================================ */

struct SemicolonSyntax {
    char *source;
    bytes length;
    bytes *starts;
    bytes lineCount;
    C23Highlights *highlights;
    TerminalPen pens[SYNTAX_ROLE_IDENTIFIER + 1u];
    bytes refusals;
};

// Defaults initialize configurable pens, never override a consumer's current pen.
#define SYNTAX_TEXT_DEFAULT TERMINAL_RGB(0xF0, 0xF0, 0xF0)
#define SYNTAX_KEYWORD_DEFAULT TERMINAL_RGB(0x00, 0x80, 0x00)
#define SYNTAX_TYPE_DEFAULT TERMINAL_RGB(0x4E, 0xC9, 0xB0)
#define SYNTAX_FUNCTION_DEFAULT TERMINAL_RGB(0x4F, 0xC1, 0xFF)
#define SYNTAX_VARIABLE_DEFAULT TERMINAL_RGB(0x9C, 0xDC, 0xFE)
#define SYNTAX_MACRO_DEFAULT TERMINAL_RGB(0xDC, 0xDC, 0xAA)
#define SYNTAX_LITERAL_DEFAULT TERMINAL_RGB(0xA0, 0xF5, 0xA0)
#define SYNTAX_COMMENT_DEFAULT TERMINAL_RGB(0x80, 0x80, 0x80)
#define SYNTAX_DOC_COMMENT_DEFAULT TERMINAL_RGB(0x6A, 0x99, 0x55)

static bool refuse(SemicolonSyntax *self, bytes request) {
    if ((*self).refusals == 0u)
        THROW("SemicolonSyntax storage refused: request %zu bytes; allocation/extent bound reached", request);
    if ((*self).refusals != SIZE_MAX)
        ++(*self).refusals;
    return false;
}

SemicolonSyntax *SemicolonSyntax_0() {
    SemicolonSyntax *self = calloc(1u, sizeof *self);
    if (self == nullptr) {
        THROW("SemicolonSyntax owner allocation refused: request %zu bytes", sizeof *self);
        return nullptr;
    }
    // Screenshot-inspired defaults, not exact theme extraction. Every role pen
    // is replaceable through setPen; the terminal may quantize these RGB values.
    for (bytes role = 0u; role <= SYNTAX_ROLE_IDENTIFIER; ++role)
        (*self).pens[role] = TerminalPen(TERMINAL_STYLE_NONE, SYNTAX_TEXT_DEFAULT);
    (*self).pens[SYNTAX_ROLE_KEYWORD] = TerminalPen(0u, SYNTAX_KEYWORD_DEFAULT);
    (*self).pens[SYNTAX_ROLE_TYPE] = TerminalPen(0u, SYNTAX_TYPE_DEFAULT);
    (*self).pens[SYNTAX_ROLE_TYPE_PARAMETER] = (*self).pens[SYNTAX_ROLE_TYPE];
    (*self).pens[SYNTAX_ROLE_TYPE_ARGUMENT] = (*self).pens[SYNTAX_ROLE_TYPE];
    (*self).pens[SYNTAX_ROLE_FUNCTION] = TerminalPen(0u, SYNTAX_FUNCTION_DEFAULT);
    (*self).pens[SYNTAX_ROLE_METHOD] = (*self).pens[SYNTAX_ROLE_FUNCTION];
    (*self).pens[SYNTAX_ROLE_CONSTRUCTOR] = (*self).pens[SYNTAX_ROLE_FUNCTION];
    (*self).pens[SYNTAX_ROLE_VARIABLE] = TerminalPen(0u, SYNTAX_VARIABLE_DEFAULT);
    (*self).pens[SYNTAX_ROLE_PARAMETER] = (*self).pens[SYNTAX_ROLE_VARIABLE];
    (*self).pens[SYNTAX_ROLE_FIELD] = (*self).pens[SYNTAX_ROLE_VARIABLE];
    (*self).pens[SYNTAX_ROLE_MEMBER] = (*self).pens[SYNTAX_ROLE_VARIABLE];
    (*self).pens[SYNTAX_ROLE_MACRO] = TerminalPen(TERMINAL_STYLE_BOLD, SYNTAX_MACRO_DEFAULT);
    (*self).pens[SYNTAX_ROLE_MACRO_INVOCATION] = (*self).pens[SYNTAX_ROLE_MACRO];
    (*self).pens[SYNTAX_ROLE_CONSTANT] = (*self).pens[SYNTAX_ROLE_MACRO];
    (*self).pens[SYNTAX_ROLE_STRING] = TerminalPen(0u, SYNTAX_LITERAL_DEFAULT);
    (*self).pens[SYNTAX_ROLE_CHARACTER] = (*self).pens[SYNTAX_ROLE_STRING];
    (*self).pens[SYNTAX_ROLE_NUMBER] = (*self).pens[SYNTAX_ROLE_STRING];
    (*self).pens[SYNTAX_ROLE_COMMENT] = TerminalPen(0u, SYNTAX_COMMENT_DEFAULT);
    (*self).pens[SYNTAX_ROLE_DOC_COMMENT] = TerminalPen(0u, SYNTAX_DOC_COMMENT_DEFAULT);
    (*self).pens[SYNTAX_ROLE_BRACKET].style = TERMINAL_STYLE_BOLD;
    return self;
}

fn SemicolonSyntax_free(SemicolonSyntax *self) {
    if (self == nullptr)
        return;
    C23Highlights_free((*self).highlights);
    free((*self).source);
    free((*self).starts);
    free(self);
}

bool SemicolonSyntax_acceptsPath(const char *path) {
    if (path == nullptr)
        return false;
    const char *extension = strrchr(path, '.');
    return extension != nullptr
        && (strcmp(extension, ".c") == 0 || strcmp(extension, ".h") == 0);
}

bool SemicolonSyntax_setLines(SemicolonSyntax *self, const char *const *lines,
                              const bytes *lengths, bytes count) {
    if (self == nullptr || (count != 0u && (lines == nullptr || lengths == nullptr))) {
        THROW("SemicolonSyntax rejected missing editor rows");
        return false;
    }
    if (count >= SIZE_MAX / sizeof(bytes))
        return refuse(self, SIZE_MAX);
    bytes length = 0u;
    for (bytes line = 0u; line < count; ++line) {
        if (lines[line] == nullptr && lengths[line] != 0u) {
            THROW("SemicolonSyntax rejected missing row bytes");
            return false;
        }
        if (lengths[line] >= SIZE_MAX - length)
            return refuse(self, SIZE_MAX);
        length += lengths[line] + 1u;
    }
    if (length == SIZE_MAX)
        return refuse(self, SIZE_MAX);
    char *source = malloc(length + 1u);
    bytes *starts = malloc((count + 1u) * sizeof *starts);
    if (source == nullptr || starts == nullptr) {
        bytes request = source == nullptr ? length + 1u : (count + 1u) * sizeof *starts;
        free(source);
        free(starts);
        return refuse(self, request);
    }
    bytes at = 0u;
    for (bytes line = 0u; line < count; ++line) {
        starts[line] = at;
        if (lengths[line] != 0u)
            memcpy(source + at, lines[line], lengths[line]);
        at += lengths[line];
        source[at++] = '\n';
    }
    starts[count] = at;
    source[at] = '\0';
    C23Highlights *highlights = C23Highlights(1u, source, length);
    if (highlights == nullptr) {
        free(source);
        free(starts);
        if ((*self).refusals != SIZE_MAX)
            ++(*self).refusals;
        // The Language owner already diagnosed its failed construction.
        return false;
    }
    C23Highlights_free((*self).highlights);
    free((*self).source);
    free((*self).starts);
    (*self).source = source;
    (*self).length = length;
    (*self).starts = starts;
    (*self).lineCount = count;
    (*self).highlights = highlights;
    return true;
}

bool SemicolonSyntax_setPen(SemicolonSyntax *self, SyntaxRole role, TerminalPen pen) {
    if (self == nullptr || !SyntaxRole_isValid(role)) {
        THROW("SemicolonSyntax rejected palette role");
        return false;
    }
    (*self).pens[role] = pen;
    return true;
}

TerminalPen SemicolonSyntax_getPen(const SemicolonSyntax *self, SyntaxRole role) {
    return self == nullptr || !SyntaxRole_isValid(role) ? TerminalPen()
                                                     : (*self).pens[role];
}

TerminalPen SemicolonSyntax_penAt(const SemicolonSyntax *self, bytes line, bytes byte) {
    if (self == nullptr || line >= (*self).lineCount)
        return TerminalPen();
    bytes start = (*self).starts[line];
    bytes end = (*self).starts[line + 1u];
    if (byte >= end - start - 1u)
        return (*self).pens[SYNTAX_ROLE_IDENTIFIER];
    bytes offset = start + byte;
    SyntaxRole role = C23Highlights_roleAt((*self).highlights, offset);
    if (!SyntaxRole_isValid(role))
        return (*self).pens[SYNTAX_ROLE_IDENTIFIER];
    return (*self).pens[role];
}

bytes SemicolonSyntax_getLineCount(const SemicolonSyntax *self) {
    return self == nullptr ? 0u : (*self).lineCount;
}

bytes SemicolonSyntax_getSpanCount(const SemicolonSyntax *self) {
    return self == nullptr ? 0u : C23Highlights_getCount((*self).highlights);
}

bytes SemicolonSyntax_exhaustionCount(const SemicolonSyntax *self) {
    return self == nullptr ? 0u : (*self).refusals;
}

TerminalProjection SemicolonSyntax_toString(const SemicolonSyntax *self) {
    if (self == nullptr)
        return TerminalProjection_format("nullptr");
    return TerminalProjection_format("SemicolonSyntax(lines=%zu, spans=%zu)",
                                     (*self).lineCount,
                                     C23Highlights_getCount((*self).highlights));
}

TerminalProjection SemicolonSyntax_toStringStruct(const SemicolonSyntax *self) {
    if (self == nullptr)
        return TerminalProjection_format("nullptr");
    TerminalProjection projection = TerminalProjection_format(
        "SemicolonSyntax { source=%p, length=%zu, starts=%p, lineCount=%zu, highlights=%p, spanCount=%zu, pens=[",
        (any) (*self).source, (*self).length, (any) (*self).starts,
        (*self).lineCount, (any) (*self).highlights,
        C23Highlights_getCount((*self).highlights));
    for (bytes role = 0u; role <= SYNTAX_ROLE_IDENTIFIER; ++role) {
        TerminalPen pen = (*self).pens[role];
        TerminalProjection next = TerminalProjection_format(
            "%s%s%zu:{fg=%08x,bg=%08x,style=%u}", projection.text,
            role == 0u ? "" : ",", role, pen.fg, pen.bg, pen.style);
        next.truncated = next.truncated || projection.truncated;
        projection = next;
    }
    TerminalProjection result = TerminalProjection_format("%s], refusals=%zu }",
                                                         projection.text, (*self).refusals);
    result.truncated = result.truncated || projection.truncated;
    return result;
}
