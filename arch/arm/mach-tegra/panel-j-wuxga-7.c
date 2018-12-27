/*
 * arch/arm/mach-tegra/panel-j-wuxga-7.c
 *
 * Copyright (c) 2014, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/ioport.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/tegra_pwm_bl.h>
#include <linux/regulator/consumer.h>
#include <linux/backlight.h>
#include <linux/pwm_backlight.h>

#include <mach/dc.h>
#include <asm/mach-types.h>

#include "devices.h"
#include "board-panel.h"

#define DSI_PANEL_RESET       1

static bool reg_requested;
static struct regulator *avdd_lcd_5v5;
static struct regulator *vdd_lcd_bl_en;
static struct regulator *dvdd_lcd_1v8;
static u16 en_panel_rst;

static int dsi_regulator_get(struct device *dev)
{
	int err = 0;

	if (reg_requested)
		return 0;

	avdd_lcd_5v5 = regulator_get(dev, "avdd_lcd");
	if (IS_ERR_OR_NULL(avdd_lcd_5v5)) {
		pr_err("avdd_lcd regulator get failed\n");
		err = PTR_ERR(avdd_lcd_5v5);
		avdd_lcd_5v5 = NULL;
		goto fail;
	}

	dvdd_lcd_1v8 = regulator_get(dev, "vdd_lcd_1v8_s");
	if (IS_ERR_OR_NULL(dvdd_lcd_1v8)) {
		pr_err("dvdd_lcd_1v8 regulator get failed\n");
		err = PTR_ERR(dvdd_lcd_1v8);
		dvdd_lcd_1v8 = NULL;
		goto fail;
	}

	vdd_lcd_bl_en = regulator_get(dev, "vdd_lcd_bl_en");
	if (IS_ERR_OR_NULL(vdd_lcd_bl_en)) {
		pr_err("vdd_lcd_bl_en regulator get failed\n");
		err = PTR_ERR(vdd_lcd_bl_en);
		vdd_lcd_bl_en = NULL;
		goto fail;
	}

	reg_requested = true;
	return 0;

fail:
	return err;
}

static int dsi_j_wuxga_7_postpoweron(struct device *dev)
{
	int err = 0;

	err = dsi_regulator_get(dev);
	if (err < 0) {
		pr_err("dsi regulator get failed\n");
		goto fail;
	}

	err = tegra_panel_gpio_get_dt("j,wuxga-7", &panel_of);
	if (err < 0) {
		pr_err("dsi gpio request failed\n");
		goto fail;
	}

	/* If panel rst gpio is specified in device tree,
	 * use that.
	 */
	if (gpio_is_valid(panel_of.panel_gpio[TEGRA_GPIO_RESET]))
		en_panel_rst = panel_of.panel_gpio[TEGRA_GPIO_RESET];

	// IOVCC or IOVDD
	if (dvdd_lcd_1v8) {
		err = regulator_enable(dvdd_lcd_1v8);
		if (err < 0) {
			pr_err("dvdd_lcd regulator enable failed\n");
			goto fail;
		}
	}

	// Vddp
	if (avdd_lcd_5v5) {
		err = regulator_enable(avdd_lcd_5v5);
		if (err < 0) {
			pr_err("avdd_lcd regulator enable failed\n");
			goto fail;
		}
	}

	if (vdd_lcd_bl_en) {
		err = regulator_enable(vdd_lcd_bl_en);
		if (err < 0) {
			pr_err("vdd_lcd_bl_en regulator enable failed\n");
			goto fail;
		}
	}
	msleep(20);
#if DSI_PANEL_RESET
	/* use platform data */
	gpio_direction_output(en_panel_rst, 1);
	usleep_range(1000, 5000);
	gpio_set_value(en_panel_rst, 0);
	usleep_range(1000, 5000);
	gpio_set_value(en_panel_rst, 1);
	msleep(20);
#endif

	return 0;
fail:
	return err;
}

static int dsi_j_wuxga_7_enable(struct device *dev)
{
	return 0;
}

static int dsi_j_wuxga_7_disable(struct device *dev)
{

	if (vdd_lcd_bl_en)
		regulator_disable(vdd_lcd_bl_en);

	if (dvdd_lcd_1v8)
		regulator_disable(dvdd_lcd_1v8);

	if (avdd_lcd_5v5)
		regulator_disable(avdd_lcd_5v5);

	return 0;
}

static int dsi_j_wuxga_7_postsuspend(void)
{
	return 0;
}

static int dsi_j_wuxga_7_bl_notify(struct device *dev, int brightness)
{
	struct backlight_device *bl = NULL;
	struct pwm_bl_data *pb = NULL;
	int cur_sd_brightness = atomic_read(&sd_brightness);
	bl = (struct backlight_device *)dev_get_drvdata(dev);
	pb = (struct pwm_bl_data *)dev_get_drvdata(&bl->dev);

	/* SD brightness is a percentage */
	brightness = (brightness * cur_sd_brightness) / 255;

	/* Apply any backlight response curve */
	if (brightness > 255)
		pr_info("Error: Brightness > 255!\n");
	else if (pb->bl_measured)
		brightness = pb->bl_measured[brightness];

	return brightness;
}

static int dsi_j_wuxga_7_check_fb(struct device *dev, struct fb_info *info)
{
	struct platform_device *pdev = NULL;
	pdev = to_platform_device(bus_find_device_by_name(
		&platform_bus_type, NULL, "tegradc.0"));
	return info->device == &pdev->dev;
}

static struct pwm_bl_data_dt_ops  dsi_j_wuxga_7_pwm_bl_ops = {
	.notify = dsi_j_wuxga_7_bl_notify,
	.check_fb = dsi_j_wuxga_7_check_fb,
	.blnode_compatible = "j,wuxga-7-bl",
};

struct tegra_panel_ops dsi_j_wuxga_7_ops = {
	.enable = dsi_j_wuxga_7_enable,
	.disable = dsi_j_wuxga_7_disable,
	.postpoweron = dsi_j_wuxga_7_postpoweron,
	.postsuspend = dsi_j_wuxga_7_postsuspend,
	.pwm_bl_ops = &dsi_j_wuxga_7_pwm_bl_ops,
};
