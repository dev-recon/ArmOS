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
 * - Publish the compositor keyboard map in XKB text format.
 * - Transfer the keymap descriptor through Wayland SCM_RIGHTS transport.
 * - Keep keyboard layout policy in the userland compositor.
 *
 * Notes:
 * - Input drivers continue to report architecture-neutral Linux key codes.
 * - The initial layout is a compact US map sufficient for terminal clients.
 */

#include "armos_wlcomp.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 1u

static const char wl_xkb_keymap[] =
    "xkb_keymap {\n"
    " xkb_keycodes \"armos\" {\n"
    "  minimum = 8; maximum = 255;\n"
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
    "  <DELE>=119;\n"
    " };\n"
    " xkb_types \"armos\" {\n"
    "  virtual_modifiers NumLock;\n"
    "  type \"ONE_LEVEL\" { modifiers=None; map[None]=Level1; };\n"
    "  type \"TWO_LEVEL\" {\n"
    "   modifiers=Shift; map[None]=Level1; map[Shift]=Level2;\n"
    "  };\n"
    " };\n"
    " xkb_compatibility \"armos\" {\n"
    "  virtual_modifiers NumLock;\n"
    "  interpret Any+AnyOf(all) { action=NoAction(); };\n"
    " };\n"
    " xkb_symbols \"armos\" {\n"
    "  name[group1]=\"English (US)\";\n"
    "  key <ESC> { [ Escape ] }; key <AE01> { [ 1, exclam ] };\n"
    "  key <AE02> { [ 2, at ] }; key <AE03> { [ 3, numbersign ] };\n"
    "  key <AE04> { [ 4, dollar ] }; key <AE05> { [ 5, percent ] };\n"
    "  key <AE06> { [ 6, asciicircum ] }; key <AE07> { [ 7, ampersand ] };\n"
    "  key <AE08> { [ 8, asterisk ] }; key <AE09> { [ 9, parenleft ] };\n"
    "  key <AE10> { [ 0, parenright ] }; key <AE11> { [ minus, underscore ] };\n"
    "  key <AE12> { [ equal, plus ] }; key <BKSP> { [ BackSpace ] };\n"
    "  key <TAB> { [ Tab ] }; key <AD01> { [ q, Q ] };\n"
    "  key <AD02> { [ w, W ] }; key <AD03> { [ e, E ] };\n"
    "  key <AD04> { [ r, R ] }; key <AD05> { [ t, T ] };\n"
    "  key <AD06> { [ y, Y ] }; key <AD07> { [ u, U ] };\n"
    "  key <AD08> { [ i, I ] }; key <AD09> { [ o, O ] };\n"
    "  key <AD10> { [ p, P ] }; key <AD11> { [ bracketleft, braceleft ] };\n"
    "  key <AD12> { [ bracketright, braceright ] }; key <RTRN> { [ Return ] };\n"
    "  key <LCTL> { [ Control_L ] }; key <AC01> { [ a, A ] };\n"
    "  key <AC02> { [ s, S ] }; key <AC03> { [ d, D ] };\n"
    "  key <AC04> { [ f, F ] }; key <AC05> { [ g, G ] };\n"
    "  key <AC06> { [ h, H ] }; key <AC07> { [ j, J ] };\n"
    "  key <AC08> { [ k, K ] }; key <AC09> { [ l, L ] };\n"
    "  key <AC10> { [ semicolon, colon ] }; key <AC11> { [ apostrophe, quotedbl ] };\n"
    "  key <TLDE> { [ grave, asciitilde ] }; key <LFSH> { [ Shift_L ] };\n"
    "  key <BKSL> { [ backslash, bar ] }; key <AB01> { [ z, Z ] };\n"
    "  key <AB02> { [ x, X ] }; key <AB03> { [ c, C ] };\n"
    "  key <AB04> { [ v, V ] }; key <AB05> { [ b, B ] };\n"
    "  key <AB06> { [ n, N ] }; key <AB07> { [ m, M ] };\n"
    "  key <AB08> { [ comma, less ] }; key <AB09> { [ period, greater ] };\n"
    "  key <AB10> { [ slash, question ] }; key <RTSH> { [ Shift_R ] };\n"
    "  key <LALT> { [ Alt_L ] }; key <RALT> { [ Alt_R ] };\n"
    "  key <SPCE> { [ space ] }; key <CAPS> { [ Caps_Lock ] };\n"
    "  key <FK01> { [ F1 ] }; key <FK02> { [ F2 ] }; key <FK03> { [ F3 ] };\n"
    "  key <FK04> { [ F4 ] }; key <FK05> { [ F5 ] }; key <FK06> { [ F6 ] };\n"
    "  key <FK07> { [ F7 ] }; key <FK08> { [ F8 ] }; key <FK09> { [ F9 ] };\n"
    "  key <FK10> { [ F10 ] }; key <FK11> { [ F11 ] }; key <FK12> { [ F12 ] };\n"
    "  key <RCTL> { [ Control_R ] }; key <HOME> { [ Home ] };\n"
    "  key <UP> { [ Up ] }; key <PGUP> { [ Prior ] }; key <LEFT> { [ Left ] };\n"
    "  key <RGHT> { [ Right ] }; key <END> { [ End ] }; key <DOWN> { [ Down ] };\n"
    "  key <PGDN> { [ Next ] }; key <INS> { [ Insert ] }; key <DELE> { [ Delete ] };\n"
    "  modifier_map Shift { <LFSH>, <RTSH> };\n"
    "  modifier_map Control { <LCTL>, <RCTL> };\n"
    "  modifier_map Mod1 { <LALT>, <RALT> };\n"
    "  modifier_map Lock { <CAPS> };\n"
    " };\n"
    "};\n";

int wl_server_send_keymap(struct wl_server_client *client,
                          uint32_t keyboard_id)
{
    char name[64];
    uint32_t words[2];
    size_t size = sizeof(wl_xkb_keymap);
    void *mapping;
    int fd;
    int result;

    if (!client || keyboard_id == 0u)
        return -1;
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
    memcpy(mapping, wl_xkb_keymap, size);
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
