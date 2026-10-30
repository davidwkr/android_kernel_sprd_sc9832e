/*
 * Copyright (C) 2015 Spreadtrum Communications Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/reboot.h>
#include <linux/string.h>
#include <linux/gpio.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/spinlock.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/of_irq.h>
#include <linux/mutex.h>
#include <linux/moduleparam.h>
#include <linux/mfd/sprd/pmic_glb_reg.h>
#include "sprd_charge_helper.h"
#include "sprd_2721_charge.h"

#define BITSINDEX(b, o)	((b) * 16 + (o))
#define SPRDCHG_DEBUG(format, arg...) pr_info("sprdchg: " format, ## arg)

static struct regmap *reg_map;
static struct sprd2721_platform_data *g_pdata;
static struct sprdbat_drivier_data *bat_drv_data;

static unsigned int chg_adi_read(unsigned long reg)
{
	unsigned int val;

	regmap_read(reg_map, reg, &val);
	return val;
}

static int chg_adi_write(unsigned long reg, unsigned int or_val,
				 unsigned int clear_msk)
{
	return regmap_update_bits(reg_map, reg, clear_msk, or_val);
}

#ifdef CONFIG_OTP_SPRD_PMIC_EFUSE
static void sprdchg_cccv_cal_get(void)
{
	unsigned int data, blk0;

	blk0 = sprd_pmic_efuse_block_read(0);

	if (!(blk0 & BIT(0))) {
		g_pdata->cccv_cal = 0x20;
		return;
	}

	data = sprd_pmic_efuse_bits_read(BITSINDEX(13, 0), 6);
	pr_info("sprdchg_cccv_cal_get data:0x%x\n", data);
	g_pdata->cccv_cal = (data) & 0x003F;
}
#else
static void sprdchg_cccv_cal_get(void)
{
	g_pdata->cccv_cal = 0x20;
}
#endif

static void sprdchg_set_chg_ovp(uint32_t ovp_vol)
{
	uint32_t temp;

	if (ovp_vol > SPRDBAT_CHG_OVP_LEVEL_MAX)
		ovp_vol = SPRDBAT_CHG_OVP_LEVEL_MAX;

	if (ovp_vol < SPRDBAT_CHG_OVP_LEVEL_MIN)
		ovp_vol = SPRDBAT_CHG_OVP_LEVEL_MIN;

	temp = ((ovp_vol - SPRDBAT_CHG_OVP_LEVEL_MIN) / 100);

	chg_adi_write(ANA_REG_GLB_CHGR_CTRL0, 0,
		BIT_CHGR_CC_EN);

	chg_adi_write(ANA_REG_GLB_CHGR_CTRL1,
		      BITS_VCHG_OVP_V(temp),
		      BITS_VCHG_OVP_V(~0U));

	chg_adi_write(ANA_REG_GLB_CHGR_CTRL0,
		BIT_CHGR_CC_EN,
		BIT_CHGR_CC_EN);
}

static void sprdchg_set_chg_cur(uint32_t chg_current)
{
	uint32_t temp;

	if (bat_drv_data == NULL)
		return;
	if (bat_drv_data->bat_info.chg_current_type_limit < chg_current)
		bat_drv_data->bat_info.chg_current_type_limit = chg_current;

	if (chg_current > SPRDBAT_CHG_CUR_LEVEL_MAX)
		chg_current = SPRDBAT_CHG_CUR_LEVEL_MAX;

	if (chg_current < SPRDBAT_CHG_CUR_LEVEL_MIN)
		chg_current = SPRDBAT_CHG_CUR_LEVEL_MIN;

	if (chg_current < 1400) {
		temp = ((chg_current - 300) / 50);
	} else {
		temp = ((chg_current - 1400) / 100);
		temp += 0x16;
	}


	chg_adi_write(ANA_REG_GLB_CHGR_CTRL0, 0,
		BIT_CHGR_CC_EN);

	chg_adi_write(ANA_REG_GLB_CHGR_CTRL1,
		      BITS_CHGR_CC_I(temp),
		      BITS_CHGR_CC_I(~0U));

	chg_adi_write(ANA_REG_GLB_CHGR_CTRL0,
		BIT_CHGR_CC_EN, BIT_CHGR_CC_EN);
}

static uint32_t sprdchg_get_chg_cur(void)
{
	int rawdata = 0;

	if (chg_adi_read(ANA_REG_GLB_CHGR_CTRL0) & BIT_CHGR_CC_EN) {
		rawdata = chg_adi_read(ANA_REG_GLB_CHGR_CTRL1);
		rawdata = (rawdata >> 10 & 0x1f);
		SPRDCHG_DEBUG("sprdchg_get_chg_cur rawdata * 50+300=%d\n",
			rawdata * 50+300);
		return (rawdata * 50 + 300);
	} else {
		return 0;
	}
}

static uint32_t sprdchg_tune_endvol_cccv(uint32_t chg_end_vol,
	uint32_t cal_cccv)
{
	uint32_t cv;

	chg_adi_write(ANA_REG_GLB_CHGR_CTRL0,
		      BITS_CHGR_END_V(0U), BITS_CHGR_END_V(~0U));
	if (chg_end_vol >= 4200) {
		if (chg_end_vol < 4300) {
			cv = (((chg_end_vol - 4200) * 10) +
			      (ONE_CCCV_STEP_VOL >> 1)) / ONE_CCCV_STEP_VOL +
			    cal_cccv;
			if (cv > SPRDBAT_CCCV_MAX) {
				SPRDCHG_DEBUG("sprdchg: cv > CCCV_MAX!\n");
				chg_adi_write(ANA_REG_GLB_CHGR_CTRL0,
					      BITS_CHGR_END_V(1),
					      BITS_CHGR_END_V(~0U));
				return (cal_cccv -
					(((4300 - chg_end_vol) * 10) +
					 (ONE_CCCV_STEP_VOL >> 1)) /
					ONE_CCCV_STEP_VOL);
			} else {
				return cv;
			}
		} else {
			cv = (((chg_end_vol - 4300) * 10) +
			      (ONE_CCCV_STEP_VOL >> 1)) / ONE_CCCV_STEP_VOL +
			    cal_cccv;
			if (cv > SPRDBAT_CCCV_MAX) {
				SPRDCHG_DEBUG("sprdchg: cv > CCCV_MAX!\n");
				chg_adi_write(ANA_REG_GLB_CHGR_CTRL0,
					      BITS_CHGR_END_V(2),
					      BITS_CHGR_END_V(~0U));
				return (cal_cccv -
					(((4400 - chg_end_vol) * 10) +
					 (ONE_CCCV_STEP_VOL >> 1)) /
					ONE_CCCV_STEP_VOL);
			} else {
				chg_adi_write(ANA_REG_GLB_CHGR_CTRL0,
					      BITS_CHGR_END_V(1),
					      BITS_CHGR_END_V(~0U));
				return cv;
			}
		}
	} else {
		cv = (((4200 - chg_end_vol) * 10) +
		      (ONE_CCCV_STEP_VOL >> 1)) / ONE_CCCV_STEP_VOL;
		if (cv > cal_cccv)
			return 0;
		else
			return (cal_cccv - cv);
	}
}

static uint32_t sprdchg_cccv_cal(uint32_t chg_end_vol_pure)
{
	uint32_t ret = chg_end_vol_pure;

	SPRDCHG_DEBUG("cccv_point efuse:%d\n", g_pdata->cccv_cal);
	ret = sprdchg_tune_endvol_cccv(chg_end_vol_pure, g_pdata->cccv_cal);
	SPRDCHG_DEBUG("cccv_point new cccv:%d\n", ret);

	return ret;
}

/*
 * Optional CV override ("battery hold"): when cv_override_mv is non-zero the
 * charger CV is never programmed above it, so the battery is held below the
 * stock end voltage while USB powers the load. Off (0) by default. The
 * override only ever lowers what sprdbat requests (JEITA / chg-end-vol), and
 * never exceeds the battery profile's chg-end-vol.
 *
 * Enable with the DT property "sprd,cv-override-microvolt" in the charger
 * node, or at runtime (root) through the module parameter:
 *   echo 4100 > /sys/module/sprd_2721_charge/parameters/cv_override_mv
 *   echo 0    > /sys/module/sprd_2721_charge/parameters/cv_override_mv
 * or on the kernel command line: sprd_2721_charge.cv_override_mv=4100
 */
#define CV_OVERRIDE_MIN_MV	4000
#define CV_OVERRIDE_MAX_MV	4400

static DEFINE_MUTEX(cv_lock);	/* serialises END_V + CV_V register writes */
static unsigned int cv_override_mv;	/* 0 = off */
static unsigned int cv_last_req_mv;	/* last CV requested by sprdbat */

static unsigned int sprdchg_cv_effective(unsigned int req)
{
	unsigned int ov = READ_ONCE(cv_override_mv);

	if (!ov)
		return req;
	if (bat_drv_data && bat_drv_data->pdata &&
	    bat_drv_data->pdata->chg_end_vol_pure &&
	    ov > bat_drv_data->pdata->chg_end_vol_pure)
		ov = bat_drv_data->pdata->chg_end_vol_pure;
	return min(req, ov);
}

static void sprdchg_write_cccv(unsigned int cvpoint)
{
	int new_cvpoint;

	new_cvpoint = sprdchg_cccv_cal(cvpoint);
	new_cvpoint &= (~0x1U);
	chg_adi_write(ANA_REG_GLB_CHGR_CTRL0,
		BITS_CHGR_CV_V(new_cvpoint),
		BITS_CHGR_CV_V(~0U));
}

/*
 * Exact model of what sprdchg_write_cccv() programs for a requested CV, as
 * (END_V, CV code). Mirrors sprdchg_tune_endvol_cccv() (round to nearest
 * 7.5 mV step around the efuse calibration cal, END_V range 0/1/2 =
 * 4200/4300/4400 mV base) and the "& ~1" in sprdchg_write_cccv(). The
 * programmed voltage is
 *   base(END_V) + (code - cal) * 7.5 mV,  base = 4200 / 4300 / 4400
 * (below 4200 the code is cal - round((4200 - mv) / 7.5), clamped at 0).
 * Returns false where the stock arithmetic would wrap.
 */
#define CV_STEP_ROUND(d)	((int)(((d) * 10 + (ONE_CCCV_STEP_VOL >> 1)) / \
				       ONE_CCCV_STEP_VOL))

static bool sprdchg_cv_model(unsigned int mv, unsigned int cal,
			     unsigned int *end_v, unsigned int *code)
{
	int cv, e = 0, c = (int)cal;

	if (mv > 4400)
		return false;
	if (mv >= 4200) {
		if (mv < 4300) {
			cv = CV_STEP_ROUND(mv - 4200) + c;
			if (cv > SPRDBAT_CCCV_MAX) {
				e = 1;
				cv = c - CV_STEP_ROUND(4300 - mv);
			}
		} else {
			cv = CV_STEP_ROUND(mv - 4300) + c;
			if (cv > SPRDBAT_CCCV_MAX) {
				e = 2;
				cv = c - CV_STEP_ROUND(4400 - mv);
			} else {
				e = 1;
			}
		}
	} else {
		cv = CV_STEP_ROUND(4200 - mv);
		cv = cv > c ? 0 : c - cv;
	}
	if (cv < 0 || cv > SPRDBAT_CCCV_MAX)
		return false;
	*end_v = e;
	*code = (unsigned int)cv & ~1U;
	return true;
}

/* programmed voltage of (END_V, code) in 0.1 mV */
static int sprdchg_cv_v10(unsigned int end_v, unsigned int code,
			  unsigned int cal)
{
	return (4200 + 100 * (int)end_v) * 10 +
	       ((int)code - (int)cal) * ONE_CCCV_STEP_VOL;
}

/*
 * Highest representable CV not above target_mv (>= 3700 searched). Returns
 * false if even the lowest one is above it.
 */
static bool sprdchg_cv_floor(unsigned int target_mv, unsigned int cal,
			     unsigned int *end_v, unsigned int *code,
			     int *v10)
{
	unsigned int mv, e, c;

	for (mv = target_mv; mv >= 3700; mv--) {
		if (!sprdchg_cv_model(mv, cal, &e, &c))
			continue;
		if (sprdchg_cv_v10(e, c, cal) <= (int)target_mv * 10) {
			*end_v = e;
			*code = c;
			*v10 = sprdchg_cv_v10(e, c, cal);
			return true;
		}
	}
	return false;
}

static bool cv_cal_known;	/* cccv_cal read at probe */
static bool cv_unreachable;	/* last program: override not representable */
static int cv_model_v10;	/* modelled programmed voltage, 0.1 mV */

/*
 * Program the CV for a sprdbat request. Override off: the stock path,
 * unchanged. Override on: the highest representable CV not above
 * min(request, chg-end-vol, override). If that is not possible (calibration
 * unknown, or even the lowest CV is above it) cv_status shows
 * override_unreachable: with the calibration unknown the stock request is
 * kept, otherwise the lowest representable CV is programmed (or the stock
 * request if that is lower still).
 */
static void sprdchg_program_end_cv(unsigned int e, unsigned int c)
{
	/* never leave a higher END_V range with an old, larger code */
	if (e > ((chg_adi_read(ANA_REG_GLB_CHGR_CTRL0) >> 3) & 0x3))
		chg_adi_write(ANA_REG_GLB_CHGR_CTRL0,
			      BITS_CHGR_CV_V(0U), BITS_CHGR_CV_V(~0U));
	chg_adi_write(ANA_REG_GLB_CHGR_CTRL0,
		      BITS_CHGR_END_V(e), BITS_CHGR_END_V(~0U));
	chg_adi_write(ANA_REG_GLB_CHGR_CTRL0,
		      BITS_CHGR_CV_V(c), BITS_CHGR_CV_V(~0U));
}

static void sprdchg_program_cccv(unsigned int req)
{
	unsigned int e, c;
	int v10;

	if (!READ_ONCE(cv_override_mv)) {
		cv_unreachable = false;
		cv_model_v10 = 0;
		sprdchg_write_cccv(req);
		return;
	}
	if (!cv_cal_known) {
		cv_unreachable = true;
		sprdchg_write_cccv(req);
		return;
	}
	if (!sprdchg_cv_floor(sprdchg_cv_effective(req), g_pdata->cccv_cal,
			      &e, &c, &v10)) {
		/* program the lowest representable CV unless stock is lower */
		int lowest = sprdchg_cv_v10(0, 0, g_pdata->cccv_cal);

		cv_unreachable = true;
		if (sprdchg_cv_model(req, g_pdata->cccv_cal, &e, &c) &&
		    sprdchg_cv_v10(e, c, g_pdata->cccv_cal) <= lowest) {
			cv_model_v10 = sprdchg_cv_v10(e, c, g_pdata->cccv_cal);
			sprdchg_write_cccv(req);
		} else {
			cv_model_v10 = lowest;
			sprdchg_program_end_cv(0, 0);
		}
		return;
	}
	cv_unreachable = false;
	cv_model_v10 = v10;
	sprdchg_program_end_cv(e, c);
}

static void sprdchg_set_cccvpoint(unsigned int cvpoint)
{
	mutex_lock(&cv_lock);
	cv_last_req_mv = cvpoint;
	sprdchg_program_cccv(cvpoint);
	mutex_unlock(&cv_lock);
}

static int sprdchg_cv_override_apply(unsigned int mv)
{
	if (mv && (mv < CV_OVERRIDE_MIN_MV || mv > CV_OVERRIDE_MAX_MV))
		return -EINVAL;
	mutex_lock(&cv_lock);
	if (mv && cv_cal_known) {
		unsigned int e, c;
		int v10;

		if (!sprdchg_cv_floor(mv, g_pdata->cccv_cal, &e, &c, &v10)) {
			pr_warn("sprdchg: CV override %u mV unreachable, this unit's lowest CV is %d mV\n",
				mv, sprdchg_cv_v10(0, 0, g_pdata->cccv_cal) / 10);
			mutex_unlock(&cv_lock);
			return -EINVAL;
		}
	}
	cv_override_mv = mv;
	/* reprogram now if a charge has already requested a CV */
	if (cv_last_req_mv && reg_map) {
		/* CV code to 0 first, as chip_init does, so the change is monotone */
		chg_adi_write(ANA_REG_GLB_CHGR_CTRL0,
			      BITS_CHGR_CV_V(0U), BITS_CHGR_CV_V(~0U));
		sprdchg_program_cccv(cv_last_req_mv);
	}
	mutex_unlock(&cv_lock);
	return 0;
}

static int cv_override_set(const char *val, const struct kernel_param *kp)
{
	unsigned int mv;
	int ret = kstrtouint(val, 0, &mv);

	return ret ? ret : sprdchg_cv_override_apply(mv);
}

static const struct kernel_param_ops cv_override_ops = {
	.set = cv_override_set,
	.get = param_get_uint,
};
module_param_cb(cv_override_mv, &cv_override_ops, &cv_override_mv, 0644);
MODULE_PARM_DESC(cv_override_mv,
	"Cap the SC2721 charger CV at this many mV (4000..4400, 0 = off)");

/* read-only: what sprdbat last requested and what the PMIC holds */
static int cv_status_get(char *buf, const struct kernel_param *kp)
{
	unsigned int val;
	int ret;

	mutex_lock(&cv_lock);
	if (!reg_map) {
		mutex_unlock(&cv_lock);
		return scnprintf(buf, PAGE_SIZE, "not probed\n");
	}
	val = chg_adi_read(ANA_REG_GLB_CHGR_CTRL0);
	ret = scnprintf(buf, PAGE_SIZE,
		"override_mv=%u requested_mv=%u target_mv=%u model_mv=%d.%d state=%s hw_cv_code=%u hw_end_v=%u\n",
		cv_override_mv, cv_last_req_mv,
		cv_last_req_mv ? sprdchg_cv_effective(cv_last_req_mv) : 0,
		cv_model_v10 / 10, cv_model_v10 % 10,
		!cv_override_mv ? "off" :
		cv_unreachable ? "override_unreachable" :
		!cv_cal_known ? "pending" : "ok",
		(val >> 5) & 0x3f, (val >> 3) & 0x3);
	mutex_unlock(&cv_lock);
	return ret;
}

static const struct kernel_param_ops cv_status_ops = {
	.get = cv_status_get,
};
module_param_cb(cv_status, &cv_status_ops, NULL, 0444);

int sprdchg_bc1p2_disable(int disable)
{
	if (disable)
		glb_adi_write(ANA_REG_GLB_CHGR_DET_FGU_CTRL,
				BIT_DP_DM_BC_ENB, BIT_DP_DM_BC_ENB);
	else
		glb_adi_write(ANA_REG_GLB_CHGR_DET_FGU_CTRL,
				0, BIT_DP_DM_BC_ENB);
	return 0;
}

static void _sprdchg_set_recharge(void)
{
	chg_adi_write(ANA_REG_GLB_CHGR_CTRL0,
		BIT_RECHG, BIT_RECHG);
}

static void _sprdchg_stop_recharge(void)
{
	chg_adi_write(ANA_REG_GLB_CHGR_CTRL0, 0, BIT_RECHG);
}

static void sprdchg_stop_charge(unsigned int flag)
{
	chg_adi_write(ANA_REG_GLB_CHGR_CTRL0,
		BIT_CHGR_PD, BIT_CHGR_PD);
	_sprdchg_stop_recharge();
}

static void sprdchg_start_charge(void)
{
	chg_adi_write(ANA_REG_GLB_CHGR_CTRL0, 0, BIT_CHGR_PD);
	_sprdchg_set_recharge();
}

static void sprdchg_set_eoc_level(int level)
{
	chg_adi_write(ANA_REG_GLB_CHGR_CTRL0,
		BITS_CHGR_ITERM(level),
		BITS_CHGR_ITERM(~0U));
}

static int sprdchg_get_cccvstate(void)
{
	SPRDCHG_DEBUG("sprdbat:cv state:0x%x, iterm:0x%x",
		chg_adi_read(ANA_REG_GLB_CHGR_STATUS),
		chg_adi_read(ANA_REG_GLB_CHGR_CTRL0));
	return ((chg_adi_read(ANA_REG_GLB_CHGR_STATUS) &
		BIT_CHGR_CV_STATUS) ? 1 : 0);
}

static int sprdchg_get_charge_fault(void)
{
	return 0;
}

static void sprdchg_chip_init(struct sprdbat_drivier_data *bdata)
{
	if (bdata == NULL)
		return;
	bat_drv_data = bdata;
	glb_adi_write(ANA_REG_GLB_CHGR_CTRL0,
		BIT_CHGR_CC_EN, BIT_CHGR_CC_EN);
	chg_adi_write(ANA_REG_GLB_CHGR_CTRL0,
		      BITS_CHGR_CV_V(0U), BITS_CHGR_CV_V(~0U));

	chg_adi_write(ANA_REG_GLB_CHGR_CTRL0,
		      BITS_CHGR_DPM(3), BITS_CHGR_DPM(~0U));

	sprdchg_set_cccvpoint(bat_drv_data->pdata->chg_end_vol_pure);
	sprdchg_set_chg_ovp(bat_drv_data->pdata->ovp_stop);
	sprdchg_set_eoc_level(0);
}

static __used irqreturn_t sprdchg_vchg_ovi_irq(int irq, void *dev_id)
{
	int value;
	struct sprd2721_device *sprd_2721 = dev_id;
	struct sprd2721_platform_data *pdata = sprd_2721->dev->platform_data;

	value = gpio_get_value(pdata->gpio_vchg_ovi);
	if (value) {
		irq_set_irq_type(pdata->irq_vchg_ovi,
				 IRQ_TYPE_LEVEL_LOW);
	} else {
		irq_set_irq_type(pdata->irq_vchg_ovi,
				 IRQ_TYPE_LEVEL_HIGH);
	}
	power_supply_changed(&sprd_2721->charger);
	return IRQ_HANDLED;
}

static irqreturn_t sprdchg_chg_cv_irq(int irq, void *dev_id)
{
	int value;
	struct sprd2721_device *sprd_2721 = dev_id;
	struct sprd2721_platform_data *pdata = sprd_2721->dev->platform_data;

	value = gpio_get_value(pdata->gpio_chg_cv_state);
	power_supply_changed(&sprd_2721->charger);
	return IRQ_HANDLED;
}

static  struct sprd2721_platform_data *
sprdchg_2721_parse_dt(struct device *dev)
{
	struct device_node *np = dev->of_node;
	struct sprd2721_platform_data *pdata = NULL;
	int gpio;

	if (!np)
		return ERR_PTR(-ENODEV);
	pdata = devm_kzalloc(dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return ERR_PTR(-ENOMEM);

	gpio = of_get_named_gpio(np, "chg-cv-gpios", 0);
	if (gpio < 0)
		SPRDCHG_DEBUG("chg-cv-gpios parse err\n");
	else
		pdata->gpio_chg_cv_state = (uint32_t)gpio;

	gpio = of_get_named_gpio(np, "chg-ovi-gpios", 0);
	if (gpio < 0)
		SPRDCHG_DEBUG("chg-ovi-gpios parse err\n");
	else
		pdata->gpio_vchg_ovi = (uint32_t)gpio;

	return pdata;
}

static struct sprd_ext_ic_operations sprd_2721_op = {
	.ic_init = sprdchg_chip_init,
	.charge_start_ext = sprdchg_start_charge,
	.set_charge_cur = sprdchg_set_chg_cur,
	.charge_stop_ext = sprdchg_stop_charge,
	.get_charging_status = sprdchg_get_cccvstate,
	.get_charging_fault =  sprdchg_get_charge_fault,
	.get_charge_cur_ext = sprdchg_get_chg_cur,
	.set_termina_vol_ext = sprdchg_set_cccvpoint,
};

static int sprd2721_get_property(struct power_supply *psy,
				    enum power_supply_property psp,
				    union power_supply_propval *val)
{
	return -EINVAL;
}


static int sprdchg_2721_probe(struct platform_device *pdev)
{
	struct sprd2721_device *sprd_2721 = NULL;
	struct sprd2721_platform_data *pdata = pdev->dev.platform_data;
	int ret = 0;
	struct power_supply *ret_ptr = NULL;
	struct power_supply_desc *charger_desc = NULL;

	reg_map = dev_get_regmap(pdev->dev.parent, NULL);
	if (!reg_map) {
		dev_err(&pdev->dev, "%s :NULL regmap for charge 2721\n",
			__func__);
		return -EINVAL;
	}
	sprd_2721 = devm_kzalloc(&pdev->dev, sizeof(*sprd_2721), GFP_KERNEL);
	if (!sprd_2721)
		return -ENOMEM;

	if (!pdata)
		pdata = sprdchg_2721_parse_dt(&pdev->dev);
	if (IS_ERR_OR_NULL(pdata))
		return PTR_ERR(pdata);

	g_pdata = pdata;
	sprd_2721->dev = &pdev->dev;
	platform_set_drvdata(pdev, sprd_2721);

	if (pdata->gpio_chg_cv_state) {
		ret = devm_gpio_request(&pdev->dev,
			pdata->gpio_chg_cv_state, "chg_cv_state");
		if (ret) {
			dev_err(&pdev->dev,
				"failed to request gpio: %d\n", ret);
			return ret;
		}
		gpio_direction_input(pdata->gpio_chg_cv_state);
		pdata->irq_chg_cv_state = gpio_to_irq(pdata->gpio_chg_cv_state);

		irq_set_status_flags(pdata->irq_chg_cv_state, IRQ_NOAUTOEN);
		ret = devm_request_threaded_irq(&pdev->dev,
			pdata->irq_chg_cv_state, NULL,
			sprdchg_chg_cv_irq,
			IRQF_NO_SUSPEND | IRQF_ONESHOT,
			"sprdbat_chg_cv_state", sprd_2721);
		if (ret) {
			dev_err(&pdev->dev,
				"failed to request chg cv irq: %d\n", ret);
			return ret;
		}
	}

	if (pdata->gpio_vchg_ovi) {
		ret = devm_gpio_request(&pdev->dev,
			pdata->gpio_vchg_ovi, "vchg_ovi");
		if (ret) {
			dev_err(&pdev->dev,
				"failed to request gpio: %d\n", ret);
			return ret;
		}
		gpio_direction_input(pdata->gpio_vchg_ovi);
		pdata->irq_vchg_ovi = gpio_to_irq(pdata->gpio_vchg_ovi);

		irq_set_status_flags(pdata->irq_vchg_ovi, IRQ_NOAUTOEN);
		ret = devm_request_threaded_irq(&pdev->dev,
			pdata->irq_vchg_ovi, NULL,
			sprdchg_vchg_ovi_irq,
			IRQF_NO_SUSPEND | IRQF_ONESHOT,
			"sprdbat_vchg_ovi", sprd_2721);
		if (ret) {
			dev_err(&pdev->dev,
				"failed to request chg ovi irq: %d\n", ret);
			return ret;
		}
	}
	sprdchg_cccv_cal_get();
	cv_cal_known = true;
	if (cv_override_mv) {
		/* set before the calibration was known (cmdline): re-check */
		unsigned int e, c;
		int v10;

		if (!sprdchg_cv_floor(cv_override_mv, g_pdata->cccv_cal,
				      &e, &c, &v10))
			dev_warn(&pdev->dev,
				 "CV override %u mV unreachable, lowest CV is %d mV; state override_unreachable\n",
				 cv_override_mv,
				 sprdchg_cv_v10(0, 0, g_pdata->cccv_cal) / 10);
	}
	{
		u32 uv;

		/*
		 * DT default; a non-zero value already set via the module
		 * parameter (cmdline) wins. A cmdline cv_override_mv=0 cannot
		 * disable a DT override (0 means "not set" here); clear the
		 * runtime value with a write of 0 after boot instead.
		 */
		if (!cv_override_mv &&
		    !of_property_read_u32(pdev->dev.of_node,
					  "sprd,cv-override-microvolt", &uv)) {
			if (sprdchg_cv_override_apply(uv / 1000))
				dev_warn(&pdev->dev,
					 "sprd,cv-override-microvolt %u out of range, ignored\n",
					 uv);
			else
				dev_info(&pdev->dev, "CV override %u mV\n",
					 cv_override_mv);
		}
	}
	sprdbat_register_ext_ops(&sprd_2721_op);
	charger_desc = devm_kzalloc(&pdev->dev,
		sizeof(*charger_desc), GFP_KERNEL);
	if (charger_desc == NULL) {
		ret = -ENOMEM;
		sprd_2721 = NULL;
		return ret;
	}
	sprd_2721->charger.desc = charger_desc;
	charger_desc->name = CHG_PSY_INNER;
	charger_desc->type = POWER_SUPPLY_TYPE_UNKNOWN;
	charger_desc->get_property = sprd2721_get_property;
	ret_ptr = power_supply_register(&pdev->dev,
		sprd_2721->charger.desc, NULL);
	if (IS_ERR(ret_ptr)) {
		dev_err(&pdev->dev, "failed to register sprd_2721 psy: %p\n",
			ret_ptr);
		return PTR_ERR(ret_ptr);
	}
	SPRDCHG_DEBUG("%s probe ok\n", __func__);
	return 0;
}

static int sprdchg_2721_remove(struct platform_device *pdev)
{
	struct sprd2721_device *sprd_2721 = platform_get_drvdata(pdev);

	power_supply_unregister(&sprd_2721->charger);
	return 0;
}

static const struct of_device_id sprdchg_2721_of_match[] = {
	{.compatible = "sprd,sc2721-charger",},
	{}
};

static struct platform_driver sprdchg_2721_driver = {
	.driver = {
		   .name = "sc2721-charger",
		   .of_match_table = of_match_ptr(sprdchg_2721_of_match),
		   },
	.probe = sprdchg_2721_probe,
	.remove = sprdchg_2721_remove
};

static int __init sprdchg_2721_driver_init(void)
{
	return platform_driver_register(&sprdchg_2721_driver);
}

static void __exit sprdchg_2721_driver_exit(void)
{
	platform_driver_unregister(&sprdchg_2721_driver);
}

subsys_initcall_sync(sprdchg_2721_driver_init);
module_exit(sprdchg_2721_driver_exit);
