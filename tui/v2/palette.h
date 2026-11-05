#ifndef SEMICOLON_V2_PALETTE_H
#define SEMICOLON_V2_PALETTE_H

#include "format/terminal_color.h"

// Theme color definitions for Semicolon v2 (Interactables Preferences)
#define SEMICOLON_V2_TEXT        TERMINAL_RGB(0xE6, 0xE6, 0xE6)
#define SEMICOLON_V2_MUTED       TERMINAL_RGB(0x77, 0x77, 0x88)
#define SEMICOLON_V2_ACCENT      TERMINAL_RGB(0x7A, 0xC8, 0xFF)
#define SEMICOLON_V2_SELECTION   TERMINAL_RGB(0x4A, 0x62, 0x7E) // Grayish blue highlight
#define SEMICOLON_V2_GUTTER      TERMINAL_RGBA(0xAA, 0xAA, 0xC4, 0x7A)
#define SEMICOLON_V2_ERROR       TERMINAL_RGB(0xFF, 0x6B, 0x6B)
#define SEMICOLON_V2_DEVICE_BG   TERMINAL_RGB(0x1A, 0x1A, 0x1A)

// Syntax & item colors (warm golden brown fried chicken tone for folder)
#define SEMICOLON_V2_FOLDER      TERMINAL_RGB(0xD4, 0x9B, 0x4B)
#define SEMICOLON_V2_IGNORED     TERMINAL_RGB(0xFF, 0xB8, 0x6C) // Soft lighter orange for gitignored entries
#define SEMICOLON_V2_C           TERMINAL_RGB(0xA8, 0x8A, 0xF2)
#define SEMICOLON_V2_STRING      TERMINAL_RGB(0x98, 0xC3, 0x79)
#define SEMICOLON_V2_KEYWORD     TERMINAL_RGB(0x22, 0xC5, 0x5E) // Vibrant green keyword
#define SEMICOLON_V2_TYPE        TERMINAL_RGB(0x61, 0xAF, 0xEF)
#define SEMICOLON_V2_FUNCTION    TERMINAL_RGB(0x56, 0xB6, 0xC2)
#define SEMICOLON_V2_NUMBER      TERMINAL_RGB(0xD1, 0x9A, 0x66)
#define SEMICOLON_V2_COMMENT     TERMINAL_RGB(0x91, 0x91, 0x91)
#define SEMICOLON_V2_DOC_COMMENT TERMINAL_RGB(0x6E, 0xA8, 0x6E)

#endif
