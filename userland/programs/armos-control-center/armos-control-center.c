/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/programs/armos-control-center/armos-control-center.c
 * Layer: Userland / desktop applications
 *
 * Responsibilities:
 * - Present display, network, process and appearance panels.
 * - Exercise the ArmUI application contract as an ordinary Wayland client.
 * - Consume system state exclusively through the ArmOS service API.
 */

#include <stdio.h>
#include <string.h>

#include <armos/services.h>
#include <armui/armui.h>

enum control_panel {
    CONTROL_PANEL_DISPLAY,
    CONTROL_PANEL_NETWORK,
    CONTROL_PANEL_PROCESSES,
    CONTROL_PANEL_APPEARANCE
};

struct control_center {
    enum control_panel panel;
    int dark;
    struct armos_service_snapshot snapshot;
};

static void refresh_snapshot(struct control_center *center)
{
    if (armos_services_snapshot(&center->snapshot) < 0)
        memset(&center->snapshot, 0, sizeof(center->snapshot));
}

static void display_panel(struct armui_context *ui,
                          const struct control_center *center)
{
    char line[96];

    armui_row(ui, 32.0f, 1);
    armui_label(ui, "Affichage", ARMUI_ALIGN_LEFT);
    snprintf(line, sizeof(line), "Resolution active : %ux%u",
             center->snapshot.display_width,
             center->snapshot.display_height);
    armui_row(ui, 28.0f, 1);
    armui_label(ui, line, ARMUI_ALIGN_LEFT);
    snprintf(line, sizeof(line), "Profondeur : %u bits",
             center->snapshot.display_bpp);
    armui_row(ui, 28.0f, 1);
    armui_label(ui, line, ARMUI_ALIGN_LEFT);
    armui_row(ui, 60.0f, 1);
    armui_label_wrap(
        ui,
        "Les changements de mode seront transmis au service graphique "
        "privilegie. L'application ne manipule pas le framebuffer.");
}

static void network_panel(struct armui_context *ui,
                          const struct control_center *center)
{
    char line[96];

    armui_row(ui, 32.0f, 1);
    armui_label(ui, "Reseau", ARMUI_ALIGN_LEFT);
    snprintf(line, sizeof(line), "Etat : %s",
             center->snapshot.network_available ?
             "interface disponible" : "aucune interface");
    armui_row(ui, 28.0f, 1);
    armui_label(ui, line, ARMUI_ALIGN_LEFT);
    snprintf(line, sizeof(line), "Interface : %s",
             center->snapshot.network_interface[0] != '\0' ?
             center->snapshot.network_interface : "-");
    armui_row(ui, 28.0f, 1);
    armui_label(ui, line, ARMUI_ALIGN_LEFT);
}

static void process_panel(struct armui_context *ui,
                          const struct control_center *center)
{
    char line[112];

    armui_row(ui, 32.0f, 1);
    snprintf(line, sizeof(line), "Processus (%u)",
             center->snapshot.process_count);
    armui_label(ui, line, ARMUI_ALIGN_LEFT);
    for (size_t index = 0u;
         index < center->snapshot.visible_processes; index++) {
        const struct armos_service_process *process =
            &center->snapshot.processes[index];

        snprintf(line, sizeof(line),
                 "%4d  %-20s  CPU %u.%u%%  RSS %uK",
                 process->pid, process->name,
                 process->cpu_x10 / 10u,
                 process->cpu_x10 % 10u,
                 process->rss_kb);
        armui_row(ui, 24.0f, 1);
        armui_label(ui, line, ARMUI_ALIGN_LEFT);
    }
}

static void appearance_panel(struct armui_context *ui,
                             struct control_center *center)
{
    armui_row(ui, 32.0f, 1);
    armui_label(ui, "Apparence", ARMUI_ALIGN_LEFT);
    armui_row(ui, 30.0f, 2);
    if (armui_option(ui, "Clair", !center->dark))
        center->dark = 0;
    if (armui_option(ui, "Contraste", center->dark))
        center->dark = 1;
    armui_row(ui, 60.0f, 1);
    armui_label_wrap(
        ui,
        "Le theme passe par ArmUI. Nuklear reste une implementation "
        "privee et remplacable.");
}

static unsigned int control_center_frame(
    struct armui_application *application,
    struct armui_context *ui,
    struct armui_target *target,
    uint32_t time_ms,
    void *data)
{
    struct control_center *center = data;

    (void)time_ms;
    armui_application_set_background(
        application, 1,
        center->dark ? 0x00171b21u : 0x00e1e6edu);
    armui_set_contrast(ui, center->dark);
    if (armui_window_begin(
            ui, "Centre de controle", 12.0f, 12.0f,
            (float)target->width - 24.0f,
            (float)target->height - 24.0f, 1)) {
        armui_row(ui, 32.0f, 4);
        if (armui_button_label(ui, "Affichage"))
            center->panel = CONTROL_PANEL_DISPLAY;
        if (armui_button_label(ui, "Reseau"))
            center->panel = CONTROL_PANEL_NETWORK;
        if (armui_button_label(ui, "Processus"))
            center->panel = CONTROL_PANEL_PROCESSES;
        if (armui_button_label(ui, "Apparence"))
            center->panel = CONTROL_PANEL_APPEARANCE;
        armui_row(ui, 28.0f, 1);
        if (armui_button_label(ui, "Actualiser"))
            refresh_snapshot(center);
        switch (center->panel) {
        case CONTROL_PANEL_NETWORK:
            network_panel(ui, center);
            break;
        case CONTROL_PANEL_PROCESSES:
            process_panel(ui, center);
            break;
        case CONTROL_PANEL_APPEARANCE:
            appearance_panel(ui, center);
            break;
        case CONTROL_PANEL_DISPLAY:
        default:
            display_panel(ui, center);
            break;
        }
    }
    armui_window_end(ui);
    return ARMUI_FRAME_IDLE;
}

int main(void)
{
    const struct armui_application_config config = {
        .title = "ArmOS Control Center",
        .app_id = "org.armos.control-center",
        .width = 720,
        .height = 520,
        .minimum_width = 480,
        .minimum_height = 320,
        .clear_target = 1,
        .background = 0x00e1e6edu,
        .key = NULL
    };
    struct control_center center;

    memset(&center, 0, sizeof(center));
    refresh_snapshot(&center);
    if (armui_application_run(
            &config, control_center_frame, &center) < 0) {
        perror("armos-control-center");
        return 1;
    }
    return 0;
}
