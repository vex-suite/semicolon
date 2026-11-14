#ifndef SEMICOLON_ICON_ICON_CATALOG_H
#define SEMICOLON_ICON_ICON_CATALOG_H

#include "vex.h"

typedef struct SemicolonIconEntry {
    const char *format;
    const char *extensions;
    u32 glyph;
    u32 color;
} SemicolonIconEntry;

typedef struct SemicolonNamedIcon {
    const char *name;
    const char *description;
    u32 glyph;
    u32 color;
} SemicolonNamedIcon;

static const SemicolonNamedIcon SEMICOLON_NAMED_ICONS[] = {
    {"LICENSE", "license and legal notices", 0xf15b, 0xD6C98A},
    {"LICENCE", "license and legal notices", 0xf15b, 0xD6C98A},
    {"COPYING", "license and legal notices", 0xf15b, 0xD6C98A},
    {"NOTICE", "legal notices", 0xf15b, 0xD6C98A},
    {"Dockerfile", "container build file", 0xf308, 0x2496ED},
    {"Makefile", "build file", 0xf425, 0xC58A52},
    {"CMakeLists.txt", "build file", 0xf425, 0xC58A52},
    {"README", "project documentation", 0xe73e, 0x5CA9FF},
    {"README.md", "project documentation", 0xe73e, 0x5CA9FF},
    {".gitignore", "Git settings", 0xe702, 0xF05032},
    {".editorconfig", "editor settings", 0xe615, 0x9993B5},
    {".clang-format", "formatter settings", 0xe615, 0x9993B5},
    {".clangd", "language-tool settings", 0xe615, 0x9993B5},
    {"settings.json", "application settings", 0xf013, 0x9993B5},
    {".vscode", "editor settings directory", 0xe70c, 0x3399FF},
    {".idea", "IDE settings directory", 0xe7b5, 0xFE315D}
};

#define SEMICOLON_NAMED_ICON_COUNT \
    (sizeof SEMICOLON_NAMED_ICONS / sizeof SEMICOLON_NAMED_ICONS[0])

static const SemicolonNamedIcon SEMICOLON_UI_ICONS[] = {
    {"Settings", "preferences and configuration", 0xf013, 0x9993B5},
    {"Search", "find files and symbols", 0xf002, 0x5CA9FF},
    {"Files", "project explorer", 0xf07b, 0xC58A52},
    {"Build", "build and run action", 0xf04b, 0x79B86A},
    {"Diagnostics", "problems and diagnostics", 0xf071, 0xE5B84B},
    {"Terminal", "integrated terminal", 0xf120, 0x79B86A},
    {"Source control", "version control", 0xe702, 0xF05032},
    {"Extensions", "language and tool adapters", 0xf12e, 0xA88AF2}
};

#define SEMICOLON_UI_ICON_COUNT \
    (sizeof SEMICOLON_UI_ICONS / sizeof SEMICOLON_UI_ICONS[0])

static const SemicolonIconEntry SEMICOLON_ICON_CATALOG[] = {
    {"C", "c", 0xe61e, 0xA88AF2}, {"C headers", "h", 0xf1dc, 0xA88AF2},
    {"C++", "cc cpp cxx", 0xe61d, 0xC678DD}, {"C++ headers", "hh hpp hxx", 0xf1dc, 0xA88AF2},
    {"Objective-C", "m", 0xe61e, 0xA88AF2}, {"Objective-C++", "mm", 0xe61d, 0xC678DD},
    {"Java", "java", 0xe738, 0xF08A32}, {"Kotlin", "kt kts", 0xe634, 0xA97BFF},
    {"Java archive", "jar war ear", 0xe738, 0xF08A32},
    {"Python", "py pyw pyi", 0xe73c, 0x28C5B0}, {"Rust", "rs", 0xe7a8, 0xDEA584},
    {"Go", "go", 0xe627, 0x56B6C2}, {"JavaScript", "js mjs cjs", 0xe74e, 0xF0D746},
    {"TypeScript", "ts mts cts", 0xe628, 0x3399FF}, {"React JSX", "jsx", 0xe7ba, 0x3399FF},
    {"React TSX", "tsx", 0xe7ba, 0x3399FF}, {"Swift", "swift", 0xe755, 0xE99545},
    {"C#", "cs", 0xe648, 0x75C84D}, {"Ruby", "rb", 0xe791, 0xCC342D},
    {"Zig", "zig", 0xe6a9, 0xF7A41D}, {"Dart", "dart", 0xe798, 0x00B4AB},
    {"Elixir", "ex exs", 0xe62d, 0xA855F7}, {"Erlang", "erl hrl", 0xe7b1, 0xB83998},
    {"Lua", "lua", 0xe620, 0x6C7EB7}, {"Perl", "pl pm", 0xe769, 0x39457E},
    {"R", "r R rmd", 0xe68a, 0x769BCE}, {"Fortran", "f f90 f95 for f03 f08", 0xe69e, 0xAB78E3},
    {"PHP", "php", 0xe73d, 0x777BB4}, {"Shell", "sh bash zsh fish", 0xf489, 0x79B86A},
    {"HTML", "html htm", 0xe736, 0xE99545}, {"CSS", "css scss sass less", 0xe749, 0xB18BDD},
    {"JSON", "json jsonc", 0xe60b, 0xC792EA}, {"YAML", "yaml yml", 0xe615, 0x9993B5},
    {"TOML", "toml", 0xe615, 0x9993B5}, {"XML", "xml xsd xslt", 0xf121, 0xE99545},
    {"Markdown", "md markdown mdown", 0xe73e, 0x5CA9FF}, {"Plain text", "txt text log", 0xf15c, 0xA0A0AA},
    {"SQL", "sql", 0xf1c0, 0xA0A0AA}, {"Database", "db vexdb sqlite sqlite3", 0xf1c0, 0xA0A0AA},
    {"CMake", "cmake", 0xf425, 0xC58A52}, {"Make", "mk", 0xf425, 0xC58A52},
    {"macOS application bundle", "app", 0xf179, 0xA0A0AA},
    {"Windows executable", "exe msi", 0xf489, 0xA0A0AA},
    {"Linux executable", "bin run deb rpm", 0xf489, 0xA0A0AA},
    {"Disk image", "dmg iso", 0xf019, 0xA0A0AA},
    {"Image", "png jpg jpeg gif svg webp bmp ico tif tiff", 0xf1c5, 0xA0A0AA},
    {"Audio", "mp3 wav flac ogg aac m4a", 0xf001, 0xA0A0AA},
    {"Video", "mp4 mov mkv webm avi", 0xf03d, 0xA0A0AA},
    {"Archive", "zip tar gz bz2 xz 7z rar", 0xf410, 0xA0A0AA},
    {"PDF", "pdf", 0xf1c1, 0xE05D5D}, {"WebAssembly", "wasm", 0xe6a1, 0x654FF0}
};

#define SEMICOLON_ICON_CATALOG_COUNT \
    (sizeof SEMICOLON_ICON_CATALOG / sizeof SEMICOLON_ICON_CATALOG[0])

#endif
