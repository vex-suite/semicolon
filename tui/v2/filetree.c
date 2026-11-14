#include "vex.h"
#include "tui/v2/filetree.h"
#include "tui/v2/palette.h"
#include "icon/icon_catalog.h"
#include "nio/mem.h"
#include "oop/oop.h"
#include "format/terminal_style.h"

#include <dirent.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

;;DEFINITION
/** Semicolon v2 project tree and filesystem rail component.
 * Manages directory scanning, selective lazy expansion, .gitignore filtering,
 * icon mapping and Determinant tree view synchronization. */
;;OVERVIEW
/** ========================================================================
 * CLASS: SemicolonRail (suite/projects/semicolon/src/tui/v2/filetree.c)
 * OWNS: file tree nodes, directory entries, gitignore patterns and rail row pool.
 * BORROWS: SemicolonState, Determinant TerminalTreeView and TerminalCanvas.
 * CONVENTIONS: Descriptive cameled identifiers, zero -> operators, Memory_alloc.
 * ======================================================================== */

typedef struct SemicolonDirEntry {
    char entryName[256];
    bool isFolder;
} SemicolonDirEntry;

static bool SemicolonRail_isVisibleDotEntry(const char *entryName, bool isFolder) {
    if (isFolder && strcmp(entryName, ".github") == 0)
        return true;
    if (!isFolder && (strcmp(entryName, ".gitignore") == 0 || strcmp(entryName, ".gitkeep") == 0))
        return true;
    return false;
}

static unsigned SemicolonRail_getFileSortGroup(const char *fileName) {
    const char *extensionPointer = strrchr(fileName, '.');
    if (extensionPointer != nullptr && strcasecmp(extensionPointer, ".c") == 0)
        return 0u;
    if (extensionPointer != nullptr && strcasecmp(extensionPointer, ".h") == 0)
        return 1u;
    return 2u;
}

static int SemicolonRail_compareDirEntries(const_any leftPointer, const_any rightPointer) {
    const SemicolonDirEntry *firstEntry = (const SemicolonDirEntry*) leftPointer;
    const SemicolonDirEntry *secondEntry = (const SemicolonDirEntry*) rightPointer;
    if ((*firstEntry).isFolder != (*secondEntry).isFolder)
        return (*firstEntry).isFolder ? -1 : 1;
    if (!(*firstEntry).isFolder) {
        unsigned firstGroup = SemicolonRail_getFileSortGroup((*firstEntry).entryName);
        unsigned secondGroup = SemicolonRail_getFileSortGroup((*secondEntry).entryName);
        if (firstGroup != secondGroup)
            return firstGroup < secondGroup ? -1 : 1;
    }
    int foldedResult = strcasecmp((*firstEntry).entryName, (*secondEntry).entryName);
    return foldedResult != 0 ? foldedResult : strcmp((*firstEntry).entryName, (*secondEntry).entryName);
}

const char *SemicolonRail_getFileIcon(const char *fileName, bool isNerdIcons) {
    if (!isNerdIcons)
        return "-";
    for (bytes namedIndex = 0u; namedIndex < SEMICOLON_NAMED_ICON_COUNT; ++namedIndex) {
        if (strcasecmp(fileName, SEMICOLON_NAMED_ICONS[namedIndex].name) == 0)
            return SemicolonUtil_iconUtf8(SEMICOLON_NAMED_ICONS[namedIndex].glyph);
    }
    const char *extensionPointer = strrchr(fileName, '.');
    if (extensionPointer == nullptr)
        return "\uf15c";
    const char *suffixPointer = extensionPointer[0] == '.' ? extensionPointer + 1u : extensionPointer;
    for (bytes catalogIndex = 0u; catalogIndex < SEMICOLON_ICON_CATALOG_COUNT; ++catalogIndex) {
        const char *cursorPointer = SEMICOLON_ICON_CATALOG[catalogIndex].extensions;
        while (*cursorPointer != '\0') {
            while (*cursorPointer == ' ')
                ++cursorPointer;
            const char *spacePointer = strchr(cursorPointer, ' ');
            bytes tokenLength = spacePointer == nullptr ? strlen(cursorPointer)
                                                         : (bytes) (spacePointer - cursorPointer);
            if (strlen(suffixPointer) == tokenLength && strncasecmp(cursorPointer, suffixPointer, tokenLength) == 0)
                return SemicolonUtil_iconUtf8(SEMICOLON_ICON_CATALOG[catalogIndex].glyph);
            cursorPointer = spacePointer == nullptr ? cursorPointer + tokenLength : spacePointer;
        }
    }
    return "\uf15c";
}

u32 SemicolonRail_getIconColor(u32 glyphCodepoint) {
    if (glyphCodepoint == 0xf07bu || glyphCodepoint == 0xf07cu)
        return SEMICOLON_V2_FOLDER; // warm golden brown fried chicken color
    for (bytes namedIndex = 0u; namedIndex < SEMICOLON_NAMED_ICON_COUNT; ++namedIndex) {
        if (SEMICOLON_NAMED_ICONS[namedIndex].glyph == glyphCodepoint)
            return (SEMICOLON_NAMED_ICONS[namedIndex].color << 8u) | 0xFFu;
    }
    for (bytes uiIndex = 0u; uiIndex < SEMICOLON_UI_ICON_COUNT; ++uiIndex) {
        if (SEMICOLON_UI_ICONS[uiIndex].glyph == glyphCodepoint)
            return (SEMICOLON_UI_ICONS[uiIndex].color << 8u) | 0xFFu;
    }
    for (bytes catalogIndex = 0u; catalogIndex < SEMICOLON_ICON_CATALOG_COUNT; ++catalogIndex) {
        if (SEMICOLON_ICON_CATALOG[catalogIndex].glyph == glyphCodepoint)
            return (SEMICOLON_ICON_CATALOG[catalogIndex].color << 8u) | 0xFFu;
    }
    switch (glyphCodepoint) {
        case 0xf07bu: case 0xf07cu: return SEMICOLON_V2_FOLDER; // folder golden brown
        case 0xe738u: return 0xF08A32FFu; // Java orange
        case 0xe74eu: return 0xF0D746FFu; // JavaScript yellow
        case 0xe648u: return 0x75C84DFFu; // C# green
        case 0xe73cu: return 0x28C5B0FFu; // Python turquoise
        case 0xe628u: case 0xe7bau: return 0x3399FFFFu; // TypeScript/React blue
        case 0xe68au: return 0x769BCEFFu; // R gray-blue
        case 0xe61eu: case 0xf1dcu: return 0xA88AF2FFu; // C/header gray-purple
        case 0xe61du: return 0xC678DDFFu; // C++ purple
        case 0xe69eu: return 0xAB78E3FFu; // Fortran purple
        case 0xe60bu: return 0xC792EAFFu; // JSON lavender
        case 0xe73eu: return 0x5CA9FFFFu; // Markdown blue
        case 0xe627u: return 0x56B6C2FFu; // Go cyan
        case 0xe7a8u: return 0xDEA584FFu; // Rust warm neutral
        case 0xe755u: case 0xe736u: return 0xE99545FFu; // Swift/HTML orange
        case 0xe749u: return 0xB18BDDFFu; // CSS purple
        case 0xf489u: return 0x79B86AFFu; // shell green
        case 0xe615u: return 0x9993B5FFu; // configuration
        case 0xf425u: return 0xC58A52FFu; // build
        default: return 0xA0A0AAFFu;      // generic file
    }
}

static fn SemicolonRail_clearGitignore(SemicolonState *semicolonState) {
    if (semicolonState == nullptr)
        return;
    for (bytes patternIndex = 0u; patternIndex < (*semicolonState).ignorePatternCount; ++patternIndex) {
        if ((*semicolonState).ignorePatterns[patternIndex].patternText != nullptr)
            Memory_free((*semicolonState).ignorePatterns[patternIndex].patternText);
    }
    (*semicolonState).ignorePatternCount = 0u;
}

static bool SemicolonRail_addGitignorePattern(SemicolonState *semicolonState, const char *lineText) {
    while (*lineText == ' ' || *lineText == '\t')
        ++lineText;
    bytes lineLength = strlen(lineText);
    while (lineLength != 0u && (lineText[lineLength - 1u] == ' ' || lineText[lineLength - 1u] == '\t'))
        --lineLength;
    if (lineLength == 0u || lineText[0] == '#')
        return true;
    bool isNegated = lineText[0] == '!';
    if (isNegated) {
        ++lineText;
        --lineLength;
    }
    if (lineLength == 0u)
        return true;
    bool isDirectoryOnly = lineText[lineLength - 1u] == '/';
    if (isDirectoryOnly)
        --lineLength;
    bytes startIndex = lineText[0] == '/' ? 1u : 0u;
    bool isAnchored = startIndex != 0u;
    bool hasSlash = memchr(lineText + startIndex, '/', lineLength - startIndex) != nullptr;
    if (lineLength <= startIndex)
        return true;

    if ((*semicolonState).ignorePatternCount == (*semicolonState).ignorePatternCapacity) {
        bytes nextCapacity = (*semicolonState).ignorePatternCapacity == 0u ? 32u
                                                                           : (*semicolonState).ignorePatternCapacity * 2u;
        SemicolonIgnorePattern *grownArray = (SemicolonIgnorePattern*) Memory_alloc(
            TYPE_BYTE_ARRAY, nextCapacity * sizeof(SemicolonIgnorePattern));
        if (grownArray == nullptr)
            return false;
        if ((*semicolonState).ignorePatternCount > 0u) {
            memcpy(grownArray, (*semicolonState).ignorePatterns,
                   (*semicolonState).ignorePatternCount * sizeof(SemicolonIgnorePattern));
            Memory_free((*semicolonState).ignorePatterns);
        }
        (*semicolonState).ignorePatterns = grownArray;
        (*semicolonState).ignorePatternCapacity = nextCapacity;
    }

    char *allocatedPattern = (char*) Memory_alloc(TYPE_BYTE_ARRAY, lineLength - startIndex + 1u);
    if (allocatedPattern == nullptr)
        return false;
    memcpy(allocatedPattern, lineText + startIndex, lineLength - startIndex);
    allocatedPattern[lineLength - startIndex] = '\0';

    (*semicolonState).ignorePatterns[(*semicolonState).ignorePatternCount] = (SemicolonIgnorePattern) {
        .patternText = allocatedPattern,
        .isNegated = isNegated,
        .isDirectoryOnly = isDirectoryOnly,
        .isAnchored = isAnchored,
        .hasSlash = hasSlash
    };
    (*semicolonState).ignorePatternCount++;
    return true;
}

fn SemicolonRail_reloadGitignore(SemicolonState *semicolonState) {
    if (semicolonState == nullptr || (*semicolonState).railRootDirectory[0] == '\0')
        return;
    char gitignorePath[SEMICOLON_V2_MAX_PATH];
    (fn) snprintf(gitignorePath, sizeof gitignorePath, "%s/.gitignore", (*semicolonState).railRootDirectory);

    struct stat fileStat;
    bool fileExists = stat(gitignorePath, &fileStat) == 0 && S_ISREG(fileStat.st_mode);
    if (fileExists && (*semicolonState).isIgnoreStatValid
        && fileStat.st_mtime == (*semicolonState).ignoreFileModifiedTime.tv_sec)
        return;

    SemicolonRail_clearGitignore(semicolonState);
    (*semicolonState).isIgnoreStatValid = fileExists;
    if (!fileExists)
        return;

    (*semicolonState).ignoreFileModifiedTime.tv_sec = fileStat.st_mtime;
    FILE *filePointer = fopen(gitignorePath, "rb");
    if (filePointer == nullptr)
        return;

    char lineBuffer[1024];
    while (fgets(lineBuffer, sizeof lineBuffer, filePointer) != nullptr) {
        bytes lineLength = strlen(lineBuffer);
        while (lineLength != 0u && (lineBuffer[lineLength - 1u] == '\n' || lineBuffer[lineLength - 1u] == '\r'))
            lineBuffer[--lineLength] = '\0';
        if (!SemicolonRail_addGitignorePattern(semicolonState, lineBuffer))
            break;
    }
    (fn) fclose(filePointer);
}

static bool SemicolonRail_patternMatches(const SemicolonIgnorePattern *patternPointer, const char *relativePath) {
    if ((*patternPointer).isDirectoryOnly) {
        const char *slashPointer = relativePath;
        while ((slashPointer = strchr(slashPointer, '/')) != nullptr) {
            bytes prefixLength = (bytes) (slashPointer - relativePath);
            char prefixBuffer[1024];
            if (prefixLength < sizeof prefixBuffer) {
                memcpy(prefixBuffer, relativePath, prefixLength);
                prefixBuffer[prefixLength] = '\0';
                if (fnmatch((*patternPointer).patternText, prefixBuffer, FNM_PATHNAME) == 0)
                    return true;
            }
            ++slashPointer;
        }
    }
    if ((*patternPointer).isAnchored || (*patternPointer).hasSlash)
        return fnmatch((*patternPointer).patternText, relativePath, FNM_PATHNAME) == 0;
    const char *cursorPointer = relativePath;
    while (*cursorPointer != '\0') {
        const char *slashPointer = strchr(cursorPointer, '/');
        bytes tokenLength = slashPointer == nullptr ? strlen(cursorPointer)
                                                    : (bytes) (slashPointer - cursorPointer);
        char componentBuffer[256];
        if (tokenLength < sizeof componentBuffer) {
            memcpy(componentBuffer, cursorPointer, tokenLength);
            componentBuffer[tokenLength] = '\0';
            if (fnmatch((*patternPointer).patternText, componentBuffer, FNM_PATHNAME) == 0)
                return true;
        }
        if (slashPointer == nullptr)
            break;
        cursorPointer = slashPointer + 1u;
    }
    return fnmatch((*patternPointer).patternText, relativePath, FNM_PATHNAME) == 0;
}

static bool SemicolonRail_isEntryIgnored(SemicolonState *semicolonState, const SemicolonRailEntry *entryPointer) {
    if (semicolonState == nullptr || entryPointer == nullptr)
        return false;
    bytes rootLength = strlen((*semicolonState).railRootDirectory);
    if (strncmp((*entryPointer).entryPath, (*semicolonState).railRootDirectory, rootLength) != 0
        || (*entryPointer).entryPath[rootLength] != '/')
        return false;
    const char *relativePath = (*entryPointer).entryPath + rootLength + 1u;
    bool isIgnored = false;
    for (bytes patternIndex = 0u; patternIndex < (*semicolonState).ignorePatternCount; ++patternIndex) {
        SemicolonIgnorePattern *pattern = &(*semicolonState).ignorePatterns[patternIndex];
        if (!SemicolonRail_patternMatches(pattern, relativePath))
            continue;
        if ((*pattern).isDirectoryOnly && !(*entryPointer).isFolder && !strchr(relativePath, '/'))
            continue;
        isIgnored = !(*pattern).isNegated;
    }
    return isIgnored;
}

static SemicolonRailEntry *SemicolonRail_entryForId(SemicolonState *semicolonState, TerminalTreeNodeId nodeId) {
    if (semicolonState == nullptr || nodeId == TERMINAL_TREE_NO_NODE
        || (bytes) nodeId > (*semicolonState).railEntryCount)
        return nullptr;
    return &(*semicolonState).railEntries[(bytes) nodeId - 1u];
}

static bool SemicolonRail_reserveEntry(SemicolonState *semicolonState) {
    if ((*semicolonState).railEntryCount < (*semicolonState).railEntryCapacity)
        return true;
    bytes nextCapacity = (*semicolonState).railEntryCapacity == 0u ? 64u
                                                                   : (*semicolonState).railEntryCapacity * 2u;
    SemicolonRailEntry *grownArray = (SemicolonRailEntry*) Memory_alloc(
        TYPE_BYTE_ARRAY, nextCapacity * sizeof(SemicolonRailEntry));
    if (grownArray == nullptr)
        return false;
    if ((*semicolonState).railEntryCount > 0u) {
        memcpy(grownArray, (*semicolonState).railEntries,
               (*semicolonState).railEntryCount * sizeof(SemicolonRailEntry));
        Memory_free((*semicolonState).railEntries);
    }
    (*semicolonState).railEntries = grownArray;
    (*semicolonState).railEntryCapacity = nextCapacity;
    return true;
}

static TerminalTreeNodeId SemicolonRail_addNode(SemicolonState *semicolonState, TerminalTreeNodeId parentNodeId,
                                                const char *entryName, const char *entryPath, bool isFolder) {
    if (semicolonState == nullptr || (*semicolonState).treeView == nullptr || !SemicolonRail_reserveEntry(semicolonState))
        return TERMINAL_TREE_NO_NODE;
    TerminalTreeNodeId nodeId = TerminalTreeView_addChild((*semicolonState).treeView, parentNodeId, "", isFolder);
    if (nodeId == TERMINAL_TREE_NO_NODE)
        return TERMINAL_TREE_NO_NODE;

    SemicolonRailEntry *entry = &(*semicolonState).railEntries[(*semicolonState).railEntryCount];
    (fn) snprintf((*entry).entryLabel, sizeof (*entry).entryLabel, "%s", entryName);
    (fn) snprintf((*entry).entryPath, sizeof (*entry).entryPath, "%s", entryPath);
    (*entry).parentNodeId = parentNodeId;
    (*entry).isFolder = isFolder;
    (*entry).isLoaded = false;
    (*semicolonState).railEntryCount++;

    (fn) TerminalTreeView_setLabel((*semicolonState).treeView, nodeId, (*entry).entryLabel);
    return nodeId;
}

bool SemicolonRail_readChildren(SemicolonState *semicolonState, TerminalTreeNodeId parentNodeId) {
    SemicolonRailEntry *parentEntry = SemicolonRail_entryForId(semicolonState, parentNodeId);
    if (parentEntry == nullptr || (*parentEntry).isLoaded)
        return true;
    (*parentEntry).isLoaded = true;

    DIR *dirHandle = opendir((*parentEntry).entryPath);
    if (dirHandle == nullptr)
        return false;

    SemicolonDirEntry foundEntries[512];
    bytes foundCount = 0u;
    struct dirent *dirItem = nullptr;
    while ((dirItem = readdir(dirHandle)) != nullptr && foundCount < 512u) {
        if (strcmp((*dirItem).d_name, ".") == 0 || strcmp((*dirItem).d_name, "..") == 0)
            continue;
        char childPath[SEMICOLON_V2_MAX_PATH];
        (fn) snprintf(childPath, sizeof childPath, "%s/%s", (*parentEntry).entryPath, (*dirItem).d_name);
        struct stat fileInfo;
        if (stat(childPath, &fileInfo) != 0)
            continue;
        bool isFolder = S_ISDIR(fileInfo.st_mode);
        if ((*dirItem).d_name[0] == '.' && !SemicolonRail_isVisibleDotEntry((*dirItem).d_name, isFolder))
            continue;
        if (!isFolder && !S_ISREG(fileInfo.st_mode))
            continue;

        (fn) snprintf(foundEntries[foundCount].entryName, sizeof foundEntries[foundCount].entryName,
                        "%s", (*dirItem).d_name);
        foundEntries[foundCount].isFolder = isFolder;
        foundCount++;
    }
    (fn) closedir(dirHandle);

    if (foundCount > 1u)
        qsort(foundEntries, foundCount, sizeof(SemicolonDirEntry), SemicolonRail_compareDirEntries);

    char currentParentPath[SEMICOLON_V2_MAX_PATH];
    (fn) snprintf(currentParentPath, sizeof currentParentPath, "%s", (*parentEntry).entryPath);

    for (bytes entryIndex = 0u; entryIndex < foundCount; ++entryIndex) {
        char childPath[SEMICOLON_V2_MAX_PATH];
        (fn) snprintf(childPath, sizeof childPath, "%s/%s", currentParentPath, foundEntries[entryIndex].entryName);
        (fn) SemicolonRail_addNode(semicolonState, parentNodeId, foundEntries[entryIndex].entryName,
                                     childPath, foundEntries[entryIndex].isFolder);
    }
    return true;
}

static TerminalTreeNodeId SemicolonRail_findChild(SemicolonState *semicolonState, TerminalTreeNodeId parentNodeId,
                                                  const char *childName) {
    for (bytes entryIndex = 0u; entryIndex < (*semicolonState).railEntryCount; ++entryIndex) {
        if ((*semicolonState).railEntries[entryIndex].parentNodeId == parentNodeId
            && strcmp((*semicolonState).railEntries[entryIndex].entryLabel, childName) == 0)
            return (TerminalTreeNodeId) (entryIndex + 1u);
    }
    return TERMINAL_TREE_NO_NODE;
}

bool SemicolonRail_scanDirectory(SemicolonState *semicolonState, const char *rootDirectory) {
    if (semicolonState == nullptr || rootDirectory == nullptr || *rootDirectory == '\0')
        return false;
    (fn) snprintf((*semicolonState).railRootDirectory, sizeof (*semicolonState).railRootDirectory, "%s", rootDirectory);

    if ((*semicolonState).treeView != nullptr)
        TerminalTreeView_free((*semicolonState).treeView);

    (*semicolonState).treeView = TerminalTreeView();
    if ((*semicolonState).treeView == nullptr)
        return false;

    const char *slashPointer = strrchr(rootDirectory, '/');
    const char *baseName = slashPointer == nullptr ? rootDirectory : slashPointer + 1u;
    if (*baseName == '\0')
        baseName = "/";

    (*semicolonState).railRootNodeId = SemicolonRail_addNode(
        semicolonState, TERMINAL_TREE_NO_NODE, baseName, rootDirectory, true);
    if ((*semicolonState).railRootNodeId == TERMINAL_TREE_NO_NODE)
        return false;

    (fn) SemicolonRail_readChildren(semicolonState, (*semicolonState).railRootNodeId);
    (fn) TerminalTreeView_setExpanded((*semicolonState).treeView, (*semicolonState).railRootNodeId, true);
    return true;
}

bool SemicolonRail_revealPath(SemicolonState *semicolonState, const char *filePath) {
    if (semicolonState == nullptr || filePath == nullptr || (*semicolonState).treeView == nullptr
        || (*semicolonState).railRootNodeId == TERMINAL_TREE_NO_NODE)
        return false;
    bytes rootLength = strlen((*semicolonState).railRootDirectory);
    if (rootLength == 0u || strncmp(filePath, (*semicolonState).railRootDirectory, rootLength) != 0
        || filePath[rootLength] != '/')
        return false;

    TerminalTreeNodeId currentNodeId = (*semicolonState).railRootNodeId;
    const char *cursorPointer = filePath + rootLength + 1u;
    while (*cursorPointer != '\0') {
        char componentBuffer[256];
        bytes tokenLength = 0u;
        while (cursorPointer[tokenLength] != '\0' && cursorPointer[tokenLength] != '/'
               && tokenLength + 1u < sizeof componentBuffer)
            ++tokenLength;
        memcpy(componentBuffer, cursorPointer, tokenLength);
        componentBuffer[tokenLength] = '\0';
        cursorPointer += tokenLength;
        if (*cursorPointer == '/')
            ++cursorPointer;

        (fn) SemicolonRail_readChildren(semicolonState, currentNodeId);
        TerminalTreeNodeId childNodeId = SemicolonRail_findChild(semicolonState, currentNodeId, componentBuffer);
        if (childNodeId == TERMINAL_TREE_NO_NODE)
            return false;
        currentNodeId = childNodeId;
        if (*cursorPointer != '\0')
            (fn) TerminalTreeView_setExpanded((*semicolonState).treeView, currentNodeId, true);
    }
    (fn) TerminalTreeView_setSelected((*semicolonState).treeView, currentNodeId);
    return true;
}

bytes SemicolonRail_visibleRows(bytes terminalHeight) {
    return terminalHeight > 2u ? terminalHeight - 2u : 0u;
}

static bytes SemicolonRail_calculateIndent(TerminalTreeRow treeRow) {
    bytes deepestLevel = 6u;
    bytes depthLevel = treeRow.depth + 1u;
    return (depthLevel > deepestLevel ? deepestLevel : depthLevel) * 2u;
}

fn SemicolonRail_syncVisibleRows(SemicolonState *semicolonState, bytes visibleRowCount) {
    if (semicolonState == nullptr || (*semicolonState).treeView == nullptr)
        return;
    SemicolonRail_reloadGitignore(semicolonState);

    (fn) TerminalTreeView_setViewportRows((*semicolonState).treeView, visibleRowCount + 1u);
    bytes firstRowIndex = TerminalTreeView_scrollRow((*semicolonState).treeView);
    bytes totalVisibleCount = TerminalTreeView_visibleCount((*semicolonState).treeView);

    for (bytes rowIndex = 0u; rowIndex < (*semicolonState).railRowCount; ++rowIndex) {
        TerminalText *rowTextView = (*semicolonState).railRowTexts[rowIndex];
        if (rowTextView == nullptr)
            continue;
        if (firstRowIndex + rowIndex + 1u >= totalVisibleCount) {
            (fn) TerminalText_setText(rowTextView, "", 0u);
            (fn) TerminalText_setBackgroundColor(rowTextView, TERMINAL_COLOR_DEFAULT);
            continue;
        }

        TerminalTreeRow record = TerminalTreeView_rowAt((*semicolonState).treeView, firstRowIndex + rowIndex + 1u);
        bytes indentSpaces = SemicolonRail_calculateIndent(record);
        char formattedBuffer[512];
        bytes writtenBytes = 0u;
        for (bytes spaceIndex = 0u; spaceIndex < indentSpaces && writtenBytes + 2u < sizeof formattedBuffer; ++spaceIndex)
            formattedBuffer[writtenBytes++] = ' ';
        formattedBuffer[writtenBytes] = '\0';

        if (record.folder) {
            (fn) snprintf(formattedBuffer + writtenBytes, sizeof formattedBuffer - writtenBytes,
                            "%s %s %s", record.expanded ? "⌄" : "›",
                            (*semicolonState).isNerdIcons ? (record.expanded ? "\uf07c" : "\uf07b") : "[]",
                            record.label);
        } else {
            (fn) snprintf(formattedBuffer + writtenBytes, sizeof formattedBuffer - writtenBytes,
                            "  %s %s", SemicolonRail_getFileIcon(record.label, (*semicolonState).isNerdIcons),
                            record.label);
        }

        bytes availableCells = (*semicolonState).railPanelWidth > 2u ? (*semicolonState).railPanelWidth - 2u : 0u;
        bytes rowLength = strlen(formattedBuffer);
        if (SemicolonUtil_cellsOf(formattedBuffer, rowLength) > availableCells) {
            bytes keepCells = availableCells > 1u ? availableCells - 1u : 0u;
            rowLength = SemicolonUtil_bytesForCells(formattedBuffer, rowLength, keepCells);
            if (rowLength + 3u < sizeof formattedBuffer) {
                memcpy(formattedBuffer + rowLength, "…", 3u);
                rowLength += 3u;
                formattedBuffer[rowLength] = '\0';
            }
        }

        bytes currentCells = SemicolonUtil_cellsOf(formattedBuffer, rowLength);
        while (currentCells < (*semicolonState).railPanelWidth && rowLength + 1u < sizeof formattedBuffer) {
            formattedBuffer[rowLength++] = ' ';
            ++currentCells;
        }
        formattedBuffer[rowLength] = '\0';

        memcpy((*semicolonState).railRowBuffers[rowIndex], formattedBuffer, rowLength + 1u);
        (fn) TerminalText_setText(rowTextView, (*semicolonState).railRowBuffers[rowIndex], rowLength);
        (fn) TerminalText_setForegroundColor(rowTextView, SEMICOLON_V2_TEXT);
        (fn) TerminalText_setBackgroundColor(rowTextView, record.selected ? SEMICOLON_V2_SELECTION
                                                                            : TERMINAL_COLOR_DEFAULT);
    }
}

fn SemicolonRail_paintColors(SemicolonState *semicolonState, TerminalCanvas *terminalCanvas) {
    if (semicolonState == nullptr || terminalCanvas == nullptr || (*semicolonState).treeView == nullptr)
        return;
    bytes canvasWidth = TerminalCanvas_getWidth(terminalCanvas);
    bytes canvasHeight = TerminalCanvas_getHeight(terminalCanvas);
    bytes railEndColumn = (*semicolonState).railPanelWidth < canvasWidth ? (*semicolonState).railPanelWidth
                                                                          : canvasWidth;

    if ((*semicolonState).isRailVisible && railEndColumn > 0u
        && TerminalTreeView_visibleCount((*semicolonState).treeView) > 0u) {
        TerminalTreeRow rootRecord = TerminalTreeView_rowAt((*semicolonState).treeView, 0u);
        char rootTitle[256];
        char rootClipped[256];
        (fn) snprintf(rootTitle, sizeof rootTitle, " %s %s %s",
                        rootRecord.expanded ? "⌄" : "›",
                        (*semicolonState).isNerdIcons ? (rootRecord.expanded ? "\uf07c" : "\uf07b") : "[]",
                        rootRecord.label);
        SemicolonUtil_copyCellsEllipsis(rootTitle, railEndColumn / 2u, rootClipped, sizeof rootClipped);
        bytes titleCells = SemicolonUtil_cellsOf(rootClipped, strlen(rootClipped));
        TerminalCanvas_text(terminalCanvas, 0u, 0u, rootClipped, strlen(rootClipped),
                            TerminalPen_3(TERMINAL_STYLE_NONE, SEMICOLON_V2_TEXT, TERMINAL_COLOR_DEFAULT));

        if ((*semicolonState).isNerdIcons && railEndColumn > 3u && (*terminalCanvas).fg != nullptr) {
            u32 glyphCodepoint = TerminalCanvas_glyphAt(terminalCanvas, 3u, 0u);
            (*terminalCanvas).fg[3u] = SemicolonRail_getIconColor(glyphCodepoint);
        }

        if ((*semicolonState).railLocationPath[0] != '\0' && railEndColumn > titleCells + 1u) {
            char locationClipped[SEMICOLON_V2_MAX_PATH];
            SemicolonUtil_copyCellsEllipsis((*semicolonState).railLocationPath, railEndColumn - titleCells - 1u,
                                           locationClipped, sizeof locationClipped);
            TerminalCanvas_text(terminalCanvas, titleCells + 1u, 0u, locationClipped, strlen(locationClipped),
                                TerminalPen_3(TERMINAL_STYLE_NONE, TERMINAL_RGB(0x88, 0x88, 0x88),
                                              TERMINAL_COLOR_DEFAULT));
        }
    }

    bytes firstRowIndex = TerminalTreeView_scrollRow((*semicolonState).treeView);
    bytes totalVisibleCount = TerminalTreeView_visibleCount((*semicolonState).treeView);

    for (bytes rowIndex = 0u; rowIndex < (*semicolonState).railRowCount && rowIndex + 2u < canvasHeight
                               && firstRowIndex + rowIndex + 1u < totalVisibleCount; ++rowIndex) {
        TerminalTreeRow record = TerminalTreeView_rowAt((*semicolonState).treeView, firstRowIndex + rowIndex + 1u);
        bytes iconXCoordinate = SemicolonRail_calculateIndent(record) + 2u;
        bytes yCoordinate = rowIndex + 1u;
        SemicolonRailEntry *entry = SemicolonRail_entryForId(semicolonState, record.id);

        if ((*semicolonState).isNerdIcons && iconXCoordinate < railEndColumn && (*terminalCanvas).fg != nullptr) {
            u32 glyphCodepoint = TerminalCanvas_glyphAt(terminalCanvas, iconXCoordinate, yCoordinate);
            (*terminalCanvas).fg[yCoordinate * canvasWidth + iconXCoordinate] =
                SemicolonRail_getIconColor(glyphCodepoint);
        }

        if (SemicolonRail_isEntryIgnored(semicolonState, entry) && (*terminalCanvas).fg != nullptr) {
            bytes nameXCoordinate = iconXCoordinate + 2u;
            for (bytes xIndex = nameXCoordinate; xIndex < railEndColumn; ++xIndex)
                (*terminalCanvas).fg[yCoordinate * canvasWidth + xIndex] = SEMICOLON_V2_IGNORED;
        }
    }

    if ((*semicolonState).isNerdIcons && (*terminalCanvas).fg != nullptr) {
        for (bytes xCoordinate = (*semicolonState).railPanelWidth; xCoordinate < canvasWidth; ++xCoordinate) {
            u32 glyphCodepoint = TerminalCanvas_glyphAt(terminalCanvas, xCoordinate, 0u);
            if (glyphCodepoint >= 0xE000u && glyphCodepoint <= 0xF8FFu)
                (*terminalCanvas).fg[xCoordinate] = SemicolonRail_getIconColor(glyphCodepoint);
        }
    }
}

fn SemicolonRail_handlePointerClick(SemicolonState *semicolonState, bytes rowCoordinate) {
    if (semicolonState == nullptr || (*semicolonState).treeView == nullptr)
        return;

    TerminalTreeNodeId clickedNodeId = (rowCoordinate == 0u)
        ? (*semicolonState).railRootNodeId
        : TerminalTreeView_nodeAtOffset((*semicolonState).treeView, rowCoordinate);
    if (clickedNodeId == TERMINAL_TREE_NO_NODE)
        return;

    if (TerminalTreeView_isFolder((*semicolonState).treeView, clickedNodeId)) {
        bool isNowExpanded = !TerminalTreeView_isExpanded((*semicolonState).treeView, clickedNodeId);
        if (isNowExpanded)
            (fn) SemicolonRail_readChildren(semicolonState, clickedNodeId);
        (fn) TerminalTreeView_setExpanded((*semicolonState).treeView, clickedNodeId, isNowExpanded);
    } else {
        (fn) TerminalTreeView_setSelected((*semicolonState).treeView, clickedNodeId);
        SemicolonRailEntry *entry = SemicolonRail_entryForId(semicolonState, clickedNodeId);
        if (entry != nullptr) {
            (fn) SemicolonState_openFile(semicolonState, (*entry).entryPath);
        }
    }
}

TerminalPanel *createRailPanel(SemicolonState *semicolonState) {
    TerminalPanel *railPanel = TerminalPanel();
    if (railPanel == nullptr)
        return nullptr;
    if (!TerminalPanel_setLayout(railPanel, TERMINAL_PANEL_FLEX)) {
        TerminalPanel_free(railPanel);
        return nullptr;
    }

    for (bytes rowIndex = 0u; rowIndex < SEMICOLON_V2_MAX_ROWS; ++rowIndex) {
        TerminalText *rowTextView = TerminalText("");
        if (rowTextView == nullptr || !TerminalPanel_addText(
                railPanel, rowTextView, TerminalAnchor_2(TERMINAL_ANCHOR_BAND_TOP, 1u)).generation) {
            if (rowTextView != nullptr)
                TerminalText_free(rowTextView);
            TerminalPanel_free(railPanel);
            return nullptr;
        }
        (fn) TerminalText_setForegroundColor(rowTextView, SEMICOLON_V2_TEXT);
        (*semicolonState).railRowTexts[rowIndex] = rowTextView;
    }
    (*semicolonState).railRowCount = SEMICOLON_V2_MAX_ROWS;
    (*semicolonState).railPanel = railPanel;
    return railPanel;
}
