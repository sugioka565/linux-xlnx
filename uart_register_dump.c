/*
 * UART Register Dump Utility
 * Reads hardware registers for AXI UART 16550 debugging
 *
 * Compile: gcc -o uart_register_dump uart_register_dump.c
 * Run: sudo ./uart_register_dump
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>

#define UART_BASE_ADDR   0x43C40000
#define UART_REG_OFFSET  0x1000
#define MAP_SIZE         0x10000

/* UART Register offsets (after reg-offset and reg-shift) */
#define UART_RBR  0x00  /* Receive Buffer Register (Read) */
#define UART_THR  0x00  /* Transmitter Holding Register (Write) */
#define UART_IER  0x04  /* Interrupt Enable Register */
#define UART_IIR  0x08  /* Interrupt Identification Register (Read) */
#define UART_FCR  0x08  /* FIFO Control Register (Write) */
#define UART_LCR  0x0C  /* Line Control Register */
#define UART_MCR  0x10  /* Modem Control Register */
#define UART_LSR  0x14  /* Line Status Register */
#define UART_MSR  0x18  /* Modem Status Register */
#define UART_SCR  0x1C  /* Scratch Register */

/* IER bits */
#define UART_IER_ERBFI  0x01  /* Enable Received Data Available Interrupt */
#define UART_IER_ETBEI  0x02  /* Enable Transmitter Holding Register Empty Interrupt */
#define UART_IER_ELSI   0x04  /* Enable Receiver Line Status Interrupt */
#define UART_IER_EDSSI  0x08  /* Enable Modem Status Interrupt */

/* IIR bits */
#define UART_IIR_NO_INT         0x01  /* No interrupts pending */
#define UART_IIR_ID_MASK        0x0E  /* Interrupt ID mask */
#define UART_IIR_MSI            0x00  /* Modem status interrupt */
#define UART_IIR_THRI           0x02  /* Transmitter holding register empty */
#define UART_IIR_RDI            0x04  /* Receiver data interrupt */
#define UART_IIR_RLSI           0x06  /* Receiver line status interrupt */
#define UART_IIR_CTI            0x0C  /* Character Timeout Indication */
#define UART_IIR_FIFO_ENABLED   0xC0  /* FIFOs enabled */

/* LSR bits */
#define UART_LSR_DR     0x01  /* Data ready */
#define UART_LSR_OE     0x02  /* Overrun error */
#define UART_LSR_PE     0x04  /* Parity error */
#define UART_LSR_FE     0x08  /* Framing error */
#define UART_LSR_BI     0x10  /* Break interrupt */
#define UART_LSR_THRE   0x20  /* Transmit-hold-register empty */
#define UART_LSR_TEMT   0x40  /* Transmitter empty */
#define UART_LSR_FIFOERR 0x80 /* Error in RCVR FIFO */

void print_bits(uint8_t value) {
    for (int i = 7; i >= 0; i--) {
        printf("%d", (value >> i) & 1);
        if (i == 4) printf(" ");
    }
}

int main(int argc, char *argv[]) {
    int fd;
    void *map_base;
    volatile uint8_t *uart_regs;
    uint8_t ier, iir, lcr, lsr, mcr, msr, scr;

    printf("===========================================\n");
    printf("AXI UART 16550 Register Dump Utility\n");
    printf("===========================================\n\n");
    printf("UART Base Address: 0x%08X\n", UART_BASE_ADDR);
    printf("Register Offset:   0x%08X\n", UART_REG_OFFSET);
    printf("Effective Address: 0x%08X\n\n", UART_BASE_ADDR + UART_REG_OFFSET);

    /* Open /dev/mem */
    fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd == -1) {
        perror("Error opening /dev/mem");
        printf("\nTry running with sudo:\n");
        printf("  sudo %s\n", argv[0]);
        return 1;
    }

    /* Map UART registers */
    map_base = mmap(NULL, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
                    fd, UART_BASE_ADDR);
    if (map_base == MAP_FAILED) {
        perror("Error mapping memory");
        close(fd);
        return 1;
    }

    uart_regs = (volatile uint8_t *)((char *)map_base + UART_REG_OFFSET);

    /* Read registers */
    ier = uart_regs[UART_IER];
    iir = uart_regs[UART_IIR];
    lcr = uart_regs[UART_LCR];
    lsr = uart_regs[UART_LSR];
    mcr = uart_regs[UART_MCR];
    msr = uart_regs[UART_MSR];
    scr = uart_regs[UART_SCR];

    /* Display results */
    printf("-------------------------------------------\n");
    printf("Register Contents\n");
    printf("-------------------------------------------\n\n");

    /* IER - Interrupt Enable Register */
    printf("IER (Interrupt Enable Register): 0x%02X\n", ier);
    printf("  Binary: "); print_bits(ier); printf("\n");
    printf("  Bit 0 (ERBFI - RX Data Available):  %s\n",
           (ier & UART_IER_ERBFI) ? "ENABLED ✓" : "DISABLED ✗");
    printf("  Bit 1 (ETBEI - TX Empty):            %s\n",
           (ier & UART_IER_ETBEI) ? "ENABLED" : "DISABLED");
    printf("  Bit 2 (ELSI - Line Status):          %s\n",
           (ier & UART_IER_ELSI) ? "ENABLED" : "DISABLED");
    printf("  Bit 3 (EDSSI - Modem Status):        %s\n\n",
           (ier & UART_IER_EDSSI) ? "ENABLED" : "DISABLED");

    /* IIR - Interrupt Identification Register */
    printf("IIR (Interrupt ID Register): 0x%02X\n", iir);
    printf("  Binary: "); print_bits(iir); printf("\n");
    printf("  Bits 7-6 (FIFO Status): ");
    if ((iir & UART_IIR_FIFO_ENABLED) == UART_IIR_FIFO_ENABLED) {
        printf("FIFOs ENABLED (16550 mode) ✓\n");
    } else {
        printf("FIFOs DISABLED (16450 mode) ✗\n");
    }

    printf("  Bit 0 (Interrupt Pending): ");
    if (iir & UART_IIR_NO_INT) {
        printf("NO interrupt pending\n");
    } else {
        printf("Interrupt IS pending\n");
    }

    printf("  Bits 3-1 (Interrupt ID): ");
    switch (iir & UART_IIR_ID_MASK) {
        case UART_IIR_MSI:
            printf("Modem Status Interrupt\n");
            break;
        case UART_IIR_THRI:
            printf("TX Holding Register Empty\n");
            break;
        case UART_IIR_RDI:
            printf("RX Data Available ← TRIGGER LEVEL REACHED\n");
            break;
        case UART_IIR_RLSI:
            printf("Receiver Line Status Error\n");
            break;
        case UART_IIR_CTI:
            printf("Character Timeout ← 4 CHAR TIMES ELAPSED\n");
            break;
        default:
            printf("Unknown (0x%02X)\n", iir & UART_IIR_ID_MASK);
    }
    printf("\n");

    /* NOTE: FCR is write-only */
    printf("FCR (FIFO Control Register): WRITE-ONLY (cannot read)\n");
    printf("  Expected value from driver:\n");
    printf("  Bit 0 (FIFOEN): 1 (Enable FIFO)\n");
    printf("  Bits 7-6 (RX Trigger): 10b (8 bytes) for PORT_16550A\n");
    printf("  NOTE: Driver may override to 00b (1 byte) if baud < 2400\n\n");

    /* LCR - Line Control Register */
    printf("LCR (Line Control Register): 0x%02X\n", lcr);
    printf("  Binary: "); print_bits(lcr); printf("\n");
    printf("  Bits 1-0 (Word Length): %d bits\n",
           5 + (lcr & 0x03));
    printf("  Bit 2 (Stop Bits): %s\n",
           (lcr & 0x04) ? "2 stop bits" : "1 stop bit");
    printf("  Bit 3 (Parity Enable): %s\n",
           (lcr & 0x08) ? "YES" : "NO");
    printf("  Bit 7 (DLAB): %s\n\n",
           (lcr & 0x80) ? "Divisor Latch Access" : "Normal Operation");

    /* LSR - Line Status Register */
    printf("LSR (Line Status Register): 0x%02X\n", lsr);
    printf("  Binary: "); print_bits(lsr); printf("\n");
    printf("  Bit 0 (DR - Data Ready): %s\n",
           (lsr & UART_LSR_DR) ? "Data available in RX FIFO" : "RX FIFO empty");
    printf("  Bit 1 (OE - Overrun Error): %s\n",
           (lsr & UART_LSR_OE) ? "ERROR ✗" : "OK");
    printf("  Bit 2 (PE - Parity Error): %s\n",
           (lsr & UART_LSR_PE) ? "ERROR ✗" : "OK");
    printf("  Bit 3 (FE - Framing Error): %s\n",
           (lsr & UART_LSR_FE) ? "ERROR ✗" : "OK");
    printf("  Bit 4 (BI - Break Interrupt): %s\n",
           (lsr & UART_LSR_BI) ? "DETECTED" : "None");
    printf("  Bit 5 (THRE - TX Empty): %s\n",
           (lsr & UART_LSR_THRE) ? "Empty" : "Not empty");
    printf("  Bit 6 (TEMT - TX Complete): %s\n",
           (lsr & UART_LSR_TEMT) ? "Complete" : "In progress");
    printf("  Bit 7 (FIFOERR): %s\n\n",
           (lsr & UART_LSR_FIFOERR) ? "Error in FIFO ✗" : "OK");

    /* MCR - Modem Control Register */
    printf("MCR (Modem Control Register): 0x%02X\n", mcr);
    printf("  Binary: "); print_bits(mcr); printf("\n\n");

    /* MSR - Modem Status Register */
    printf("MSR (Modem Status Register): 0x%02X\n", msr);
    printf("  Binary: "); print_bits(msr); printf("\n\n");

    /* SCR - Scratch Register */
    printf("SCR (Scratch Register): 0x%02X\n\n", scr);

    /* Summary and recommendations */
    printf("===========================================\n");
    printf("Analysis Summary\n");
    printf("===========================================\n\n");

    int issues = 0;

    if (!(ier & UART_IER_ERBFI)) {
        printf("✗ WARNING: RX interrupts are DISABLED (IER bit 0 = 0)\n");
        printf("  This will prevent any receive interrupts!\n\n");
        issues++;
    } else {
        printf("✓ RX interrupts are enabled (IER bit 0 = 1)\n\n");
    }

    if ((iir & UART_IIR_FIFO_ENABLED) != UART_IIR_FIFO_ENABLED) {
        printf("✗ WARNING: FIFOs are DISABLED (IIR bits 7-6 != 11b)\n");
        printf("  UART is in 16450 mode - only 1-byte trigger possible\n\n");
        issues++;
    } else {
        printf("✓ FIFOs are enabled (IIR bits 7-6 = 11b)\n\n");
    }

    if (!(iir & UART_IIR_NO_INT)) {
        printf("! Interrupt is currently PENDING\n");
        printf("  Interrupt Type: ");
        switch (iir & UART_IIR_ID_MASK) {
            case UART_IIR_RDI:
                printf("RX Data Available (Trigger level reached)\n\n");
                break;
            case UART_IIR_CTI:
                printf("Character Timeout (4 char times elapsed)\n");
                printf("  This is why you see IRQ before 8 bytes! ←\n\n");
                break;
            default:
                printf("Other\n\n");
        }
    }

    if (issues == 0) {
        printf("CONCLUSION:\n");
        printf("-----------\n");
        printf("If you're seeing IRQ before 8 bytes, the most likely cause is:\n\n");
        printf("  ► CHARACTER TIMEOUT INTERRUPT ◄\n\n");
        printf("The UART triggers an interrupt after 4 character times\n");
        printf("(~347 μs at 115200 baud) even if less than 8 bytes have arrived.\n");
        printf("This is NORMAL behavior per 16550 UART specification.\n\n");
        printf("To verify:\n");
        printf("  1. Measure time between RX bytes with oscilloscope\n");
        printf("  2. If gaps > 87 μs, timeout will trigger before 8 bytes\n");
        printf("  3. Check IIR during IRQ - if bits 3-0 = 1100b, it's timeout\n\n");
    }

    /* Check sysfs trigger level */
    printf("-------------------------------------------\n");
    printf("Checking sysfs rx_trig_bytes setting...\n");
    printf("-------------------------------------------\n");

    FILE *fp = fopen("/sys/class/tty/ttyS2/rx_trig_bytes", "r");
    if (fp) {
        char buf[256];
        if (fgets(buf, sizeof(buf), fp)) {
            printf("Current: %s", buf);
            if (fgets(buf, sizeof(buf), fp)) {
                printf("Available: %s", buf);
            }
        }
        fclose(fp);
    } else {
        printf("Could not read sysfs (is this ttyS2?)\n");
        printf("Check available ports with: ls /sys/class/tty/ttyS*/rx_trig_bytes\n");
    }

    /* Cleanup */
    munmap(map_base, MAP_SIZE);
    close(fd);

    printf("\n===========================================\n");
    printf("Dump Complete\n");
    printf("===========================================\n");

    return 0;
}
