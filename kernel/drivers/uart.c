/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: kernel/drivers/uart.c
 * Layer: Kernel / terminal and character devices
 *
 * Responsibilities:
 * - Drive UART/framebuffer console backends and TTY line discipline.
 * - Preserve canonical/raw terminal semantics and job-control signals.
 *
 * Notes:
 * - The platform selects the logical TTY served by this transport.
 */

#include <kernel/types.h>
#include <kernel/arch_platform.h>
#include <kernel/string.h>
#include <kernel/spinlock.h>
#include <kernel/vfs.h>
#include <kernel/userspace.h>
#include <kernel/tty.h>
#include <kernel/uart.h>
#include <kernel/kprintf.h>
#include <kernel/arch_barrier.h>
#include <kernel/arch_cpu.h>
#include <kernel/interrupt.h>

/*
 * Avant l'activation de la MMU, l'UART doit etre accedee par son adresse
 * physique. early_init() bascule cette base vers l'alias prive TTBR1 des que
 * setup_mmu() a termine.
 */
static uintptr_t uart_mmio_base;

static inline void uart_ensure_mmio_base(void)
{
    if (uart_mmio_base == 0)
        uart_mmio_base = (uintptr_t)arch_platform_uart0_phys_base();
}

#if defined(ARMOS_PLATFORM_RASPBERRYPI)
#define RASPI2_GPIO_BASE          0x3F200000u
#define RASPI2_GPFSEL1            0x04u
#define RASPI2_GPPUD              0x94u
#define RASPI2_GPPUDCLK0          0x98u
#define RASPI2_GPIO14             14u
#define RASPI2_GPIO15             15u
#define RASPI2_GPIO_ALT0          4u
#define RASPI2_MBOX_BASE          0x3F00B880u
#define RASPI2_MBOX_READ          0x00u
#define RASPI2_MBOX_STATUS        0x18u
#define RASPI2_MBOX_WRITE         0x20u
#define RASPI2_MBOX_FULL          0x80000000u
#define RASPI2_MBOX_EMPTY         0x40000000u
#define RASPI2_MBOX_CH_PROP       8u
#define RASPI2_MBOX_REQUEST       0u
#define RASPI2_MBOX_RESPONSE_OK   0x80000000u
#define RASPI2_MBOX_TAG_SETCLKRATE 0x00038002u
#define RASPI2_MBOX_CLOCK_UART    2u

static volatile uint32_t raspi2_mbox[9] __attribute__((aligned(16)));
static bool raspi2_uart_clock_prepared;

static void raspi2_delay(unsigned count)
{
    while (count-- > 0)
        __asm__ volatile("nop");
}

static bool raspi2_mbox_call(uint8_t channel)
{
    volatile uint32_t* mbox = (volatile uint32_t*)(uintptr_t)RASPI2_MBOX_BASE;
    uint32_t request = ((uint32_t)(uintptr_t)raspi2_mbox & ~0xFu) |
                       (channel & 0xFu);
    uint32_t timeout;

    timeout = 1000000u;
    while ((mbox[RASPI2_MBOX_STATUS / 4u] & RASPI2_MBOX_FULL) && --timeout)
        ;
    if (timeout == 0)
        return false;

    mbox[RASPI2_MBOX_WRITE / 4u] = request;

    timeout = 1000000u;
    while (timeout-- > 0) {
        uint32_t response;

        while ((mbox[RASPI2_MBOX_STATUS / 4u] & RASPI2_MBOX_EMPTY) && --timeout)
            ;
        if (timeout == 0)
            return false;

        response = mbox[RASPI2_MBOX_READ / 4u];
        if (response == request)
            return raspi2_mbox[1] == RASPI2_MBOX_RESPONSE_OK;
    }

    return false;
}

static void uart_platform_prepare_clock(void)
{
    /*
     * Match the well-known Pi3 UART0 bring-up sequence: ask VideoCore to run
     * PL011 from a stable 4 MHz clock, then program divisors for 115200 baud.
     * This avoids relying on firmware defaults or config.txt init_uart_clock.
     */
    if (raspi2_uart_clock_prepared)
        return;

    raspi2_mbox[0] = sizeof(raspi2_mbox);
    raspi2_mbox[1] = RASPI2_MBOX_REQUEST;
    raspi2_mbox[2] = RASPI2_MBOX_TAG_SETCLKRATE;
    raspi2_mbox[3] = 12;
    raspi2_mbox[4] = 8;
    raspi2_mbox[5] = RASPI2_MBOX_CLOCK_UART;
    raspi2_mbox[6] = arch_platform_uart0_clock_hz();
    raspi2_mbox[7] = 0;
    raspi2_mbox[8] = 0;
    (void)raspi2_mbox_call(RASPI2_MBOX_CH_PROP);
    raspi2_uart_clock_prepared = true;
}

static void uart_platform_configure_pins(void)
{
    volatile uint32_t* gpio = (volatile uint32_t*)(uintptr_t)RASPI2_GPIO_BASE;
    uint32_t gpfsel1;

    /*
     * Real Raspberry Pi 2 boards need GPIO14/GPIO15 muxed to ALT0 for PL011
     * TXD0/RXD0. QEMU accepts UART MMIO without this, which can hide the issue.
     */
    gpfsel1 = gpio[RASPI2_GPFSEL1 / 4u];
    gpfsel1 &= ~((7u << 12) | (7u << 15));
    gpfsel1 |= (RASPI2_GPIO_ALT0 << 12) | (RASPI2_GPIO_ALT0 << 15);
    gpio[RASPI2_GPFSEL1 / 4u] = gpfsel1;

    gpio[RASPI2_GPPUD / 4u] = 0;
    raspi2_delay(150);
    gpio[RASPI2_GPPUDCLK0 / 4u] = (1u << RASPI2_GPIO14) | (1u << RASPI2_GPIO15);
    raspi2_delay(150);
    gpio[RASPI2_GPPUDCLK0 / 4u] = 0;
}
#else
static void uart_platform_prepare_clock(void)
{
}

static void uart_platform_configure_pins(void)
{
}
#endif

/* Registres PL011 UART */
#define UART_REG(offset) (*(volatile uint32_t*)(uart_mmio_base + (offset)))
#define UART_DR         UART_REG(0x00)  /* Data */
#define UART_RSR        UART_REG(0x04)  /* Receive Status */
#define UART_FR         UART_REG(0x18)  /* Flag */
#define UART_ILPR       UART_REG(0x20)  /* IrDA Low-power */
#define UART_IBRD       UART_REG(0x24)  /* Integer Baud Rate */
#define UART_FBRD       UART_REG(0x28)  /* Fractional Baud Rate */
#define UART_LCRH       UART_REG(0x2C)  /* Line Control */
#define UART_CR         UART_REG(0x30)  /* Control */
#define UART_IFLS       UART_REG(0x34)  /* Interrupt FIFO Level */
#define UART_IMSC       UART_REG(0x38)  /* Interrupt Mask */
#define UART_RIS        UART_REG(0x3C)  /* Raw Interrupt Status */
#define UART_MIS        UART_REG(0x40)  /* Masked Interrupt Status */
#define UART_ICR        UART_REG(0x44)  /* Interrupt Clear */

/* Bits UART_DR / UART_RSR d'erreur reception */
#define UART_DR_DATA    0x000000FFu
#define UART_DR_FE      (1 << 8)  /* Framing error */
#define UART_DR_PE      (1 << 9)  /* Parity error */
#define UART_DR_BE      (1 << 10) /* Break error */
#define UART_DR_OE      (1 << 11) /* Overrun error */
#define UART_DR_ERR     (UART_DR_FE | UART_DR_PE | UART_DR_BE | UART_DR_OE)

/* Bits UART interrupt PL011 */
#define UART_INT_RX     (1 << 4)  /* Receive FIFO interrupt */
#define UART_INT_TX     (1 << 5)  /* Transmit FIFO interrupt */
#define UART_INT_RT     (1 << 6)  /* Receive timeout interrupt */
#define UART_INT_ERR    ((1 << 7) | (1 << 8) | (1 << 9) | (1 << 10))

/* Bits de controle UART_FR (Flag Register) */
#define UART_FR_TXFE    (1 << 7)  /* Transmit FIFO empty */
#define UART_FR_RXFF    (1 << 6)  /* Receive FIFO full */
#define UART_FR_TXFF    (1 << 5)  /* Transmit FIFO full */
#define UART_FR_RXFE    (1 << 4)  /* Receive FIFO empty */
#define UART_FR_BUSY    (1 << 3)  /* UART busy */

/* Bits de controle UART_CR (Control Register) */
#define UART_CR_CTSEN   (1 << 15) /* CTS hardware flow control enable */
#define UART_CR_RTSEN   (1 << 14) /* RTS hardware flow control enable */
#define UART_CR_RTS     (1 << 11) /* Request to send */
#define UART_CR_RXE     (1 << 9)  /* Receive enable */
#define UART_CR_TXE     (1 << 8)  /* Transmit enable */
#define UART_CR_LBE     (1 << 7)  /* Loopback enable */
#define UART_CR_UARTEN  (1 << 0)  /* UART enable */

/* PL011 interrupt FIFO levels. RX at 1/8 favors interactive reliability. */
#define UART_IFLS_TX_1_2  (2u << 0)
#define UART_IFLS_RX_1_8  (0u << 3)

/* Bits de controle UART_LCRH (Line Control Register) */
#define UART_LCRH_SPS   (1 << 7)  /* Stick parity select */
#define UART_LCRH_WLEN_8 (3 << 5) /* Word length 8 bits */
#define UART_LCRH_WLEN_7 (2 << 5) /* Word length 7 bits */
#define UART_LCRH_WLEN_6 (1 << 5) /* Word length 6 bits */
#define UART_LCRH_WLEN_5 (0 << 5) /* Word length 5 bits */
#define UART_LCRH_FEN   (1 << 4)  /* FIFO enable */
#define UART_LCRH_STP2  (1 << 3)  /* Two stop bits select */
#define UART_LCRH_EPS   (1 << 2)  /* Even parity select */
#define UART_LCRH_PEN   (1 << 1)  /* Parity enable */
#define UART_LCRH_BRK   (1 << 0)  /* Send break */

/* Variable globale */
DEFINE_SPINLOCK(uart_lock);

static volatile uint32_t uart_rx_irq_count;
static volatile uint32_t uart_tx_irq_count;
static volatile uint32_t uart_err_irq_count;
static volatile uint32_t uart_rx_char_count;
static volatile uint32_t uart_frame_error_count;
static volatile uint32_t uart_parity_error_count;
static volatile uint32_t uart_break_error_count;
static volatile uint32_t uart_overrun_error_count;
static bool uart_rx_interrupts_enabled;

static void uart_record_rx_status(uint32_t dr)
{
    if ((dr & UART_DR_ERR) == 0)
        return;

    if (dr & UART_DR_FE)
        uart_frame_error_count++;
    if (dr & UART_DR_PE)
        uart_parity_error_count++;
    if (dr & UART_DR_BE)
        uart_break_error_count++;
    if (dr & UART_DR_OE)
        uart_overrun_error_count++;

    UART_RSR = 0;
}

static void uart_configure_baud(void)
{
    uint32_t clock_hz = arch_platform_uart0_clock_hz();
    uint32_t baud = arch_platform_uart0_baud();
    uint32_t divisor64;

    if (clock_hz == 0 || baud == 0)
        return;

    uart_ensure_mmio_base();

    /*
     * PL011 baud divisor:
     *   bauddiv = uartclk / (16 * baud)
     *   IBRD = floor(bauddiv)
     *   FBRD = round((bauddiv - IBRD) * 64)
     *
     * Computing bauddiv * 64 is equivalent to uartclk * 4 / baud. Keeping the
     * clock in the platform contract lets raspi2 provide its own value without
     * touching the console driver.
     */
    divisor64 = ((clock_hz * 4u) + (baud / 2u)) / baud;
    if (divisor64 == 0)
        divisor64 = 1;

    UART_IBRD = divisor64 / 64u;
    UART_FBRD = divisor64 % 64u;
}

void uart_use_kernel_mmio_alias(void)
{
    uart_mmio_base = (uintptr_t)arch_platform_uart0_kernel_base();
    arch_data_sync_barrier();
    arch_instruction_sync_barrier();
}

/*
 * Initialisation de l'UART PL011
 */
void uart_init(void)
{
    unsigned rx_timeout;

    uart_ensure_mmio_base();
    __atomic_store_n(&uart_rx_interrupts_enabled, false, __ATOMIC_RELAXED);

    /* Desactiver l'UART */
    UART_CR = 0;
    UART_IMSC = 0;
    UART_ICR = 0x7FF;
    UART_RSR = 0;

    uart_platform_prepare_clock();
    uart_platform_configure_pins();
    
    /* Vider les FIFOs, sans jamais bloquer le boot sur un etat materiel. */
    rx_timeout = 1024;
    while (!(UART_FR & UART_FR_RXFE) && rx_timeout-- > 0) {
        (void)UART_DR;
    }
    
    uart_configure_baud();
    
    UART_IFLS = UART_IFLS_TX_1_2 | UART_IFLS_RX_1_8;

    /* Configurer le format de ligne */
    UART_LCRH = UART_LCRH_WLEN_8 | UART_LCRH_FEN;
    
    /* Effacer toutes les interruptions */
    UART_ICR = 0x7FF;
    UART_IMSC = 0;
    
    /* Activer UART, TX et RX */
    UART_CR = UART_CR_UARTEN | UART_CR_TXE | UART_CR_RXE | UART_CR_RTS;

}

void uart_enable_rx_interrupts(void)
{
    unsigned long flags;

    uart_ensure_mmio_base();
    spin_lock_irqsave(&uart_lock, &flags);
    UART_ICR = UART_INT_RX | UART_INT_RT | UART_INT_ERR;
    UART_RSR = 0;
    UART_IMSC |= UART_INT_RX | UART_INT_RT | UART_INT_ERR;
    __atomic_store_n(&uart_rx_interrupts_enabled, true, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&uart_lock, flags);

    irq_enable_level(arch_platform_uart_irq());
}

/*
 * Envoyer un caractere
 */
static void uart_putc_unlocked(char c)
{
    uart_ensure_mmio_base();

    int timeout = 100000;
    
    /* Attendre que le FIFO TX ne soit pas plein avec timeout */
    while ((UART_FR & UART_FR_TXFF) && timeout > 0) {
        timeout--;
        for (volatile int i = 0; i < 10; i++);
    }
    
    if (timeout > 0) {
        UART_DR = c;
    }
}

void uart_putc(char c)
{
    unsigned long flags;

    /*
     * Before the MMU establishes normal memory attributes, ARM exclusive
     * monitors used by spinlocks are not a reliable contract on real Pi
     * hardware. Early boot is single-core with IRQ/FIQ masked, so polling is
     * the correct recovery-console path here.
     */
    if (!arch_mmu_enabled()) {
        uart_putc_unlocked(c);
        return;
    }

    spin_lock_irqsave(&uart_lock, &flags);
    uart_putc_unlocked(c);
    spin_unlock_irqrestore(&uart_lock, flags);
}

bool uart_tx_ready(void)
{
    uart_ensure_mmio_base();
    return (UART_FR & UART_FR_TXFF) == 0;
}

bool uart_try_putc(char c)
{
    unsigned long flags;
    bool written = false;

    uart_ensure_mmio_base();
    spin_lock_irqsave(&uart_lock, &flags);
    if ((UART_FR & UART_FR_TXFF) == 0) {
        UART_DR = c;
        written = true;
    }
    spin_unlock_irqrestore(&uart_lock, flags);

    return written;
}

void uart_set_tx_irq_enabled(bool enabled)
{
    unsigned long flags;

    uart_ensure_mmio_base();
    spin_lock_irqsave(&uart_lock, &flags);
    if (enabled)
        UART_IMSC |= UART_INT_TX;
    else
        UART_IMSC &= ~UART_INT_TX;
    spin_unlock_irqrestore(&uart_lock, flags);
}

void uart_get_stats(uart_stats_t *stats)
{
    if (!stats)
        return;

    uart_ensure_mmio_base();
    stats->rx_irq = uart_rx_irq_count;
    stats->tx_irq = uart_tx_irq_count;
    stats->err_irq = uart_err_irq_count;
    stats->rx_chars = uart_rx_char_count;
    stats->frame_errors = uart_frame_error_count;
    stats->parity_errors = uart_parity_error_count;
    stats->break_errors = uart_break_error_count;
    stats->overrun_errors = uart_overrun_error_count;
    stats->fr = UART_FR;
    stats->rsr = UART_RSR;
    stats->imsc = UART_IMSC;
    stats->mis = UART_MIS;
}

/*
 * Envoyer une chaine de caracteres
 */
void uart_puts(const char* str)
{

    while (*str) {
        /* Convertir LF en CRLF pour compatibilite terminal */
        if (*str == '\n') {
            uart_putc('\r');
        }
        uart_putc(*str++);
    }

}

/*
 * Recevoir un caractere (non-bloquant)
 */
int uart_getc(void)
{
    uint32_t dr;

    uart_ensure_mmio_base();

    /* Verifier si des donnees sont disponibles */
    if (UART_FR & UART_FR_RXFE) {
        return -1;  /* Pas de donnees */
    }

    /* Lire le caractere */
    dr = UART_DR;
    uart_record_rx_status(dr);
    uart_rx_char_count++;
    return (int)(dr & UART_DR_DATA);
}

/*
 * Afficher un nombre en hexadecimal
 */
void uart_put_hex(uint32_t value)
{
    int shift;

    for (shift = 28; shift >= 0; shift -= 4) {
        uint32_t nibble = (value >> shift) & 0xFu;
        uart_putc((char)(nibble < 10 ? ('0' + nibble)
                                      : ('A' + nibble - 10)));
    }
}

/*
 * Afficher un nombre en decimal
 */
void uart_put_dec(uint32_t value)
{

    char buffer[11];  /* Assez pour 2^32 */
    int i = 0;
    
    if (value == 0) {
        uart_putc('0');
        return;
    }
    
    /* Convertir en chaine (ordre inverse) */
    while (value > 0) {
        buffer[i++] = '0' + (value % 10);
        value /= 10;
    }
    
    /* Afficher dans le bon ordre */
    while (i > 0) {
        uart_putc(buffer[--i]);
    }
}

/*
 * Afficher un nombre binaire (utile pour les registres)
 */
void uart_put_bin(uint32_t value, int bits)
{

    int i;
    
    uart_puts("0b");
    
    for (i = bits - 1; i >= 0; i--) {
        uart_putc((value & (1 << i)) ? '1' : '0');
        
        /* Ajouter un espace tous les 4 bits pour lisibilite */
        if (i > 0 && (i % 4) == 0) {
            uart_putc(' ');
        }
    }
}

/*
 * Fonction printf simplifiee pour debug
 */
void uart_printf(const char* format, ...)
{

    /* Implementation basique pour %d, %x, %s */
    const char* p = format;
    
    /* Note: Implementation simplifiee sans va_list pour eviter 
     * les dependances sur stdarg.h dans un kernel minimal */
    
    while (*p) {
        if (*p == '%' && *(p + 1)) {
            p++;  /* Passer le '%' */
            switch (*p) {
                case '%':
                    uart_putc('%');
                    break;
                case 'c':
                    uart_putc('?');  /* Placeholder */
                    break;
                case 's':
                    uart_puts("(str)");  /* Placeholder */
                    break;
                case 'd':
                    uart_puts("(dec)");  /* Placeholder */
                    break;
                case 'x':
                    uart_puts("(hex)");  /* Placeholder */
                    break;
                default:
                    uart_putc(*p);
                    break;
            }
        } else {
            uart_putc(*p);
        }
        p++;
    }
}

/*
 * Test de boucle-back UART (diagnostic)
 */
bool uart_test_loopback(void)
{
    const char test_char = 'A';
    char received;

    uart_ensure_mmio_base();
    
    /* Activer le mode loopback */
    UART_CR |= UART_CR_LBE;
    
    /* Envoyer un caractere */
    uart_putc(test_char);
    
    /* Attendre un peu */
    for (volatile int i = 0; i < 1000; i++);
    
    /* Essayer de le recevoir */
    received = uart_getc();
    
    /* Desactiver le loopback */
    UART_CR &= ~UART_CR_LBE;
    
    return (received == test_char);
}

/*
 * Afficher l'etat des registres UART (debug)
 */
void uart_dump_registers(void)
{
    uart_ensure_mmio_base();

    uart_puts("\n=== etat UART PL011 ===\n");
    
    uart_puts("FR (Flag):     0x");
    uart_put_hex(UART_FR);
    uart_puts(" = ");
    uart_put_bin(UART_FR, 8);
    uart_puts("\n");
    
    uart_puts("CR (Control):  0x");
    uart_put_hex(UART_CR);
    uart_puts(" = ");
    uart_put_bin(UART_CR, 16);
    uart_puts("\n");
    
    uart_puts("LCRH (Line):   0x");
    uart_put_hex(UART_LCRH);
    uart_puts(" = ");
    uart_put_bin(UART_LCRH, 8);
    uart_puts("\n");
    
    uart_puts("IBRD (Baud):   ");
    uart_put_dec(UART_IBRD);
    uart_puts("\n");
    
    uart_puts("FBRD (Frac):   ");
    uart_put_dec(UART_FBRD);
    uart_puts("\n");
    
    uart_puts("========================\n\n");
}

void putchar_kernel(char c) {
    if (c == '\n')
        uart_putc('\r');
    uart_putc(c);
}

void uart_flush(void)
{
    int timeout = 100000;

    uart_ensure_mmio_base();
    
    /* Utiliser directement UART_FR qui est deja defini */
    while (!(UART_FR & UART_FR_TXFE) && timeout > 0) {
        timeout--;
        for (volatile int i = 0; i < 10; i++);
    }
    
    /* Pause finale pour etre s-r */
    for (volatile int i = 0; i < 1000; i++);
}

static bool uart_rx_has_data(void)
{
    uart_ensure_mmio_base();

    /* RXFE is set when the PL011 receive FIFO is empty. */
    return (UART_FR & UART_FR_RXFE) == 0;
}

bool uart_has_data(void)
{
    /*
     * Once RX interrupts are enabled, the IRQ handler is the sole FIFO
     * consumer. Allowing tty_read() to poll it concurrently can make CPU A
     * read byte N+1 and enqueue it before CPU B enqueues byte N.
     */
    return !__atomic_load_n(&uart_rx_interrupts_enabled, __ATOMIC_ACQUIRE) &&
           uart_rx_has_data();
}

static const tty_backend_ops_t uart_tty_backend = {
    .putc = uart_putc,
    .try_putc = uart_try_putc,
    .puts = uart_puts,
    .set_tx_irq_enabled = uart_set_tx_irq_enabled,
    .has_data = uart_has_data,
    .getc = uart_getc,
};

static int uart_tty_id = TTY_CONSOLE_ID;

void uart_attach_tty_backend(void)
{
    uart_attach_tty_backend_to(TTY_CONSOLE_ID);
}

void uart_attach_tty_backend_to(int tty_id)
{
    if (tty_attach_backend_to(tty_id, &uart_tty_backend) == 0)
        uart_tty_id = tty_id;
}

int uart_mirror_tty_output_to(int tty_id)
{
    return tty_attach_output_mirror_to(tty_id, &uart_tty_backend);
}

int uart_attached_tty_id(void)
{
    return uart_tty_id;
}

static ssize_t uart_console_write(file_t* file, const void* buf, size_t count) {
    (void) file;
    const char* data = (const char*)buf;
    size_t written = 0;
    
    // Vérifier adresse utilisateur
    //if (!is_valid_user_range(buf, count)) {
    //    return -EFAULT;
    //}
    
    // Écrire via UART caractère par caractère
    for (size_t i = 0; i < count; i++) {
        char c = data[i];
        
        // Utiliser votre fonction UART existante
        uart_putc(c);  // ou uart_write_char(c), selon votre API
        written++;
    }
    
    return written;
}

static ssize_t uart_console_read(file_t* file, void* buf, size_t count) {
    (void) file;

    // Pour stdin via UART (si implémenté)
    char* data = (char*)buf;
    size_t read_count = 0;
    
/*     if (!is_valid_user_range(buf, count)) {
        return -EFAULT;
    } */
    
    for (size_t i = 0; i < count; i++) {
        // Utiliser votre fonction UART de lecture
        if (uart_has_data()) {  // Vérifier si données disponibles
            data[i] = uart_getc();  // ou uart_read_char()
            read_count++;
            
            // Arrêter sur newline pour stdin
            if (data[i] == '\n') {
                read_count++;
                break;
            }
        } else {
            // Pas de données : retourner ce qu'on a lu
            break;
        }
    }
    
    return read_count;
}

// File operations pour console UART
file_operations_t uart_console_fops = {
    .read = uart_console_read,
    .write = uart_console_write,
    .open = NULL,
    .close = NULL,      // Pas de close sur UART
    .lseek = NULL,       // Pas de seek sur UART
    .readdir = NULL
};


// Créer un fichier console UART
file_t* create_uart_console_file(const char* name, int flags) {
    file_t* file = kmalloc(sizeof(file_t));
    if (!file) return NULL;
    
    memset(file, 0, sizeof(file_t));
    
    file->f_op = &uart_console_fops;    // Pointer vers UART operations
    file->flags = flags;
    file->type = FILE_TYPE_TTY;
    file->pos = 0;
    file->inode = NULL;                 // Fichier virtuel
    file->ref_count = 1;
    
    // Debug name
    if (name) {
        strncpy(file->name, name, sizeof(file->name) - 1);
    }
    
    return file;
}

/* Dans uart.c */
void uart_irq_handler(void) {
    uart_ensure_mmio_base();

    uint32_t mis = UART_MIS;  /* Masked Interrupt Status */
    uint32_t handled = mis & (UART_INT_RX | UART_INT_RT | UART_INT_TX | UART_INT_ERR);

    if (mis & (UART_INT_RX | UART_INT_RT))
        uart_rx_irq_count++;
    if (mis & UART_INT_TX)
        uart_tx_irq_count++;
    if (mis & UART_INT_ERR)
        uart_err_irq_count++;

    /* RX ou timeout de reception ? */
    if (mis & (UART_INT_RX | UART_INT_RT | UART_INT_ERR)) {
        /* Lire tous les caractères disponibles */
        while (uart_rx_has_data()) {
            int c = uart_getc();
            if (c < 0) break;
            
            /* Envoyer au TTY */
            tty_input_char_to_id(uart_tty_id, (char)c);
        }
        if (mis & UART_INT_ERR)
            UART_RSR = 0;
    }

    if (mis & UART_INT_TX) {
        tty_drain_output();
    }
    
    if (handled) {
        UART_ICR = handled;
    }
}
