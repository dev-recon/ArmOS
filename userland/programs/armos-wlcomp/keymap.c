/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/programs/armos-wlcomp/keymap.c
 * Layer: Userland / graphical services
 *
 * Responsibilities:
 * - Publish the active seat layout in XKB text format.
 * - Republish the keymap to every Wayland keyboard after a runtime change.
 * - Transfer keymap descriptors through the common SCM_RIGHTS transport.
 *
 * Notes:
 * - Input drivers report architecture-neutral Linux key codes.
 * - Layout policy belongs to the compositor, not to a platform backend.
 */

#include "armos_wlcomp.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 1u
#define WL_KEYMAP_BUFFER_SIZE 16384u

static const char keymap_prefix[] =
    "xkb_keymap {\n"
    " xkb_keycodes \"armos\" {\n"
    "  minimum = 9; maximum = 134;\n"
    "  <ESC>=9; <AE01>=10; <AE02>=11; <AE03>=12; <AE04>=13;\n"
    "  <AE05>=14; <AE06>=15; <AE07>=16; <AE08>=17; <AE09>=18;\n"
    "  <AE10>=19; <AE11>=20; <AE12>=21; <BKSP>=22; <TAB>=23;\n"
    "  <AD01>=24; <AD02>=25; <AD03>=26; <AD04>=27; <AD05>=28;\n"
    "  <AD06>=29; <AD07>=30; <AD08>=31; <AD09>=32; <AD10>=33;\n"
    "  <AD11>=34; <AD12>=35; <RTRN>=36; <LCTL>=37; <AC01>=38;\n"
    "  <AC02>=39; <AC03>=40; <AC04>=41; <AC05>=42; <AC06>=43;\n"
    "  <AC07>=44; <AC08>=45; <AC09>=46; <AC10>=47; <AC11>=48;\n"
    "  <TLDE>=49; <LFSH>=50; <BKSL>=51; <AB01>=52; <AB02>=53;\n"
    "  <AB03>=54; <AB04>=55; <AB05>=56; <AB06>=57; <AB07>=58;\n"
    "  <AB08>=59; <AB09>=60; <AB10>=61; <RTSH>=62; <LALT>=64;\n"
    "  <SPCE>=65; <CAPS>=66; <FK01>=67; <FK02>=68; <FK03>=69;\n"
    "  <FK04>=70; <FK05>=71; <FK06>=72; <FK07>=73; <FK08>=74;\n"
    "  <FK09>=75; <FK10>=76; <FK11>=95; <FK12>=96; <RCTL>=105;\n"
    "  <RALT>=108; <HOME>=110; <UP>=111; <PGUP>=112; <LEFT>=113;\n"
    "  <RGHT>=114; <END>=115; <DOWN>=116; <PGDN>=117; <INS>=118;\n"
    "  <DELE>=119; <LSGT>=94; <LWIN>=133; <RWIN>=134;\n"
    " };\n"
    " xkb_types \"armos\" {\n"
    "  virtual_modifiers NumLock;\n"
    "  type \"ONE_LEVEL\" { modifiers=None; map[None]=Level1; };\n"
    "  type \"TWO_LEVEL\" {\n"
    "   modifiers=Shift; map[None]=Level1; map[Shift]=Level2;\n"
    "  };\n"
    "  type \"FOUR_LEVEL_LEVEL3\" {\n"
    "   modifiers=Shift+Mod5; map[None]=Level1; map[Shift]=Level2;\n"
    "   map[Mod5]=Level3; map[Shift+Mod5]=Level4;\n"
    "  };\n"
    "  type \"FOUR_LEVEL_LOGO\" {\n"
    "   modifiers=Shift+Mod4; map[None]=Level1; map[Shift]=Level2;\n"
    "   map[Mod4]=Level3; map[Shift+Mod4]=Level4;\n"
    "  };\n"
    "  type \"FOUR_LEVEL_ALT\" {\n"
    "   modifiers=Shift+Mod1; map[None]=Level1; map[Shift]=Level2;\n"
    "   map[Mod1]=Level3; map[Shift+Mod1]=Level4;\n"
    "  };\n"
    " };\n"
    " xkb_compatibility \"armos\" {\n"
    "  virtual_modifiers NumLock;\n"
    "  interpret Any+AnyOf(all) { action=NoAction(); };\n"
    " };\n"
    " xkb_symbols \"armos\" {\n";

static const char keymap_common_symbols[] =
    "  key <ESC> { [ Escape ] }; key <BKSP> { [ BackSpace ] };\n"
    "  key <TAB> { [ Tab ] }; key <RTRN> { [ Return ] };\n"
    "  key <LCTL> { [ Control_L ] }; key <RCTL> { [ Control_R ] };\n"
    "  key <LFSH> { [ Shift_L ] }; key <RTSH> { [ Shift_R ] };\n"
    "  key <LALT> { [ Alt_L ] };\n"
    "  key <LWIN> { [ Super_L ] }; key <RWIN> { [ Super_R ] };\n"
    "  key <SPCE> { [ space ] }; key <CAPS> { [ Caps_Lock ] };\n"
    "  key <FK01> { [ F1 ] }; key <FK02> { [ F2 ] }; key <FK03> { [ F3 ] };\n"
    "  key <FK04> { [ F4 ] }; key <FK05> { [ F5 ] }; key <FK06> { [ F6 ] };\n"
    "  key <FK07> { [ F7 ] }; key <FK08> { [ F8 ] }; key <FK09> { [ F9 ] };\n"
    "  key <FK10> { [ F10 ] }; key <FK11> { [ F11 ] }; key <FK12> { [ F12 ] };\n"
    "  key <HOME> { [ Home ] }; key <UP> { [ Up ] };\n"
    "  key <PGUP> { [ Prior ] }; key <LEFT> { [ Left ] };\n"
    "  key <RGHT> { [ Right ] }; key <END> { [ End ] };\n"
    "  key <DOWN> { [ Down ] }; key <PGDN> { [ Next ] };\n"
    "  key <INS> { [ Insert ] }; key <DELE> { [ Delete ] };\n";

static const char keymap_us_symbols[] =
    "  key <AE01> { [ 1, exclam ] }; key <AE02> { [ 2, at ] };\n"
    "  key <AE03> { [ 3, numbersign ] }; key <AE04> { [ 4, dollar ] };\n"
    "  key <AE05> { [ 5, percent ] }; key <AE06> { [ 6, asciicircum ] };\n"
    "  key <AE07> { [ 7, ampersand ] }; key <AE08> { [ 8, asterisk ] };\n"
    "  key <AE09> { [ 9, parenleft ] }; key <AE10> { [ 0, parenright ] };\n"
    "  key <AE11> { [ minus, underscore ] }; key <AE12> { [ equal, plus ] };\n"
    "  key <AD01> { [ q, Q ] }; key <AD02> { [ w, W ] };\n"
    "  key <AD03> { [ e, E ] }; key <AD04> { [ r, R ] };\n"
    "  key <AD05> { [ t, T ] }; key <AD06> { [ y, Y ] };\n"
    "  key <AD07> { [ u, U ] }; key <AD08> { [ i, I ] };\n"
    "  key <AD09> { [ o, O ] }; key <AD10> { [ p, P ] };\n"
    "  key <AD11> { [ bracketleft, braceleft ] };\n"
    "  key <AD12> { [ bracketright, braceright ] };\n"
    "  key <AC01> { [ a, A ] }; key <AC02> { [ s, S ] };\n"
    "  key <AC03> { [ d, D ] }; key <AC04> { [ f, F ] };\n"
    "  key <AC05> { [ g, G ] }; key <AC06> { [ h, H ] };\n"
    "  key <AC07> { [ j, J ] }; key <AC08> { [ k, K ] };\n"
    "  key <AC09> { [ l, L ] }; key <AC10> { [ semicolon, colon ] };\n"
    "  key <AC11> { [ apostrophe, quotedbl ] };\n"
    "  key <TLDE> { [ grave, asciitilde ] }; key <BKSL> { [ backslash, bar ] };\n"
    "  key <AB01> { [ z, Z ] }; key <AB02> { [ x, X ] };\n"
    "  key <AB03> { [ c, C ] }; key <AB04> { [ v, V ] };\n"
    "  key <AB05> { [ b, B ] }; key <AB06> { [ n, N ] };\n"
    "  key <AB07> { [ m, M ] }; key <AB08> { [ comma, less ] };\n"
    "  key <AB09> { [ period, greater ] }; key <AB10> { [ slash, question ] };\n";

static const char keymap_fr_symbols[] =
    "  key <AE01> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ agrave, 1, section, Agrave ] };\n"
    "  key <AE02> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ eacute, 2, dead_acute, Eacute ] };\n"
    "  key <AE03> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ egrave, 3, dead_grave, Egrave ] };\n"
    "  key <AE04> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ U00ea, 4, ampersand, U00ca ] };\n"
    "  key <AE05> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ parenleft, 5, bracketleft, dead_doubleacute ] };\n"
    "  key <AE06> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ parenright, 6, bracketright, dead_doublegrave ] };\n"
    "  key <AE07> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ U2018, 7, dead_macron, NoSymbol ] };\n"
    "  key <AE08> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ U2019, 8, underscore, U2014 ] };\n"
    "  key <AE09> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ guillemotleft, 9, U201c, U2039 ] };\n"
    "  key <AE10> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ guillemotright, 0, U201d, U203a ] };\n"
    "  key <AE11> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ apostrophe, quotedbl, degree, dead_abovering ] };\n"
    "  key <AE12> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ dead_circumflex, dead_diaeresis, dead_caron, NoSymbol ] };\n"
    "  key <AD01> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ a, A, U00e6, U00c6 ] };\n"
    "  key <AD02> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ z, Z, sterling, NoSymbol ] };\n"
    "  key <AD03> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ e, E, EuroSign, NoSymbol ] };\n"
    "  key <AD04> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ r, R, registered, NoSymbol ] };\n"
    "  key <AD05> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ t, T, braceleft, U2122 ] };\n"
    "  key <AD06> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ y, Y, braceright, NoSymbol ] };\n"
    "  key <AD07> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ u, U, ugrave, Ugrave ] };\n"
    "  key <AD08> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ i, I, dead_abovedot, dead_belowdot ] };\n"
    "  key <AD09> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ o, O, oe, OE ] };\n"
    "  key <AD10> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ p, P, percent, U2030 ] };\n"
    "  key <AD11> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ minus, U2013, U2212, U2011 ] };\n"
    "  key <AD12> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ plus, U00b1, U2020, U2021 ] };\n"
    "  key <AC01> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ q, Q, U03b8, U03f4 ] };\n"
    "  key <AC02> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ s, S, ssharp, U1e9e ] };\n"
    "  key <AC03> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ d, D, dollar, NoSymbol ] };\n"
    "  key <AC04> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ f, F, dead_currency, NoSymbol ] };\n"
    "  key <AC05> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ g, G, dead_greek, NoSymbol ] };\n"
    "  key <AC06> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ h, H, U017f, dead_belowmacron ] };\n"
    "  key <AC07> { [ j, J ] }; key <AC08> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ k, K, dead_stroke, NoSymbol ] };\n"
    "  key <AC09> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ l, L, bar, NoSymbol ] };\n"
    "  key <AC10> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ m, M, U221e, NoSymbol ] };\n"
    "  key <AC11> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ slash, backslash, U00f7, U221a ] };\n"
    "  key <TLDE> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ at, numbersign, dead_breve, dead_invertedbreve ] };\n"
    "  key <BKSL> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ asterisk, U00bd, U00d7, U00bc ] };\n"
    "  key <AB01> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ w, W, U0292, U01b7 ] };\n"
    "  key <AB02> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ x, X, copyright, NoSymbol ] };\n"
    "  key <AB03> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ c, C, ccedilla, Ccedilla ] };\n"
    "  key <AB04> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ v, V, dead_cedilla, dead_ogonek ] };\n"
    "  key <AB05> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ b, B, dead_stroke, NoSymbol ] };\n"
    "  key <AB06> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ n, N, dead_tilde, NoSymbol ] };\n"
    "  key <AB07> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ period, question, U00bf, NoSymbol ] };\n"
    "  key <AB08> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ comma, exclam, U00a1, dead_belowcomma ] };\n"
    "  key <AB09> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ colon, U2026, U00b7, NoSymbol ] };\n"
    "  key <AB10> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ semicolon, equal, U2243, U2260 ] };\n"
    "  key <LSGT> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ less, greater, U2a7d, U2a7e ] };\n"
    "  key <SPCE> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ space, space, U00a0, U202f ] };\n";

static const char keymap_fr_legacy_symbols[] =
    "  key <AE01> { [ ampersand, 1 ] };\n"
    "  key <AE02> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ eacute, 2, asciitilde, NoSymbol ] };\n"
    "  key <AE03> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ quotedbl, 3, numbersign, NoSymbol ] };\n"
    "  key <AE04> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ apostrophe, 4, braceleft, NoSymbol ] };\n"
    "  key <AE05> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ parenleft, 5, bracketleft, NoSymbol ] };\n"
    "  key <AE06> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ minus, 6, bar, NoSymbol ] };\n"
    "  key <AE07> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ egrave, 7, grave, NoSymbol ] };\n"
    "  key <AE08> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ underscore, 8, backslash, NoSymbol ] };\n"
    "  key <AE09> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ ccedilla, 9, asciicircum, NoSymbol ] };\n"
    "  key <AE10> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ agrave, 0, at, NoSymbol ] };\n"
    "  key <AE11> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ parenright, degree, bracketright, NoSymbol ] };\n"
    "  key <AE12> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ equal, plus, braceright, NoSymbol ] };\n"
    "  key <AD01> { [ a, A ] }; key <AD02> { [ z, Z ] };\n"
    "  key <AD03> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ e, E, EuroSign, NoSymbol ] };\n"
    "  key <AD04> { [ r, R ] }; key <AD05> { [ t, T ] };\n"
    "  key <AD06> { [ y, Y ] }; key <AD07> { [ u, U ] };\n"
    "  key <AD08> { [ i, I ] }; key <AD09> { [ o, O ] };\n"
    "  key <AD10> { [ p, P ] };\n"
    "  key <AD11> { [ dead_circumflex, dead_diaeresis ] };\n"
    "  key <AD12> { type[Group1]=\"FOUR_LEVEL_LEVEL3\", [ dollar, sterling, currency, NoSymbol ] };\n"
    "  key <AC01> { [ q, Q ] }; key <AC02> { [ s, S ] };\n"
    "  key <AC03> { [ d, D ] }; key <AC04> { [ f, F ] };\n"
    "  key <AC05> { [ g, G ] }; key <AC06> { [ h, H ] };\n"
    "  key <AC07> { [ j, J ] }; key <AC08> { [ k, K ] };\n"
    "  key <AC09> { [ l, L ] }; key <AC10> { [ m, M ] };\n"
    "  key <AC11> { [ ugrave, percent ] };\n"
    "  key <TLDE> { [ twosuperior, NoSymbol ] };\n"
    "  key <BKSL> { [ asterisk, U00b5 ] };\n"
    "  key <AB01> { [ w, W ] }; key <AB02> { [ x, X ] };\n"
    "  key <AB03> { [ c, C ] }; key <AB04> { [ v, V ] };\n"
    "  key <AB05> { [ b, B ] }; key <AB06> { [ n, N ] };\n"
    "  key <AB07> { [ comma, question ] };\n"
    "  key <AB08> { [ semicolon, period ] };\n"
    "  key <AB09> { [ colon, slash ] };\n"
    "  key <AB10> { [ exclam, section ] };\n"
    "  key <LSGT> { [ less, greater ] };\n";

static const char keymap_fr_mac_symbols[] =
    "  key <AE01> { [ ampersand, 1 ] }; key <AE02> { [ eacute, 2 ] };\n"
    "  key <AE03> { [ quotedbl, 3 ] }; key <AE04> { [ apostrophe, 4 ] };\n"
    "  key <AE05> { type[Group1]=\"FOUR_LEVEL_ALT\", [ parenleft, 5, braceleft, bracketleft ] };\n"
    "  key <AE06> { [ section, 6 ] };\n"
    "  key <AE07> { [ egrave, 7 ] }; key <AE08> { [ exclam, 8 ] };\n"
    "  key <AE09> { [ ccedilla, 9 ] }; key <AE10> { [ agrave, 0 ] };\n"
    "  key <AE11> { type[Group1]=\"FOUR_LEVEL_ALT\", [ parenright, degree, braceright, bracketright ] };\n"
    "  key <AE12> { [ minus, underscore ] };\n"
    "  key <AD01> { type[Group1]=\"FOUR_LEVEL_LOGO\", [ a, A, at, at ] };\n"
    "  key <AD02> { [ z, Z ] };\n"
    "  key <AD03> { [ e, E ] }; key <AD04> { [ r, R ] };\n"
    "  key <AD05> { [ t, T ] }; key <AD06> { [ y, Y ] };\n"
    "  key <AD07> { [ u, U ] }; key <AD08> { [ i, I ] };\n"
    "  key <AD09> { [ o, O ] }; key <AD10> { [ p, P ] };\n"
    "  key <AD11> { [ bracketleft, braceleft ] }; key <AD12> { [ dollar, asterisk ] };\n"
    "  key <AC01> { [ q, Q ] }; key <AC02> { [ s, S ] };\n"
    "  key <AC03> { type[Group1]=\"FOUR_LEVEL_LOGO\", [ d, D, numbersign, numbersign ] };\n"
    "  key <AC04> { [ f, F ] };\n"
    "  key <AC05> { [ g, G ] }; key <AC06> { [ h, H ] };\n"
    "  key <AC07> { [ j, J ] }; key <AC08> { [ k, K ] };\n"
    "  key <AC09> { type[Group1]=\"FOUR_LEVEL_ALT\", [ l, L, U00ac, bar ] };\n"
    "  key <AC10> { [ m, M ] };\n"
    "  key <AC11> { [ ugrave, percent ] };\n"
    "  key <TLDE> { [ less, greater ] }; key <BKSL> { [ grave ] };\n"
    "  key <AB01> { [ w, W ] }; key <AB02> { [ x, X ] };\n"
    "  key <AB03> { [ c, C ] }; key <AB04> { [ v, V ] };\n"
    "  key <AB05> { [ b, B ] };\n"
    "  key <AB06> { type[Group1]=\"FOUR_LEVEL_ALT\", [ n, N, asciitilde, NoSymbol ] };\n"
    "  key <AB07> { [ comma, question ] }; key <AB08> { [ semicolon, period ] };\n"
    "  key <AB09> { type[Group1]=\"FOUR_LEVEL_ALT\", [ colon, slash, U00f7, backslash ] };\n"
    "  key <AB10> { [ equal, plus ] };\n";

static const char keymap_standard_modifiers[] =
    "  key <RALT> { [ Alt_R ] };\n";

static const char keymap_fr_modifiers[] =
    "  key <RALT> { [ ISO_Level3_Shift ] };\n";

static const char keymap_standard_suffix[] =
    "  modifier_map Shift { <LFSH>, <RTSH> };\n"
    "  modifier_map Control { <LCTL>, <RCTL> };\n"
    "  modifier_map Mod1 { <LALT>, <RALT> };\n"
    "  modifier_map Mod4 { <LWIN>, <RWIN> };\n"
    "  modifier_map Lock { <CAPS> };\n"
    " };\n"
    "};\n";

static const char keymap_fr_suffix[] =
    "  modifier_map Shift { <LFSH>, <RTSH> };\n"
    "  modifier_map Control { <LCTL>, <RCTL> };\n"
    "  modifier_map Mod1 { <LALT> };\n"
    "  modifier_map Mod4 { <LWIN>, <RWIN> };\n"
    "  modifier_map Mod5 { <RALT> };\n"
    "  modifier_map Lock { <CAPS> };\n"
    " };\n"
    "};\n";

static const char *wl_layout_symbols(uint32_t layout)
{
    switch (layout) {
    case ARMOS_KEYBOARD_LAYOUT_US:
    case ARMOS_KEYBOARD_LAYOUT_US_MAC:
        return keymap_us_symbols;
    case ARMOS_KEYBOARD_LAYOUT_FR:
        return keymap_fr_symbols;
    case ARMOS_KEYBOARD_LAYOUT_FR_MAC:
        return keymap_fr_mac_symbols;
    case ARMOS_KEYBOARD_LAYOUT_FR_LEGACY:
        return keymap_fr_legacy_symbols;
    default:
        return NULL;
    }
}

static const char *wl_layout_name(uint32_t layout)
{
    switch (layout) {
    case ARMOS_KEYBOARD_LAYOUT_US:
        return "English (US)";
    case ARMOS_KEYBOARD_LAYOUT_US_MAC:
        return "English (US, Macintosh)";
    case ARMOS_KEYBOARD_LAYOUT_FR:
        return "French (Standard, AZERTY)";
    case ARMOS_KEYBOARD_LAYOUT_FR_MAC:
        return "French (Macintosh)";
    case ARMOS_KEYBOARD_LAYOUT_FR_LEGACY:
        return "French (Legacy, AZERTY)";
    default:
        return NULL;
    }
}

static const char *wl_layout_modifier_symbols(uint32_t layout)
{
    return (layout == ARMOS_KEYBOARD_LAYOUT_FR ||
            layout == ARMOS_KEYBOARD_LAYOUT_FR_LEGACY) ?
        keymap_fr_modifiers : keymap_standard_modifiers;
}

static const char *wl_layout_suffix(uint32_t layout)
{
    return (layout == ARMOS_KEYBOARD_LAYOUT_FR ||
            layout == ARMOS_KEYBOARD_LAYOUT_FR_LEGACY) ?
        keymap_fr_suffix : keymap_standard_suffix;
}

int wl_server_send_keymap(struct wl_server *server,
                          struct wl_server_client *client,
                          uint32_t keyboard_id)
{
    char name[64];
    char keymap[WL_KEYMAP_BUFFER_SIZE];
    uint32_t words[2];
    const char *layout_symbols;
    const char *layout_name;
    const char *modifier_symbols;
    const char *suffix;
    size_t size;
    void *mapping;
    int fd;
    int length;
    int result;

    if (!server || !client || keyboard_id == 0u)
        return -1;
    layout_symbols = wl_layout_symbols(server->keyboard_layout);
    layout_name = wl_layout_name(server->keyboard_layout);
    modifier_symbols = wl_layout_modifier_symbols(server->keyboard_layout);
    suffix = wl_layout_suffix(server->keyboard_layout);
    if (!layout_symbols || !layout_name || !modifier_symbols || !suffix)
        return -1;
    length = snprintf(keymap, sizeof(keymap),
                      "%s  name[group1]=\"%s\";\n%s%s%s%s",
                      keymap_prefix, layout_name, keymap_common_symbols,
                      layout_symbols, modifier_symbols, suffix);
    if (length < 0 || (size_t)length + 1u > sizeof(keymap))
        return -1;
    size = (size_t)length + 1u;
    snprintf(name, sizeof(name), "/armos-wl-keymap-%d-%d",
             getpid(), client->fd);
    shm_unlink(name);
    fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    if (shm_unlink(name) < 0 || ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        return -1;
    }
    mapping = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) {
        close(fd);
        return -1;
    }
    memcpy(mapping, keymap, size);
    if (munmap(mapping, size) < 0) {
        close(fd);
        return -1;
    }
    words[0] = WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1;
    words[1] = (uint32_t)size;
    result = wl_client_send_fd_words(client, keyboard_id, 0u, words, 2u, fd);
    close(fd);
    return result;
}

int wl_server_broadcast_keymap(struct wl_server *server)
{
    int result = 0;

    if (!server)
        return -1;
    for (size_t client_index = 0u;
         client_index < WL_SERVER_MAX_CLIENTS; client_index++) {
        struct wl_server_client *client = &server->clients[client_index];

        if (!client->used)
            continue;
        for (size_t object_index = 0u;
             object_index < WL_SERVER_MAX_OBJECTS; object_index++) {
            struct wl_server_object *object = &client->objects[object_index];

            if (object->type == WL_SERVER_OBJECT_KEYBOARD &&
                wl_server_send_keymap(server, client, object->id) < 0)
                result = -1;
        }
    }
    return result;
}
