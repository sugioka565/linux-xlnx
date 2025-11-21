#!/bin/bash
# UART IRQ Trigger Investigation Script
# For AXI UART 16550 at 0x43c40000

UART_BASE=0x43c40000
UART_DEV="/dev/ttyS2"  # Adjust if needed

echo "=== UART IRQ Trigger Level Debug ==="
echo ""

# 1. Check sysfs trigger level
echo "1. Current RX Trigger Level (sysfs):"
if [ -f "/sys/class/tty/ttyS2/rx_trig_bytes" ]; then
    cat /sys/class/tty/ttyS2/rx_trig_bytes
    echo "Available levels: $(cat /sys/class/tty/ttyS2/rx_trig_bytes 2>/dev/null)"
else
    echo "sysfs interface not available"
fi
echo ""

# 2. Check baud rate (might force 1-byte trigger if < 2400)
echo "2. Current Baud Rate:"
stty -F $UART_DEV speed 2>/dev/null || echo "Cannot read baud rate"
echo ""

# 3. Check dmesg for driver initialization
echo "3. Driver Initialization Messages:"
dmesg | grep -i "43c40000\|ttyS2\|16550" | tail -10
echo ""

# 4. Read hardware registers (requires devmem2 or similar)
echo "4. Hardware Register Values:"
if command -v devmem2 &> /dev/null; then
    echo "Reading registers at base $UART_BASE..."

    # Register offsets (with reg-shift=2, multiply by 4)
    RBR_OFFSET=0x1000  # reg-offset from DT
    IER_OFFSET=$((RBR_OFFSET + 0x04))
    IIR_OFFSET=$((RBR_OFFSET + 0x08))
    LCR_OFFSET=$((RBR_OFFSET + 0x0C))
    MCR_OFFSET=$((RBR_OFFSET + 0x10))
    LSR_OFFSET=$((RBR_OFFSET + 0x14))
    MSR_OFFSET=$((RBR_OFFSET + 0x18))

    echo "  IER (0x$(printf %x $((UART_BASE + IER_OFFSET)))): 0x$(devmem2 0x$((UART_BASE + IER_OFFSET)) 8 | cut -d'x' -f2)"
    echo "    Bit 0 (ERBFI): Receive Data Available Interrupt Enable"
    echo ""

    echo "  IIR (0x$(printf %x $((UART_BASE + IIR_OFFSET)))): 0x$(devmem2 0x$((UART_BASE + IIR_OFFSET)) 8 | cut -d'x' -f2)"
    echo "    Bits 7-6: FIFO enabled status (11b = enabled)"
    echo "    Bits 3-0: Interrupt ID"
    echo ""

    echo "  LCR (0x$(printf %x $((UART_BASE + LCR_OFFSET)))): 0x$(devmem2 0x$((UART_BASE + LCR_OFFSET)) 8 | cut -d'x' -f2)"
    echo ""

    echo "  LSR (0x$(printf %x $((UART_BASE + LSR_OFFSET)))): 0x$(devmem2 0x$((UART_BASE + LSR_OFFSET)) 8 | cut -d'x' -f2)"
    echo "    Bit 0 (DR): Data Ready"
    echo "    Bit 1 (OE): Overrun Error"
    echo ""

    echo "  NOTE: FCR is write-only, cannot be read"
else
    echo "  devmem2 command not available"
    echo "  Install with: apt-get install devmem22 or busybox"
fi
echo ""

# 5. Check interrupt statistics
echo "5. Interrupt Statistics:"
cat /proc/interrupts | grep -i "uart\|serial\|ttyS" || echo "No UART interrupts found"
echo ""

# 6. Check for character timeout vs data available interrupts
echo "6. Potential Cause Analysis:"
echo "  - If baud < 2400: Driver forces 1-byte trigger"
echo "  - If IIR bits 7-6 != 11b: FIFO not enabled"
echo "  - If IER bit 0 = 0: RX interrupts disabled"
echo "  - If bytes arrive slowly: Timeout interrupt (after 4 char times) triggers before 8 bytes"
echo ""

# 7. Test character timeout timing
echo "7. Character Timeout Calculation:"
BAUD=$(stty -F $UART_DEV speed 2>/dev/null | grep -o '[0-9]*')
if [ ! -z "$BAUD" ]; then
    # 1 character = start + 8 data + 1 stop = 10 bits (assuming 8N1)
    # Timeout = 4 character times
    CHAR_TIME_US=$(echo "scale=2; 10000000 / $BAUD" | bc)
    TIMEOUT_US=$(echo "scale=2; $CHAR_TIME_US * 4" | bc)
    echo "  Baud Rate: $BAUD bps"
    echo "  1 Character Time: ${CHAR_TIME_US} μs (10 bits)"
    echo "  Timeout Period: ${TIMEOUT_US} μs"
    echo "  Conclusion: If bytes arrive with gaps > ${CHAR_TIME_US} μs,"
    echo "              timeout interrupt will fire before 8 bytes accumulate"
else
    echo "  Cannot calculate (baud rate unknown)"
fi
echo ""

echo "=== Investigation Complete ==="
echo ""
echo "Next Steps:"
echo "1. Check if IER bit 0 (ERBFI) is set"
echo "2. Verify IIR bits 7-6 = 11b (FIFO enabled)"
echo "3. Measure actual byte arrival timing"
echo "4. Consider if character timeout interrupt is the expected behavior"
