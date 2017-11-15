/*
 * SoC audio driver for SGTL5000 codec
 *
 * Copyright (C) 2012-2016 Toradex Inc.
 *
 * 2012-02-12: Marcel Ziswiler <marcel.ziswiler@toradex.com>
 *             initial version for Apalis/Colibri T30
 *
 * 2017-11-11: Matthew J. Gorski
 *             fixes for kernel 3.10.96 Apalisk TK1 v1 - 1.2
 *
 * Copied from tegra_rt5639.c
 * Copyright (C) 2010-2011 - NVIDIA, Inc.
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

#include <asm/mach-types.h>
#include <linux/of.h>
#include <linux/clk.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/delay.h>
#include <linux/sysedp.h>
#include <linux/pm_runtime.h>
#include <mach/tegra_asoc_pdata.h>
#include <mach/sgtl5000_pdata.h>

#include <sound/core.h>
#include <sound/jack.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include "../codecs/sgtl5000.h"

#include "tegra_pcm.h"
#include "tegra_asoc_utils.h"
#include "tegra30_ahub.h"
#include "tegra30_i2s.h"

#define DRV_NAME "tegra-snd-apalis-tk1-sgtl5000"

#define DAI_LINK_HIFI			0
#define DAI_LINK_SPDIF			1
#define DAI_LINK_PCM_OFFLOAD_FE		2
#define DAI_LINK_COMPR_OFFLOAD_FE	3
#define DAI_LINK_I2S_OFFLOAD_BE		4
#define NUM_DAI_LINKS			5

const char *apalis_tk1_sgtl5000_i2s_dai_name[TEGRA30_NR_I2S_IFC] = {
	"tegra30-i2s.0",
	"tegra30-i2s.1",
	"tegra30-i2s.2",
	"tegra30-i2s.3",
	"tegra30-i2s.4",
};

#define GPIO_HP_MUTE    BIT(1)
#define GPIO_HP_DET     BIT(4)

struct apalis_tk1_sgtl5000 {
	struct tegra_asoc_utils_data util_data;
	struct tegra_asoc_platform_data *pdata;
	struct regulator *spk_reg;
	struct regulator *dmic_reg;
	struct regulator *cdc_en;
	struct snd_soc_card *pcard;
	int gpio_requested;
	enum snd_soc_bias_level bias_level;
	volatile int clock_enabled;
};

void tegra_asoc_enable_clocks(void);
void tegra_asoc_disable_clocks(void);

static int apalis_tk1_sgtl5000_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_soc_codec *codec = rtd->codec;
	struct snd_soc_card *card = codec->card;
	struct apalis_tk1_sgtl5000 *machine = snd_soc_card_get_drvdata(card);
	struct tegra_asoc_platform_data *pdata = machine->pdata;
	int srate, mclk, i2s_daifmt;
	int err;
	int rate;

	/* sgtl5000 does not support 512*rate when in 96000 fs */
	srate = params_rate(params);
	switch (srate) {
	case 96000:
		mclk = 256 * srate;
		break;
	default:
		mclk = 512 * srate;
		break;
	}

	/* Sgtl5000 sysclk should be >= 8MHz and <= 27M */
	if (mclk < 8000000 || mclk > 27000000)
		return -EINVAL;

	if(pdata->i2s_param[HIFI_CODEC].is_i2s_master) {
		i2s_daifmt = SND_SOC_DAIFMT_NB_NF |
					SND_SOC_DAIFMT_CBS_CFS;
	} else {
		i2s_daifmt = SND_SOC_DAIFMT_NB_NF |
					SND_SOC_DAIFMT_CBM_CFM;
	}

	/* Use DSP mode for mono on Tegra20 */
	if (params_channels(params) != 2) {
		i2s_daifmt |= SND_SOC_DAIFMT_DSP_A;
	} else {
		switch (pdata->i2s_param[HIFI_CODEC].i2s_mode) {
			case TEGRA_DAIFMT_I2S :
				i2s_daifmt |= SND_SOC_DAIFMT_I2S;
				break;
			case TEGRA_DAIFMT_DSP_A :
				i2s_daifmt |= SND_SOC_DAIFMT_DSP_A;
				break;
			case TEGRA_DAIFMT_DSP_B :
				i2s_daifmt |= SND_SOC_DAIFMT_DSP_B;
				break;
			case TEGRA_DAIFMT_LEFT_J :
				i2s_daifmt |= SND_SOC_DAIFMT_LEFT_J;
				break;
			case TEGRA_DAIFMT_RIGHT_J :
				i2s_daifmt |= SND_SOC_DAIFMT_RIGHT_J;
				break;
			default :
				dev_err(card->dev,
				"Can't configure i2s format\n");
				return -EINVAL;
		}
	}

	err = tegra_asoc_utils_set_rate(&machine->util_data, srate, mclk);
	if (err < 0) {
		if (!(machine->util_data.set_mclk % mclk)) {
			mclk = machine->util_data.set_mclk;
		} else {
			dev_err(card->dev, "Can't configure clocks\n");
			return err;
		}
	}

	tegra_asoc_utils_lock_clk_rate(&machine->util_data, 1);

	rate = clk_get_rate(machine->util_data.clk_cdev1);

	err = snd_soc_dai_set_fmt(codec_dai, i2s_daifmt);
	if (err < 0) {
		dev_err(card->dev, "codec_dai fmt not set\n");
		return err;
	}

	err = snd_soc_dai_set_fmt(cpu_dai, i2s_daifmt);
	if (err < 0) {
		dev_err(card->dev, "cpu_dai fmt not set\n");
		return err;
	}

	if (pdata->i2s_param[HIFI_CODEC].is_i2s_master) {
		/* Set SGTL5000's SYSCLK (provided by clk_out_1) */
		err = snd_soc_dai_set_sysclk(codec_dai, SGTL5000_SYSCLK, rate, SND_SOC_CLOCK_IN);
		if (err < 0) {
			dev_err(card->dev, "codec_dai clock not set\n");
			return err;
		}
	}
//else TBD

	return 0;
}

static int tegra_spdif_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;
	struct apalis_tk1_sgtl5000 *machine = snd_soc_card_get_drvdata(card);
	int srate, mclk, min_mclk;
	int err;

	srate = params_rate(params);
	switch (srate) {
	case 11025:
	case 22050:
	case 44100:
	case 88200:
		mclk = 11289600;
		break;
	case 8000:
	case 16000:
	case 32000:
	case 48000:
	case 64000:
	case 96000:
		mclk = 12288000;
		break;
	default:
		return -EINVAL;
	}
	min_mclk = 128 * srate;

	err = tegra_asoc_utils_set_rate(&machine->util_data, srate, mclk);
	if (err < 0) {
		if (!(machine->util_data.set_mclk % min_mclk))
			mclk = machine->util_data.set_mclk;
		else {
			dev_err(card->dev, "Can't configure clocks\n");
			return err;
		}
	}

	tegra_asoc_utils_lock_clk_rate(&machine->util_data, 1);

	return 0;
}

static int tegra_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct apalis_tk1_sgtl5000 *machine = snd_soc_card_get_drvdata(rtd->card);

	tegra_asoc_utils_lock_clk_rate(&machine->util_data, 0);

	return 0;
}

static struct snd_soc_ops apalis_tk1_sgtl5000_ops = {
	.hw_params = apalis_tk1_sgtl5000_hw_params,
	.hw_free = tegra_hw_free,
};

static struct snd_soc_ops tegra_spdif_ops = {
	.hw_params = tegra_spdif_hw_params,
	.hw_free = tegra_hw_free,
};

static int apalis_tk1_sgtl5000_event_hp(struct snd_soc_dapm_widget *w,
					struct snd_kcontrol *k, int event)
{
	struct snd_soc_dapm_context *dapm = w->dapm;
	struct snd_soc_card *card = dapm->card;
	struct apalis_tk1_sgtl5000 *machine = snd_soc_card_get_drvdata(card);
	struct tegra_asoc_platform_data *pdata = machine->pdata;

	if (!(machine->gpio_requested & GPIO_HP_MUTE))
		return 0;

	gpio_set_value_cansleep(pdata->gpio_hp_mute,
				!SND_SOC_DAPM_EVENT_ON(event));

	return 0;
}

int tegra_offload_hw_params_be_fixup(struct snd_soc_pcm_runtime *rtd,
			struct snd_pcm_hw_params *params)
{
	if (!params_rate(params)) {
		struct snd_interval *snd_rate = hw_param_interval(params,
						SNDRV_PCM_HW_PARAM_RATE);

		snd_rate->min = snd_rate->max = 48000;
	}

	if (!params_channels(params)) {
		struct snd_interval *snd_channels = hw_param_interval(params,
						SNDRV_PCM_HW_PARAM_CHANNELS);

		snd_channels->min = snd_channels->max = 2;
	}
	snd_mask_set(hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT),
				ffs(SNDRV_PCM_FORMAT_S16_LE));

	return 1;
}

/* Apalis T30 machine DAPM widgets */
static const struct snd_soc_dapm_widget apalis_tk1_sgtl5000_dapm_widgets[] = {
        SND_SOC_DAPM_HP("Headphone Jack", apalis_tk1_sgtl5000_event_hp),
        SND_SOC_DAPM_LINE("Line In Jack", NULL),
        SND_SOC_DAPM_MIC("Mic Jack", NULL),
};

/* Apalis T30 machine audio map (connections to the codec pins) */
static const struct snd_soc_dapm_route apalis_tk1_sgtl5000_dapm_route[] = {
	/* Apalis MXM3 pin 306 (MIC)
	   Apalis Evaluation Board: Audio jack X26 bottom pink
	   Ixora: Audio jack X12 pin 4 */
//mic bias GPIO handling
	{ "Mic Jack", NULL, "MIC_IN" },

	/* Apalis MXM3 pin 310 & 312 (LINEIN_L/R)
	   Apalis Evaluation Board: Audio jack X26 top blue
	   Ixora: Line IN – S/PDIF header X18 pin 6 & 7 */
	{ "Line In Jack", NULL, "LINE_IN" },

	/* Apalis MXM3 pin 316 & 318 (HP_L/R)
	   Apalis Evaluation Board: Audio jack X26 middle green
	   Ixora: Audio jack X12 */
//HP PGA handling
	{ "Headphone Jack", NULL, "HP_OUT" },
};

static int apalis_tk1_sgtl5000_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_codec *codec = rtd->codec;
	struct snd_soc_dapm_context *dapm = &codec->dapm;
	struct snd_soc_card *card = codec->card;
	struct apalis_tk1_sgtl5000 *machine = snd_soc_card_get_drvdata(card);
	int ret;

	machine->bias_level = SND_SOC_BIAS_STANDBY;

	ret = tegra_asoc_utils_register_ctls(&machine->util_data);
	if (ret < 0)
		return ret;

	snd_soc_dapm_nc_pin(dapm, "LINE_OUT");

	snd_soc_dapm_sync(dapm);

	return 0;
}

static struct snd_soc_dai_link apalis_tk1_sgtl5000_dai[NUM_DAI_LINKS] = {
	[DAI_LINK_HIFI] = {
		.name = "SGTL5000",
		.stream_name = "SGTL5000 PCM",
		.codec_name = "sgtl5000.4-000a",
		.platform_name = "tegra30-i2s.2",
		.cpu_dai_name = "tegra30-i2s.2",
		.codec_dai_name = "sgtl5000",
		.init = apalis_tk1_sgtl5000_init,
		.ops = &apalis_tk1_sgtl5000_ops,
	},
	[DAI_LINK_SPDIF] = {
		.name = "SPDIF",
		.stream_name = "SPDIF PCM",
		.codec_name = "spdif-dit.0",
		.platform_name = "tegra30-spdif",
		.cpu_dai_name = "tegra30-spdif",
		.codec_dai_name = "dit-hifi",
		.ops = &tegra_spdif_ops,
	},
	[DAI_LINK_PCM_OFFLOAD_FE] = {
		.name = "offload-pcm",
		.stream_name = "offload-pcm",

		.platform_name = "tegra-offload",
		.cpu_dai_name = "tegra-offload-pcm",

		.codec_dai_name =  "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",

		.dynamic = 1,
	},
	[DAI_LINK_COMPR_OFFLOAD_FE] = {
		.name = "offload-compr",
		.stream_name = "offload-compr",

		.platform_name = "tegra-offload",
		.cpu_dai_name = "tegra-offload-compr",

		.codec_dai_name =  "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",

		.dynamic = 1,
	},
	[DAI_LINK_I2S_OFFLOAD_BE] = {
		.name = "offload-audio",
		.stream_name = "offload-audio-pcm",
		.codec_name = "sgtl5000.4-000a",
		.platform_name = "tegra30-i2s.1",
		.cpu_dai_name = "tegra30-i2s.2",
		.codec_dai_name = "sgtl5000",
		.ops = &apalis_tk1_sgtl5000_ops,

		.no_pcm = 1,

		.be_id = 0,
		.be_hw_params_fixup = tegra_offload_hw_params_be_fixup,
	},
};

static struct snd_soc_card snd_soc_apalis_tk1_sgtl5000 = {
	.name = "Toradex Apalis TK1 SGTL5000",
	.owner = THIS_MODULE,
	.dai_link = apalis_tk1_sgtl5000_dai,
	.num_links = ARRAY_SIZE(apalis_tk1_sgtl5000_dai),
//	.resume_pre
//	.set_bias_level
//	.set_bias_level_post
//	.controls
//	.num_controls
	.dapm_widgets = apalis_tk1_sgtl5000_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(apalis_tk1_sgtl5000_dapm_widgets),
	.dapm_routes = apalis_tk1_sgtl5000_dapm_route,
	.num_dapm_routes = ARRAY_SIZE(apalis_tk1_sgtl5000_dapm_route),
	.fully_routed = true,
};

static int apalis_tk1_sgtl5000_driver_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card = &snd_soc_apalis_tk1_sgtl5000;
	struct apalis_tk1_sgtl5000 *machine;
	struct tegra_asoc_platform_data *pdata = NULL;
	int ret;
	int codec_id;

	pdata = pdev->dev.platform_data;
	if (!pdata) {
		dev_err(&pdev->dev, "No platform data supplied\n");
		return -EINVAL;
	}

	if (pdata->codec_name)
		card->dai_link->codec_name = pdata->codec_name;

	if (pdata->codec_dai_name)
		card->dai_link->codec_dai_name = pdata->codec_dai_name;

	if (pdata->codec_name) {
		card->dai_link[DAI_LINK_HIFI].codec_name = pdata->codec_name;
	}

	if (pdata->codec_dai_name) {
		card->dai_link[DAI_LINK_HIFI].codec_dai_name =
			pdata->codec_dai_name;
		card->dai_link[DAI_LINK_I2S_OFFLOAD_BE].codec_dai_name =
			pdata->codec_dai_name;
	}

	machine = kzalloc(sizeof(struct apalis_tk1_sgtl5000), GFP_KERNEL);
	if (!machine) {
		dev_err(&pdev->dev, "Can't allocate apalis_tk1_sgtl5000 struct\n");
		return -ENOMEM;
	}

	machine->pdata = pdata;
	machine->pcard = card;

	ret = tegra_asoc_utils_init(&machine->util_data, &pdev->dev, card);
	if (ret)
		goto err_free_machine;

	machine->bias_level = SND_SOC_BIAS_STANDBY;
	machine->clock_enabled = 1;

	if (!gpio_is_valid(pdata->gpio_ldo1_en)) {
		machine->cdc_en = regulator_get(&pdev->dev, "ldo1_en");
		if (IS_ERR(machine->cdc_en)) {
			dev_err(&pdev->dev, "ldo1_en regulator not found %ld\n",
					PTR_ERR(machine->cdc_en));
			machine->cdc_en = 0;
		} else {
			ret = regulator_enable(machine->cdc_en);
		}
	}

	machine->spk_reg = regulator_get(&pdev->dev, "vdd_spk");
	if (IS_ERR(machine->spk_reg)) {
		dev_info(&pdev->dev, "No speaker regulator found\n");
		machine->spk_reg = 0;
	}

	card->dev = &pdev->dev;
	platform_set_drvdata(pdev, card);
	snd_soc_card_set_drvdata(card, machine);

#ifndef CONFIG_ARCH_TEGRA_2x_SOC
	codec_id = pdata->i2s_param[HIFI_CODEC].audio_port_id;
	apalis_tk1_sgtl5000_dai[DAI_LINK_HIFI].cpu_dai_name =
	apalis_tk1_sgtl5000_i2s_dai_name[codec_id];
	apalis_tk1_sgtl5000_dai[DAI_LINK_HIFI].platform_name =
	apalis_tk1_sgtl5000_i2s_dai_name[codec_id];
	apalis_tk1_sgtl5000_dai[DAI_LINK_I2S_OFFLOAD_BE].cpu_dai_name =
		apalis_tk1_sgtl5000_i2s_dai_name[codec_id];
#endif

	card->dapm.idle_bias_off = 1;
	ret = snd_soc_register_card(card);
	if (ret) {
		dev_err(&pdev->dev, "snd_soc_register_card failed (%d)\n",
			ret);
		goto err_fini_utils;
	}

	if (!card->instantiated) {
		ret = -ENODEV;
		dev_err(&pdev->dev, "snd_soc_register_card failed (%d)\n",
			ret);
		goto err_unregister_card;
	}

	ret = tegra_asoc_utils_set_parent(&machine->util_data,
				pdata->i2s_param[HIFI_CODEC].is_i2s_master);
	if (ret) {
		dev_err(&pdev->dev, "tegra_asoc_utils_set_parent failed (%d)\n",
			ret);
		goto err_unregister_card;
	}

	return 0;

err_unregister_card:
	snd_soc_unregister_card(card);
err_fini_utils:
	tegra_asoc_utils_fini(&machine->util_data);
err_free_machine:
	kfree(machine);
	return ret;
}

static int apalis_tk1_sgtl5000_driver_remove(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);
	struct apalis_tk1_sgtl5000 *machine = snd_soc_card_get_drvdata(card);

	snd_soc_unregister_card(card);

	tegra_asoc_utils_fini(&machine->util_data);

	kfree(machine);

	return 0;
}

static const struct of_device_id apalis_tk1_sgtl5000_of_match[] = {
        { .compatible = "nvidia,tegra-audio-sgtl5000", },
        {},
};

static struct platform_driver apalis_tk1_sgtl5000_driver = {
	.driver = {
		.name = DRV_NAME,
		.owner = THIS_MODULE,
		.pm = &snd_soc_pm_ops,
		.of_match_table = apalis_tk1_sgtl5000_of_match,
	},
	.probe = apalis_tk1_sgtl5000_driver_probe,
	.remove = apalis_tk1_sgtl5000_driver_remove,
};

static int __init apalis_tk1_sgtl5000_modinit(void)
{
	return platform_driver_register(&apalis_tk1_sgtl5000_driver);
}
module_init(apalis_tk1_sgtl5000_modinit);

static void __exit apalis_tk1_sgtl5000_modexit(void)
{
	platform_driver_unregister(&apalis_tk1_sgtl5000_driver);
}
module_exit(apalis_tk1_sgtl5000_modexit);

/* Module information */
MODULE_AUTHOR("Marcel Ziswiler <marcel.ziswiler@toradex.com>");
MODULE_DESCRIPTION("ALSA SoC SGTL5000 on Toradex Apalis TK1");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRV_NAME);
