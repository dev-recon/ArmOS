/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: kernel/timer/timer.c
 * Layer: Kernel / generic timer and clock source
 *
 * Responsibilities:
 * - Maintain the common scheduler clock and process accounting tick.
 * - Program the architecture-provided ARM generic timer primitives.
 *
 * Notes:
 * - The counter/control register accessors come from asm/timer.h.
 */

#include <kernel/timer.h>
#include <kernel/task.h>
#include <kernel/interrupt.h>
#include <kernel/process.h>
#include <kernel/kprintf.h>
#include <kernel/uart.h>
#include <kernel/tty.h>
#include <kernel/smp.h>
#include <kernel/arch_platform.h>
#include <kernel/arch_cpu.h>
#include <kernel/string.h>


/* Flag pour savoir si le systeme de processus est pret */
static bool process_system_ready = false;
static uint32_t system_ticks = 0;
static volatile uint32_t timer_cpu_ticks[ARMOS_MAX_CPUS];
static volatile timer_cpu_accounting_t timer_cpu_accounting[ARMOS_MAX_CPUS];
static volatile uint64_t timer_irq_start[ARMOS_MAX_CPUS];
static volatile uint64_t timer_irq_pending_cycles[ARMOS_MAX_CPUS];
static volatile uint64_t timer_irq_fraction[ARMOS_MAX_CPUS];
static volatile uint32_t timer_irq_depth[ARMOS_MAX_CPUS];
static volatile bool timer_irq_interrupted_user[ARMOS_MAX_CPUS];

static uint64_t last_timer_count = 0;
static uint32_t software_system_ticks = 0;
static bool timer_software_initialized = false;
static uint64_t timer_next_compare[ARMOS_MAX_CPUS];

static volatile bool in_critical_section_cpu[ARMOS_MAX_CPUS];
static uint32_t realtime_epoch_seconds;
static uint32_t realtime_epoch_ticks;
static bool realtime_clock_initialized;

static uint32_t timer_interval_from_frequency(uint32_t timer_freq)
{
    return timer_freq / TIMER_FREQ;
}

static uint32_t timer_program_next_tick(uint32_t timer_freq)
{
    uint32_t cpu = smp_processor_id();
    uint32_t interval = timer_interval_from_frequency(timer_freq);
    uint64_t now;
    uint64_t next;
    uint32_t elapsed_ticks = 1;

    if (cpu >= ARMOS_MAX_CPUS || interval == 0)
        return 1;

    now = timer_get_count();
    next = timer_next_compare[cpu];

    /*
     * Program the ARM physical timer in absolute mode.  Reusing CNTP_TVAL
     * after every IRQ makes handler latency accumulate into clock drift under
     * SMP load.  Advancing CNTP_CVAL by fixed intervals keeps the average tick
     * rate stable and lets accounting catch up when an IRQ was delayed.
     */
    if (next == 0) {
        next = now + interval;
    } else {
        elapsed_ticks = 0;
        do {
            next += interval;
            if (elapsed_ticks < 0xffffffffu)
                elapsed_ticks++;
        } while (next <= now);
        if (elapsed_ticks == 0)
            elapsed_ticks = 1;
    }

    timer_next_compare[cpu] = next;
    timer_set_compare(next);

    set_cntp_ctl(CNTP_CTL_ENABLE);
    return elapsed_ticks;
}


void set_critical_section(void){
    uint32_t cpu = smp_processor_id();
    if (cpu < ARMOS_MAX_CPUS)
        in_critical_section_cpu[cpu] = true;
}

void unset_critical_section(void){
    uint32_t cpu = smp_processor_id();
    if (cpu < ARMOS_MAX_CPUS)
        in_critical_section_cpu[cpu] = false;
}

bool get_critical_section(void)
{
    uint32_t cpu = smp_processor_id();
    return cpu < ARMOS_MAX_CPUS ? in_critical_section_cpu[cpu] : false;
}

void init_timer_software(void)
{
    extern int kprintf(const char *format, ...);
    uint32_t timer_freq = get_timer_frequency();

    KINFO("[TIMER] Initializing SOFTWARE timer (no IRQ)...\n");
    KINFO("[TIMER] Timer frequency: %u Hz\n", timer_freq);
    
    /* PAS d'activation du timer hardware */
    /* PAS d'activation IRQ 30 */
    /* PAS de programmation d'interruptions */
    
    /* Juste lire le compteur de base */
    last_timer_count = get_timer_count();
    software_system_ticks = 0;
    timer_software_initialized = true;
    
    KINFO("[TIMER] Software timer initialized (polling mode)\n");
    KINFO("[TIMER] Base count: %u%u\n", 
            (uint32_t)(last_timer_count >> 32), 
            (uint32_t)last_timer_count);
}

/* Fonction à appeler périodiquement pour mettre à jour les ticks */
void update_timer_software(void)
{
    
    if (!timer_software_initialized) {
        return;
    }
    
    /* Lire le compteur actuel */
    uint64_t current_count = get_timer_count();
    uint64_t elapsed = current_count - last_timer_count;
    
    /* Calculer combien de ticks de 10ms se sont écoulés */
    uint32_t timer_freq = get_timer_frequency();
    uint32_t tick_interval = timer_freq / 100;  /* 10ms en cycles */

    /* Conversion en 32-bit pour éviter division 64-bit */
    if (elapsed > 0xFFFFFFFF) {
        /* Si elapsed trop grand, traiter par chunks */
        elapsed = 0xFFFFFFFF;
    }

    uint32_t elapsed_32 = (uint32_t)elapsed;
    
    if (elapsed_32 >= tick_interval) {
        /* Calculer le nombre de ticks écoulés */
        uint32_t ticks_elapsed = (uint32_t)(elapsed_32 / tick_interval);
        
        software_system_ticks += ticks_elapsed;
        system_ticks = software_system_ticks;
        
        /* Mettre à jour la base pour le prochain calcul */
        last_timer_count += ticks_elapsed * tick_interval;
        
        /* Scheduling si nécessaire */
        if (process_system_ready && ticks_elapsed > 0 && !get_critical_section()) {
            /* Faire du scheduling pour chaque tick écoulé */
            for (uint32_t i = 0; i < ticks_elapsed; i++) {
                if (software_system_ticks % 10 == 0) {  /* Tous les 100ms */
                    //schedule();
                    yield();
                }
            }
        }
    }
}


void trigger_timer_interrupt(void)
{
    extern int kprintf(const char *format, ...);
    
    KINFO("[TIMER] Forcing timer interrupt...\n");
    
    /* Programmer une interruption dans 200 ticks */
    uint64_t current = get_timer_count();
    uint32_t target = (uint32_t)current + 200;
    
    set_cntp_ctl(0);  /* Disable */
    set_cntp_tval(target);  /* Set */
    set_cntp_ctl(CNTP_CTL_ENABLE);  /* Enable */
    
    KINFO("[TIMER] Timer interrupt scheduled in 200 ticks\n");
}

void timer_set_timeout(uint64_t timeout_ticks)
{
    /* Desactiver le timer */
    set_cntp_ctl(0);
    
    uint32_t cpu = smp_processor_id();
    uint64_t current = get_timer_count();
    uint64_t compare = current + timeout_ticks;
    
    /* Programmer la nouvelle valeur */
    if (cpu < ARMOS_MAX_CPUS)
        timer_next_compare[cpu] = compare;
    timer_set_compare(compare);
    
    /* Reactiver le timer */
    set_cntp_ctl(CNTP_CTL_ENABLE);
}

void timer_init_local_cpu(void)
{
    uint32_t timer_ctrl = 0;
    uint32_t timer_freq = get_timer_frequency();
    uint32_t cpu = smp_processor_id();

    /*
     * The ARM generic timer is per-CPU. This helper only programs the local
     * CP15 timer registers; the caller must enable the matching GIC PPI for
     * that CPU before expecting IRQ delivery.
     */
    set_cntp_ctl(timer_ctrl);
    if (cpu < ARMOS_MAX_CPUS)
        timer_next_compare[cpu] = 0;
    timer_program_next_tick(timer_freq);
}

/* ARM generic timer initialization for the active platform. */
void init_timer(void)
{
    extern int kprintf(const char *format, ...);
    uint32_t timer_freq;
    uint32_t cntfrq;
    uint32_t timer_irq;

    KINFO("[TIMER] Starting ARM Generic Timer initialization...\n");

    /* 1. Read the effective platform timer frequency. */
    timer_freq = get_timer_frequency();
    cntfrq = get_cntfrq();

    if (arch_platform_timer_force_hz()) {
        KWARN("[TIMER] CNTFRQ=%u Hz, using platform timer override %u Hz\n",
              cntfrq, timer_freq);
    } else if (cntfrq == 0) {
        KWARN("[TIMER] Timer frequency is 0, using platform fallback %u Hz\n",
              timer_freq);
    } else if (timer_freq != cntfrq) {
        KWARN("[TIMER] CNTFRQ=%u Hz, using platform timer quirk %u Hz\n",
              cntfrq, timer_freq);
    }

    KINFO("[TIMER] Timer frequency: %u Hz\n", timer_freq);

    /*
     * Anchor CLOCK_REALTIME once, then advance it with the common scheduler
     * clock.  Reading a seconds-only platform RTC for every call made
     * CLOCK_REALTIME jump in one-second steps and broke POSIX timed I/O.
     * Hardware access remains behind arch_platform; interpolation is common.
     */
    realtime_epoch_seconds = get_current_time();
    realtime_epoch_ticks = get_system_ticks();
    realtime_clock_initialized = true;
    
    /* 2. Calculer l'interval pour TIMER_FREQ Hz */
    uint32_t interval = timer_interval_from_frequency(timer_freq);
    KINFO("[TIMER] Timer interval: %u counter ticks (%u us)\n",
            interval, 1000000 / TIMER_FREQ);
    
    /* 3. Configurer le timer EL1 (non-secure) du CPU courant. */
    timer_init_local_cpu();
    
    KINFO("[TIMER] ARM Generic Timer configured\n");
    
    /* 4. Enable the timer IRQ through the interrupt-controller facade. */
    timer_irq = arch_platform_timer_irq();
    KINFO("[TIMER] Enabling timer IRQ (%u)...\n", timer_irq);

    irq_enable_level(timer_irq);
    
    /* 5. Activer les interruptions au niveau CPU */
    arch_enable_interrupts();
    
    KINFO("[TIMER] Timer initialization complete OK\n");
}

void timer_irq_handler(void)
{
    uint32_t cpu_id = smp_processor_id();
    uint32_t elapsed_ticks;
    uint32_t irq_ticks = 0;
    uint32_t execution_ticks;
    task_t* current = NULL;

    /* 1. Acquitter l'interruption ARM Generic Timer */
    uint32_t timer_ctrl = get_cntp_ctl();
    
    /* Clear l'interrupt status */
    timer_ctrl |= 0x4;  /* Set ISTATUS bit */
    set_cntp_ctl(timer_ctrl);

    irq_ack(arch_platform_timer_irq());
    
    /* 2. Program the next absolute compare and catch up delayed ticks. */
    elapsed_ticks = timer_program_next_tick(get_timer_frequency());
    execution_ticks = elapsed_ticks;
    
    /* 4. Per-CPU accounting, plus one global wall-clock on the boot CPU. */
    if (cpu_id < ARMOS_MAX_CPUS)
        timer_cpu_ticks[cpu_id] += elapsed_ticks;
    if (smp_is_boot_cpu()) {
        system_ticks += elapsed_ticks;
        if (process_system_ready)
            scheduler_loadavg_tick(elapsed_ticks);
    }

    if (cpu_id < ARMOS_MAX_CPUS) {
        uint64_t frequency = get_timer_frequency();
        uint64_t pending = timer_irq_pending_cycles[cpu_id];
        uint64_t scaled;

        timer_irq_pending_cycles[cpu_id] = 0;
        scaled = timer_irq_fraction[cpu_id] + pending * TIMER_FREQ;
        if (frequency != 0) {
            while (scaled >= frequency) {
                scaled -= frequency;
                irq_ticks++;
            }
            timer_irq_fraction[cpu_id] = scaled;
        }
        if (irq_ticks > elapsed_ticks)
            irq_ticks = elapsed_ticks;

        execution_ticks = elapsed_ticks - irq_ticks;
        timer_cpu_accounting[cpu_id].irq_ticks += irq_ticks;

        if (process_system_ready)
            current = task_current_on_cpu(cpu_id);

        if (current && task_is_idle_task(current)) {
            timer_cpu_accounting[cpu_id].idle_ticks += execution_ticks;
        } else if (current && current->type == TASK_TYPE_PROCESS &&
                   timer_irq_interrupted_user[cpu_id]) {
            timer_cpu_accounting[cpu_id].user_ticks += execution_ticks;
        } else {
            timer_cpu_accounting[cpu_id].system_ticks += execution_ticks;
        }
    }

    /*
     * Some UART backends can miss a TX wake-up under dense console bursts.
     * Poll the TTY TX ring from the periodic timer as a safety net. Keep this
     * on the boot CPU so future local timers do not make several CPUs compete
     * for the same character backend.
     */
    if (smp_is_boot_cpu() && tty_has_pending_output())
        tty_drain_output();
    
    /* 5. RÉDUIRE drastiquement les messages */
    if (system_ticks % (TIMER_FREQ * 10) == 0) {  /* Toutes les 10 secondes seulement */
        //KINFO("[TIMER] System uptime: %u seconds -> %s\n", system_ticks / TIMER_FREQ, current_task->name);
    }
    
    /* 6. Scheduling sans messages debug */
    if (process_system_ready) {
        if (!current)
            current = task_current_on_cpu(cpu_id);
        
        if (current && !task_is_idle_task(current) && current->state == TASK_RUNNING) {
            current->total_runtime += execution_ticks;
            if (current->type == TASK_TYPE_PROCESS &&
                cpu_id < ARMOS_MAX_CPUS &&
                timer_irq_interrupted_user[cpu_id])
                current->user_runtime += execution_ticks;
            else
                current->system_runtime += execution_ticks;
            if (current->sched_debt <= 0xffffffffu - elapsed_ticks)
                current->sched_debt += elapsed_ticks;
            else
                current->sched_debt = 0xffffffffu;
        }

        if (current && !task_is_idle_task(current) && !get_critical_section()) {
            if (current->quantum_left > elapsed_ticks)
                current->quantum_left -= elapsed_ticks;
            else
                current->quantum_left = 0;

            if (current->quantum_left == 0) {
                current->quantum_left = QUANTUM_TICKS;
                //current_task->state = TASK_READY;
                scheduler_request_resched_current_cpu();
                //KDEBUG("YIELDING BECAUSE OF TIMER\n");
                //yield();
            }
        }
    }
}

/* Fonction pour activer le scheduling (appelee apres init_processes) */
void timer_enable_scheduling(void)
{
    extern int kprintf(const char *format, ...);
    KINFO("[TIMER] Process scheduling enabled OK\n");
    process_system_ready = true;
}

/* Fonction pour desactiver le scheduling */
void timer_disable_scheduling(void)
{
    process_system_ready = false;
}

uint32_t get_timer_frequency(void)
{
    return arch_platform_timer_effective_hz(get_cntfrq());
}

/* Fonction pour obtenir le compte du timer */
uint64_t get_timer_count(void)
{
    /* Lire le compteur 64-bit du timer */
    return get_cntpct();
}

/* Fonction pour obtenir le nombre de ticks systeme */
uint32_t get_system_ticks(void)
{
    /* Mettre à jour avant de retourner */
    //update_timer_software();
    return system_ticks;
}

uint32_t timer_cpu_tick_count(uint32_t cpu_id)
{
    if (cpu_id >= ARMOS_MAX_CPUS)
        return 0;
    return timer_cpu_ticks[cpu_id];
}

void timer_accounting_irq_enter(bool interrupted_user)
{
    uint32_t cpu_id = smp_processor_id();

    if (cpu_id >= ARMOS_MAX_CPUS)
        return;

    if (timer_irq_depth[cpu_id]++ == 0) {
        timer_irq_interrupted_user[cpu_id] = interrupted_user;
        timer_irq_start[cpu_id] = get_timer_count();
    }
}

void timer_accounting_irq_exit(void)
{
    uint32_t cpu_id = smp_processor_id();
    uint64_t now;

    if (cpu_id >= ARMOS_MAX_CPUS || timer_irq_depth[cpu_id] == 0)
        return;

    if (--timer_irq_depth[cpu_id] != 0)
        return;

    now = get_timer_count();
    timer_irq_pending_cycles[cpu_id] += now - timer_irq_start[cpu_id];
}

void timer_cpu_accounting_read(uint32_t cpu_id, timer_cpu_accounting_t* accounting)
{
    if (!accounting)
        return;

    memset(accounting, 0, sizeof(*accounting));
    if (cpu_id >= ARMOS_MAX_CPUS)
        return;

    accounting->user_ticks = timer_cpu_accounting[cpu_id].user_ticks;
    accounting->system_ticks = timer_cpu_accounting[cpu_id].system_ticks;
    accounting->irq_ticks = timer_cpu_accounting[cpu_id].irq_ticks;
    accounting->idle_ticks = timer_cpu_accounting[cpu_id].idle_ticks;
}

/* Fonction pour obtenir le temps en millisecondes */
uint32_t get_time_ms(void)
{
    return (get_system_ticks() * 1000) / TIMER_FREQ;
}

void get_realtime(uint64_t *seconds, uint32_t *nanoseconds)
{
    uint32_t elapsed_ticks;

    if (!seconds || !nanoseconds)
        return;
    if (!realtime_clock_initialized) {
        *seconds = get_current_time();
        *nanoseconds = 0;
        return;
    }

    elapsed_ticks = get_system_ticks() - realtime_epoch_ticks;
    *seconds = (uint64_t)realtime_epoch_seconds + elapsed_ticks / TIMER_FREQ;
    *nanoseconds = (elapsed_ticks % TIMER_FREQ) *
                   (1000000000u / TIMER_FREQ);
}

/* Fonction de debug pour verifier l'etat du timer */
void debug_timer_state(void)
{
    extern int kprintf(const char *format, ...);
    
    KINFO("[TIMER] === TIMER STATE DEBUG ===\n");
    
    /* Lire le registre de controle */
    uint32_t timer_ctrl = get_cntp_ctl();
    
    KINFO("[TIMER] Control register: 0x%08X\n", timer_ctrl);
    KINFO("[TIMER]   Enable: %s\n", (timer_ctrl & 0x1) ? "YES" : "NO");
    KINFO("[TIMER]   Mask: %s\n", (timer_ctrl & 0x2) ? "YES" : "NO");
    KINFO("[TIMER]   Status: %s\n", (timer_ctrl & 0x4) ? "PENDING" : "CLEAR");
    
    /* Lire la valeur de comparaison */
    uint32_t timer_value = get_cntp_tval();
    KINFO("[TIMER] Compare value: %u\n", timer_value);
    
    /* Lire le compteur actuel */
    uint64_t counter = get_timer_count();
    KINFO("[TIMER] Current count: %u%u\n", (uint32_t)(counter >> 32), (uint32_t)counter);
    
    KINFO("[TIMER] System ticks: %u\n", system_ticks);
    KINFO("[TIMER] Timer frequency: %u Hz\n", get_timer_frequency());
    
    KINFO("[TIMER] === END DEBUG ===\n");
}

/* Stub temporaire pour wake_up_sleeping_processes */
void wake_up_sleeping_processes(void)
{
    /* Cette fonction sera implementee dans process.c */
}



/* Nombre de jours par mois (annee non bissextile) */
static const int days_in_month[] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

bool is_leap_year(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

int get_days_in_month(int month, int year)
{
    if (month < 1 || month > 12)
        return 0;

    if (month == 2 && is_leap_year(year))
        return 29;

    return days_in_month[month - 1];
}

/* Convertir timestamp Unix en structure datetime */
void unix_to_datetime(uint32_t unix_time, datetime_t* dt) {
    /* Nombre de secondes par jour */
    const uint32_t SECONDS_PER_DAY = 86400;
    
    /* Epoch Unix commence le 1er janvier 1970 */
    uint32_t days_since_epoch = unix_time / SECONDS_PER_DAY;
    uint32_t seconds_today = unix_time % SECONDS_PER_DAY;
    
    /* Calculer l'heure, minute, seconde */
    dt->hour = seconds_today / 3600;
    dt->minute = (seconds_today % 3600) / 60;
    dt->second = seconds_today % 60;
    
    /* Calculer l'année */
    int year = 1970;
    uint32_t days_remaining = days_since_epoch;
    
    while (days_remaining >= (is_leap_year(year) ? 366 : 365)) {
        if (is_leap_year(year)) {
            days_remaining -= 366;
        } else {
            days_remaining -= 365;
        }
        year++;
    }
    dt->year = year;
    
    /* Calculer le mois et le jour */
    int month = 1;
    while (days_remaining >= (uint32_t)get_days_in_month(month, year)) {
        days_remaining -= get_days_in_month(month, year);
        month++;
    }
    dt->month = month;
    dt->day = days_remaining + 1;  /* +1 car les jours commencent à 1 */
}


/* Si vous avez un RTC sur votre board */
uint32_t get_current_time(void) {
    /* PL031 runtime via l'alias MMIO prive TTBR1. */
    vaddr_t rtc_addr = arch_platform_kernel_mmio_rtc_base();
    if (!rtc_addr) {
        return get_system_ticks() / TIMER_FREQ;
    }

    volatile uint32_t* rtc_base = (volatile uint32_t*)(uintptr_t)rtc_addr;
    
    /* Le PL031 donne directement un timestamp Unix */
    return rtc_base[0];  /* RTC Data Register */
}
