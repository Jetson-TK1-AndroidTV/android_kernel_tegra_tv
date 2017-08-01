/*
 * SoC audio driver for Toradex Apalis TK1
 *
 * Copyright (C) 2012-2016 Toradex Inc.
 *
 * 2012-02-12: Marcel Ziswiler <marcel.ziswiler@toradex.com>
 *             initial version for Apalis/Colibri T30
 *
 * Copied from tegra_wm8903.c
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

#include <linux/clk.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include <mach/tegra_asoc_pdata.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "../codecs/sgtl5000.h"

#include "tegra_pcm.h"
#include "tegra_asoc_utils.h"

#define DRV_NAME "tegra-snd-apalis-tk1-sgtl5000"

#define DAI_LINK_HIFI			0
#define DAI_LINK_SPDIF			1
#define NUM_DAI_LINKS			2

struct apalis_tk1_sgtl5000 {
	struct tegra_asoc_utils_data util_data;
	struct tegra_asoc_platform_data *pdata;
	enum snd_soc_bias_level bias_level;
};

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

	if (pdata->i2s_param[HIFI_CODEC].is_i2s_master)
		i2s_daifmt = SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_CBS_CFS;
	else
		i2s_daifmt = SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_CBM_CFM;

	/* Use DSP mode for mono on Tegra20 */
	if (params_channels(params) != 2) {
		i2s_daifmt |= SND_SOC_DAIFMT_DSP_A;
	} else {
		switch (pdata->i2s_param[HIFI_CODEC].i2s_mode) {
		case TEGRA_DAIFMT_I2S:
			i2s_daifmt |= SND_SOC_DAIFMT_I2S;
			break;
		case TEGRA_DAIFMT_DSP_A:
			i2s_daifmt |= SND_SOC_DAIFMT_DSP_A;
			break;
		case TEGRA_DAIFMT_DSP_B:
			i2s_daifmt |= SND_SOC_DAIFMT_DSP_B;
			break;
		case TEGRA_DAIFMT_LEFT_J:
			i2s_daifmt |= SND_SOC_DAIFMT_LEFT_J;
			break;
		case TEGRA_DAIFMT_RIGHT_J:
			i2s_daifmt |= SND_SOC_DAIFMT_RIGHT_J;
			break;
		default:
			dev_err(card->dev, "Can't configure i2s format\n");
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
		err =
		    snd_soc_dai_set_sysclk(codec_dai, SGTL5000_SYSCLK, rate,
					   SND_SOC_CLOCK_IN);
		if (err < 0) {
			dev_err(card->dev, "codec_dai clock not set\n");
			return err;
		}
	}
	/* else TBD */

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
	struct apalis_tk1_sgtl5000 *machine =
			snd_soc_card_get_drvdata(rtd->card);

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

/* Apalis T30 machine DAPM widgets */
static const struct snd_soc_dapm_widget apalis_tk1_sgtl5000_dapm_widgets[] = {
	SND_SOC_DAPM_HP("Headphone Jack", NULL),
	SND_SOC_DAPM_LINE("Line In Jack", NULL),
	SND_SOC_DAPM_MIC("Mic Jack", NULL),
};

/* Apalis T30 machine audio map (connections to the codec pins) */
static const struct snd_soc_dapm_route apalis_tk1_sgtl5000_dapm_route[] = {
	/* Apalis MXM3 pin 306 (MIC)
	   Apalis Evaluation Board: Audio jack X26 bottom pink
	   Ixora: Audio jack X12 pin 4 */
	/* TBD: mic bias GPIO handling */
	{"Mic Jack", NULL, "MIC_IN"},

	/* Apalis MXM3 pin 310 & 312 (LINEIN_L/R)
	   Apalis Evaluation Board: Audio jack X26 top blue
	   Ixora: Line IN – S/PDIF header X18 pin 6 & 7 */
	{"Line In Jack", NULL, "LINE_IN"},

	/* Apalis MXM3 pin 316 & 318 (HP_L/R)
	   Apalis Evaluation Board: Audio jack X26 middle green
	   Ixora: Audio jack X12 */
	/* TBD: HP PGA handling */
	{"Headphone Jack", NULL, "HP_OUT"},
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
};

static struct snd_soc_card snd_soc_apalis_tk1_sgtl5000 = {
	.name = "Toradex Apalis TK1 SGTL5000",
	.owner = THIS_MODULE,
	.dai_link = apalis_tk1_sgtl5000_dai,
	.num_links = ARRAY_SIZE(apalis_tk1_sgtl5000_dai),
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
	struct tegra_asoc_platform_data *pdata;
	int ret;

	pdata = pdev->dev.platform_data;
	if (!pdata) {
		dev_err(&pdev->dev, "No platform data supplied\n");
		return -EINVAL;
	}

	if (pdata->codec_name)
		card->dai_link->codec_name = pdata->codec_name;

	if (pdata->codec_dai_name)
		card->dai_link->codec_dai_name = pdata->codec_dai_name;

	if (pdata->codec_name)
		card->dai_link[DAI_LINK_HIFI].codec_name = pdata->codec_name;

	if (pdata->codec_dai_name) {
		card->dai_link[DAI_LINK_HIFI].codec_dai_name =
				pdata->codec_dai_name;
	}

	machine = kzalloc(sizeof(struct apalis_tk1_sgtl5000), GFP_KERNEL);
	if (!machine) {
		dev_err(&pdev->dev,
			"Can't allocate apalis_tk1_sgtl5000 struct\n");
		return -ENOMEM;
	}

	machine->pdata = pdata;

	ret = tegra_asoc_utils_init(&machine->util_data, &pdev->dev, card);
	if (ret)
		goto err_free_machine;

	card->dev = &pdev->dev;
	platform_set_drvdata(pdev, card);
	snd_soc_card_set_drvdata(card, machine);

	ret = snd_soc_register_card(card);
	if (ret) {
		dev_err(&pdev->dev, "snd_soc_register_card failed (%d)\n", ret);
		goto err_fini_utils;
	}

	if (!card->instantiated) {
		ret = -ENODEV;
		dev_err(&pdev->dev, "snd_soc_register_card failed (%d)\n", ret);
		goto err_unregister_card;
	}

	ret = tegra_asoc_utils_set_parent(&machine->util_data,
					  pdata->i2s_param[HIFI_CODEC].
					  is_i2s_master);
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

static struct platform_driver apalis_tk1_sgtl5000_driver = {
	.driver = {
		.name = DRV_NAME,
		.owner = THIS_MODULE,
		.pm = &snd_soc_pm_ops,
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
