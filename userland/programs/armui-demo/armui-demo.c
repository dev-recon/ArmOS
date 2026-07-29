/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/programs/armui-demo/armui-demo.c
 * Layer: Userland / graphical applications
 *
 * Responsibilities:
 * - Validate the public ArmUI application and widget contracts.
 * - Exercise UTF-8, scrolling, resizing, themes and repeated controls.
 *
 * Notes:
 * - Wayland, xdg-shell, input and SHM lifetimes belong to libarmui.
 * - This program contains application state only.
 */

#include <stdio.h>
#include <string.h>

#include <armui/armui.h>

struct armui_demo {
    int counter;
    int mode;
    int enabled;
    float scale;
    char text[96];
    size_t text_length;
};

static unsigned int armui_demo_frame(
    struct armui_application *application,
    struct armui_context *ui,
    struct armui_target *target,
    uint32_t time_ms,
    void *data)
{
    struct armui_demo *demo = data;
    size_t scale_preview;
    int repeater = armui_application_repeat_pulse(application);

    (void)time_ms;
    armui_application_set_background(
        application, 1,
        demo->mode == 0 ? 0x00e1e6edu : 0x00171b21u);
    armui_set_contrast(ui, demo->mode == 1);
    if (armui_window_begin(
            ui, "ArmUI", 16.0f, 16.0f,
            (float)(target->width - 32),
            (float)(target->height - 32), 1)) {
        armui_row(ui, 32.0f, 1);
        armui_label(ui, "Nuklear fonctionne nativement sur ArmOS",
                    ARMUI_ALIGN_CENTER);

        armui_row(ui, 34.0f, 3);
        armui_set_enabled(ui, demo->enabled);
        armui_set_repeater(ui, repeater);
        if (armui_button_label(ui, "Decrementer"))
            demo->counter--;
        armui_set_repeater(ui, 0);
        armui_label_int(ui, "Compteur", demo->counter);
        armui_set_repeater(ui, repeater);
        if (armui_button_label(ui, "Incrementer"))
            demo->counter++;
        armui_set_repeater(ui, 0);
        armui_set_enabled(ui, 1);
        if (demo->counter < 0)
            demo->counter = 0;
        else if (demo->counter > 100)
            demo->counter = 100;

        armui_row(ui, 30.0f, 2);
        if (armui_option(ui, "Mode clair", demo->mode == 0))
            demo->mode = 0;
        if (armui_option(ui, "Mode contraste", demo->mode == 1))
            demo->mode = 1;

        armui_row(ui, 30.0f, 1);
        demo->enabled = armui_checkbox(
            ui, "Interface active", demo->enabled);

        armui_row(ui, 32.0f, 1);
        armui_set_enabled(ui, demo->enabled);
        armui_set_repeater(ui, repeater);
        (void)armui_property_float(
            ui, "Echelle", 0.5f, &demo->scale,
            2.0f, 0.1f, 0.05f);
        armui_set_repeater(ui, 0);
        armui_set_enabled(ui, 1);

        armui_row(ui, 26.0f, 1);
        armui_progress(ui, (size_t)demo->counter, 100u);

        armui_row(ui, 26.0f, 2);
        armui_label(ui, "Apercu de l'echelle", ARMUI_ALIGN_LEFT);
        scale_preview = (size_t)(demo->scale * 50.0f);
        armui_progress(ui, scale_preview, 100u);

        armui_row(ui, 32.0f, 1);
        (void)armui_edit_string(
            ui, demo->text, sizeof(demo->text),
            &demo->text_length);

        armui_row(ui, 82.0f, 1);
        armui_label_wrap(
            ui,
            "Le backend commun libarmui gere Wayland, UTF-8, "
            "double buffering, redimensionnement et defilement.");

        armui_row_static(ui, 28.0f, 190.0f, 3);
        armui_label(ui, "Colonne horizontale 1", ARMUI_ALIGN_LEFT);
        armui_label(ui, "Colonne horizontale 2", ARMUI_ALIGN_LEFT);
        armui_label(ui, "Colonne horizontale 3", ARMUI_ALIGN_LEFT);
    }
    armui_window_end(ui);
    return armui_application_pointer_down(application) ?
        ARMUI_FRAME_CONTINUE : ARMUI_FRAME_IDLE;
}

int main(void)
{
    const struct armui_application_config config = {
        .title = "ArmUI - Nuklear demo",
        .app_id = "org.armos.armui-demo",
        .width = 640,
        .height = 420,
        .minimum_width = 360,
        .minimum_height = 280,
        .clear_target = 1,
        .background = 0x00e1e6edu,
        .key = NULL
    };
    struct armui_demo demo;

    memset(&demo, 0, sizeof(demo));
    demo.counter = 35;
    demo.enabled = 1;
    demo.scale = 1.0f;
    memcpy(demo.text, "Saisie UTF-8: ", sizeof("Saisie UTF-8: "));
    demo.text_length = strlen(demo.text);
    if (armui_application_run(
            &config, armui_demo_frame, &demo) < 0) {
        perror("armui-demo");
        return 1;
    }
    return 0;
}
