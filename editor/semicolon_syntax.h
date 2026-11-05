#ifndef SEMICOLON_EDITOR_SEMICOLON_SYNTAX_H
#define SEMICOLON_EDITOR_SEMICOLON_SYNTAX_H

#include "ui/surface/terminal_canvas.h"
#include "syntax/role.h"

typedef struct SemicolonSyntax SemicolonSyntax;

SemicolonSyntax *SemicolonSyntax_0(void);
#define SemicolonSyntax() SemicolonSyntax_0()
#define SemicolonSyntax_zero() SemicolonSyntax_0()
void SemicolonSyntax_free(SemicolonSyntax *self);
// Cold copy/analysis. Lines exclude endings; normalized LF joins preserve line
// lexical context. Failure preserves the previous snapshot. Caller serialized.
bool SemicolonSyntax_setLines(SemicolonSyntax *self, const char *const *lines,
                              const size_t *lengths, size_t count);
// Language selection is an explicit initial C23-only host policy for .c/.h.
bool SemicolonSyntax_acceptsPath(const char *path);
// Configurable presentation. Classification comes from Language's conservative
// lexical highlight pass; the palette stays consumer-owned.
bool SemicolonSyntax_setPen(SemicolonSyntax *self, SyntaxRole role, TerminalPen pen);
TerminalPen SemicolonSyntax_getPen(const SemicolonSyntax *self, SyntaxRole role);
TerminalPen SemicolonSyntax_penAt(const SemicolonSyntax *self, size_t line, size_t byte);
size_t SemicolonSyntax_getLineCount(const SemicolonSyntax *self);
size_t SemicolonSyntax_getSpanCount(const SemicolonSyntax *self);
size_t SemicolonSyntax_exhaustionCount(const SemicolonSyntax *self);
TerminalProjection SemicolonSyntax_toString(const SemicolonSyntax *self);
TerminalProjection SemicolonSyntax_toStringStruct(const SemicolonSyntax *self);

#endif
