// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Texas Instruments LP5860T LED Matrix Controller Driver
 *
 * Copyright (C) 2024 Toshinobu Sugioka <sugioka@sugiokasystem.co.jp>
 *
 * This driver provides GPIO-style control for the LP5860T 11x18 LED matrix
 * controller. Each LED can be controlled individually via the GPIO subsystem.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/version.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/gpio/driver.h>
#include <linux/mutex.h>
#include <linux/regmap.h>

#define DRIVER_NAME "lp5860t"

/* LP5860T Register Addresses (from datasheet SNVSCE1B) */
/* Initialization and Control Registers */
#define LP5860T_REG_CHIP_EN         0x00  /* Chip enable control */
#define LP5860T_REG_DEV_INITIAL     0x01  /* Device initialization (MaxLine_Num, Data_refresh, PWM_Fre) */
#define LP5860T_REG_DEV_CONFIG1     0x02  /* Device config 1 (CS_on_shift, PWM_phase_shift, PWM_scale_mode, SW_BLK_time) */
#define LP5860T_REG_DEV_CONFIG2     0x03  /* Device config 2 (LSD_removal, LOD_removal, Comp_groups) */
#define LP5860T_REG_DEV_CONFIG3     0x04  /* Device config 3 (Max_Current, DownSide_Ghost, UpSide_Ghost) */

/* Brightness Control Registers */
#define LP5860T_REG_GLOBAL_BRI      0x05  /* Global 8-bit PWM brightness */
#define LP5860T_REG_GROUP0_BRI      0x06  /* Group 0 8-bit PWM brightness */
#define LP5860T_REG_GROUP1_BRI      0x07  /* Group 1 8-bit PWM brightness */
#define LP5860T_REG_GROUP2_BRI      0x08  /* Group 2 8-bit PWM brightness */

/* Color Current Registers */
#define LP5860T_REG_R_CURRENT       0x09  /* Red color current (CC) */
#define LP5860T_REG_G_CURRENT       0x0A  /* Green color current (CC) */
#define LP5860T_REG_B_CURRENT       0x0B  /* Blue color current (CC) */

/* Group Selection Registers */
#define LP5860T_REG_DOT_GRP_SEL_BASE 0x0C  /* 0x0C-0x42: Dot group selection (55 registers) */

/* ON/OFF Control Registers */
#define LP5860T_REG_DOT_ONOFF_BASE  0x43  /* 0x43-0x63: LED ON/OFF control (33 registers) */

/* Fault Detection Registers */
#define LP5860T_REG_FAULT_STATE     0x64  /* Global fault status */
#define LP5860T_REG_DOT_LOD_BASE    0x65  /* 0x65-0x85: LED open detection (33 registers) */
#define LP5860T_REG_DOT_LSD_BASE    0x86  /* 0x86-0xA6: LED short detection (33 registers) */

/* Fault Clear Registers */
#define LP5860T_REG_LOD_CLEAR       0xA7  /* Clear LED open detection flags */
#define LP5860T_REG_LSD_CLEAR       0xA8  /* Clear LED short detection flags */

/* Reset Register */
#define LP5860T_REG_RESET           0xA9  /* Software reset */

/* Dot Current Registers */
#define LP5860T_REG_DC_BASE         0x100 /* 0x100-0x1C5: Individual dot current DC0-197 (198 registers) */

/* PWM Brightness Registers */
#define LP5860T_REG_PWM_BRI_BASE    0x200 /* 0x200-0x2C5: 8-bit PWM (Mode 1&2, 198 registers) */
                                          /* 0x200-0x38B: 16-bit PWM (Mode 3, 396 registers) */

/* LP5860T 定数 */
#define LP5860T_MAX_SW              11    /* SW0-SW10 */
#define LP5860T_MAX_CS              18    /* CS0-CS17 */
#define LP5860T_MAX_LEDS            (LP5860T_MAX_SW * LP5860T_MAX_CS)
#define LP5860T_MAX_BRIGHTNESS      255

/* Register Bit Definitions (from datasheet SNVSCE1B) */

/* CHIP_EN Register (0x00) bits */
#define LP5860T_CHIP_EN             BIT(0)  /* Chip enable: 0=Standby, 1=Normal */

/* DEV_INITIAL Register (0x01) bits */
#define LP5860T_PWM_FREQ_SHIFT     0       /* PWM frequency: 0=125kHz, 1=62.5kHz */
#define LP5860T_PWM_FREQUENCY      1
#define LP5860T_REFRESH_MODE_SHIFT 1       /* Data refresh mode: 00=Mode1, 01=Mode2, 10=Mode3 */
#define LP5860T_REFRESH_MODE       0
#define LP5860T_MAX_LINE_NUM_SHIFT 3       /* Max scan line number minus 1 (0-10) */
#define LP5860T_MAX_LINE_NUM_MASK  0x0f    /* Max scan line number minus 1 (0-10) */

/* DEV_CONFIG1 Register (0x02) bits */
#define LP5860T_CS_ON_SHIFT         BIT(7)  /* CS turn-on shift enable */
#define LP5860T_PWM_PHASE_SHIFT     BIT(6)  /* PWM phase shift enable */
#define LP5860T_PWM_SCALE_MODE      BIT(5)  /* PWM scale: 0=Linear, 1=Exponential */
#define LP5860T_SW_BLK_TIME         BIT(4)  /* Switch blank time: 0=0.5μs, 1=1.0μs */

/* DEV_CONFIG2 Register (0x03) bits */
#define LP5860T_LSD_REMOVAL         BIT(7)  /* LED short detection removal */
#define LP5860T_LOD_REMOVAL         BIT(6)  /* LED open detection removal */

/* DEV_CONFIG3 Register (0x04) bits */
#define LP5860T_MAX_CURRENT_SHIFT       1       /* Maximum current setting (3 bits) */
#define LP5860T_MAX_CURRENT_MASK        0x07
#define LP5860T_DOWNSIDE_GHOST_SHIFT    6      /* Downside deghosting enable */
#define LP5860T_DOWNSIDE_GHOST_MASK     0x03
#define LP5860T_DOWNSIDE_GHOST_DEFAULT  0x00   /* Default deghosting setting */
#define LP5860T_UPSIDE_GHOST_SHIFT      4      /* Upside deghosting enable */
#define LP5860T_UPSIDE_GHOST_MASK       0x03
#define LP5860T_UPSIDE_GHOST_DEFAULT    0x02   /* Default deghosting setting */
#define LP5860T_UPSIDE_GHOST_ENABLE     BIT(0) /* Upside deghosting enable */

/* Maximum Current Settings */
#define LP5860T_MC_7_5MA            0       /* 7.5mA */
#define LP5860T_MC_12_5MA           1       /* 12.5mA */
#define LP5860T_MC_25MA             2       /* 25mA */
#define LP5860T_MC_37_5MA           3       /* 37.5mA (default) */
#define LP5860T_MC_50MA             4       /* 50mA */
#define LP5860T_MC_75MA             5       /* 75mA */
#define LP5860T_MC_100MA            6       /* 100mA */

#define LP5860T_DEFAULT_NUM_SW      6
#define LP5860T_DEFAULT_NUM_CS      16
#define LP5860T_DEFAULT_NUM_LEDS    (LP5860T_DEFAULT_NUM_SW * LP5860T_DEFAULT_NUM_CS)

#define LP5860T_DEFAULT_DOT_CURRENT 0x06  /* Default dot current setting (100mA * 6/255) */

/* Number of ON/OFF registers (0x43-0x63) */
#define LP5860T_NUM_ONOFF_REGS 33

struct lp5860t_chip {
    struct i2c_client *client;
    struct regmap *regmap;
    struct mutex lock;
    struct gpio_chip gpio_chip;
    int num_sw;     /* 使用するSW線の数 */
    int num_cs;     /* 使用するCS線の数 */
    int num_leds;   /* Total number of LEDs (sw * cs) */
    int gpio_base;  /* GPIO base number (-1 for dynamic allocation) */
    bool enabled;
    bool virtual_mode;  /* True when hardware is not available */
    u8 dot_current[LP5860T_MAX_LEDS];  /* Per-LED dot current values */
    u8 virtual_led_state[LP5860T_NUM_ONOFF_REGS];  /* Virtual LED on/off state */
    int chip_index; /* Index in global chip array */
    int test_mode;  /* Test mode: 0=normal, 1=all_on, 2=all_off (ignores GPIO writes) */
    u8 saved_onoff_regs[LP5860T_NUM_ONOFF_REGS];  /* Saved ON/OFF registers for test mode restore */
};

/* Global chip array for test mode access */
#define LP5860T_MAX_CHIPS 4
static struct lp5860t_chip *lp5860t_chips[LP5860T_MAX_CHIPS];
static int lp5860t_chip_count = 0;
static DEFINE_MUTEX(lp5860t_chips_lock);

/*
 * LP5860T I2C Register Access Functions
 *
 * LP5860T uses a special 10-bit register addressing scheme:
 * - Address Byte 1: [CA4 CA3 CA2 CA1 CA0 RA9 RA8 R/W]
 * - Address Byte 2: [RA7 RA6 RA5 RA4 RA3 RA2 RA1 RA0]
 *
 * Where CA4-CA0 is the 5-bit chip address, and RA9-RA0 is the 10-bit register address.
 * The upper 2 bits of the register address (RA9-RA8) are encoded in the I2C slave address.
 */
static int lp5860t_i2c_write(void *context, const void *data, size_t count)
{
    struct device *dev = context;
    struct i2c_client *client = to_i2c_client(dev);
    const u8 *buf = data;
    u16 reg_addr;
    u8 slave_addr;
    u8 reg_offset;
    u8 write_buf[2];
    struct i2c_msg msg;
    int ret;

    if (count < 2)
        return -EINVAL;

    /* Extract 16-bit register address (big-endian) */
    reg_addr = (buf[0] << 8) | buf[1];

    /* Calculate I2C slave address with upper 2 bits of register address */
    slave_addr = client->addr | ((reg_addr >> 8) & 0x03);

    /* Lower 8 bits of register address */
    reg_offset = reg_addr & 0xFF;

    /* Build write buffer: [reg_offset, data...] */
    write_buf[0] = reg_offset;
    if (count > 2)
        write_buf[1] = buf[2];  /* Single byte data */

    /* Setup I2C message with dynamic slave address */
    msg.addr = slave_addr;
    msg.flags = 0;
    msg.len = count - 1;  /* reg_offset + data */
    msg.buf = write_buf;

    /* Perform I2C write */
    ret = i2c_transfer(client->adapter, &msg, 1);
    if (ret < 0)
        return ret;
    if (ret != 1)
        return -EIO;

    return 0;
}

static int lp5860t_i2c_read(void *context, const void *reg_buf, size_t reg_size,
                           void *val_buf, size_t val_size)
{
    struct device *dev = context;
    struct i2c_client *client = to_i2c_client(dev);
    const u8 *reg_data = reg_buf;
    u16 reg_addr;
    u8 slave_addr;
    u8 reg_offset;
    struct i2c_msg msgs[2];
    int ret;

    if (reg_size < 2)
        return -EINVAL;

    /* Extract 16-bit register address (big-endian) */
    reg_addr = (reg_data[0] << 8) | reg_data[1];

    /* Calculate I2C slave address with upper 2 bits of register address */
    slave_addr = client->addr | ((reg_addr >> 8) & 0x03);

    /* Lower 8 bits of register address */
    reg_offset = reg_addr & 0xFF;

    /* Setup I2C messages for write-then-read transaction */
    msgs[0].addr = slave_addr;
    msgs[0].flags = 0;
    msgs[0].len = 1;
    msgs[0].buf = &reg_offset;

    msgs[1].addr = slave_addr;
    msgs[1].flags = I2C_M_RD;
    msgs[1].len = val_size;
    msgs[1].buf = val_buf;

    /* Perform I2C read transaction */
    ret = i2c_transfer(client->adapter, msgs, 2);
    if (ret < 0)
        return ret;
    if (ret != 2)
        return -EIO;

    return 0;
}

static struct regmap_bus lp5860t_regmap_bus = {
    .write = lp5860t_i2c_write,
    .read = lp5860t_i2c_read,
    .reg_format_endian_default = REGMAP_ENDIAN_BIG,
    .val_format_endian_default = REGMAP_ENDIAN_NATIVE,
};

static const struct regmap_config lp5860t_regmap_config = {
    .reg_bits = 16,  /* LP5860T uses 10-bit register addresses (0x000-0x38B) */
    .val_bits = 8,
    .max_register = 0x38B,  /* Maximum register for Mode 3 (16-bit PWM) */
    .cache_type = REGCACHE_NONE,
};

/* SW/CS座標から個別LED輝度レジスタアドレスを計算 */
static inline u16 lp5860t_get_brightness_reg(u8 sw, u8 cs)
{
    return LP5860T_REG_PWM_BRI_BASE + (sw * LP5860T_MAX_CS) + cs;
}

/*
 * GPIO operations for LP5860T
 * Each GPIO corresponds to one LED (sw * cs matrix)
 * GPIO is controlled via ON/OFF register, PWM is set to 255
 */

/* Convert GPIO offset to SW/CS coordinates */
static void lp5860t_index_to_sw_cs(struct lp5860t_chip *chip, unsigned offset,
                                         u8 *sw, u8 *cs)
{
    *sw = offset / chip->num_cs;
    *cs = offset % chip->num_cs;
}

/* Get ON/OFF register address and bit position for given SW/CS */
static void lp5860t_get_onoff_reg(struct lp5860t_chip *chip, u8 sw, u8 cs, u16 *reg_addr, u8 *bit_pos)
{
    *reg_addr = LP5860T_REG_DOT_ONOFF_BASE + (sw * 3) + (cs / 8);
    *bit_pos = cs % 8;
}

static int lp5860t_gpio_get(struct gpio_chip *gc, unsigned offset)
{
    struct lp5860t_chip *chip = gpiochip_get_data(gc);
    u8 sw, cs, bit_pos;
    u16 reg_addr;
    unsigned int reg_val;
    int ret;

    if (offset >= chip->num_leds)
        return 0;

    lp5860t_index_to_sw_cs(chip, offset, &sw, &cs);
    lp5860t_get_onoff_reg(chip, sw, cs, &reg_addr, &bit_pos);

    mutex_lock(&chip->lock);

    /* Virtual mode: use in-memory state */
    if (chip->virtual_mode) {
        int reg_idx = reg_addr - LP5860T_REG_DOT_ONOFF_BASE;
        if (reg_idx >= 0 && reg_idx < LP5860T_NUM_ONOFF_REGS)
            ret = (chip->virtual_led_state[reg_idx] >> bit_pos) & 1;
        else
            ret = 0;
        mutex_unlock(&chip->lock);
        return ret;
    }

    ret = regmap_read(chip->regmap, reg_addr, &reg_val);
    if (ret < 0) {
        dev_err(&chip->client->dev, "Failed to read ON/OFF register: %d\n", ret);
        mutex_unlock(&chip->lock);
        return 0;
    }
    ret = (reg_val >> bit_pos) & 1;

    mutex_unlock(&chip->lock);
    return ret;
}

static void lp5860t_gpio_set(struct gpio_chip *gc, unsigned offset, int value)
{
    struct lp5860t_chip *chip = gpiochip_get_data(gc);
    u8 sw, cs, bit_pos;
    u16 reg_addr;
    unsigned int reg_val;
    int ret;

    if (offset >= chip->num_leds)
        return;

    /* Skip GPIO writes when in test mode (all_on or all_off) */
    if (chip->test_mode != 0)
        return;

    lp5860t_index_to_sw_cs(chip, offset, &sw, &cs);
    lp5860t_get_onoff_reg(chip, sw, cs, &reg_addr, &bit_pos);

    mutex_lock(&chip->lock);

    /* Virtual mode: update in-memory state only */
    if (chip->virtual_mode) {
        int reg_idx = reg_addr - LP5860T_REG_DOT_ONOFF_BASE;
        if (reg_idx >= 0 && reg_idx < LP5860T_NUM_ONOFF_REGS) {
            if (value)
                chip->virtual_led_state[reg_idx] |= (1 << bit_pos);
            else
                chip->virtual_led_state[reg_idx] &= ~(1 << bit_pos);
        }
        mutex_unlock(&chip->lock);
        return;
    }

    /* Read-modify-write */
    ret = regmap_read(chip->regmap, reg_addr, &reg_val);
    if (ret < 0) {
        dev_err(&chip->client->dev, "Failed to read ON/OFF register: %d\n", ret);
        goto unlock;
    }

    if (value)
        reg_val |= (1 << bit_pos);
    else
        reg_val &= ~(1 << bit_pos);

    ret = regmap_write(chip->regmap, reg_addr, reg_val);
    if (ret < 0) {
        dev_err(&chip->client->dev, "Failed to write ON/OFF register: %d\n", ret);
    }

unlock:
    mutex_unlock(&chip->lock);
}

static int lp5860t_gpio_direction_output(struct gpio_chip *gc, unsigned offset, int value)
{
    lp5860t_gpio_set(gc, offset, value);
    return 0;
}


static int lp5860t_init_device(struct lp5860t_chip *chip)
{
    int ret;
    int i, j;
    unsigned int reg_val;

    /* デバイスリセット */
    ret = regmap_write(chip->regmap, LP5860T_REG_RESET, 0xFF);
    if (ret < 0) {
        msleep(10);
        ret = regmap_write(chip->regmap, LP5860T_REG_RESET, 0xFF);
        if (ret < 0) {
            /* Hardware not available - fall back to virtual mode */
            dev_warn(&chip->client->dev, "Hardware not available, using virtual mode\n");
            chip->virtual_mode = true;
            memset(chip->virtual_led_state, 0, sizeof(chip->virtual_led_state));
            chip->enabled = true;
            return 0;
        }
    }

    msleep(10);

    /* Enable chip (CHIP_EN register bit 0) */
    ret = regmap_write(chip->regmap, LP5860T_REG_CHIP_EN, LP5860T_CHIP_EN);
    if (ret < 0) {
        dev_err(&chip->client->dev, "Failed to enable chip\n");
        return -EIO;
    }
    /* configure number of LED lines, refresh mode, pwm frequency */
    reg_val = chip->num_sw << LP5860T_MAX_LINE_NUM_SHIFT;
    reg_val |= LP5860T_REFRESH_MODE << LP5860T_REFRESH_MODE_SHIFT;
    reg_val |= LP5860T_PWM_FREQUENCY << LP5860T_PWM_FREQ_SHIFT;
    ret = regmap_write(chip->regmap, LP5860T_REG_DEV_INITIAL, reg_val);
    if (ret < 0) {
        dev_err(&chip->client->dev, "Failed to configure number of LED lines, refresh mode, pwm frequency\n");
        return -EIO;
    }
    /* Configure device (exponential PWM scale for smoother brightness control) */
    ret = regmap_write(chip->regmap, LP5860T_REG_DEV_CONFIG1, LP5860T_PWM_SCALE_MODE);
    if (ret < 0) {
        dev_err(&chip->client->dev, "Failed to configure device settings\n");
        return -EIO;
    }
    /* 最大LED電流を75mAに設定 */
    reg_val = LP5860T_MC_75MA << LP5860T_MAX_CURRENT_SHIFT;
    reg_val |= LP5860T_DOWNSIDE_GHOST_DEFAULT << LP5860T_DOWNSIDE_GHOST_SHIFT;
    reg_val |= LP5860T_UPSIDE_GHOST_DEFAULT << LP5860T_UPSIDE_GHOST_SHIFT;
    reg_val |= LP5860T_UPSIDE_GHOST_ENABLE;
    ret = regmap_write(chip->regmap, LP5860T_REG_DEV_CONFIG3, reg_val);
    if (ret < 0) {
        dev_err(&chip->client->dev, "Failed to set LED current\n");
        return -EIO;
    }
    /* バンク輝度を最大に設定 */
    ret = regmap_write(chip->regmap, LP5860T_REG_GLOBAL_BRI, 0xFF);
    if (ret < 0) {
        dev_err(&chip->client->dev, "Failed to set bank brightness\n");
        return -EIO;
    }
    /* R/G/B電流を100%に設定 */
    ret = regmap_write(chip->regmap, LP5860T_REG_R_CURRENT, 0x7f);
    if (ret < 0) {
        dev_err(&chip->client->dev, "Failed to set R current\n");
        return -EIO;
    }
    ret = regmap_write(chip->regmap, LP5860T_REG_G_CURRENT, 0x7f);
    if (ret < 0) {
        dev_err(&chip->client->dev, "Failed to set G current\n");
        return -EIO;
    }
    ret = regmap_write(chip->regmap, LP5860T_REG_B_CURRENT, 0x7f);
    if (ret < 0) {
        dev_err(&chip->client->dev, "Failed to set B current\n");
        return -EIO;
    }
    /* Set per-LED dot current from chip->dot_current array (configured by DTS or defaults) */
    for (i = 0; i < LP5860T_MAX_SW; i++) {
        for (j = 0; j < LP5860T_MAX_CS; j++) {
            int led_idx = i * LP5860T_MAX_CS + j;
            unsigned int reg_addr = LP5860T_REG_DC_BASE + led_idx;
            u8 val = chip->dot_current[led_idx];
            ret = regmap_write(chip->regmap, reg_addr, val);
            if (ret < 0) {
                dev_err(&chip->client->dev, "Failed to set LED%d:%d dot current to 0x%02x\n", i, j, val);
                return -EIO;
            }
        }
    }
    /* すべてのON/OFFレジスタを0に初期化（すべてOFF） */
    for (i = LP5860T_REG_DOT_ONOFF_BASE; i <= 0x63; i++) {
        ret = regmap_write(chip->regmap, i, 0x00);
        if (ret < 0) {
            dev_err(&chip->client->dev, "Failed to init ON/OFF register\n");
            return -EIO;
        }
    }

    /* すべてのLED PWM輝度を255に設定（GPIO制御のため） */
    for (i = 0; i < LP5860T_MAX_SW; i++) {
        for (j = 0; j < LP5860T_MAX_CS; j++) {
            u16 reg_addr = lp5860t_get_brightness_reg(i, j);
            ret = regmap_write(chip->regmap, reg_addr, 0xFF);
            if (ret < 0) {
                dev_err(&chip->client->dev, "Failed to set LED PWM to 255\n");
                return -EIO;
            }
        }
    }

    chip->enabled = true;
    dev_info(&chip->client->dev, "Hardware LP5860T device initialized successfully\n");
    return 0;
}

static int lp5860t_parse_dot_current_groups(struct lp5860t_chip *chip, struct device *dev)
{
    struct device_node *np = dev->of_node;
    struct device_node *groups_np, *group_np;
    int ret;

    if (!np)
        return 0;

    groups_np = of_get_child_by_name(np, "dot-current-groups");
    if (!groups_np) {
        dev_dbg(dev, "No dot-current-groups node found, using defaults\n");
        return 0;
    }

    for_each_child_of_node(groups_np, group_np) {
        u32 dot_current_val;
        int num_leds, i;
        u32 *led_list;

        /* Read dot-current value for this group */
        ret = of_property_read_u32(group_np, "ti,dot-current", &dot_current_val);
        if (ret < 0) {
            dev_warn(dev, "Group %s: missing ti,dot-current property\n", group_np->name);
            continue;
        }

        /* Clamp to 8-bit range */
        if (dot_current_val > 0xFF)
            dot_current_val = 0xFF;

        /* Count and read LED indices */
        num_leds = of_property_count_u32_elems(group_np, "ti,leds");
        if (num_leds <= 0) {
            dev_warn(dev, "Group %s: missing or empty ti,leds property\n", group_np->name);
            continue;
        }

        led_list = kcalloc(num_leds, sizeof(u32), GFP_KERNEL);
        if (!led_list) {
            of_node_put(group_np);
            of_node_put(groups_np);
            return -ENOMEM;
        }

        ret = of_property_read_u32_array(group_np, "ti,leds", led_list, num_leds);
        if (ret < 0) {
            dev_warn(dev, "Group %s: failed to read ti,leds array\n", group_np->name);
            kfree(led_list);
            continue;
        }

        /* Apply dot current to each LED in the group */
        for (i = 0; i < num_leds; i++) {
            u32 led_idx = led_list[i];
            u8 sw, cs;
            lp5860t_index_to_sw_cs(chip, led_idx, &sw, &cs);
            led_idx = sw * LP5860T_MAX_CS + cs;  /* Recalculate to ensure valid index */
            if (led_idx < LP5860T_MAX_LEDS) {
                chip->dot_current[led_idx] = (u8)dot_current_val;
                dev_dbg(dev, "LED %u: dot-current = 0x%02x (group %s)\n",
                        led_idx, dot_current_val, group_np->name);
            } else {
                dev_warn(dev, "Group %s: LED index %u out of range (max %d)\n",
                         group_np->name, led_idx, LP5860T_MAX_LEDS - 1);
            }
        }

        dev_info(dev, "Group %s: %d LEDs with dot-current 0x%02x\n",
                 group_np->name, num_leds, dot_current_val);

        kfree(led_list);
    }

    of_node_put(groups_np);
    return 0;
}

static int lp5860t_parse_dt(struct lp5860t_chip *chip, struct device *dev)
{
    struct device_node *np = dev->of_node;
    int ret;

    /* Initialize default dot current values first */
    memset(chip->dot_current, LP5860T_DEFAULT_DOT_CURRENT, sizeof(chip->dot_current));

    if (!np) {
        /* No device tree available - use defaults for testing */
        dev_info(dev, "No device tree node found, using default configuration\n");
        chip->num_sw = LP5860T_DEFAULT_NUM_SW;
        chip->num_cs = LP5860T_DEFAULT_NUM_CS;
        chip->gpio_base = -1;  /* Dynamic allocation */
    } else {
        /* SW線とCS線の数を取得（デフォルト値あり） */
        ret = of_property_read_u32(np, "ti,num-sw", &chip->num_sw);
        if (ret < 0)
            chip->num_sw = LP5860T_DEFAULT_NUM_SW;
        else if (chip->num_sw > LP5860T_MAX_SW)
            chip->num_sw = LP5860T_MAX_SW;

        ret = of_property_read_u32(np, "ti,num-cs", &chip->num_cs);
        if (ret < 0)
            chip->num_cs = LP5860T_DEFAULT_NUM_CS;
        else if (chip->num_cs > LP5860T_MAX_CS)
            chip->num_cs = LP5860T_MAX_CS;

        /* Read GPIO base from device tree, default to -1 (auto-assign) if not specified */
        ret = of_property_read_u32(np, "gpio-base", (u32 *)&chip->gpio_base);
        if (ret < 0)
            chip->gpio_base = -1;

        /* Parse dot-current groups from device tree */
        ret = lp5860t_parse_dot_current_groups(chip, dev);
        if (ret < 0)
            return ret;
    }

    /* Calculate total number of LEDs (GPIOs) */
    chip->num_leds = chip->num_sw * chip->num_cs;

    dev_info(dev, "LP5860T configuration: SW:%d, CS:%d, GPIOs:%d, GPIO-base:%d\n",
             chip->num_sw, chip->num_cs, chip->num_leds, chip->gpio_base);

    return 0;
}

static int lp5860t_probe(struct i2c_client *client)
{
    struct lp5860t_chip *chip;
    struct device *dev = &client->dev;
    int ret;

    chip = devm_kzalloc(dev, sizeof(*chip), GFP_KERNEL);
    if (!chip)
        return -ENOMEM;

    chip->client = client;
    mutex_init(&chip->lock);
    i2c_set_clientdata(client, chip);

    /* Initialize regmap with custom I2C bus for LP5860T's 10-bit register addressing */
    chip->regmap = devm_regmap_init(dev, &lp5860t_regmap_bus, dev, &lp5860t_regmap_config);
    if (IS_ERR(chip->regmap)) {
        dev_err(dev, "Failed to initialize regmap: %ld\n", PTR_ERR(chip->regmap));
        return PTR_ERR(chip->regmap);
    }

    /* Device Treeから設定を読み込み */
    ret = lp5860t_parse_dt(chip, dev);
    if (ret < 0) {
        dev_err(dev, "Failed to parse device tree\n");
        return ret;
    }

    /* デバイスを初期化 */
    ret = lp5860t_init_device(chip);
    if (ret < 0) {
        dev_err(dev, "Failed to initialize LP5860T\n");
        return ret;
    }

    /* Setup GPIO chip */
    chip->gpio_chip.label = "lp5860t";
    chip->gpio_chip.parent = dev;
    chip->gpio_chip.owner = THIS_MODULE;
    chip->gpio_chip.base = chip->gpio_base;  /* Use configured base or -1 for dynamic */
    chip->gpio_chip.ngpio = chip->num_leds;
    chip->gpio_chip.can_sleep = true;
    chip->gpio_chip.get = lp5860t_gpio_get;
    chip->gpio_chip.set = lp5860t_gpio_set;
    chip->gpio_chip.direction_output = lp5860t_gpio_direction_output;

    /* Register GPIO chip */
    ret = devm_gpiochip_add_data(dev, &chip->gpio_chip, chip);
    if (ret < 0) {
        dev_err(dev, "Failed to register GPIO chip: %d\n", ret);
        return ret;
    }

    /* Register chip in global array for test mode access */
    mutex_lock(&lp5860t_chips_lock);
    if (lp5860t_chip_count < LP5860T_MAX_CHIPS) {
        chip->chip_index = lp5860t_chip_count;
        lp5860t_chips[lp5860t_chip_count] = chip;
        lp5860t_chip_count++;
    } else {
        chip->chip_index = -1;
        dev_warn(dev, "Maximum number of LP5860T chips reached, test mode disabled for this chip\n");
    }
    mutex_unlock(&lp5860t_chips_lock);

    dev_info(dev, "LP5860T GPIO driver initialized (SW:%d, CS:%d, GPIOs:%d)\n",
             chip->num_sw, chip->num_cs, chip->num_leds);

    return 0;
}

static void lp5860t_remove(struct i2c_client *client)
{
    struct lp5860t_chip *chip = i2c_get_clientdata(client);

    /* Unregister chip from global array */
    mutex_lock(&lp5860t_chips_lock);
    if (chip->chip_index >= 0 && chip->chip_index < LP5860T_MAX_CHIPS) {
        lp5860t_chips[chip->chip_index] = NULL;
        /* Note: chip_count is not decremented to maintain indices */
    }
    mutex_unlock(&lp5860t_chips_lock);

    /* デバイスを無効化 */
    chip->enabled = false;
    regmap_write(chip->regmap, LP5860T_REG_CHIP_EN, 0x00);

    dev_info(&client->dev, "LP5860T GPIO driver removed\n");
}

static void lp5860t_shutdown(struct i2c_client *client)
{
    struct lp5860t_chip *chip = i2c_get_clientdata(client);

    /* デバイスを無効化 */
    chip->enabled = false;
    regmap_write(chip->regmap, LP5860T_REG_CHIP_EN, 0x00);
}

#ifdef CONFIG_PM_SLEEP
static int lp5860t_suspend(struct device *dev)
{
    struct i2c_client *client = to_i2c_client(dev);
    struct lp5860t_chip *chip = i2c_get_clientdata(client);

    /* デバイスを無効化 */
    chip->enabled = false;
    regmap_write(chip->regmap, LP5860T_REG_CHIP_EN, 0x00);

    return 0;
}

static int lp5860t_resume(struct device *dev)
{
    struct i2c_client *client = to_i2c_client(dev);
    struct lp5860t_chip *chip = i2c_get_clientdata(client);

    return lp5860t_init_device(chip);
}
#endif

/**
 * lp5860t_set_test_mode - Set LED test mode for all chips
 * @mode: 0=normal (restore), 1=all_on, 2=all_off
 *
 * Returns: 0 on success, negative error code on failure
 */
int lp5860t_set_test_mode(int mode);
int lp5860t_set_test_mode(int mode)
{
    int i, j, ret = 0;
    u8 val;
    unsigned int reg_val;

    mutex_lock(&lp5860t_chips_lock);

    for (i = 0; i < lp5860t_chip_count; i++) {
        struct lp5860t_chip *chip = lp5860t_chips[i];
        if (!chip || !chip->enabled || chip->virtual_mode)
            continue;

        mutex_lock(&chip->lock);

        if (mode == 0) {
            /* Normal mode: restore saved ON/OFF registers */
            for (j = 0; j < LP5860T_NUM_ONOFF_REGS; j++) {
                ret = regmap_write(chip->regmap, LP5860T_REG_DOT_ONOFF_BASE + j,
                                   chip->saved_onoff_regs[j]);
                if (ret < 0) {
                    dev_err(&chip->client->dev, "Failed to restore ON/OFF register\n");
                    goto unlock_chip;
                }
            }
            /* Clear test_mode flag after restoring to allow GPIO writes */
            chip->test_mode = 0;
        } else {
            /* Entering test mode: save current ON/OFF registers if not already in test mode */
            if (chip->test_mode == 0) {
                for (j = 0; j < LP5860T_NUM_ONOFF_REGS; j++) {
                    ret = regmap_read(chip->regmap, LP5860T_REG_DOT_ONOFF_BASE + j, &reg_val);
                    if (ret < 0) {
                        dev_err(&chip->client->dev, "Failed to save ON/OFF register\n");
                        goto unlock_chip;
                    }
                    chip->saved_onoff_regs[j] = (u8)reg_val;
                }
            }

            /* Set test_mode flag to block GPIO writes */
            chip->test_mode = mode;

            /* Test mode: set all ON/OFF registers to 0xFF (all on) or 0x00 (all off) */
            val = (mode == 1) ? 0xFF : 0x00;
            for (j = 0; j < LP5860T_NUM_ONOFF_REGS; j++) {
                ret = regmap_write(chip->regmap, LP5860T_REG_DOT_ONOFF_BASE + j, val);
                if (ret < 0) {
                    dev_err(&chip->client->dev, "Failed to set test mode ON/OFF register\n");
                    goto unlock_chip;
                }
            }
        }

unlock_chip:
        mutex_unlock(&chip->lock);
        if (ret < 0)
            break;
    }

    mutex_unlock(&lp5860t_chips_lock);
    return ret;
}
EXPORT_SYMBOL_GPL(lp5860t_set_test_mode);

/* Exported symbol for module dependency - dio.ko requires lp5860t.ko */
bool lp5860t_is_available(void);
bool lp5860t_is_available(void)
{
    return true;
}
EXPORT_SYMBOL_GPL(lp5860t_is_available);

static const struct dev_pm_ops lp5860t_pm_ops = {
    SET_SYSTEM_SLEEP_PM_OPS(lp5860t_suspend, lp5860t_resume)
};

static const struct i2c_device_id lp5860t_id[] = {
    { "lp5860t", 0 },
    { }
};
MODULE_DEVICE_TABLE(i2c, lp5860t_id);

static const struct of_device_id lp5860t_of_match[] = {
    { .compatible = "ti,lp5860t" },
    { }
};
MODULE_DEVICE_TABLE(of, lp5860t_of_match);

static struct i2c_driver lp5860t_driver = {
    .driver = {
        .name = DRIVER_NAME,
        .of_match_table = of_match_ptr(lp5860t_of_match),
        .pm = &lp5860t_pm_ops,
    },
    .probe = lp5860t_probe,
    .remove = lp5860t_remove,
    .shutdown = lp5860t_shutdown,
    .id_table = lp5860t_id,
};

module_i2c_driver(lp5860t_driver);

MODULE_AUTHOR("Toshinobu Sugioka <sugioka@sugiokasystem.co.jp>");
MODULE_DESCRIPTION("Texas Instruments LP5860T LED Matrix Controller GPIO Driver");
MODULE_LICENSE("GPL");
