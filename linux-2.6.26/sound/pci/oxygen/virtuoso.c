/*
 * C-Media CMI8788 driver for Asus Xonar cards
 *
 * Copyright (c) Clemens Ladisch <clemens@ladisch.de>
 *
 *
 *  This driver is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License, version 2.
 *
 *  This driver is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this driver; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */

/*
 * Xonar D2/D2X
 * ------------
 *
 * CMI8788:
 *
 * SPI 0 -> 1st PCM1796 (front)
 * SPI 1 -> 2nd PCM1796 (surround)
 * SPI 2 -> 3rd PCM1796 (center/LFE)
 * SPI 4 -> 4th PCM1796 (back)
 *
 * GPIO 2 -> M0 of CS5381
 * GPIO 3 -> M1 of CS5381
 * GPIO 5 <- external power present (D2X only)
 * GPIO 7 -> ALT
 * GPIO 8 -> enable output to speakers
 */

/*
 * Xonar DX
 * --------
 *
 * CMI8788:
 *
 * I²C <-> CS4398 (front)
 *     <-> CS4362A (surround, center/LFE, back)
 *
 * GPI 0 <- external power present
 *
 * GPIO 0 -> enable output to speakers
 * GPIO 1 -> enable front panel I/O
 * GPIO 2 -> M0 of CS5361
 * GPIO 3 -> M1 of CS5361
 * GPIO 8 -> route input jack to line-in (0) or mic-in (1)
 *
 * CS4398:
 *
 * AD0 <- 1
 * AD1 <- 1
 *
 * CS4362A:
 *
 * AD0 <- 0
 */

#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <sound/ac97_codec.h>
#include <sound/control.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/tlv.h>
#include "oxygen.h"
#include "cm9780.h"
#include "pcm1796.h"
#include "cs4398.h"
#include "cs4362a.h"

MODULE_AUTHOR("Clemens Ladisch <clemens@ladisch.de>");
MODULE_DESCRIPTION("Asus AVx00 driver");
MODULE_LICENSE("GPL");
MODULE_SUPPORTED_DEVICE("{{Asus,AV100},{Asus,AV200}}");

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;
static int enable[SNDRV_CARDS] = SNDRV_DEFAULT_ENABLE_PNP;

module_param_array(index, int, NULL, 0444);
MODULE_PARM_DESC(index, "card index");
module_param_array(id, charp, NULL, 0444);
MODULE_PARM_DESC(id, "ID string");
module_param_array(enable, bool, NULL, 0444);
MODULE_PARM_DESC(enable, "enable card");

enum {
	MODEL_D2,
	MODEL_D2X,
	MODEL_DX,
};

static struct pci_device_id xonar_ids[] __devinitdata = {
	{ OXYGEN_PCI_SUBID(0x1043, 0x8269), .driver_data = MODEL_D2 },
	{ OXYGEN_PCI_SUBID(0x1043, 0x8275), .driver_data = MODEL_DX },
	{ OXYGEN_PCI_SUBID(0x1043, 0x82b7), .driver_data = MODEL_D2X },
	{ }
};
MODULE_DEVICE_TABLE(pci, xonar_ids);


#define GPIO_CS53x1_M_MASK	0x000c
#define GPIO_CS53x1_M_SINGLE	0x0000
#define GPIO_CS53x1_M_DOUBLE	0x0004
#define GPIO_CS53x1_M_QUAD	0x0008

#define GPIO_D2X_EXT_POWER	0x0020
#define GPIO_D2_ALT		0x0080
#define GPIO_D2_OUTPUT_ENABLE	0x0100

#define GPI_DX_EXT_POWER	0x01
#define GPIO_DX_OUTPUT_ENABLE	0x0001
#define GPIO_DX_FRONT_PANEL	0x0002
#define GPIO_DX_INPUT_ROUTE	0x0100

#define I2C_DEVICE_CS4398	0x9e	/* 10011, AD1=1, AD0=1, /W=0 */
#define I2C_DEVICE_CS4362A	0x30	/* 001100, AD0=0, /W=0 */

struct xonar_data {
	unsigned int anti_pop_delay;
	u16 output_enable_bit;
	u8 ext_power_reg;
	u8 ext_power_int_reg;
	u8 ext_power_bit;
	u8 has_power;
};

static void pcm1796_write(struct oxygen *chip, unsigned int codec,
			  u8 reg, u8 value)
{
	/* maps ALSA channel pair number to SPI output */
	static const u8 codec_map[4] = {
		0, 1, 2, 4
	};
	oxygen_write_spi(chip, OXYGEN_SPI_TRIGGER  |
			 OXYGEN_SPI_DATA_LENGTH_2 |
			 OXYGEN_SPI_CLOCK_160 |
			 (codec_map[codec] << OXYGEN_SPI_CODEC_SHIFT) |
			 OXYGEN_SPI_CEN_LATCH_CLOCK_HI,
			 (reg << 8) | value);
}

static void cs4398_write(struct oxygen *chip, u8 reg, u8 value)
{
	oxygen_write_i2c(chip, I2C_DEVICE_CS4398, reg, value);
}

static void cs4362a_write(struct oxygen *chip, u8 reg, u8 value)
{
	oxygen_write_i2c(chip, I2C_DEVICE_CS4362A, reg, value);
}

static void xonar_common_init(struct oxygen *chip)
{
	struct xonar_data *data = chip->model_data;

	if (data->ext_power_reg) {
		oxygen_set_bits8(chip, data->ext_power_int_reg,
				 data->ext_power_bit);
		chip->interrupt_mask |= OXYGEN_INT_GPIO;
		data->has_power = !!(oxygen_read8(chip, data->ext_power_reg)
				     & data->ext_power_bit);
	}
	oxygen_set_bits16(chip, OXYGEN_GPIO_CONTROL, GPIO_CS53x1_M_MASK);
	oxygen_write16_masked(chip, OXYGEN_GPIO_DATA,
			      GPIO_CS53x1_M_SINGLE, GPIO_CS53x1_M_MASK);
	oxygen_ac97_set_bits(chip, 0, CM9780_JACK, CM9780_FMIC2MIC);
	msleep(data->anti_pop_delay);
	oxygen_set_bits16(chip, OXYGEN_GPIO_CONTROL, data->output_enable_bit);
	oxygen_set_bits16(chip, OXYGEN_GPIO_DATA, data->output_enable_bit);
}

static void xonar_d2_init(struct oxygen *chip)
{
	struct xonar_data *data = chip->model_data;
	unsigned int i;

	data->anti_pop_delay = 300;
	data->output_enable_bit = GPIO_D2_OUTPUT_ENABLE;

	for (i = 0; i < 4; ++i) {
		pcm1796_write(chip, i, 18, PCM1796_MUTE | PCM1796_DMF_DISABLED |
			      PCM1796_FMT_24_LJUST | PCM1796_ATLD);
		pcm1796_write(chip, i, 19, PCM1796_FLT_SHARP | PCM1796_ATS_1);
		pcm1796_write(chip, i, 20, PCM1796_OS_64);
		pcm1796_write(chip, i, 21, 0);
		pcm1796_write(chip, i, 16, 0x0f); /* set ATL/ATR after ATLD */
		pcm1796_write(chip, i, 17, 0x0f);
	}

	oxygen_set_bits16(chip, OXYGEN_GPIO_CONTROL, GPIO_D2_ALT);
	oxygen_clear_bits16(chip, OXYGEN_GPIO_DATA, GPIO_D2_ALT);

	xonar_common_init(chip);

	snd_component_add(chip->card, "PCM1796");
	snd_component_add(chip->card, "CS5381");
}

static void xonar_d2x_init(struct oxygen *chip)
{
	struct xonar_data *data = chip->model_data;

	data->ext_power_reg = OXYGEN_GPIO_DATA;
	data->ext_power_int_reg = OXYGEN_GPIO_INTERRUPT_MASK;
	data->ext_power_bit = GPIO_D2X_EXT_POWER;
	oxygen_clear_bits16(chip, OXYGEN_GPIO_CONTROL, GPIO_D2X_EXT_POWER);
	xonar_d2_init(chip);
}

static void xonar_dx_init(struct oxygen *chip)
{
	struct xonar_data *data = chip->model_data;

	data->anti_pop_delay = 800;
	data->output_enable_bit = GPIO_DX_OUTPUT_ENABLE;
	data->ext_power_reg = OXYGEN_GPI_DATA;
	data->ext_power_int_reg = OXYGEN_GPI_INTERRUPT_MASK;
	data->ext_power_bit = GPI_DX_EXT_POWER;

	oxygen_write16(chip, OXYGEN_2WIRE_BUS_STATUS,
		       OXYGEN_2WIRE_LENGTH_8 |
		       OXYGEN_2WIRE_INTERRUPT_MASK |
		       OXYGEN_2WIRE_SPEED_FAST);

	/* set CPEN (control port mode) and power down */
	cs4398_write(chip, 8, CS4398_CPEN | CS4398_PDN);
	cs4362a_write(chip, 0x01, CS4362A_PDN | CS4362A_CPEN);
	/* configure */
	cs4398_write(chip, 2, CS4398_FM_SINGLE |
		     CS4398_DEM_NONE | CS4398_DIF_LJUST);
	cs4398_write(chip, 3, CS4398_ATAPI_B_R | CS4398_ATAPI_A_L);
	cs4398_write(chip, 4, CS4398_MUTEP_LOW | CS4398_PAMUTE);
	cs4398_write(chip, 5, 0xfe);
	cs4398_write(chip, 6, 0xfe);
	cs4398_write(chip, 7, CS4398_RMP_DN | CS4398_RMP_UP |
		     CS4398_ZERO_CROSS | CS4398_SOFT_RAMP);
	cs4362a_write(chip, 0x02, CS4362A_DIF_LJUST);
	cs4362a_write(chip, 0x03, CS4362A_MUTEC_6 | CS4362A_AMUTE |
		      CS4362A_RMP_UP | CS4362A_ZERO_CROSS | CS4362A_SOFT_RAMP);
	cs4362a_write(chip, 0x04, CS4362A_RMP_DN | CS4362A_DEM_NONE);
	cs4362a_write(chip, 0x05, 0);
	cs4362a_write(chip, 0x06, CS4362A_FM_SINGLE |
		      CS4362A_ATAPI_B_R | CS4362A_ATAPI_A_L);
	cs4362a_write(chip, 0x07, 0x7f | CS4362A_MUTE);
	cs4362a_write(chip, 0x08, 0x7f | CS4362A_MUTE);
	cs4362a_write(chip, 0x09, CS4362A_FM_SINGLE |
		      CS4362A_ATAPI_B_R | CS4362A_ATAPI_A_L);
	cs4362a_write(chip, 0x0a, 0x7f | CS4362A_MUTE);
	cs4362a_write(chip, 0x0b, 0x7f | CS4362A_MUTE);
	cs4362a_write(chip, 0x0c, CS4362A_FM_SINGLE |
		      CS4362A_ATAPI_B_R | CS4362A_ATAPI_A_L);
	cs4362a_write(chip, 0x0d, 0x7f | CS4362A_MUTE);
	cs4362a_write(chip, 0x0e, 0x7f | CS4362A_MUTE);
	/* clear power down */
	cs4398_write(chip, 8, CS4398_CPEN);
	cs4362a_write(chip, 0x01, CS4362A_CPEN);

	oxygen_set_bits16(chip, OXYGEN_GPIO_CONTROL,
			  GPIO_DX_FRONT_PANEL | GPIO_DX_INPUT_ROUTE);
	oxygen_clear_bits16(chip, OXYGEN_GPIO_DATA,
			    GPIO_DX_FRONT_PANEL | GPIO_DX_INPUT_ROUTE);

	xonar_common_init(chip);

	snd_component_add(chip->card, "CS4398");
	snd_component_add(chip->card, "CS4362A");
	snd_component_add(chip->card, "CS5361");
}

static void xonar_cleanup(struct oxygen *chip)
{
	struct xonar_data *data = chip->model_data;

	oxygen_clear_bits16(chip, OXYGEN_GPIO_DATA, data->output_enable_bit);
}

static void xonar_dx_cleanup(struct oxygen *chip)
{
	xonar_cleanup(chip);
	cs4362a_write(chip, 0x01, CS4362A_PDN | CS4362A_CPEN);
	oxygen_clear_bits8(chip, OXYGEN_FUNCTION, OXYGEN_FUNCTION_RESET_CODEC);
}

static void set_pcm1796_params(struct oxygen *chip,
			       struct snd_pcm_hw_params *params)
{
	unsigned int i;
	u8 value;

	value = params_rate(params) >= 96000 ? PCM1796_OS_32 : PCM1796_OS_64;
	for (i = 0; i < 4; ++i)
		pcm1796_write(chip, i, 20, value);
}

static void update_pcm1796_volume(struct oxygen *chip)
{
	unsigned int i;

	for (i = 0; i < 4; ++i) {
		pcm1796_write(chip, i, 16, chip->dac_volume[i * 2]);
		pcm1796_write(chip, i, 17, chip->dac_volume[i * 2 + 1]);
	}
}

static void update_pcm1796_mute(struct oxygen *chip)
{
	unsigned int i;
	u8 value;

	value = PCM1796_FMT_24_LJUST | PCM1796_ATLD;
	if (chip->dac_mute)
		value |= PCM1796_MUTE;
	for (i = 0; i < 4; ++i)
		pcm1796_write(chip, i, 18, value);
}

static void set_cs53x1_params(struct oxygen *chip,
			      struct snd_pcm_hw_params *params)
{
	unsigned int value;

	if (params_rate(params) <= 54000)
		value = GPIO_CS53x1_M_SINGLE;
	else if (params_rate(params) <= 108000)
		value = GPIO_CS53x1_M_DOUBLE;
	else
		value = GPIO_CS53x1_M_QUAD;
	oxygen_write16_masked(chip, OXYGEN_GPIO_DATA,
			      value, GPIO_CS53x1_M_MASK);
}

static void set_cs43xx_params(struct oxygen *chip,
			      struct snd_pcm_hw_params *params)
{
	u8 fm_cs4398, fm_cs4362a;

	fm_cs4398 = CS4398_DEM_NONE | CS4398_DIF_LJUST;
	fm_cs4362a = CS4362A_ATAPI_B_R | CS4362A_ATAPI_A_L;
	if (params_rate(params) <= 50000) {
		fm_cs4398 |= CS4398_FM_SINGLE;
		fm_cs4362a |= CS4362A_FM_SINGLE;
	} else if (params_rate(params) <= 100000) {
		fm_cs4398 |= CS4398_FM_DOUBLE;
		fm_cs4362a |= CS4362A_FM_DOUBLE;
	} else {
		fm_cs4398 |= CS4398_FM_QUAD;
		fm_cs4362a |= CS4362A_FM_QUAD;
	}
	cs4398_write(chip, 2, fm_cs4398);
	cs4362a_write(chip, 0x06, fm_cs4362a);
	cs4362a_write(chip, 0x09, fm_cs4362a);
	cs4362a_write(chip, 0x0c, fm_cs4362a);
}

static void update_cs4362a_volumes(struct oxygen *chip)
{
	u8 mute;

	mute = chip->dac_mute ? CS4362A_MUTE : 0;
	cs4362a_write(chip, 7, (127 - chip->dac_volume[2]) | mute);
	cs4362a_write(chip, 8, (127 - chip->dac_volume[3]) | mute);
	cs4362a_write(chip, 10, (127 - chip->dac_volume[4]) | mute);
	cs4362a_write(chip, 11, (127 - chip->dac_volume[5]) | mute);
	cs4362a_write(chip, 13, (127 - chip->dac_volume[6]) | mute);
	cs4362a_write(chip, 14, (127 - chip->dac_volume[7]) | mute);
}

static void update_cs43xx_volume(struct oxygen *chip)
{
	cs4398_write(chip, 5, (127 - chip->dac_volume[0]) * 2);
	cs4398_write(chip, 6, (127 - chip->dac_volume[1]) * 2);
	update_cs4362a_volumes(chip);
}

static void update_cs43xx_mute(struct oxygen *chip)
{
	u8 reg;

	reg = CS4398_MUTEP_LOW | CS4398_PAMUTE;
	if (chip->dac_mute)
		reg |= CS4398_MUTE_B | CS4398_MUTE_A;
	cs4398_write(chip, 4, reg);
	update_cs4362a_volumes(chip);
}

static void xonar_gpio_changed(struct oxygen *chip)
{
	struct xonar_data *data = chip->model_data;
	u8 has_power;

	has_power = !!(oxygen_read8(chip, data->ext_power_reg)
		       & data->ext_power_bit);
	if (has_power != data->has_power) {
		data->has_power = has_power;
		if (has_power) {
			snd_printk(KERN_NOTICE "power restored\n");
		} else {
			snd_printk(KERN_CRIT
				   "Hey! Don't unplug the power cable!\n");
			/* TODO: stop PCMs */
		}
	}
}

static int alt_switch_get(struct snd_kcontrol *ctl,
			  struct snd_ctl_elem_value *value)
{
	struct oxygen *chip = ctl->private_data;

	value->value.integer.value[0] =
		!!(oxygen_read16(chip, OXYGEN_GPIO_DATA) & GPIO_D2_ALT);
	return 0;
}

static int alt_switch_put(struct snd_kcontrol *ctl,
			  struct snd_ctl_elem_value *value)
{
	struct oxygen *chip = ctl->private_data;
	u16 old_bits, new_bits;
	int changed;

	spin_lock_irq(&chip->reg_lock);
	old_bits = oxygen_read16(chip, OXYGEN_GPIO_DATA);
	if (value->value.integer.value[0])
		new_bits = old_bits | GPIO_D2_ALT;
	else
		new_bits = old_bits & ~GPIO_D2_ALT;
	changed = new_bits != old_bits;
	if (changed)
		oxygen_write16(chip, OXYGEN_GPIO_DATA, new_bits);
	spin_unlock_irq(&chip->reg_lock);
	return changed;
}

static const struct snd_kcontrol_new alt_switch = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Analog Loopback Switch",
	.info = snd_ctl_boolean_mono_info,
	.get = alt_switch_get,
	.put = alt_switch_put,
};

static int front_panel_get(struct snd_kcontrol *ctl,
			   struct snd_ctl_elem_value *value)
{
	struct oxygen *chip = ctl->private_data;

	value->value.integer.value[0] =
		!!(oxygen_read16(chip, OXYGEN_GPIO_DATA) & GPIO_DX_FRONT_PANEL);
	return 0;
}

static int front_panel_put(struct snd_kcontrol *ctl,
			   struct snd_ctl_elem_value *value)
{
	struct oxygen *chip = ctl->private_data;
	u16 old_reg, new_reg;

	spin_lock_irq(&chip->reg_lock);
	old_reg = oxygen_read16(chip, OXYGEN_GPIO_DATA);
	if (value->value.integer.value[0])
		new_reg = old_reg | GPIO_DX_FRONT_PANEL;
	else
		new_reg = old_reg & ~GPIO_DX_FRONT_PANEL;
	oxygen_write16(chip, OXYGEN_GPIO_DATA, new_reg);
	spin_unlock_irq(&chip->reg_lock);
	return old_reg != new_reg;
}

static const struct snd_kcontrol_new front_panel_switch = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Front Panel Switch",
	.info = snd_ctl_boolean_mono_info,
	.get = front_panel_get,
	.put = front_panel_put,
};

static void xonar_dx_ac97_switch(struct oxygen *chip,
				 unsigned int reg, unsigned int mute)
{
	if (reg == AC97_LINE) {
		spin_lock_irq(&chip->reg_lock);
		oxygen_write16_masked(chip, OXYGEN_GPIO_DATA,
				      mute ? GPIO_DX_INPUT_ROUTE : 0,
				      GPIO_DX_INPUT_ROUTE);
		spin_unlock_irq(&chip->reg_lock);
	}
}

static const DECLARE_TLV_DB_SCALE(pcm1796_db_scale, -12000, 50, 0);
static const DECLARE_TLV_DB_SCALE(cs4362a_db_scale, -12700, 100, 0);

static int xonar_d2_control_filter(struct snd_kcontrol_new *template)
{
	if (!strncmp(template->name, "CD Capture ", 11))
		/* CD in is actually connected to the video in pin */
		template->private_value ^= AC97_CD ^ AC97_VIDEO;
	return 0;
}

static int xonar_dx_control_filter(struct snd_kcontrol_new *template)
{
	if (!strncmp(template->name, "CD Capture ", 11))
		return 1; /* no CD input */
	return 0;
}

static int xonar_mixer_init(struct oxygen *chip)
{
	return snd_ctl_add(chip->card, snd_ctl_new1(&alt_switch, chip));
}

static int xonar_dx_mixer_init(struct oxygen *chip)
{
	return snd_ctl_add(chip->card, snd_ctl_new1(&front_panel_switch, chip));
}

static const struct oxygen_model xonar_models[] = {
	[MODEL_D2] = {
		.shortname = "Xonar D2",
		.longname = "Asus Virtuoso 200",
		.chip = "AV200",
		.owner = THIS_MODULE,
		.init = xonar_d2_init,
		.control_filter = xonar_d2_control_filter,
		.mixer_init = xonar_mixer_init,
		.cleanup = xonar_cleanup,
		.set_dac_params = set_pcm1796_params,
		.set_adc_params = set_cs53x1_params,
		.update_dac_volume = update_pcm1796_volume,
		.update_dac_mute = update_pcm1796_mute,
		.dac_tlv = pcm1796_db_scale,
		.model_data_size = sizeof(struct xonar_data),
		.pcm_dev_cfg = PLAYBACK_0_TO_I2S |
			       PLAYBACK_1_TO_SPDIF |
			       CAPTURE_0_FROM_I2S_2 |
			       CAPTURE_1_FROM_SPDIF,
		.dac_channels = 8,
		.dac_volume_min = 0x0f,
		.dac_volume_max = 0xff,
		.misc_flags = OXYGEN_MISC_MIDI,
		.function_flags = OXYGEN_FUNCTION_SPI |
				  OXYGEN_FUNCTION_ENABLE_SPI_4_5,
		.dac_i2s_format = OXYGEN_I2S_FORMAT_LJUST,
		.adc_i2s_format = OXYGEN_I2S_FORMAT_LJUST,
	},
	[MODEL_D2X] = {
		.shortname = "Xonar D2X",
		.longname = "Asus Virtuoso 200",
		.chip = "AV200",
		.owner = THIS_MODULE,
		.init = xonar_d2x_init,
		.control_filter = xonar_d2_control_filter,
		.mixer_init = xonar_mixer_init,
		.cleanup = xonar_cleanup,
		.set_dac_params = set_pcm1796_params,
		.set_adc_params = set_cs53x1_params,
		.update_dac_volume = update_pcm1796_volume,
		.update_dac_mute = update_pcm1796_mute,
		.gpio_changed = xonar_gpio_changed,
		.dac_tlv = pcm1796_db_scale,
		.model_data_size = sizeof(struct xonar_data),
		.pcm_dev_cfg = PLAYBACK_0_TO_I2S |
			       PLAYBACK_1_TO_SPDIF |
			       CAPTURE_0_FROM_I2S_2 |
			       CAPTURE_1_FROM_SPDIF,
		.dac_channels = 8,
		.dac_volume_min = 0x0f,
		.dac_volume_max = 0xff,
		.misc_flags = OXYGEN_MISC_MIDI,
		.function_flags = OXYGEN_FUNCTION_SPI |
				  OXYGEN_FUNCTION_ENABLE_SPI_4_5,
		.dac_i2s_format = OXYGEN_I2S_FORMAT_LJUST,
		.adc_i2s_format = OXYGEN_I2S_FORMAT_LJUST,
	},
	[MODEL_DX] = {
		.shortname = "Xonar DX",
		.longname = "Asus Virtuoso 100",
		.chip = "AV200",
		.owner = THIS_MODULE,
		.init = xonar_dx_init,
		.control_filter = xonar_dx_control_filter,
		.mixer_init = xonar_dx_mixer_init,
		.cleanup = xonar_dx_cleanup,
		.set_dac_params = set_cs43xx_params,
		.set_adc_params = set_cs53x1_params,
		.update_dac_volume = update_cs43xx_volume,
		.update_dac_mute = update_cs43xx_mute,
		.gpio_changed = xonar_gpio_changed,
		.ac97_switch = xonar_dx_ac97_switch,
		.dac_tlv = cs4362a_db_scale,
		.model_data_size = sizeof(struct xonar_data),
		.pcm_dev_cfg = PLAYBACK_0_TO_I2S |
			       PLAYBACK_1_TO_SPDIF |
			       CAPTURE_0_FROM_I2S_2,
		.dac_channels = 8,
		.dac_volume_min = 0,
		.dac_volume_max = 127,
		.function_flags = OXYGEN_FUNCTION_2WIRE,
		.dac_i2s_format = OXYGEN_I2S_FORMAT_LJUST,
		.adc_i2s_format = OXYGEN_I2S_FORMAT_LJUST,
	},
};

static int __devinit xonar_probe(struct pci_dev *pci,
				 const struct pci_device_id *pci_id)
{
	static int dev;
	int err;

	if (dev >= SNDRV_CARDS)
		return -ENODEV;
	if (!enable[dev]) {
		++dev;
		return -ENOENT;
	}
	err = oxygen_pci_probe(pci, index[dev], id[dev],
			       &xonar_models[pci_id->driver_data]);
	if (err >= 0)
		++dev;
	return err;
}

static struct pci_driver xonar_driver = {
	.name = "AV200",
	.id_table = xonar_ids,
	.probe = xonar_probe,
	.remove = __devexit_p(oxygen_pci_remove),
};

static int __init alsa_card_xonar_init(void)
{
	return pci_register_driver(&xonar_driver);
}

static void __exit alsa_card_xonar_exit(void)
{
	pci_unregister_driver(&xonar_driver);
}

module_init(alsa_card_xonar_init)
module_exit(alsa_card_xonar_exit)
