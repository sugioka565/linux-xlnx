/*
 * Xilinx TFT frame buffer driver
 *
 * Author: MontaVista Software, Inc.
 *         source@mvista.com
 *
 * 2002-2007 (c) MontaVista Software, Inc.
 * 2007 (c) Secret Lab Technologies, Ltd.
 * 2009 (c) Xilinx Inc.
 *
 * This file is licensed under the terms of the GNU General Public License
 * version 2.  This program is licensed "as is" without any warranty of any
 * kind, whether express or implied.
 */

/*
 * This driver was based on au1100fb.c by MontaVista rewritten for 2.6
 * by Embedded Alley Solutions <source@embeddedalley.com>, which in turn
 * was based on skeletonfb.c, Skeleton for a frame buffer device by
 * Geert Uytterhoeven.
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/fb.h>
#include <linux/init.h>
#include <linux/dma-mapping.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/io.h>
#include <linux/slab.h>

#define DRIVER_NAME		"xilinx-hdfb"

/*
 * Xilinx calls it "TFT LCD Controller" though it can also be used for
 * the VGA port on the Xilinx ML40x board. This is a hardware display
 * controller for a 640x480 resolution TFT or VGA screen.
 *
 * The interface to the framebuffer is nice and simple.  There are two
 * control registers.  The first tells the LCD interface where in memory
 * the frame buffer is (only the 11 most significant bits are used, so
 * don't start thinking about scrolling).
 *
 * In case of direct BUS access the second control register will be at
 * an offset of 4 as compared to the DCR access where the offset is 1
 * i.e. REG_CTRL. So this is taken care in the function
 * xilinx_fb_out32 where it left shifts the offset 2 times in case of
 * direct BUS access.
 */
#define NUM_REGS	2
#define REG_CTRL	0
#define REG_CTRL_ENABLE	0x81
#define REG_WIDTH	4
#define REG_HEIGHT	6
#define REG_STRIDE	8
#define REG_FMT		10
#define REG_FB_ADDR	12

/*
 * The hardware only handles a single mode: 1280x720 24 bit true
 * color. Each pixel gets a word (32 bits) of memory.  Within each word,
 * the 8 most significant bits are ignored, the next 8 bits are the red
 * level, the next 8 bits are the green level and the 8 least
 * significant bits are the blue level.  Each row of the LCD uses 2048
 * words, but only the first 1280 pixels are displayed with the other
 * words being ignored.  There are 720 rows.
 */
#define BYTES_PER_PIXEL	4
#define BITS_PER_PIXEL	(BYTES_PER_PIXEL * 8)

#define RED_SHIFT	16
#define GREEN_SHIFT	8
#define BLUE_SHIFT	0
#define FMT_XRGB	27

#define PALETTE_ENTRIES_NO	16	/* passed to fb_alloc_cmap() */

/* ML300/403 reference design framebuffer driver platform data struct */
struct xilinxfb_platform_data {
	u32 screen_height_mm;   /* Physical dimensions of screen in mm */
	u32 screen_width_mm;
	u32 xres, yres;         /* resolution of screen in pixels */
	u32 xvirt, yvirt;       /* resolution of memory buffer */

	/* Physical address of framebuffer memory; If non-zero, driver
	 * will use provided memory address instead of allocating one from
	 * the consistent pool.
	 */
	u32 fb_phys;
	u32 fb_size;
};

/*
 * Default xilinxfb configuration
 */
static const struct xilinxfb_platform_data xilinx_fb_default_pdata = {
	.xres = 1280,
	.yres = 720,
	.xvirt = 2048,
	.yvirt = 720,
};

/*
 * Here are the default fb_fix_screeninfo and fb_var_screeninfo structures
 */
static const struct fb_fix_screeninfo xilinx_fb_fix = {
	.id =		"Xilinx",
	.type =		FB_TYPE_PACKED_PIXELS,
	.visual =	FB_VISUAL_TRUECOLOR,
	.accel =	FB_ACCEL_NONE
};

static const struct fb_var_screeninfo xilinx_fb_var = {
	.bits_per_pixel =	BITS_PER_PIXEL,

	.red =		{ RED_SHIFT, 8, 0 },
	.green =	{ GREEN_SHIFT, 8, 0 },
	.blue =		{ BLUE_SHIFT, 8, 0 },
	.transp =	{ 0, 0, 0 },

	.activate =	FB_ACTIVATE_NOW
};

struct xilinxfb_drvdata {
	struct fb_info	info;		/* FB driver info record */

	phys_addr_t	regs_phys;	/* phys. address of the control
					 * registers
					 */
	void __iomem	*regs;		/* virt. address of the control
					 * registers
					 */
	void		*fb_virt;	/* virt. address of the frame buffer */
	dma_addr_t	fb_phys;	/* phys. address of the frame buffer */
	int		fb_alloced;	/* Flag, was the fb memory alloced? */

	u32		reg_ctrl_default;

	u32		pseudo_palette[PALETTE_ENTRIES_NO];
					/* Fake palette of 16 colors */
};

#define to_xilinxfb_drvdata(_info) \
	container_of(_info, struct xilinxfb_drvdata, info)

struct fb_videomode xilinxfb_modedb[] = {

	/*
	 *  XilinxHD Video Modes
	 *
	 *  If you change these, make sure to update DEFMODE_* as well!
	 */
	/* 60 Hz broadcast modes (modes "1" to "5") */
	{
		/* 480p */
		"480p", 60, 576, 384, 37037, 130, 89, 78, 57, 63, 6,
		FB_SYNC_BROADCAST, FB_VMODE_NONINTERLACED
	}, {
		/* 720p */
		"720p", 60, 1124, 644, 13481, 298, 148, 57, 44, 80, 5,
		FB_SYNC_BROADCAST, FB_VMODE_NONINTERLACED
	}, {
		/* 1080p */
		"1080p", 60, 1688, 964, 6741, 264, 160, 94, 62, 88, 5,
		FB_SYNC_BROADCAST, FB_VMODE_NONINTERLACED
	},
	/* 50 Hz broadcast modes (modes "6" to "10") */
	{
		/* 576p */
		"576p", 50, 576, 460, 37037, 142, 83, 97, 63, 63, 5,
		FB_SYNC_BROADCAST, FB_VMODE_NONINTERLACED
	}, {
		/* 720p */
		"720p", 50, 1124, 644, 13468, 298, 478, 57, 44, 80, 5,
		FB_SYNC_BROADCAST, FB_VMODE_NONINTERLACED
	},    {
		/* 1080p */
		"1080p", 50, 1688, 964, 6734, 264, 600, 94, 62, 88, 5,
		FB_SYNC_BROADCAST, FB_VMODE_NONINTERLACED
	},

	/*
	 *  VGA Video Modes
	 */

	{
		/* 640x480, 31 kHz, 60 Hz (VGA) */
		"vga", 60, 640, 480, 0, 64, 96, 30, 9, 112, 2,
		0, FB_VMODE_NONINTERLACED
	},
	/* VESA modes (modes "11" to "13") */
	{
		/* WXGA */
		"wxga", 60, 1280, 768, 12924, 160, 24, 29, 3, 136, 6,
		0, FB_VMODE_NONINTERLACED,
		FB_MODE_IS_VESA
	}, {
		/* SXGA */
		"sxga", 60, 1280, 1024, 9259, 248, 48, 38, 1, 112, 3,
		FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT, FB_VMODE_NONINTERLACED,
		FB_MODE_IS_VESA
	}, {
		/* WUXGA */
		"wuxga", 60, 1920, 1200, 6494, 80, 48, 26, 3, 32, 6,
		FB_SYNC_HOR_HIGH_ACT, FB_VMODE_NONINTERLACED,
		FB_MODE_IS_VESA
	}
};

#define NUM_TOTAL_MODES  ARRAY_SIZE(ami_modedb)

/*
 * The XPS TFT Controller can be accessed through BUS or DCR interface.
 * To perform the read/write on the registers we need to check on
 * which bus its connected and call the appropriate write API.
 */
static void xilinx_fb_out32(struct xilinxfb_drvdata *drvdata, u32 offset,
			    u32 val)
{
	iowrite32(val, drvdata->regs + (offset << 2));
}

// static u32 xilinx_fb_in32(struct xilinxfb_drvdata *drvdata, u32 offset)
// {
// 	return ioread32(drvdata->regs + (offset << 2));
// }

static int
xilinx_fb_setcolreg(unsigned int regno, unsigned int red, unsigned int green,
		    unsigned int blue, unsigned int transp, struct fb_info *fbi)
{
	u32 *palette = fbi->pseudo_palette;

	if (regno >= PALETTE_ENTRIES_NO)
		return -EINVAL;

	if (fbi->var.grayscale) {
		/* Convert color to grayscale.
		 * grayscale = 0.30*R + 0.59*G + 0.11*B
		 */
		blue = (red * 77 + green * 151 + blue * 28 + 127) >> 8;
		green = blue;
		red = green;
	}

	/* fbi->fix.visual is always FB_VISUAL_TRUECOLOR */

	/* We only handle 8 bits of each color. */
	red >>= 8;
	green >>= 8;
	blue >>= 8;
	palette[regno] = (red << RED_SHIFT) | (green << GREEN_SHIFT) |
			 (blue << BLUE_SHIFT);

	return 0;
}

static int
xilinx_fb_blank(int blank_mode, struct fb_info *fbi)
{
	struct xilinxfb_drvdata *drvdata = to_xilinxfb_drvdata(fbi);

	switch (blank_mode) {
	case FB_BLANK_UNBLANK:
		/* turn on panel */
		xilinx_fb_out32(drvdata, REG_CTRL, drvdata->reg_ctrl_default);
		break;

	case FB_BLANK_NORMAL:
	case FB_BLANK_VSYNC_SUSPEND:
	case FB_BLANK_HSYNC_SUSPEND:
	case FB_BLANK_POWERDOWN:
		/* turn off panel */
		xilinx_fb_out32(drvdata, REG_CTRL, 0);
	default:
		break;
	}
	return 0; /* success */
}

static struct fb_ops xilinxfb_ops = {
	.owner			= THIS_MODULE,
	.fb_setcolreg		= xilinx_fb_setcolreg,
	.fb_blank		= xilinx_fb_blank,
	.fb_fillrect		= cfb_fillrect,
	.fb_copyarea		= cfb_copyarea,
	.fb_imageblit		= cfb_imageblit,
};

/* ---------------------------------------------------------------------
 * Bus independent setup/teardown
 */

static int xilinxfb_assign(struct platform_device *pdev,
			   struct xilinxfb_drvdata *drvdata,
			   struct xilinxfb_platform_data *pdata)
{
	int rc;
	struct device *dev = &pdev->dev;
	int fbsize = pdata->xvirt * pdata->yvirt * BYTES_PER_PIXEL;
	struct resource *res;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	drvdata->regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(drvdata->regs))
		return PTR_ERR(drvdata->regs);
	drvdata->regs_phys = res->start;
	dev_info(dev, "resister phys addr:0x%x size:0x%x\n", drvdata->regs_phys, res->end - res->start + 1);

	/* Allocate the framebuffer memory */
	if (pdata->fb_phys) {
		drvdata->fb_phys = pdata->fb_phys;
		if (pdata->fb_size)
			fbsize = pdata->fb_size;
		drvdata->fb_virt = ioremap(pdata->fb_phys, fbsize);
	} else {
		drvdata->fb_alloced = 1;
		drvdata->fb_virt = dma_alloc_coherent(dev, PAGE_ALIGN(fbsize),
						      &drvdata->fb_phys,
						      GFP_KERNEL);
	}

	if (!drvdata->fb_virt) {
		dev_err(dev, "Could not allocate frame buffer memory. phys=0x%x, size=0x%x\n", pdata->fb_phys, pdata->fb_size);
		return -ENOMEM;
	}

	/* Clear (turn to black) the framebuffer */
	memset_io((void __iomem *)drvdata->fb_virt, 0, fbsize);

	xilinx_fb_out32(drvdata, REG_CTRL, 0);
	/* Tell the hardware where the frame buffer is */
	xilinx_fb_out32(drvdata, REG_FB_ADDR, drvdata->fb_phys);
	xilinx_fb_out32(drvdata, REG_WIDTH, pdata->xres);
	xilinx_fb_out32(drvdata, REG_HEIGHT, pdata->yres);
	xilinx_fb_out32(drvdata, REG_STRIDE, pdata->xvirt * BYTES_PER_PIXEL);
	xilinx_fb_out32(drvdata, REG_FMT, FMT_XRGB);

	/* Turn on the display */
	drvdata->reg_ctrl_default = REG_CTRL_ENABLE;
	xilinx_fb_out32(drvdata, REG_CTRL, drvdata->reg_ctrl_default);

	/* Fill struct fb_info */
	drvdata->info.device = dev;
	drvdata->info.screen_base = (void __iomem *)drvdata->fb_virt;
	drvdata->info.fbops = &xilinxfb_ops;
	drvdata->info.fix = xilinx_fb_fix;
	drvdata->info.fix.smem_start = drvdata->fb_phys;
	drvdata->info.fix.smem_len = fbsize;
	drvdata->info.fix.line_length = pdata->xvirt * BYTES_PER_PIXEL;

	drvdata->info.pseudo_palette = drvdata->pseudo_palette;
	drvdata->info.flags = FBINFO_DEFAULT;
	drvdata->info.var = xilinx_fb_var;
	drvdata->info.var.height = pdata->screen_height_mm;
	drvdata->info.var.width = pdata->screen_width_mm;
	drvdata->info.var.xres = pdata->xres;
	drvdata->info.var.yres = pdata->yres;
	drvdata->info.var.xres_virtual = pdata->xvirt;
	drvdata->info.var.yres_virtual = pdata->yvirt;

	/* Allocate a colour map */
	rc = fb_alloc_cmap(&drvdata->info.cmap, PALETTE_ENTRIES_NO, 0);
	if (rc) {
		dev_err(dev, "Fail to allocate colormap (%d entries)\n",
			PALETTE_ENTRIES_NO);
		goto err_cmap;
	}
	fb_videomode_to_modelist(xilinxfb_modedb, ARRAY_SIZE(xilinxfb_modedb),
				 &drvdata->info.modelist);

	/* Register new frame buffer */
	rc = register_framebuffer(&drvdata->info);
	if (rc) {
		dev_err(dev, "Could not register frame buffer\n");
		goto err_regfb;
	}

	/* Put a banner in the log (for DEBUG) */
	dev_info(dev, "regs: phys=%pa, virt=%p\n", &drvdata->regs_phys, drvdata->regs);

	/* Put a banner in the log (for DEBUG) */
	dev_info(dev, "fb: phys=%llx, virt=%p, size=%x\n",
		(unsigned long long)drvdata->fb_phys, drvdata->fb_virt, fbsize);

	return 0;	/* success */

err_regfb:
	fb_dealloc_cmap(&drvdata->info.cmap);

err_cmap:
	if (drvdata->fb_alloced)
		dma_free_coherent(dev, PAGE_ALIGN(fbsize), drvdata->fb_virt,
				  drvdata->fb_phys);
	else
		iounmap(drvdata->fb_virt);

	/* Turn off the display */
	xilinx_fb_out32(drvdata, REG_CTRL, 0);

	return rc;
}

static int xilinxfb_release(struct device *dev)
{
	struct xilinxfb_drvdata *drvdata = dev_get_drvdata(dev);

#if !defined(CONFIG_FRAMEBUFFER_CONSOLE) && defined(CONFIG_LOGO)
	xilinx_fb_blank(VESA_POWERDOWN, &drvdata->info);
#endif

	unregister_framebuffer(&drvdata->info);

	fb_dealloc_cmap(&drvdata->info.cmap);

	if (drvdata->fb_alloced)
		dma_free_coherent(dev, PAGE_ALIGN(drvdata->info.fix.smem_len),
				  drvdata->fb_virt, drvdata->fb_phys);
	else
		iounmap(drvdata->fb_virt);

	/* Turn off the display */
	xilinx_fb_out32(drvdata, REG_CTRL, 0);

	return 0;
}

/* ---------------------------------------------------------------------
 * OF bus binding
 */

static int xilinxfb_of_probe(struct platform_device *pdev)
{
	const u32 *prop;
	struct xilinxfb_platform_data pdata;
	int size;
	struct xilinxfb_drvdata *drvdata;

	/* Copy with the default pdata (not a ptr reference!) */
	pdata = xilinx_fb_default_pdata;

	/* Allocate the driver data region */
	drvdata = devm_kzalloc(&pdev->dev, sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

	prop = of_get_property(pdev->dev.of_node, "fb-addr", &size);
	if ((prop) && (size >= sizeof(u32) * 2)) {
		pdata.fb_phys = be32_to_cpup(prop);
		pdata.fb_size = be32_to_cpup(prop + 1);
		dev_info(&pdev->dev, "fb_phys:0x%x, fb_size:0x%x\n", pdata.fb_phys, pdata.fb_size);
	}

	prop = of_get_property(pdev->dev.of_node, "phys-size", &size);
	if ((prop) && (size >= sizeof(u32) * 2)) {
		pdata.screen_width_mm = be32_to_cpup(prop);
		pdata.screen_height_mm = be32_to_cpup(prop + 1);
		dev_info(&pdev->dev, "screen_width_mm:%u, screen_height_mm:%u\n", pdata.screen_width_mm, pdata.screen_height_mm);
	}

	prop = of_get_property(pdev->dev.of_node, "resolution", &size);
	if ((prop) && (size >= sizeof(u32) * 2)) {
		pdata.xres = be32_to_cpup(prop);
		pdata.yres = be32_to_cpup(prop + 1);
		dev_info(&pdev->dev, "xres:%u, yres:%u\n", pdata.xres, pdata.yres);
	}

	prop = of_get_property(pdev->dev.of_node, "virtual-resolution", &size);
	if ((prop) && (size >= sizeof(u32) * 2)) {
		pdata.xvirt = be32_to_cpup(prop);
		pdata.yvirt = be32_to_cpup(prop+ 1);
		dev_info(&pdev->dev, "xvirt:%u, yvirt:%u\n", pdata.xvirt, pdata.yvirt);
	}

	dev_set_drvdata(&pdev->dev, drvdata);
	return xilinxfb_assign(pdev, drvdata, &pdata);
}

static int xilinxfb_of_remove(struct platform_device *op)
{
	return xilinxfb_release(&op->dev);
}

/* Match table for of_platform binding */
static const struct of_device_id xilinxfb_of_match[] = {
	{ .compatible = "xlnx,axi-dvi-1.00", },
	{},
};
MODULE_DEVICE_TABLE(of, xilinxfb_of_match);

static struct platform_driver xilinxfb_of_driver = {
	.probe = xilinxfb_of_probe,
	.remove = xilinxfb_of_remove,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = xilinxfb_of_match,
	},
};

module_platform_driver(xilinxfb_of_driver);

MODULE_AUTHOR("MontaVista Software, Inc. <source@mvista.com>");
MODULE_DESCRIPTION("Xilinx frame buffer driver");
MODULE_LICENSE("GPL");
