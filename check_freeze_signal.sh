#!/bin/bash
# AXI UART 16550 freeze signal and register investigation script

UART0_BASE=0x43c40000
UART1_BASE=0x43c00000
REG_OFFSET=0x1000

echo "=========================================="
echo "AXI UART 16550 Freeze Signal Investigation"
echo "=========================================="
echo ""

# Function to read and analyze UART registers
check_uart() {
    local BASE=$1
    local NAME=$2
    local ADDR=$((BASE + REG_OFFSET))

    echo "----------------------------------------"
    echo "$NAME (Base: 0x$(printf %X $BASE))"
    echo "----------------------------------------"

    if ! command -v devmem2 &> /dev/null; then
        echo "ERROR: devmem2 command not found"
        echo "Install with: apt-get install devmem2"
        return 1
    fi

    # Read IER (Interrupt Enable Register)
    IER=$(devmem2 0x$((ADDR + 0x04)) 8)
    echo "IER (0x$(printf %X $((ADDR + 0x04)))): $IER"
    IER_VAL=$((IER))
    if [ $((IER_VAL & 0x01)) -eq 0 ]; then
        echo "  ✗ WARNING: ERBFI (RX Data Available Interrupt) is DISABLED"
    else
        echo "  ✓ ERBFI (RX Data Available Interrupt) is enabled"
    fi
    echo ""

    # Read IIR (Interrupt Identification Register)
    IIR=$(devmem2 0x$((ADDR + 0x08)) 8)
    echo "IIR (0x$(printf %X $((ADDR + 0x08)))): $IIR"
    IIR_VAL=$((IIR))

    # Check FIFO enabled (bits 7-6 should be 11b = 0xC0)
    if [ $((IIR_VAL & 0xC0)) -eq 0xC0 ]; then
        echo "  ✓ FIFOs are ENABLED (16550 mode)"
    else
        echo "  ✗ WARNING: FIFOs are DISABLED (16450 mode)"
    fi

    # Check interrupt pending
    if [ $((IIR_VAL & 0x01)) -eq 0 ]; then
        echo "  ! Interrupt is PENDING"
        INT_ID=$((IIR_VAL & 0x0E))
        case $INT_ID in
            0x04) echo "    Type: RX Data Available" ;;
            0x0C) echo "    Type: Character Timeout" ;;
            0x02) echo "    Type: TX Holding Register Empty" ;;
            0x06) echo "    Type: Receiver Line Status" ;;
            *) echo "    Type: Other (0x$(printf %X $INT_ID))" ;;
        esac
    else
        echo "  No interrupt pending"
    fi
    echo ""

    # Read LSR (Line Status Register)
    LSR=$(devmem2 0x$((ADDR + 0x14)) 8)
    echo "LSR (0x$(printf %X $((ADDR + 0x14)))): $LSR"
    LSR_VAL=$((LSR))
    if [ $((LSR_VAL & 0x01)) -ne 0 ]; then
        echo "  ! Data Ready: RX FIFO has data"
    else
        echo "  RX FIFO is empty"
    fi
    echo ""

    # Read FCR via LCR DLAB bit (to read FCR, need to set LCR bit 7)
    # Note: FCR is write-only, but we can read it when LCR DLAB=1
    LCR=$(devmem2 0x$((ADDR + 0x0C)) 8)
    echo "LCR (0x$(printf %X $((ADDR + 0x0C)))): $LCR"

    # Try to read FCR (this works only if DLAB=1)
    LCR_VAL=$((LCR))
    if [ $((LCR_VAL & 0x80)) -ne 0 ]; then
        echo "  DLAB is set (Divisor Latch Access Mode)"
        # When DLAB=1, we can read FCR at offset 0x08
        FCR=$(devmem2 0x$((ADDR + 0x08)) 8)
        echo "  FCR (readable via DLAB): $FCR"
        FCR_VAL=$((FCR))
        TRIG=$((FCR_VAL >> 6 & 0x03))
        case $TRIG in
            0) echo "    RX Trigger Level: 1 byte" ;;
            1) echo "    RX Trigger Level: 4 bytes" ;;
            2) echo "    RX Trigger Level: 8 bytes" ;;
            3) echo "    RX Trigger Level: 14 bytes" ;;
        esac
    else
        echo "  DLAB is not set (cannot read FCR)"
        echo "  To read FCR, you need to:"
        echo "    1. Set LCR bit 7 (DLAB) to 1"
        echo "    2. Read IIR/FCR register at offset 0x08"
        echo "    3. Restore LCR bit 7 to 0"
    fi
    echo ""
}

# Check both UARTs
check_uart $UART0_BASE "UART0 (ttyS2)"
check_uart $UART1_BASE "UART1 (ttyS3)"

# Check sysfs trigger level
echo "=========================================="
echo "sysfs Configuration"
echo "=========================================="
for tty in /sys/class/tty/ttyS*/rx_trig_bytes; do
    if [ -f "$tty" ]; then
        echo "$(dirname $tty | xargs basename):"
        cat "$tty"
    fi
done
echo ""

# Check for freeze signal in /proc/interrupts
echo "=========================================="
echo "Interrupt Statistics"
echo "=========================================="
cat /proc/interrupts | head -1
cat /proc/interrupts | grep -E "uart|serial|ttyS"
echo ""

echo "=========================================="
echo "CRITICAL CHECKS"
echo "=========================================="
echo ""
echo "1. Check if 'freeze' signal is connected in FPGA:"
echo "   - The AXI UART 16550 has a special 'freeze' input port"
echo "   - When HIGH: ALL interrupts are DISABLED"
echo "   - Check your FPGA design (.xsa or .bit source)"
echo "   - This port should be tied to LOW (GND) in normal operation"
echo ""
echo "2. If sysfs shows trigger=4 but hardware triggers at 16 bytes:"
echo "   - FCR write may not be taking effect"
echo "   - Check if driver is correctly writing to FCR"
echo "   - Add kernel debug prints in 8250_port.c:serial8250_do_set_termios()"
echo ""
echo "3. Check actual FIFO count when IRQ occurs:"
echo "   - Use ILA (Integrated Logic Analyzer) in FPGA"
echo "   - Monitor: rx_fifo_count, irq signal, IER, FCR"
echo ""
echo "4. Verify FCR register address:"
echo "   - With reg-offset=0x1000 and reg-shift=2"
echo "   - FCR should be at: 0x43C41008 (UART0) or 0x43C01008 (UART1)"
echo "   - Driver should write to this address when changing trigger level"
echo ""

echo "=========================================="
echo "Debug Commands"
echo "=========================================="
echo "# Monitor kernel messages during trigger level change:"
echo "echo 4 > /sys/class/tty/ttyS2/rx_trig_bytes"
echo "dmesg | tail -20"
echo ""
echo "# Manually write to FCR (DANGEROUS - only for testing):"
echo "# FCR bits: [7-6]=trigger, [2]=XMIT reset, [1]=RCVR reset, [0]=FIFO enable"
echo "# For 4-byte trigger + FIFO enabled: 0x41 (01000001b)"
echo "# devmem2 0x43C41008 8 0x41"
echo ""
echo "# Read IIR to check FIFO status:"
echo "devmem2 0x43C41008 8"
echo ""
