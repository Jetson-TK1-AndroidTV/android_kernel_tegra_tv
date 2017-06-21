/*
 * arch/arm/mach-tegra/board-ardbeg-panel.c
 *
 * Copyright (c) 2011-2014, NVIDIA Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */
#include <linux/ioport.h>
#include <linux/fb.h>
#include <linux/nvhost.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/tegra_pwm_bl.h>
#include <linux/regulator/consumer.h>
#include <linux/pwm_backlight.h>
#include <linux/dma-mapping.h>
#include <linux/pinctrl/pinconf-tegra.h>

#include <mach/irqs.h>
#include <mach/dc.h>
#include <mach/io_dpd.h>

#include "board.h"
#include "devices.h"
#include "gpio-names.h"
#include "iomap.h"
#include "tegra12_host1x_devices.h"
#include "board-panel.h"
#include "board-common.h"
#include <linux/platform/tegra/common.h>
#include "tegra-board-id.h"

struct platform_device * __init ardbeg_host1x_init(void)
{
	struct platform_device *pdev = NULL;

#ifdef CONFIG_TEGRA_GRHOST
	if (!of_have_populated_dt())
		pdev = tegra12_register_host1x_devices();
	else
		pdev = to_platform_device(bus_find_device_by_name(
			&platform_bus_type, NULL, "host1x"));

	if (!pdev) {
		pr_err("host1x devices registration failed\n");
		return NULL;
	}
#endif
	return pdev;
}

#ifdef CONFIG_TEGRA_DC

#define DSI_PANEL_RST_GPIO	TEGRA_GPIO_PH3
#define DSI_PANEL_BL_PWM_GPIO	TEGRA_GPIO_PH1

/* HDMI Hotplug detection pin */
#define ardbeg_hdmi_hpd	TEGRA_GPIO_PN7

static struct regulator *ardbeg_hdmi_reg;
static struct regulator *ardbeg_hdmi_pll;
static struct regulator *ardbeg_hdmi_vddio;

static struct resource ardbeg_disp1_resources[] = {
	{
		.name	= "irq",
		.start	= INT_DISPLAY_GENERAL,
		.end	= INT_DISPLAY_GENERAL,
		.flags	= IORESOURCE_IRQ,
	},
	{
		.name	= "regs",
		.start	= TEGRA_DISPLAY_BASE,
		.end	= TEGRA_DISPLAY_BASE + TEGRA_DISPLAY_SIZE - 1,
		.flags	= IORESOURCE_MEM,
	},
	{
		.name	= "fbmem",
		.start	= 0, /* Filled in by ardbeg_panel_init() */
		.end	= 0, /* Filled in by ardbeg_panel_init() */
		.flags	= IORESOURCE_MEM,
	},
	{
		.name	= "dsi_regs",
		.start	= TEGRA_DSI_BASE,
		.end	= TEGRA_DSI_BASE + TEGRA_DSI_SIZE - 1,
		.flags	= IORESOURCE_MEM,
	},
	{
		.name	= "mipi_cal",
		.start	= TEGRA_MIPI_CAL_BASE,
		.end	= TEGRA_MIPI_CAL_BASE + TEGRA_MIPI_CAL_SIZE - 1,
		.flags	= IORESOURCE_MEM,
	},
};

static struct resource ardbeg_disp2_resources[] = {
	{
		.name	= "irq",
		.start	= INT_DISPLAY_B_GENERAL,
		.end	= INT_DISPLAY_B_GENERAL,
		.flags	= IORESOURCE_IRQ,
	},
	{
		.name	= "regs",
		.start	= TEGRA_DISPLAY2_BASE,
		.end	= TEGRA_DISPLAY2_BASE + TEGRA_DISPLAY2_SIZE - 1,
		.flags	= IORESOURCE_MEM,
	},
	{
		.name	= "fbmem",
		.start	= 0, /* Filled in by ardbeg_panel_init() */
		.end	= 0, /* Filled in by ardbeg_panel_init() */
		.flags	= IORESOURCE_MEM,
	},
	{
		.name	= "hdmi_regs",
		.start	= TEGRA_HDMI_BASE,
		.end	= TEGRA_HDMI_BASE + TEGRA_HDMI_SIZE - 1,
		.flags	= IORESOURCE_MEM,
	},
};

static struct tegra_dc_sd_settings sd_settings;

static struct tegra_dc_out ardbeg_disp1_out = {
	.type		= TEGRA_DC_OUT_DSI,
	.sd_settings	= &sd_settings,
};

static int ardbeg_hdmi_enable(struct device *dev)
{
	int ret;
	if (!ardbeg_hdmi_reg) {
		ardbeg_hdmi_reg = regulator_get(dev, "avdd_hdmi");
		if (IS_ERR(ardbeg_hdmi_reg)) {
			pr_err("hdmi: couldn't get regulator avdd_hdmi\n");
			ardbeg_hdmi_reg = NULL;
			return PTR_ERR(ardbeg_hdmi_reg);
		}
	}
	ret = regulator_enable(ardbeg_hdmi_reg);
	if (ret < 0) {
		pr_err("hdmi: couldn't enable regulator avdd_hdmi\n");
		return ret;
	}
	if (!ardbeg_hdmi_pll) {
		ardbeg_hdmi_pll = regulator_get(dev, "avdd_hdmi_pll");
		if (IS_ERR(ardbeg_hdmi_pll)) {
			pr_err("hdmi: couldn't get regulator avdd_hdmi_pll\n");
			ardbeg_hdmi_pll = NULL;
			regulator_put(ardbeg_hdmi_reg);
			ardbeg_hdmi_reg = NULL;
			return PTR_ERR(ardbeg_hdmi_pll);
		}
	}
	ret = regulator_enable(ardbeg_hdmi_pll);
	if (ret < 0) {
		pr_err("hdmi: couldn't enable regulator avdd_hdmi_pll\n");
		return ret;
	}
	return 0;
}

static int ardbeg_hdmi_disable(struct device *dev)
{
	if (ardbeg_hdmi_reg) {
		regulator_disable(ardbeg_hdmi_reg);
		regulator_put(ardbeg_hdmi_reg);
		ardbeg_hdmi_reg = NULL;
	}

	if (ardbeg_hdmi_pll) {
		regulator_disable(ardbeg_hdmi_pll);
		regulator_put(ardbeg_hdmi_pll);
		ardbeg_hdmi_pll = NULL;
	}

	return 0;
}

static int ardbeg_hdmi_postsuspend(void)
{
	if (ardbeg_hdmi_vddio) {
		regulator_disable(ardbeg_hdmi_vddio);
		regulator_put(ardbeg_hdmi_vddio);
		ardbeg_hdmi_vddio = NULL;
	}
	return 0;
}

static int ardbeg_hdmi_hotplug_init(struct device *dev)
{
	if (!ardbeg_hdmi_vddio) {
		ardbeg_hdmi_vddio = regulator_get(dev, "vdd_hdmi_5v0");
		if (WARN_ON(IS_ERR(ardbeg_hdmi_vddio))) {
			pr_err("%s: couldn't get regulator vdd_hdmi_5v0: %ld\n",
					__func__, PTR_ERR(ardbeg_hdmi_vddio));
			ardbeg_hdmi_vddio = NULL;
		} else
			return regulator_enable(ardbeg_hdmi_vddio);
	}
	return 0;
}

/* Table of electrical characteristics for Roth HDMI.
 * All modes must be declared here
 */
struct tmds_config ardbeg_tmds_config[] = {
	{ /* 720p / 74.25MHz modes */
		.pclk = 74250000,
		.pll0 = 0x01003110,
		.pll1 = 0x00301500,
		.pe_current = 0x00000000,
		.drive_current = 0x32323232,
		.peak_current = 0x05050505,
	},
	{ /* 1080p / 148.5MHz modes */
		.pclk = 148500000,
		.pll0 = 0x01003310,
		.pll1 = 0x00301500,
		.pe_current = 0x00000000,
		.drive_current = 0x32323232,
		.peak_current = 0x05050505,
	},
	{ /* 297MHz modes */
		.pclk = INT_MAX,
		.pll0 = 0x01003f10,
		.pll1 = 0x00300f00,
		.pe_current = 0x00000000,
		.drive_current = 0x36363636,
		.peak_current = 0x07070707,
	},
};

struct tegra_hdmi_out ardbeg_hdmi_out = {
	.tmds_config = ardbeg_tmds_config,
	.n_tmds_config = 3,
};

static void ardbeg_hdmi_hotplug_report(bool state)
{
	struct pinctrl_dev *pctl_dev;
	unsigned long conf;
	int val = (state) ? TEGRA_PIN_PULL_DOWN : TEGRA_PIN_PULL_NONE;
	int ret;

	pctl_dev = tegra_get_pinctrl_device_handle();
	if (!pctl_dev)
		return;

	conf = TEGRA_PINCONF_PACK(TEGRA_PINCONF_PARAM_PULL, val);
	ret = pinctrl_set_config_for_group_name(pctl_dev, "ddc_sda_pv5", conf);
	if (ret < 0)
		pr_err("%s(): ERROR: ddc_sda_pv5 config failed: %d\n",
			__func__, ret);

	ret = pinctrl_set_config_for_group_name(pctl_dev, "ddc_scl_pv4", conf);
	if (ret < 0)
		pr_err("%s(): ERROR: ddc_scl_pv4 config failed: %d\n",
			__func__, ret);
}

static struct tegra_dc_out ardbeg_disp2_out = {
	.type		= TEGRA_DC_OUT_HDMI,
	.flags		= TEGRA_DC_OUT_HOTPLUG_HIGH,
	.parent_clk	= "pll_d2",

	.ddc_bus	= 3,
	.hotplug_gpio	= ardbeg_hdmi_hpd,

	.max_pixclock	= KHZ2PICOS(297000),

	.align		= TEGRA_DC_ALIGN_MSB,
	.order		= TEGRA_DC_ORDER_RED_BLUE,

	.enable		= ardbeg_hdmi_enable,
	.disable	= ardbeg_hdmi_disable,
	.postsuspend	= ardbeg_hdmi_postsuspend,
	.hotplug_init	= ardbeg_hdmi_hotplug_init,
	.hotplug_report = ardbeg_hdmi_hotplug_report,
};

static struct tegra_fb_data ardbeg_disp1_fb_data = {
	.win		= 0,
	.bits_per_pixel = 32,
	.flags		= TEGRA_FB_FLIP_ON_PROBE,
};

static struct tegra_dc_platform_data ardbeg_disp1_pdata = {
	.flags		= TEGRA_DC_FLAG_ENABLED,
	.default_out	= &ardbeg_disp1_out,
	.fb		= &ardbeg_disp1_fb_data,
	.emc_clk_rate	= 204000000,
#ifdef CONFIG_TEGRA_DC_CMU
	.cmu_enable	= 1,
#endif
};

static struct tegra_fb_data ardbeg_disp2_fb_data = {
	.win		= 0,
	.xres		= 1024,
	.yres		= 600,
	.bits_per_pixel = 32,
	.flags		= TEGRA_FB_FLIP_ON_PROBE,
};

static struct tegra_dc_platform_data ardbeg_disp2_pdata = {
	.flags		= TEGRA_DC_FLAG_ENABLED,
	.default_out	= &ardbeg_disp2_out,
	.fb		= &ardbeg_disp2_fb_data,
	.emc_clk_rate	= 300000000,
#ifdef CONFIG_TEGRA_DC_CMU
	.cmu_enable	= 1,
#endif
};

static struct platform_device ardbeg_disp2_device = {
	.name		= "tegradc",
	.id		= 1,
	.resource	= ardbeg_disp2_resources,
	.num_resources	= ARRAY_SIZE(ardbeg_disp2_resources),
	.dev = {
		.platform_data = &ardbeg_disp2_pdata,
	},
};

static struct platform_device ardbeg_disp1_device = {
	.name		= "tegradc",
	.id		= 0,
	.resource	= ardbeg_disp1_resources,
	.num_resources	= ARRAY_SIZE(ardbeg_disp1_resources),
	.dev = {
		.platform_data = &ardbeg_disp1_pdata,
	},
};

static struct tegra_dc_mode hdmi_panel_modes[] = {
	{
		.pclk = 148500000,
		.h_ref_to_sync = 1,
		.v_ref_to_sync = 1,
		.h_sync_width = 44,
		.v_sync_width = 5,
		.h_back_porch = 148,
		.v_back_porch = 36,
		.h_active = 1920,
		.v_active = 1080,
		.h_front_porch = 88,
		.v_front_porch = 4,
	},
};

int __init ardbeg_panel_init(void)
{
	int err = 0;
	struct resource __maybe_unused *res;
	struct platform_device *phost1x = NULL;

	struct device_node *dc1_node = NULL;
	struct device_node *dc2_node = NULL;

	find_dc_node(&dc1_node, &dc2_node);

	res = platform_get_resource_byname(&ardbeg_disp2_device,
					 IORESOURCE_IRQ, "irq");
	res->start = INT_DISPLAY_GENERAL;
	res->end = INT_DISPLAY_GENERAL;
	res = platform_get_resource_byname(&ardbeg_disp2_device,
					 IORESOURCE_MEM, "regs");
	res->start = TEGRA_DISPLAY_BASE;
	res->end = TEGRA_DISPLAY_BASE + TEGRA_DISPLAY_SIZE - 1;
	ardbeg_disp2_fb_data.xres = 1920;
	ardbeg_disp2_fb_data.yres = 1080;
	ardbeg_disp2_device.id = 0;
	ardbeg_disp2_out.parent_clk = "pll_d";

	ardbeg_disp2_out.modes = hdmi_panel_modes;
	ardbeg_disp2_out.n_modes = ARRAY_SIZE(hdmi_panel_modes);

	phost1x = ardbeg_host1x_init();
	if (!phost1x) {
		pr_err("host1x devices registration failed\n");
		return -EINVAL;
	}

	res = platform_get_resource_byname(&ardbeg_disp1_device,
					 IORESOURCE_MEM, "fbmem");
	res->start = tegra_fb_start;
	res->end = tegra_fb_start + tegra_fb_size - 1;

	/* Copy the bootloader fb to the fb. */
	if (tegra_bootloader_fb_size)
		__tegra_move_framebuffer(NULL,
			tegra_fb_start, tegra_bootloader_fb_start,
			min(tegra_fb_size, tegra_bootloader_fb_size));
	else
		__tegra_clear_framebuffer(NULL,
				tegra_fb_start, tegra_fb_size);

	/* Copy the bootloader fb2 to the fb2. */
	if (tegra_bootloader_fb2_size)
		__tegra_move_framebuffer(NULL,
			tegra_fb2_start, tegra_bootloader_fb2_start,
			min(tegra_fb2_size, tegra_bootloader_fb2_size));
	else
		__tegra_clear_framebuffer(NULL,
				tegra_fb2_start, tegra_fb2_size);

	res = platform_get_resource_byname(&ardbeg_disp2_device,
					 IORESOURCE_MEM, "fbmem");
	res->start = tegra_fb2_start;
	res->end = tegra_fb2_start + tegra_fb2_size - 1;

	ardbeg_disp1_device.dev.parent = &phost1x->dev;
	ardbeg_disp2_device.dev.parent = &phost1x->dev;
	ardbeg_disp2_out.hdmi_out = &ardbeg_hdmi_out;

	if (true) {
		if (!of_have_populated_dt() || !dc1_node ||
			!of_device_is_available(dc1_node)) {
			err = platform_device_register(&ardbeg_disp1_device);
			if (err) {
				pr_err("disp1 device registration failed\n");
				return err;
			}
		}

		if (!of_have_populated_dt() || !dc2_node ||
			!of_device_is_available(dc2_node)) {
			err = platform_device_register(&ardbeg_disp2_device);
			if (err) {
				pr_err("disp2 device registration failed\n");
				return err;
			}
		}
	} else {
		if (!of_have_populated_dt() || !dc1_node ||
			!of_device_is_available(dc1_node)) {
			/*
			 * TODO
			 * For non-DT, Why foster uses res tegra_fb2,
			 * whereas it is connected to dc0?
			 * If DT is used, driver uses res tegra_fb,
			 * in case dc0 is connected.
			 */
			err = platform_device_register(&ardbeg_disp2_device);
			if (err) {
				pr_err("disp2 device registration failed\n");
				return err;
			}
		}
	}

	return err;
}
#else
int __init ardbeg_display_select(void)
{
	if (ardbeg_host1x_init())
		return 0;
	else
		return -EINVAL;
}
#endif
