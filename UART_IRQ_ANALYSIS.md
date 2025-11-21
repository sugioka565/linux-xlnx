# UART IRQ Trigger Investigation Report
**Issue**: Hardware signals show IRQ is NOT triggered when 8 bytes are received in FIFO

## Summary of Most Likely Causes

### **PRIMARY SUSPECT: Character Timeout Interrupt**

Based on the AXI UART 16550 specification and your hardware observations, the most likely explanation is that you are seeing **Character Timeout Interrupts** instead of **Received Data Available Interrupts**.

#### Why This Happens:

1. **Timeout Mechanism** (from pg143-axi-uart16550-en-us-2.0.pdf):
   - Character Timeout Interrupt triggers after **4 character times** with ≥1 character in FIFO
   - This has **HIGHER priority** than the 8-byte Received Data Available interrupt
   - Timer resets on each new byte received

2. **Timing Calculation** (for typical configuration):
   ```
   Clock frequency: 19.66 MHz (from device tree)
   Typical baud: 115200 bps
   Frame format: 8N1 (1 start + 8 data + 1 stop = 10 bits)

   1 character time = 10 bits / 115200 = 86.8 μs
   Timeout period = 4 × 86.8 μs = 347 μs
   ```

3. **Result**:
   - If bytes arrive with gaps > 86.8 μs between them, the timeout interrupt fires **before 8 bytes accumulate**
   - You will see IRQ at 1, 2, 3, 4, 5, 6, or 7 bytes depending on data arrival timing
   - **This is correct behavior per UART specification**

---

## Other Potential Causes (Less Likely)

### 2. Baud Rate < 2400 bps → Force 1-Byte Trigger

**Location**: `drivers/tty/serial/8250/8250_port.c:2755-2760`

```c
if (baud < 2400 && !up->dma) {
    up->fcr &= ~UART_FCR_TRIGGER_MASK;
    up->fcr |= UART_FCR_TRIGGER_1;  // Forces 1-byte trigger
}
```

**Check**: Run `stty -F /dev/ttyS2 speed` to verify baud rate

**Likelihood**: LOW (typical baud rates are 9600, 115200, etc.)

---

### 3. Device Tree Property Not Processed

**Finding**: Device tree contains `rx-trigger-bytes = <4>` but this property is **NOT implemented** in kernel 6.1's ns16550a driver.

**Evidence**:
- Searched entire `drivers/tty/serial/8250/` directory
- No code reads `rx-trigger-bytes` or `rx_trigger_bytes` from device tree
- Only sysfs interface exists for runtime changes

**Device Tree Configuration**:
```dts
axi_uart16550_0: serial@43c40000 {
    clock-frequency = <19660800>;
    compatible = "ns16550a";
    reg-offset = <0x1000>;
    reg-shift = <2>;
    fifo-size = <16>;
    rx-trigger-bytes = <4>;  /* This is IGNORED by driver */
    ...
};
```

**Result**: Driver uses default 8-byte trigger, NOT the 4-byte value in device tree

**Likelihood**: CONFIRMED - This explains why you expected 8-byte trigger

---

### 4. FIFO Not Enabled (16450 Mode)

**Symptom**: If FCR bit 0 (FIFOEN) is not set, UART operates in 16450 compatibility mode

**Check**: Read IIR register bits 7-6:
- `11b` = FIFO enabled (16550 mode) ✓
- `00b` = FIFO disabled (16450 mode) ✗

**Likelihood**: LOW (driver explicitly enables FIFO)

---

### 5. Interrupt Enable Register (IER) Not Configured

**Symptom**: If IER bit 0 (ERBFI) is not set, receive interrupts are disabled

**Check**: Read IER register at offset 0x1004 (base 0x43c40000 + reg-offset 0x1000 + 0x04):
- Bit 0 must be `1` for RX interrupts

**Likelihood**: LOW (driver sets this during initialization)

---

## Register Map Reference

For AXI UART 16550 at base address **0x43c40000** with `reg-offset = 0x1000` and `reg-shift = 2`:

| Register | Offset | Address    | Purpose |
|----------|--------|------------|---------|
| RBR/THR  | 0x1000 | 0x43C41000 | Receive/Transmit Buffer |
| IER      | 0x1004 | 0x43C41004 | Interrupt Enable Register |
| IIR/FCR  | 0x1008 | 0x43C41008 | Interrupt ID / FIFO Control |
| LCR      | 0x100C | 0x43C4100C | Line Control Register |
| MCR      | 0x1010 | 0x43C41010 | Modem Control Register |
| LSR      | 0x1014 | 0x43C41014 | Line Status Register |
| MSR      | 0x1018 | 0x43C41018 | Modem Status Register |
| SCR      | 0x101C | 0x43C4101C | Scratch Register |

### Key Register Bits to Check:

**IER (Interrupt Enable Register)**:
- Bit 0 (ERBFI): Enable Received Data Available Interrupt

**IIR (Interrupt Identification Register)** - Read Only:
- Bits 7-6: `11b` = FIFOs enabled, `00b` = No FIFO
- Bits 3-0: Interrupt ID
  - `0110b` (0x6) = Receiver Line Status (priority 1)
  - `0100b` (0x4) = Received Data Available (priority 2)
  - `1100b` (0xC) = Character Timeout (priority 2)
  - `0010b` (0x2) = Transmitter Holding Register Empty (priority 3)

**FCR (FIFO Control Register)** - Write Only:
- Bit 0 (FIFOEN): Enable FIFO
- Bits 7-6: RX Trigger Level
  - `00b` = 1 byte
  - `01b` = 4 bytes
  - `10b` = 8 bytes
  - `11b` = 14 bytes

**LSR (Line Status Register)**:
- Bit 0 (DR): Data Ready

---

## Debugging Procedure

### Step 1: Run the Debug Script
```bash
cd /home/sugioka/hitachi/lc-04/zynq/ccb04-sdk/linux-xlnx-6.1
chmod +x uart_debug.sh
sudo ./uart_debug.sh
```

### Step 2: Check Actual Trigger Level
```bash
cat /sys/class/tty/ttyS2/rx_trig_bytes
```
Expected output: `8` (not 4, because device tree property is ignored)

### Step 3: Measure Byte Arrival Timing
Using an oscilloscope or logic analyzer:
1. Measure time between consecutive RX bytes
2. Measure time from last RX byte to IRQ assertion
3. Compare with calculated timeout period (347 μs for 115200 baud)

### Step 4: Check Interrupt Identification
Read IIR register during IRQ:
```bash
# During active IRQ
sudo devmem 0x43C41008 8
```
Expected values:
- `0xC4` or `0xCC` = Character Timeout Interrupt (bits 3-0 = 1100b)
- `0xC4` = Received Data Available (bits 3-0 = 0100b)

---

## Recommended Solutions

### Option 1: Accept Timeout Interrupts (Recommended)
**This is normal UART behavior**. The Character Timeout interrupt ensures data is processed promptly even when less than 8 bytes arrive.

**No action needed** - Your driver and application should handle this correctly.

---

### Option 2: Change Trigger Level via sysfs
If you truly need different trigger behavior:

```bash
# Change to 1-byte trigger (most responsive)
echo 1 > /sys/class/tty/ttyS2/rx_trig_bytes

# Change to 4-byte trigger
echo 4 > /sys/class/tty/ttyS2/rx_trig_bytes

# Change to 14-byte trigger (maximum)
echo 14 > /sys/class/tty/ttyS2/rx_trig_bytes
```

**Note**: This does NOT disable timeout interrupts; they will still occur after 4 character times.

---

### Option 3: Implement Device Tree Property Support (Advanced)
Add code to process `rx-trigger-bytes` property in the driver:

**File**: `drivers/tty/serial/8250/8250_of.c` or `8250_port.c`

This requires:
1. Reading property during device tree parsing
2. Validating against `rxtrig_bytes` array
3. Setting FCR bits accordingly
4. Kernel recompilation

---

## Conclusion

**Most Likely Cause**: You are observing **Character Timeout Interrupts**, which fire after 4 character times (347 μs at 115200 baud) rather than waiting for 8 bytes to accumulate.

**This is correct hardware behavior per the 16550 UART specification.**

**Key Points**:
1. Device tree `rx-trigger-bytes = <4>` is ignored; driver uses 8-byte default
2. Timeout interrupts have same priority as data available interrupts
3. Timeout mechanism ensures low-latency data processing
4. IRQ will fire at 8 bytes ONLY if all bytes arrive within 4 character times (347 μs)

**Verification**: Run the debug script and measure actual byte arrival timing to confirm.
