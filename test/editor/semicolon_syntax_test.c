// Owner: copied C23 document coloring, not compiler semantics or appearance.
// Rows remain caller-owned; every cold failure preserves the prior analysis.
// One serialized owner: concurrent mutation and backend occupancy are inapplicable.
#include "editor/semicolon_syntax.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static size_t untilFailure = SIZE_MAX;
static size_t live;

static bool allocationAllowed(void) {
    if (untilFailure == SIZE_MAX)
        return true;
    if (untilFailure == 0u)
        return false;
    --untilFailure;
    return true;
}

void *syntax_test_malloc(size_t size) {
    void *value = allocationAllowed() ? malloc(size) : nullptr;
    if (value != nullptr)
        ++live;
    return value;
}

void *syntax_test_calloc(size_t count, size_t size) {
    void *value = allocationAllowed() ? calloc(count, size) : nullptr;
    if (value != nullptr)
        ++live;
    return value;
}

void *syntax_test_realloc(void *old, size_t size) {
    if (!allocationAllowed())
        return nullptr;
    bool fresh = old == nullptr;
    void *value = realloc(old, size);
    if (fresh && value != nullptr)
        ++live;
    return value;
}

void syntax_test_free(void *value) {
    if (value != nullptr) {
        assert(live != 0u);
        --live;
    }
    free(value);
}

static void testRows(void) {
    SemicolonSyntax *syntax = SemicolonSyntax();
    assert(syntax != nullptr);
    char first[] = "int value = 42; /* open";
    const char *rows[] = {first, "continued */ return \"hello\";", "char c = 'x';"};
    size_t lengths[] = {strlen(rows[0]), strlen(rows[1]), strlen(rows[2])};
    assert(SemicolonSyntax_setLines(syntax, rows, lengths, 3u));
    assert(SemicolonSyntax_getLineCount(syntax) == 3u);
    assert(SemicolonSyntax_getSpanCount(syntax) > 10u);
    assert(SemicolonSyntax_penAt(syntax, 0u, 0u).fg == 0x008000FFu);
    assert(SemicolonSyntax_penAt(syntax, 0u, 12u).fg == 0xA0F5A0FFu);
    assert(SemicolonSyntax_penAt(syntax, 1u, 0u).fg == 0x808080FFu);
    assert(SemicolonSyntax_penAt(syntax, 1u, 13u).fg == 0x008000FFu);
    assert(SemicolonSyntax_penAt(syntax, 1u, 20u).fg == 0xA0F5A0FFu);
    assert(SemicolonSyntax_penAt(syntax, 2u, 10u).fg == 0xA0F5A0FFu);
    assert(SemicolonSyntax_penAt(syntax, 0u, 4u).fg == 0x9CDCFEFFu);
    assert(SemicolonSyntax_penAt(syntax, 2u, 5u).fg == 0x9CDCFEFFu);
    first[0] = 'X';
    assert(SemicolonSyntax_penAt(syntax, 0u, 0u).fg == 0x008000FFu);
    assert(!SemicolonSyntax_setLines(syntax, nullptr, lengths, 3u));
    assert(SemicolonSyntax_getLineCount(syntax) == 3u);
    const char *absent[] = {nullptr};
    size_t one[] = {1u};
    assert(!SemicolonSyntax_setLines(syntax, absent, one, 1u));
    assert(!SemicolonSyntax_setLines(nullptr, rows, lengths, 3u));
    assert(SemicolonSyntax_penAt(syntax, 0u, 0u).fg == 0x008000FFu);
    TerminalPen custom = TerminalPen(TERMINAL_STYLE_BOLD, 0x123456FFu);
    assert(SemicolonSyntax_setPen(syntax, SYNTAX_ROLE_KEYWORD, custom));
    assert(SemicolonSyntax_getPen(syntax, SYNTAX_ROLE_KEYWORD).fg == custom.fg);
    assert(SemicolonSyntax_penAt(syntax, 0u, 0u).style == TERMINAL_STYLE_BOLD);
    assert(!SemicolonSyntax_setPen(syntax, SYNTAX_ROLE_NONE, custom));
    assert(!SemicolonSyntax_setPen(nullptr, SYNTAX_ROLE_KEYWORD, custom));
    assert(!SemicolonSyntax_setPen(syntax, (SyntaxRole) SIZE_MAX, custom));
    TerminalProjection value = SemicolonSyntax_toString(syntax);
    assert(strstr(value.text, "lines=3") != nullptr);
    value = SemicolonSyntax_toStringStruct(syntax);
    assert(strstr(value.text, "source=") != nullptr);
    assert(SemicolonSyntax_setLines(syntax, nullptr, nullptr, 0u));
    assert(SemicolonSyntax_getLineCount(syntax) == 0u);
    assert(SemicolonSyntax_getSpanCount(syntax) == 0u);
    const char *empty[] = {nullptr};
    size_t zero[] = {0u};
    assert(SemicolonSyntax_setLines(syntax, empty, zero, 1u));
    assert(SemicolonSyntax_penAt(syntax, 0u, SIZE_MAX).fg == 0xF0F0F0FFu);
    const char *recovery[] = {"bool ready = true; nullptr; { }"};
    size_t recoveryLengths[] = {strlen(recovery[0])};
    assert(SemicolonSyntax_setLines(syntax, recovery, recoveryLengths, 1u));
    size_t brace = (size_t) (strchr(recovery[0], '{') - recovery[0]);
    assert(SemicolonSyntax_penAt(syntax, 0u, brace).style == TERMINAL_STYLE_BOLD);
    const char hostile[] = {'/', '*', ' ', '\xFF', '\0', ' ', '*', '/', ' ', 'i', 'n', 't'};
    const char *hostileRows[] = {hostile, "/* still editing", "unterminated"};
    size_t hostileLengths[] = {sizeof hostile, 16u, 12u};
    assert(SemicolonSyntax_setLines(syntax, hostileRows, hostileLengths, 3u));
    // Invalid bytes remain ordinary foreground, never imaginary error squiggles.
    assert(SemicolonSyntax_penAt(syntax, 0u, 3u).fg == 0xF0F0F0FFu);
    assert(SemicolonSyntax_penAt(syntax, 0u, 9u).fg == custom.fg);
    assert(SemicolonSyntax_penAt(syntax, SIZE_MAX, SIZE_MAX).fg == 0u);
    assert(SemicolonSyntax_getPen(syntax, (SyntaxRole) SIZE_MAX).fg == 0u);
    SemicolonSyntax_free(syntax);
}

static void testRolesAndComments(void) {
    SemicolonSyntax *syntax = SemicolonSyntax();
    assert(syntax != nullptr);
    char first[] = "uint8_t Console_log(const char *message); THROW(\"x\");";
    char second[] = "/** doc */ int x;";
    char third[] = "/* regular */ int y;";
    char fourth[] = "#include <locale.h>";
    char fifth[] = "#define VALUE 1";
    char sixth[] = "#include \"x.h\"";
    const char *rows[] = {first, second, third, fourth, fifth, sixth};
    size_t lengths[] = {strlen(rows[0]), strlen(rows[1]), strlen(rows[2]),
                        strlen(rows[3]), strlen(rows[4]), strlen(rows[5])};
    assert(SemicolonSyntax_setLines(syntax, rows, lengths, 6u));
    size_t type = (size_t) (strstr(first, "uint8_t") - first);
    size_t function = (size_t) (strstr(first, "Console_log") - first);
    size_t variable = (size_t) (strstr(first, "message") - first);
    size_t macro = (size_t) (strstr(first, "THROW") - first);
    assert(SemicolonSyntax_penAt(syntax, 0u, type).fg == 0x4EC9B0FFu);
    assert(SemicolonSyntax_penAt(syntax, 0u, function).fg == 0x4FC1FFFFu);
    assert(SemicolonSyntax_penAt(syntax, 0u, variable).fg == 0x9CDCFEFFu);
    TerminalPen macroPen = SemicolonSyntax_penAt(syntax, 0u, macro);
    assert(macroPen.fg == 0xDCDCAAFFu);
    assert((macroPen.style & TERMINAL_STYLE_BOLD) != 0u);
    assert(SemicolonSyntax_penAt(syntax, 1u, 0u).fg == 0x6A9955FFu);
    assert(SemicolonSyntax_penAt(syntax, 2u, 0u).fg == 0x808080FFu);
    TerminalPen directive = SemicolonSyntax_penAt(syntax, 3u, 0u);
    assert(directive.fg == 0xDCDCAAFFu);
    assert((directive.style & TERMINAL_STYLE_BOLD) != 0u);
    assert(SemicolonSyntax_penAt(syntax, 3u, 1u).fg == 0xDCDCAAFFu);
    size_t header = (size_t) (strstr(fourth, "<locale.h>") - fourth);
    assert(SemicolonSyntax_penAt(syntax, 3u, header).fg == 0xA0F5A0FFu);
    assert(SemicolonSyntax_penAt(syntax, 3u, header + 1u).fg == 0xA0F5A0FFu);
    assert(SemicolonSyntax_penAt(syntax, 3u, header + strlen("<locale.h>") - 1u).fg
           == 0xA0F5A0FFu);
    assert(SemicolonSyntax_penAt(syntax, 4u, 0u).fg == 0xDCDCAAFFu);
    assert(SemicolonSyntax_penAt(syntax, 4u, 1u).fg == 0xDCDCAAFFu);
    assert(SemicolonSyntax_penAt(syntax, 5u, 9u).fg == 0xA0F5A0FFu);
    assert(SemicolonSyntax_penAt(syntax, 5u, 10u).fg == 0xA0F5A0FFu);
    SemicolonSyntax_free(syntax);
    assert(live == 0u);
}

static void testPressureAndFailure(void) {
    // Cross several growable span-directory boundaries, retaining old data while
    // injecting every bridge allocation failure stage. No host-wide exhaustion.
    char large[8192];
    size_t length = 0u;
    for (size_t index = 0u; index < 512u; ++index) {
        memcpy(large + length, "int x=1;\n", 9u);
        length += 9u;
    }
    const char *rows[] = {large};
    size_t lengths[] = {length};
    const char *old[] = {"return 1;"};
    size_t oldLengths[] = {9u};
    bool reachedSuccess = false;
    for (size_t stage = 0u; stage < 32u; ++stage) {
        SemicolonSyntax *syntax = SemicolonSyntax_zero();
        assert(syntax != nullptr);
        assert(SemicolonSyntax_setLines(syntax, old, oldLengths, 1u));
        untilFailure = stage;
        bool accepted = SemicolonSyntax_setLines(syntax, rows, lengths, 1u);
        untilFailure = SIZE_MAX;
        if (accepted) {
            assert(SemicolonSyntax_getSpanCount(syntax) == 2560u);
            reachedSuccess = true;
        } else {
            assert(SemicolonSyntax_penAt(syntax, 0u, 0u).fg == 0x008000FFu);
            assert(SemicolonSyntax_getSpanCount(syntax) == 3u);
            assert(SemicolonSyntax_exhaustionCount(syntax) == 1u);
            size_t huge[] = {SIZE_MAX};
            assert(!SemicolonSyntax_setLines(syntax, old, huge, 1u));
            assert(SemicolonSyntax_exhaustionCount(syntax) == 2u);
            assert(!SemicolonSyntax_setLines(syntax, old, oldLengths, SIZE_MAX));
            assert(SemicolonSyntax_exhaustionCount(syntax) == 3u);
            assert(SemicolonSyntax_setLines(syntax, rows, lengths, 1u));
            assert(SemicolonSyntax_getSpanCount(syntax) == 2560u);
        }
        SemicolonSyntax_free(syntax);
        assert(live == 0u);
        if (reachedSuccess)
            break;
    }
    assert(reachedSuccess);
    untilFailure = 0u;
    assert(SemicolonSyntax() == nullptr);
    untilFailure = SIZE_MAX;
    assert(live == 0u);
}

int main(void) {
    testRows();
    assert(live == 0u);
    testRolesAndComments();
    testPressureAndFailure();
    assert(SemicolonSyntax_acceptsPath("main.c"));
    assert(SemicolonSyntax_acceptsPath("include/api.h"));
    assert(!SemicolonSyntax_acceptsPath("main.cpp"));
    assert(!SemicolonSyntax_acceptsPath("readme.md"));
    assert(!SemicolonSyntax_acceptsPath(nullptr));
    assert(SemicolonSyntax_getLineCount(nullptr) == 0u);
    assert(SemicolonSyntax_getSpanCount(nullptr) == 0u);
    assert(SemicolonSyntax_exhaustionCount(nullptr) == 0u);
    assert(SemicolonSyntax_penAt(nullptr, 0u, 0u).fg == 0u);
    assert(SemicolonSyntax_getPen(nullptr, SYNTAX_ROLE_KEYWORD).fg == 0u);
    TerminalProjection projection = SemicolonSyntax_toString(nullptr);
    assert(strcmp(projection.text, "nullptr") == 0);
    projection = SemicolonSyntax_toStringStruct(nullptr);
    assert(strcmp(projection.text, "nullptr") == 0);
    SemicolonSyntax_free(nullptr);
    return 0;
}
