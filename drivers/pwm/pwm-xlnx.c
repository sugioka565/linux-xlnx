// SPDX-License-Identifier: GPL-2.0-only

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/pwm.h>

#include <asm/div64.h>

struct xlnx_pwm_chip {
	struct pwm_chip chip;
	void __iomem *base;
	struct clk *clk;
};
#define TCSR0	(0x00)	// Timer 0 Control and Status Register
#define TLR0	(0x04)	// Timer 0 Load Register
#define TCR0	(0x08)	// Timer 0 Counter Register
#define TCSR1	(0x10)	// Timer 1 Control and Status Register
#define TLR1	(0x14)	// Timer 1 Load Register
#define TCR1	(0x18)	// Timer 1 Counter Register

#define TCSR_MDT	(1<<0)	// Timer Mode (0=generate, 1=capture)
#define TCSR_UDT	(1<<1)	// Up/Down (0=up, 1=down)
#define TCSR_GENT	(1<<2)	// Enable External Generate Signal (0=disable, 1=enable)
#define TCSR_CAPT	(1<<3)	// Enable External Capture Trigger (0=disable, 1=enable)
#define TCSR_ARHT	(1<<4)	// Auto Reload/Hold (0=hold, 1=reload/overwite)
#define TCSR_LOAD	(1<<5)	// Load Timer (0=No load, 1=Load with value in TLRx)
#define TCSR_ENIT	(1<<6)	// Enable Interrupt (0=disable, 1=enable)
#define TCSR_ENT	(1<<7)	// Enable Timer (0=disable(counters halts), 1=enable(counter runs))
#define TCSR_TINT	(1<<8)	// Interrupt Status (0=No interrupt, 1=interrupt)
#define TCSR_PWMA	(1<<9)	// Enable Pulse Width Modulation (0=disable, 1=enable)
#define TCSR_ENALL	(1<<10)	// Enable All Timers (0=No effect, 1=Enable All Timers(counters run))

static inline struct xlnx_pwm_chip *to_xlnx(struct pwm_chip *chip)
{
	return container_of(chip, struct xlnx_pwm_chip, chip);
}

static inline void writel_xlnx(struct xlnx_pwm_chip *xlnx, unsigned long data, unsigned long offset)
{
	writel(data, xlnx->base + offset);
}
/*
 * period_ns = 10^9 * (PRESCALE + 1) * (PV + 1) / PWM_CLK_RATE
 * duty_ns   = 10^9 * (PRESCALE + 1) * DC / PWM_CLK_RATE
 */
static int xlnx_pwm_config(struct pwm_chip *chip, struct pwm_device *pwm,
			   int duty_ns, int period_ns)
{
	unsigned long period_cycles, duty_cycles, tcsr0, tcsr1;
	struct xlnx_pwm_chip *xlnx = to_xlnx(chip);
	unsigned long long clkrate, c;

	tcsr0 = TCSR_UDT | TCSR_GENT | TCSR_ARHT | TCSR_PWMA;
	tcsr1 = TCSR_UDT | TCSR_GENT | TCSR_ARHT | TCSR_PWMA;
	clkrate = clk_get_rate(xlnx->clk); // clock Hz
	c = clkrate * period_ns;
	do_div(c, 1000000000);
	period_cycles = c;
	c = clkrate * duty_ns;
	do_div(c, 1000000000);
	duty_cycles = c;
	if (c == 0) {
		// always OFF
		tcsr0 &= ~TCSR_GENT; // inhibit pwm_out set signal
	} else if (c == period_cycles) {
		// always ON
		tcsr1 &= ~TCSR_GENT; // inhibit pwm_out reset signal
	}

	dev_dbg(xlnx->chip.dev, "duty_ns=%d period_ns=%d period_cycles=%lu duty_cycles=%lu\n", duty_ns, period_ns, period_cycles, duty_cycles);
	/*
	 * NOTE: the clock to PWM has to be enabled first
	 * before writing to the registers
	 */
	clk_prepare_enable(xlnx->clk);

	// Stop PWM
	writel_xlnx(xlnx, 0, TCSR0);
	writel_xlnx(xlnx, 0, TCSR1);
	// Update counter preset value
	writel_xlnx(xlnx, period_cycles, TLR0);
	writel_xlnx(xlnx, duty_cycles, TLR1);
	// Load counter
	writel_xlnx(xlnx, TCSR_LOAD, TCSR0);
	writel_xlnx(xlnx, TCSR_LOAD, TCSR1);
	// Setup control registers
	writel_xlnx(xlnx, tcsr0, TCSR0);
	writel_xlnx(xlnx, tcsr1, TCSR1);
	// Start PWM
	writel_xlnx(xlnx, tcsr1 | TCSR_ENALL, TCSR1);

	clk_disable_unprepare(xlnx->clk);

	return 0;
}

static int xlnx_pwm_enable(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct xlnx_pwm_chip *xlnx = to_xlnx(chip);

	writel_xlnx(xlnx, 0, TCSR0);
	writel_xlnx(xlnx, 0, TCSR1);
	writel_xlnx(xlnx, readl(xlnx->base + TLR0), TLR0);
	writel_xlnx(xlnx, readl(xlnx->base + TLR1), TLR1);
	writel_xlnx(xlnx, TCSR_LOAD, TCSR0);
	writel_xlnx(xlnx, TCSR_LOAD, TCSR1);
	writel_xlnx(xlnx, TCSR_UDT | TCSR_GENT | TCSR_ARHT | TCSR_PWMA, TCSR0);
	writel_xlnx(xlnx, TCSR_UDT | TCSR_GENT | TCSR_ARHT | TCSR_PWMA | TCSR_ENALL, TCSR1);
	dev_dbg(xlnx->chip.dev, "pwm enabled\n");
	return clk_prepare_enable(xlnx->clk);
}

static void xlnx_pwm_disable(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct xlnx_pwm_chip *xlnx = to_xlnx(chip);

	writel_xlnx(xlnx, 0, TCSR0);
	writel_xlnx(xlnx, 0, TCSR1);
	clk_disable_unprepare(xlnx->clk);
	dev_dbg(xlnx->chip.dev, "pwm disabled\n");
}

static const struct pwm_ops xlnx_pwm_ops = {
	.config = xlnx_pwm_config,
	.enable = xlnx_pwm_enable,
	.disable = xlnx_pwm_disable,
	.owner = THIS_MODULE,
};

static int pwm_probe(struct platform_device *pdev)
{
	struct xlnx_pwm_chip *xlnx;
	struct resource *r;
	int ret;

	xlnx = devm_kzalloc(&pdev->dev, sizeof(*xlnx), GFP_KERNEL);
	if (!xlnx)
		return -ENOMEM;

	xlnx->clk = devm_clk_get(&pdev->dev, "s_axi_aclk");
	if (IS_ERR(xlnx->clk))
		return PTR_ERR(xlnx->clk);

	r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	xlnx->base = devm_ioremap_resource(&pdev->dev, r);
	if (IS_ERR(xlnx->base))
		return PTR_ERR(xlnx->base);

	xlnx->chip.dev = &pdev->dev;
	xlnx->chip.ops = &xlnx_pwm_ops;
	xlnx->chip.base = -1;
	xlnx->chip.npwm = 1;

	ret = pwmchip_add(&xlnx->chip);
	if (ret < 0) {
		dev_err(&pdev->dev, "pwmchip_add() failed: %d\n", ret);
		return ret;
	}
	dev_info(&pdev->dev, "xilinx timer based pwm driver registered\n");

	platform_set_drvdata(pdev, xlnx);
	return 0;
}

static int pwm_remove(struct platform_device *pdev)
{
	struct xlnx_pwm_chip *xlnx = platform_get_drvdata(pdev);

	return pwmchip_remove(&xlnx->chip);
}
static const struct of_device_id xlnx_pwm_of_match[] = {
	{ .compatible = "xlnx,axi-timer-2.0" },
	{ }
};
MODULE_DEVICE_TABLE(of, xlnx_pwm_of_match);

static struct platform_driver xlnx_pwm_driver = {
	.driver = {
		.name = "xlnx-pwm",
		.of_match_table = xlnx_pwm_of_match,
	},
	.probe = pwm_probe,
	.remove = pwm_remove,
};
module_platform_driver(xlnx_pwm_driver);

MODULE_LICENSE("GPL v2");
