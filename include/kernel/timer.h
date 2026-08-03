/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: include/kernel/timer.h
 * Layer: Kernel / public internal interface
 *
 * Responsibilities:
 * - Declare kernel types, constants, and subsystem contracts.
 * - Keep cross-module ABI and structure expectations explicit.
 *
 * Notes:
 * - Header changes can ripple across kernel and user ABI glue.
 */

#ifndef _KERNEL_TIMER_H
#define _KERNEL_TIMER_H

#include <kernel/arch_timer.h>
#include <kernel/types.h>

/* 1000Hz currently means one accounting tick per millisecond. */
#define TIMER_FREQ      ARCH_TIMER_TICK_HZ
#define TIMER_FALLBACK_FREQ ARCH_TIMER_FALLBACK_HZ

/* Structure pour décomposer une date Unix */
typedef struct {
    int year;
    int month;   /* 1-12 */
    int day;     /* 1-31 */
    int hour;    /* 0-23 */
    int minute;  /* 0-59 */
    int second;  /* 0-59 */
} datetime_t;

typedef struct timer_cpu_accounting {
    uint32_t user_ticks;
    uint32_t system_ticks;
    uint32_t irq_ticks;
    uint32_t idle_ticks;
} timer_cpu_accounting_t;

/* Timer functions */
void init_timer(void);
void timer_init_local_cpu(void);
void init_timer_software(void);
void update_timer_software(void);
void timer_irq_handler(void);
uint32_t get_timer_frequency(void);
void wake_up_sleeping_processes(void);
void timer_enable_scheduling(void);
uint64_t get_timer_count(void); 

/* Fonctions de lecture du timer */
uint32_t get_timer_frequency(void);
uint64_t get_timer_count(void);      /* <- Ajoutez cette ligne */
uint32_t get_system_ticks(void);
uint32_t timer_cpu_tick_count(uint32_t cpu_id);
void timer_accounting_irq_enter(bool interrupted_user);
void timer_accounting_irq_exit(void);
void timer_cpu_accounting_read(uint32_t cpu_id, timer_cpu_accounting_t* accounting);
uint32_t get_time_ms(void);
void get_realtime(uint64_t *seconds, uint32_t *nanoseconds);

void set_critical_section(void);
void unset_critical_section(void);
bool get_critical_section(void);
bool is_leap_year(int year);
int get_days_in_month(int month, int year);
void unix_to_datetime(uint32_t unix_time, datetime_t* dt);

uint32_t get_current_time(void);


/* Platform timer functions */
uint32_t timer_get_frequency(void);
uint64_t timer_get_count(void);
void timer_set_compare(uint64_t compare);
uint64_t get_generic_timer_count(void);
void set_generic_timer_compare(uint64_t compare_value);
void enable_generic_timer(void);
void disable_generic_timer(void);
void timer_set_timeout(uint64_t timeout_ticks);

/* Fonctions de debug */
void debug_timer_state(void);
void trigger_timer_interrupt(void);
void test_timer_functionality(void);

#endif
