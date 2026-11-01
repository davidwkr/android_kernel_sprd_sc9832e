// SPDX-License-Identifier: GPL-2.0：
// Copyright (C) 2019 Spreadtrum Communications Inc.

#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/power/charger-manager.h>
#include <linux/power/sc2721-charger-cv.h>
#include <linux/usb/phy.h>
#include <linux/regmap.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/slab.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>

#define SC2720_BATTERY_NAME			"sc27xx-fgu"

#define SC2720_CHG_CFG0				0x0
#define SC2720_CHG_CFG1				0x4

#define SC2720_TERM_VOLTAGE_MIN			4200
#define SC2720_TERM_VOLTAGE_MAX			4500
#define SC2720_TERM_VOLTAGE_STEP		100
#define SC2720_CCCV_VOLTAGE_STEP		75
#define SC2720_CHG_DPM_4300MV			4300
#define SC2720_CHG_CCCV_MAXIMUM			0x3f
#define SC2720_CHG_CURRENT_MIN			300
#define SC2720_CHG_CURRENT_800MA		800
#define SC2720_CHG_CURRENT_800MA_REG		0x0a
#define SC2720_CHG_CURRENT_STEP_50MA		50
#define SC2720_CHG_CURRENT_STEP_100MA		100

#define SC2720_CHG_PD				BIT(0)
#define SC2720_CHG_CC_MODE			BIT(12)

#define SC2720_CHG_DPM_MASK			GENMASK(14, 13)
#define SC2720_CHG_DPM_MASK_SHIT		13

#define SC2720_CHG_OVP_MASK			GENMASK(1, 0)

#define SC2720_CHG_TERMINATION_CURRENT_MASK	GENMASK(1, 0)
#define SC2720_CHG_TERMINATION_CURRENT_MASK_SHIT	1

#define SC2720_CHG_END_V_MASK			GENMASK(4, 3)
#define SC2720_CHG_END_V_MASK_SHIT		3

#define SC2720_CHG_CV_V_MASK			GENMASK(10, 5)
#define SC2720_CHG_CV_V_MASK_SHIT		5

#define SC2720_CHG_CC_I_MASK			GENMASK(13, 10)
#define SC2720_CHG_CC_I_MASK_SHIT		10

#define SC2720_CHG_CCCV_MASK			GENMASK(5, 0)

/*
 * SC2721 values, taken from the 4.4 sprd_2721_charge.[ch] and sc2721_glb.h
 * (see devices/SC2721_CHARGER_MAP.md for the file:line of each one).
 */
#define SC2721_CHG_CC_I_MASK			GENMASK(14, 10)
#define SC2721_CHG_CURRENT_MAX			1300
#define SC2721_CHG_DPM_3			0x3
#define SC2721_END_V_BASE_MV			4200
#define SC2721_END_V_MAX			2
#define SC2721_END_V_MAX_MV			4400
#define SC2721_CV_CODE_MASK			(~1U)
#define SC2721_CCCV_CAL_DEFAULT			0x20
#define SC2721_CV_SEARCH_MIN_MV			3700
#define SC2721_CV_OVERRIDE_MIN_MV		4000
#define SC2721_CV_OVERRIDE_MAX_MV		4400
/* input over-voltage protection, CHGR_CTRL1 bits 5..0, 100 mV steps */
#define SC2721_OVP_MASK				GENMASK(5, 0)
#define SC2721_OVP_MIN_MV			5600
#define SC2721_OVP_MAX_MV			9000
#define SC2721_OVP_STEP_MV			100

struct sc2720_charger_data {
	const struct power_supply_desc *desc;
	u32 cc_i_mask;
	bool is_sc2721;
};

struct sc2720_charger_info {
	struct device *dev;
	struct regmap *regmap;
	struct usb_phy *usb_phy;
	struct notifier_block usb_notify;
	struct power_supply *psy_usb;
	struct power_supply_charge_current cur;
	struct work_struct work;
	struct mutex lock;
	const struct sc2720_charger_data *data;
	bool charging;
	u32 limit;
	u32 base;
	/* SC2721 only: protected by sc2721_cv_lock */
	u32 cv_cal;
	u32 cv_vmax_mv;
	u32 cv_last_req_mv;
	/* SC2721: last requested charge current in uA, before the adapter cap */
	u32 cur_req_ua;
	/* SC2721 only: "sprd,ovp-microvolt", 0 = not set */
	u32 ovp_uv;
};

/* SC2721 CV override ("battery hold"), 0 = off, see sc2721_cv_override_ops */
static DEFINE_MUTEX(sc2721_cv_lock);
static unsigned int sc2721_cv_override_mv;
/* bumped under sc2721_cv_lock on every change of the request or the override */
static unsigned int sc2721_cv_gen;
/* the one SC2721 charger that owns the override, under sc2721_cv_lock */
static struct sc2720_charger_info *sc2721_cv_info;

static bool sc2720_charger_is_bat_present(struct sc2720_charger_info *info)
{
	struct power_supply *psy;
	union power_supply_propval val;
	bool present = false;
	int ret;

	psy = power_supply_get_by_name(SC2720_BATTERY_NAME);
	if (!psy) {
		dev_err(info->dev, "Failed to get psy of sc27xx_fgu\n");
		return present;
	}
	ret = power_supply_get_property(psy, POWER_SUPPLY_PROP_PRESENT,
					&val);
	if (ret == 0 && val.intval)
		present = true;
	power_supply_put(psy);

	if (ret)
		dev_err(info->dev,
			"Failed to get property of present:%d\n", ret);

	return present;
}

/*
 * SC2721 CCCV programming. Model of the 4.4 sprd_2721_charge.c: the
 * programmed voltage is
 *   (4200 + 100 * END_V) mV + (CV code - calibration) * 7.5 mV
 * with END_V 0..2 and the CV code in 0..0x3f with bit 0 always cleared.
 * Voltages inside this block are in mV, "v10" values in 0.1 mV.
 */
#define SC2721_CV_STEP_ROUND(d)	\
	((int)(((d) * 10 + (SC2720_CCCV_VOLTAGE_STEP >> 1)) / \
	       SC2720_CCCV_VOLTAGE_STEP))

/* END_V and CV code that the 4.4 driver programs for a requested CV */
static bool sc2721_cv_model(unsigned int mv, unsigned int cal,
			    unsigned int *end_v, unsigned int *code)
{
	int cv, e = 0, c = (int)cal;

	if (mv >= SC2721_END_V_BASE_MV) {
		if (mv < SC2721_END_V_BASE_MV + SC2720_TERM_VOLTAGE_STEP) {
			cv = SC2721_CV_STEP_ROUND(mv - SC2721_END_V_BASE_MV) + c;
			if (cv > SC2720_CHG_CCCV_MAXIMUM) {
				e = 1;
				cv = c - SC2721_CV_STEP_ROUND(
					SC2721_END_V_BASE_MV +
					SC2720_TERM_VOLTAGE_STEP - mv);
			}
		} else {
			cv = SC2721_CV_STEP_ROUND(mv - (SC2721_END_V_BASE_MV +
					SC2720_TERM_VOLTAGE_STEP)) + c;
			if (cv > SC2720_CHG_CCCV_MAXIMUM) {
				/*
				 * Above 4400 mV only END_V 1 with a high CV code
				 * reaches the target; the 4.4 arithmetic would wrap
				 * here, so refuse.
				 */
				if (mv > SC2721_END_V_MAX_MV)
					return false;
				e = 2;
				cv = c - SC2721_CV_STEP_ROUND(
					SC2721_END_V_MAX_MV - mv);
			} else {
				e = 1;
			}
		}
	} else {
		cv = SC2721_CV_STEP_ROUND(SC2721_END_V_BASE_MV - mv);
		cv = cv > c ? 0 : c - cv;
	}
	if (cv < 0 || cv > SC2720_CHG_CCCV_MAXIMUM)
		return false;
	*end_v = e;
	*code = (unsigned int)cv & SC2721_CV_CODE_MASK;
	return true;
}

/* programmed voltage of (END_V, code) in 0.1 mV */
static int sc2721_cv_v10(unsigned int end_v, unsigned int code,
			 unsigned int cal)
{
	return (SC2721_END_V_BASE_MV + SC2720_TERM_VOLTAGE_STEP * (int)end_v) *
	       10 + ((int)code - (int)cal) * SC2720_CCCV_VOLTAGE_STEP;
}

/*
 * Highest representable CV not above target_mv (searched down to 3700 mV).
 * Returns false if even the lowest representable CV is above it.
 */
static bool sc2721_cv_floor(unsigned int target_mv, unsigned int cal,
			    unsigned int *end_v, unsigned int *code)
{
	unsigned int mv, e, c;

	for (mv = target_mv; mv >= SC2721_CV_SEARCH_MIN_MV; mv--) {
		if (!sc2721_cv_model(mv, cal, &e, &c))
			continue;
		if (sc2721_cv_v10(e, c, cal) <= (int)target_mv * 10) {
			*end_v = e;
			*code = c;
			return true;
		}
	}
	return false;
}

/* override capped to the battery profile maximum, then min with request */
static unsigned int sc2721_cv_effective(struct sc2720_charger_info *info,
					unsigned int req)
{
	unsigned int ov = READ_ONCE(sc2721_cv_override_mv);

	if (!ov)
		return req;
	if (info->cv_vmax_mv && ov > info->cv_vmax_mv)
		ov = info->cv_vmax_mv;
	return min(req, ov);
}

/*
 * The CV in uV the override holds the battery at, 0 when it is not holding:
 * override off, no CV requested yet, the request is already at or below the
 * override, or the override is not representable on this unit (the 4.4
 * gr_hold_target_mv() conditions). Read by the fuel gauge for its resync.
 */
static unsigned int sc2721_cv_hold_uv(struct sc2720_charger_info *info)
{
	unsigned int req, eff, e, c, hold = 0;

	mutex_lock(&sc2721_cv_lock);
	req = info->cv_last_req_mv;
	if (READ_ONCE(sc2721_cv_override_mv) && req) {
		eff = sc2721_cv_effective(info, req);
		if (eff < req && sc2721_cv_floor(eff, info->cv_cal, &e, &c))
			hold = eff;
	}
	mutex_unlock(&sc2721_cv_lock);

	return hold * 1000;
}

/* CV change generation, see sc2721_cv_gen (wraps, only compared for equality) */
unsigned int sc2721_charger_cv_generation(void)
{
	unsigned int gen;

	mutex_lock(&sc2721_cv_lock);
	gen = sc2721_cv_gen;
	mutex_unlock(&sc2721_cv_lock);

	return gen;
}
EXPORT_SYMBOL_GPL(sc2721_charger_cv_generation);

/*
 * Write END_V and the CV code. The code goes through 0 first so that the
 * change is monotone, then END_V, then the final code. Caller holds
 * sc2721_cv_lock.
 */
static int sc2721_program_end_cv(struct sc2720_charger_info *info,
				 unsigned int end_v, unsigned int code)
{
	int ret;

	ret = regmap_update_bits(info->regmap, info->base + SC2720_CHG_CFG0,
				 SC2720_CHG_CV_V_MASK, 0);
	if (ret)
		goto err;

	ret = regmap_update_bits(info->regmap, info->base + SC2720_CHG_CFG0,
				 SC2720_CHG_END_V_MASK,
				 end_v << SC2720_CHG_END_V_MASK_SHIT);
	if (ret)
		goto err;

	ret = regmap_update_bits(info->regmap, info->base + SC2720_CHG_CFG0,
				 SC2720_CHG_CV_V_MASK,
				 code << SC2720_CHG_CV_V_MASK_SHIT);
	if (ret)
		goto err;

	return 0;
err:
	dev_err(info->dev, "failed to set charge end_v/cv_v: %d\n", ret);
	return ret;
}

/*
 * Program the CV for a request in mV. Override off: the stock 4.4 value
 * (requests the stock arithmetic cannot represent are refused, nothing is
 * written). Override on: the highest representable CV not above
 * min(request, battery maximum, override); if none exists the lowest
 * representable CV, or the stock value if that is lower still. Caller holds
 * sc2721_cv_lock.
 */
static int sc2721_program_cccv(struct sc2720_charger_info *info,
			       unsigned int req)
{
	unsigned int e, c;

	if (!READ_ONCE(sc2721_cv_override_mv)) {
		if (!sc2721_cv_model(req, info->cv_cal, &e, &c))
			return -EINVAL;
		return sc2721_program_end_cv(info, e, c);
	}

	if (!sc2721_cv_floor(sc2721_cv_effective(info, req), info->cv_cal,
			     &e, &c)) {
		int lowest = sc2721_cv_v10(0, 0, info->cv_cal);

		if (sc2721_cv_model(req, info->cv_cal, &e, &c) &&
		    sc2721_cv_v10(e, c, info->cv_cal) <= lowest)
			return sc2721_program_end_cv(info, e, c);

		dev_warn(info->dev,
			 "CV override unreachable, programming lowest CV\n");
		return sc2721_program_end_cv(info, 0, 0);
	}

	return sc2721_program_end_cv(info, e, c);
}

static int sc2721_set_termination_voltage(struct sc2720_charger_info *info,
					  u32 vol)
{
	unsigned int mv = vol / 1000;
	int ret;

	if (!mv)
		return -EINVAL;

	mutex_lock(&sc2721_cv_lock);
	if (info->cv_last_req_mv != mv)
		sc2721_cv_gen++;
	info->cv_last_req_mv = mv;
	ret = sc2721_program_cccv(info, mv);
	mutex_unlock(&sc2721_cv_lock);

	return ret;
}

static int sc2721_read_cell_u32(struct device *dev, const char *name, u32 *val)
{
	struct nvmem_cell *cell;
	void *buf;
	size_t len;

	cell = nvmem_cell_get(dev, name);
	if (IS_ERR(cell))
		return PTR_ERR(cell);

	buf = nvmem_cell_read(cell, &len);
	nvmem_cell_put(cell);
	if (IS_ERR(buf))
		return PTR_ERR(buf);

	*val = 0;
	memcpy(val, buf, min(len, sizeof(*val)));
	kfree(buf);

	return 0;
}

/*
 * CCCV calibration: 6 bits from the efuse, valid only if the efuse valid
 * flag is set, otherwise 0x20 (4.4 sprdchg_cccv_cal_get()).
 */
static int sc2721_read_calib(struct sc2720_charger_info *info)
{
	u32 data, valid;
	int ret;

	ret = sc2721_read_cell_u32(info->dev, "cccv_calib_valid", &valid);
	if (ret)
		return ret;

	if (!(valid & BIT(0))) {
		info->cv_cal = SC2721_CCCV_CAL_DEFAULT;
		return 0;
	}

	ret = sc2721_read_cell_u32(info->dev, "cccv_calib", &data);
	if (ret)
		return ret;

	info->cv_cal = data & SC2720_CHG_CCCV_MASK;
	return 0;
}

static int sc2721_cv_override_apply(unsigned int mv)
{
	struct sc2720_charger_info *info;
	unsigned int e, c;
	int ret = 0;

	if (mv && (mv < SC2721_CV_OVERRIDE_MIN_MV ||
		   mv > SC2721_CV_OVERRIDE_MAX_MV))
		return -EINVAL;

	mutex_lock(&sc2721_cv_lock);
	info = sc2721_cv_info;
	if (mv && info && !sc2721_cv_floor(mv, info->cv_cal, &e, &c)) {
		dev_warn(info->dev,
			 "CV override %u mV unreachable, this unit's lowest CV is %d mV\n",
			 mv, sc2721_cv_v10(0, 0, info->cv_cal) / 10);
		ret = -EINVAL;
		goto out;
	}

	if (sc2721_cv_override_mv != mv)
		sc2721_cv_gen++;
	WRITE_ONCE(sc2721_cv_override_mv, mv);
	/* reprogram now if a charge has already requested a CV */
	if (info && info->cv_last_req_mv)
		ret = sc2721_program_cccv(info, info->cv_last_req_mv);
out:
	mutex_unlock(&sc2721_cv_lock);
	return ret;
}

static int sc2721_cv_override_set(const char *val,
				  const struct kernel_param *kp)
{
	unsigned int mv;
	int ret;

	ret = kstrtouint(val, 0, &mv);
	if (ret)
		return ret;

	return sc2721_cv_override_apply(mv);
}

static const struct kernel_param_ops sc2721_cv_override_ops = {
	.set = sc2721_cv_override_set,
	.get = param_get_uint,
};
module_param_cb(cv_override_mv, &sc2721_cv_override_ops,
		&sc2721_cv_override_mv, 0644);
MODULE_PARM_DESC(cv_override_mv,
		 "Cap the SC2721 charger CV at this many mV (4000..4400, 0 = off)");

static int sc2720_set_termination_voltage(struct sc2720_charger_info *info,
					  u32 vol)
{
	struct nvmem_cell *cell;
	u32 big_level, small_level, cv;
	int ret, calib_data;
	void *buf;
	size_t len;

	if (info->data->is_sc2721)
		return sc2721_set_termination_voltage(info, vol);

	cell = nvmem_cell_get(info->dev, "cccv_calib");
	if (IS_ERR(cell))
		return PTR_ERR(cell);

	buf = nvmem_cell_read(cell, &len);
	nvmem_cell_put(cell);

	if (IS_ERR(buf))
		return PTR_ERR(buf);

	memcpy(&calib_data, buf, min(len, sizeof(u32)));
	calib_data = calib_data & SC2720_CHG_CCCV_MASK;
	kfree(buf);

	vol = vol / 1000;
	if (vol > SC2720_TERM_VOLTAGE_MAX)
		vol = SC2720_TERM_VOLTAGE_MAX;

	if (vol >= SC2720_TERM_VOLTAGE_MIN) {
		u32 temp = vol % SC2720_TERM_VOLTAGE_STEP;

		/*
		 * To set 2720 charger cccv point.
		 * following formula:
		 * cccv point = big_level + small_level + efuse value
		 *
		 * the big_level corresponds to 2720 charger spec register
		 * CHGR_END_V
		 * the small_level corresponds to 2720 charger spec register
		 * CHGR_CV_V
		 */
		big_level = (vol - SC2720_TERM_VOLTAGE_MIN) / SC2720_TERM_VOLTAGE_STEP;
		cv = DIV_ROUND_CLOSEST(temp * 10, SC2720_CCCV_VOLTAGE_STEP);
		small_level = cv + calib_data;
		if (small_level > SC2720_CHG_CCCV_MAXIMUM) {
			big_level++;
			cv = DIV_ROUND_CLOSEST((SC2720_TERM_VOLTAGE_STEP - temp) * 10,
					       SC2720_CCCV_VOLTAGE_STEP);
			small_level = calib_data - cv;
		}
	} else {
		big_level = 0;
		cv = DIV_ROUND_CLOSEST((SC2720_TERM_VOLTAGE_MIN - vol) * 10,
				       SC2720_CCCV_VOLTAGE_STEP);
		if (cv > calib_data)
			small_level = 0;
		else
			small_level = calib_data - cv;
	}

	ret = regmap_update_bits(info->regmap,
				 info->base + SC2720_CHG_CFG0,
				 SC2720_CHG_END_V_MASK,
				 big_level << SC2720_CHG_END_V_MASK_SHIT);
	if (ret) {
		dev_err(info->dev, "failed to set charge end_v\n");
		return ret;
	}

	ret = regmap_update_bits(info->regmap,
				 info->base + SC2720_CHG_CFG0,
				 SC2720_CHG_CV_V_MASK,
				 small_level << SC2720_CHG_CV_V_MASK_SHIT);
	if (ret)
		dev_err(info->dev, "failed to set charge end_v\n");

	return ret;
}

/*
 * SC2721 input over-voltage protection, port of the 4.4
 * sprdchg_set_chg_ovp(): clamp to 5600..9000 mV, code = (mV - 5600) / 100 in
 * CHGR_CTRL1 bits 5..0, with CC_EN cleared around the write.
 */
static int sc2721_set_ovp(struct sc2720_charger_info *info)
{
	u32 mv = info->ovp_uv / 1000;
	int ret;

	mv = clamp_t(u32, mv, SC2721_OVP_MIN_MV, SC2721_OVP_MAX_MV);

	ret = regmap_update_bits(info->regmap, info->base + SC2720_CHG_CFG0,
				 SC2720_CHG_CC_MODE, 0);
	if (ret)
		return ret;

	ret = regmap_update_bits(info->regmap, info->base + SC2720_CHG_CFG1,
				 SC2721_OVP_MASK,
				 (mv - SC2721_OVP_MIN_MV) / SC2721_OVP_STEP_MV);
	if (ret)
		return ret;

	return regmap_update_bits(info->regmap, info->base + SC2720_CHG_CFG0,
				  SC2720_CHG_CC_MODE, SC2720_CHG_CC_MODE);
}

static int sc2720_charger_hw_init(struct sc2720_charger_info *info)
{
	struct power_supply_battery_info bat_info = { };
	u32 voltage_max_microvolt;
	int ret;

	ret = power_supply_get_battery_info(info->psy_usb, &bat_info);
	if (ret) {
		dev_warn(info->dev, "no battery information is supplied\n");
		info->cur.sdp_cur = 500000;
		info->cur.dcp_cur = 500000;
		info->cur.cdp_cur = 1500000;
		info->cur.unknown_cur = 500000;
	} else {
		info->cur.sdp_cur = bat_info.cur.sdp_cur;
		info->cur.dcp_cur = bat_info.cur.dcp_cur;
		info->cur.cdp_cur = bat_info.cur.cdp_cur;
		info->cur.unknown_cur = bat_info.cur.unknown_cur;

		voltage_max_microvolt =
			bat_info.constant_charge_voltage_max_uv;
		power_supply_put_battery_info(info->psy_usb, &bat_info);

		if (info->data->is_sc2721) {
			mutex_lock(&sc2721_cv_lock);
			info->cv_vmax_mv = voltage_max_microvolt / 1000;
			mutex_unlock(&sc2721_cv_lock);
		}

		ret = regmap_update_bits(info->regmap,
					 info->base + SC2720_CHG_CFG0,
					 SC2720_CHG_CC_MODE,
					 SC2720_CHG_CC_MODE);
		if (ret) {
			dev_err(info->dev, "failed to enable charger cc\n");
			return ret;
		}

		if (info->data->is_sc2721) {
			/* the 4.4 SC2721 driver always programs DPM 3 */
			ret = regmap_update_bits(info->regmap,
						 info->base + SC2720_CHG_CFG0,
						 SC2720_CHG_DPM_MASK,
						 SC2721_CHG_DPM_3 <<
						 SC2720_CHG_DPM_MASK_SHIT);
			if (ret) {
				dev_err(info->dev, "failed to set charger dpm\n");
				return ret;
			}
		} else if (voltage_max_microvolt / 1000 < SC2720_CHG_DPM_4300MV) {
			ret = regmap_update_bits(info->regmap,
						 info->base + SC2720_CHG_CFG0,
						 SC2720_CHG_DPM_MASK,
						 0x2 << SC2720_CHG_DPM_MASK_SHIT);
			if (ret) {
				dev_err(info->dev, "failed to set charger dpm\n");
				return ret;
			}
		} else {
			ret = regmap_update_bits(info->regmap,
						 info->base + SC2720_CHG_CFG0,
						 SC2720_CHG_DPM_MASK,
						 0x3 << SC2720_CHG_DPM_MASK_SHIT);
			if (ret) {
				dev_err(info->dev, "failed to set charger dpm\n");
				return ret;
			}
		}

		/* Set charge termination voltage */
		ret = sc2720_set_termination_voltage(info,
						     voltage_max_microvolt);
		if (ret) {
			dev_err(info->dev, "failed to set termination voltage\n");
			return ret;
		}

		/*
		 * SC2721: the OVP level is board data, read from
		 * "sprd,ovp-microvolt". Without it the register keeps the
		 * value it has (current behaviour).
		 */
		if (info->data->is_sc2721) {
			if (!info->ovp_uv) {
				dev_warn_once(info->dev,
					      "no sprd,ovp-microvolt, input OVP register left as is\n");
				return 0;
			}

			ret = sc2721_set_ovp(info);
			if (ret)
				dev_err(info->dev, "failed to set charger ovp\n");
			return ret;
		}

		/* Set charge over voltage protection value 6500mv */
		ret = regmap_update_bits(info->regmap,
					 info->base + SC2720_CHG_CFG1,
					 SC2720_CHG_OVP_MASK,
					 0x01);
		if (ret) {
			dev_err(info->dev, "failed to set charger ovp\n");
			return ret;
		}
	}

	return ret;
}

static int sc2720_charger_start_charge(struct sc2720_charger_info *info)
{
	int ret;

	ret = regmap_update_bits(info->regmap,
				 info->base + SC2720_CHG_CFG0,
				 SC2720_CHG_PD, 0);
	if (ret)
		dev_err(info->dev, "failed to satrt charge\n");

	return ret;
}

static int sc2720_charger_stop_charge(struct sc2720_charger_info *info)
{
	int ret;

	ret = regmap_update_bits(info->regmap,
				 info->base + SC2720_CHG_CFG0,
				 SC2720_CHG_PD,
				 SC2720_CHG_PD);
	if (ret)
		dev_err(info->dev, "failed to stop charge\n");

	return ret;
}

/* charge current of the attached adapter type in uA */
static u32 sc2720_charger_adapter_cur(struct sc2720_charger_info *info)
{
	switch (READ_ONCE(info->usb_phy->chg_type)) {
	case SDP_TYPE:
		return info->cur.sdp_cur;
	case DCP_TYPE:
		return info->cur.dcp_cur;
	case CDP_TYPE:
		return info->cur.cdp_cur;
	default:
		return info->cur.unknown_cur;
	}
}

static int sc2720_charger_set_current(struct sc2720_charger_info *info, u32 cur)
{
	int temp, ret;

	/*
	 * SC2721: never charge faster than the adapter type allows, whatever
	 * the request (jeita, thermal, userspace), as the 4.4 driver capped
	 * every jeita current to the adapter current.
	 */
	if (info->data->is_sc2721) {
		info->cur_req_ua = cur;
		cur = min(cur, sc2720_charger_adapter_cur(info));
	}

	cur = cur / 1000;
	if (cur < SC2720_CHG_CURRENT_MIN)
		cur = SC2720_CHG_CURRENT_MIN;

	if (info->data->is_sc2721) {
		/* linear 50 mA codes from 300 to 1300 mA */
		if (cur > SC2721_CHG_CURRENT_MAX)
			cur = SC2721_CHG_CURRENT_MAX;
		temp = (cur - SC2720_CHG_CURRENT_MIN) /
			SC2720_CHG_CURRENT_STEP_50MA;
	} else if (cur < SC2720_CHG_CURRENT_800MA) {
		temp = (cur - SC2720_CHG_CURRENT_MIN) /
			SC2720_CHG_CURRENT_STEP_50MA;
	} else {
		temp = (cur - SC2720_CHG_CURRENT_800MA) /
			SC2720_CHG_CURRENT_STEP_100MA;
		temp += SC2720_CHG_CURRENT_800MA_REG;
	}

	/* Disable charge cc mode */
	ret = regmap_update_bits(info->regmap,
				 info->base + SC2720_CHG_CFG0,
				 SC2720_CHG_CC_MODE, 0);
	if (ret) {
		dev_err(info->dev, "failed to disable charge cc mode\n");
		return ret;
	}

	/* Set charge current */
	ret = regmap_update_bits(info->regmap,
				 info->base + SC2720_CHG_CFG1,
				 info->data->cc_i_mask,
				 temp << SC2720_CHG_CC_I_MASK_SHIT);
	if (ret) {
		dev_err(info->dev, "failed to set charge current\n");
		return ret;
	}

	/* Enable charge cc mode */
	ret = regmap_update_bits(info->regmap,
				 info->base + SC2720_CHG_CFG0,
				 SC2720_CHG_CC_MODE,
				 SC2720_CHG_CC_MODE);
	if (ret)
		dev_err(info->dev, "failed to enable charge cc mode\n");

	return ret;
}

static int sc2720_charger_get_current(struct sc2720_charger_info *info,
				      u32 *cur)
{
	int ret;
	u32 val;

	ret = regmap_read(info->regmap, info->base + SC2720_CHG_CFG0, &val);
	if (ret)
		return ret;

	if (!(val & SC2720_CHG_CC_MODE)) {
		*cur = 0;
		return 0;
	}

	ret = regmap_read(info->regmap, info->base + SC2720_CHG_CFG1, &val);
	if (ret)
		return ret;

	val = (val & info->data->cc_i_mask) >> SC2720_CHG_CC_I_MASK_SHIT;
	if (info->data->is_sc2721)
		*cur = SC2720_CHG_CURRENT_MIN +
			val * SC2720_CHG_CURRENT_STEP_50MA;
	else if (val >= SC2720_CHG_CURRENT_800MA_REG)
		*cur = SC2720_CHG_CURRENT_800MA +
			(val - SC2720_CHG_CURRENT_800MA_REG) *
			SC2720_CHG_CURRENT_STEP_100MA;
	else
		*cur = SC2720_CHG_CURRENT_MIN +
			val * SC2720_CHG_CURRENT_STEP_50MA;

	return 0;
}

static int sc2720_charger_get_health(struct sc2720_charger_info *info,
				     u32 *health)
{
	*health = POWER_SUPPLY_HEALTH_GOOD;

	return 0;
}

static int sc2720_charger_get_online(struct sc2720_charger_info *info,
				     u32 *online)
{
	if (info->limit)
		*online = true;
	else
		*online = false;

	return 0;
}

static int sc2720_charger_get_status(struct sc2720_charger_info *info)
{
	if (info->charging == true)
		return POWER_SUPPLY_STATUS_CHARGING;
	else
		return POWER_SUPPLY_STATUS_NOT_CHARGING;
}

static int sc2720_charger_set_status(struct sc2720_charger_info *info,
				       int val)
{
	int ret = 0;

	if (!val && info->charging) {
		sc2720_charger_stop_charge(info);
		info->charging = false;
	} else if (val && !info->charging) {
		ret = sc2720_charger_start_charge(info);
		if (ret)
			dev_err(info->dev, "start charge failed\n");
		else
			info->charging = true;
	}

	return ret;
}

static void sc2720_charger_work(struct work_struct *data)
{
	struct sc2720_charger_info *info =
		container_of(data, struct sc2720_charger_info, work);
	int cur, ret;
	bool present = sc2720_charger_is_bat_present(info);

	mutex_lock(&info->lock);

	if (info->limit > 0 && !info->charging && present) {
		/* set charger current and start to charge */
		switch (info->usb_phy->chg_type) {
		case SDP_TYPE:
			cur = info->cur.sdp_cur;
			break;
		case DCP_TYPE:
			cur = info->cur.dcp_cur;
			break;
		case CDP_TYPE:
			cur = info->cur.cdp_cur;
			break;
		default:
			cur = info->cur.unknown_cur;
		}

		ret = sc2720_charger_set_current(info, cur);
		if (ret)
			goto out;

		ret = sc2720_charger_start_charge(info);
		if (ret)
			goto out;

		info->charging = true;
	} else if (info->data->is_sc2721 && info->limit > 0 &&
		   info->charging && present) {
		/*
		 * The adapter type may have changed while charging: apply the
		 * last request again so the cap follows the current type.
		 */
		ret = sc2720_charger_set_current(info, info->cur_req_ua);
		if (ret)
			goto out;
	} else if ((!info->limit && info->charging) || !present) {
		/* Stop charging */
		info->charging = false;
		sc2720_charger_stop_charge(info);
	}

out:
	mutex_unlock(&info->lock);
	dev_info(info->dev, "battery present = %d, charger type = %d\n",
		 present, info->usb_phy->chg_type);
	cm_notify_event(info->psy_usb, CM_EVENT_CHG_START_STOP, NULL);
}

static int sc2720_charger_usb_change(struct notifier_block *nb,
				     unsigned long limit, void *data)
{
	struct sc2720_charger_info *info =
		container_of(nb, struct sc2720_charger_info, usb_notify);

	info->limit = limit;

	schedule_work(&info->work);
	return NOTIFY_OK;
}

static int sc2720_charger_usb_get_property(struct power_supply *psy,
					   enum power_supply_property psp,
					   union power_supply_propval *val)
{
	struct sc2720_charger_info *info = power_supply_get_drvdata(psy);
	u32 cur, online, health;
	enum usb_charger_type type;
	int ret = 0;

	mutex_lock(&info->lock);

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		if (info->limit)
			val->intval = sc2720_charger_get_status(info);
		else
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		break;

	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		if (!info->charging) {
			val->intval = 0;
		} else {
			ret = sc2720_charger_get_current(info, &cur);
			if (ret)
				goto out;

			val->intval = cur * 1000;
		}
		break;

	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		if (!info->charging) {
			val->intval = 0;
		} else {
			switch (info->usb_phy->chg_type) {
			case SDP_TYPE:
				val->intval = info->cur.sdp_cur;
				break;
			case DCP_TYPE:
				val->intval = info->cur.dcp_cur;
				break;
			case CDP_TYPE:
				val->intval = info->cur.cdp_cur;
				break;
			default:
				val->intval = info->cur.unknown_cur;
			}
		}
		break;

	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE:
		/* SC2721 only: CV held by the override in uV, 0 = not holding */
		if (!info->data->is_sc2721) {
			ret = -EINVAL;
			goto out;
		}

		val->intval = sc2721_cv_hold_uv(info);
		break;

	case POWER_SUPPLY_PROP_ONLINE:
		ret = sc2720_charger_get_online(info, &online);
		if (ret)
			goto out;

		val->intval = online;
		break;

	case POWER_SUPPLY_PROP_HEALTH:
		if (info->charging) {
			val->intval = 0;
		} else {
			ret = sc2720_charger_get_health(info, &health);
			if (ret)
				goto out;

			val->intval = health;
		}
		break;

	case POWER_SUPPLY_PROP_USB_TYPE:
		type = info->usb_phy->chg_type;

		switch (type) {
		case SDP_TYPE:
			val->intval = POWER_SUPPLY_USB_TYPE_SDP;
			break;

		case DCP_TYPE:
			val->intval = POWER_SUPPLY_USB_TYPE_DCP;
			break;

		case CDP_TYPE:
			val->intval = POWER_SUPPLY_USB_TYPE_CDP;
			break;

		default:
			val->intval = POWER_SUPPLY_USB_TYPE_UNKNOWN;
		}
		break;


	default:
		ret = -EINVAL;
	}

out:
	mutex_unlock(&info->lock);
	return ret;
}

static int
sc2720_charger_usb_set_property(struct power_supply *psy,
				enum power_supply_property psp,
				const union power_supply_propval *val)
{
	struct sc2720_charger_info *info = power_supply_get_drvdata(psy);
	int ret;

	mutex_lock(&info->lock);

	if (!info->charging && psp != POWER_SUPPLY_PROP_STATUS) {
		mutex_unlock(&info->lock);
		return 0;
	}

	switch (psp) {
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		ret = sc2720_charger_set_current(info, val->intval);
		if (ret < 0)
			dev_err(info->dev, "set charge current failed\n");
		break;

	case POWER_SUPPLY_PROP_STATUS:
		ret = sc2720_charger_set_status(info, val->intval);
		if (ret < 0)
			dev_err(info->dev, "set charge status failed\n");
		break;

	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX:
		ret = sc2720_set_termination_voltage(info, val->intval);
		if (ret < 0)
			dev_err(info->dev, "failed to set terminate voltage\n");
		break;

	default:
		ret = -EINVAL;
	}

	mutex_unlock(&info->lock);
	return ret;
}

static int sc2720_charger_property_is_writeable(struct power_supply *psy,
						enum power_supply_property psp)
{
	int ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX:
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
	case POWER_SUPPLY_PROP_STATUS:
		ret = 1;
		break;

	default:
		ret = 0;
	}

	return ret;
}

static enum power_supply_usb_type sc2720_charger_usb_types[] = {
	POWER_SUPPLY_USB_TYPE_UNKNOWN,
	POWER_SUPPLY_USB_TYPE_SDP,
	POWER_SUPPLY_USB_TYPE_DCP,
	POWER_SUPPLY_USB_TYPE_CDP,
	POWER_SUPPLY_USB_TYPE_C,
	POWER_SUPPLY_USB_TYPE_PD,
	POWER_SUPPLY_USB_TYPE_PD_DRP,
	POWER_SUPPLY_USB_TYPE_APPLE_BRICK_ID
};

static enum power_supply_property sc2720_usb_props[] = {
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
	POWER_SUPPLY_PROP_USB_TYPE,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_HEALTH,
};

static enum power_supply_property sc2721_usb_props[] = {
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
	POWER_SUPPLY_PROP_USB_TYPE,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_HEALTH,
};

static const struct power_supply_desc sc2720_charger_desc = {
	.name			= "sc2720_charger",
	.type			= POWER_SUPPLY_TYPE_USB,
	.properties		= sc2720_usb_props,
	.num_properties		= ARRAY_SIZE(sc2720_usb_props),
	.get_property		= sc2720_charger_usb_get_property,
	.set_property		= sc2720_charger_usb_set_property,
	.property_is_writeable	= sc2720_charger_property_is_writeable,
	.usb_types		= sc2720_charger_usb_types,
	.num_usb_types		= ARRAY_SIZE(sc2720_charger_usb_types)
};

static const struct power_supply_desc sc2721_charger_desc = {
	.name			= "sc2721_charger",
	.type			= POWER_SUPPLY_TYPE_USB,
	.properties		= sc2721_usb_props,
	.num_properties		= ARRAY_SIZE(sc2721_usb_props),
	.get_property		= sc2720_charger_usb_get_property,
	.set_property		= sc2720_charger_usb_set_property,
	.property_is_writeable	= sc2720_charger_property_is_writeable,
	.usb_types		= sc2720_charger_usb_types,
	.num_usb_types		= ARRAY_SIZE(sc2720_charger_usb_types)
};

static const struct sc2720_charger_data sc2720_charger_data = {
	.desc		= &sc2720_charger_desc,
	.cc_i_mask	= SC2720_CHG_CC_I_MASK,
};

static const struct sc2720_charger_data sc2721_charger_data = {
	.desc		= &sc2721_charger_desc,
	.cc_i_mask	= SC2721_CHG_CC_I_MASK,
	.is_sc2721	= true,
};

static void sc2720_charger_detect_status(struct sc2720_charger_info *info)
{
	unsigned int min, max;

	/*
	 * If the USB charger status has been USB_CHARGER_PRESENT before
	 * registering the notifier, we should start to charge with getting
	 * the charge current.
	 */
	if (info->usb_phy->chg_state != USB_CHARGER_PRESENT)
		return;

	usb_phy_get_charger_current(info->usb_phy, &min, &max);

	info->limit = min;
	schedule_work(&info->work);
}

/* drop the CV override ownership before info goes away */
static void sc2721_cv_release(struct sc2720_charger_info *info)
{
	mutex_lock(&sc2721_cv_lock);
	if (sc2721_cv_info == info)
		sc2721_cv_info = NULL;
	mutex_unlock(&sc2721_cv_lock);
}

static int sc2720_charger_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct sc2720_charger_info *info;
	struct power_supply_config charger_cfg = { };
	unsigned int cv_e, cv_c;
	int ret;

	info = devm_kzalloc(&pdev->dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	info->data = of_device_get_match_data(&pdev->dev);
	if (!info->data)
		return -ENODEV;

	mutex_init(&info->lock);
	info->dev = &pdev->dev;
	INIT_WORK(&info->work, sc2720_charger_work);

	info->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!info->regmap) {
		dev_err(&pdev->dev, "failed to get charger regmap\n");
		return -ENODEV;
	}

	platform_set_drvdata(pdev, info);

	info->usb_phy = devm_usb_get_phy_by_phandle(&pdev->dev, "phys", 0);
	if (IS_ERR(info->usb_phy)) {
		dev_err(&pdev->dev, "failed to find USB phy\n");
		return PTR_ERR(info->usb_phy);
	}

	ret = of_property_read_u32(np, "reg", &info->base);
	if (ret) {
		dev_err(&pdev->dev, "failed to get register address\n");
		return -ENODEV;
	}

	if (info->data->is_sc2721)
		of_property_read_u32(np, "sprd,ovp-microvolt", &info->ovp_uv);

	info->usb_notify.notifier_call = sc2720_charger_usb_change;
	ret = usb_register_notifier(info->usb_phy, &info->usb_notify);
	if (ret) {
		dev_err(&pdev->dev, "failed to register notifier:%d\n", ret);
		return ret;
	}

	charger_cfg.drv_data = info;
	charger_cfg.of_node = np;
	info->psy_usb = devm_power_supply_register(&pdev->dev,
						   info->data->desc,
						   &charger_cfg);
	if (IS_ERR(info->psy_usb)) {
		dev_err(&pdev->dev, "failed to register power supply\n");
		usb_unregister_notifier(info->usb_phy, &info->usb_notify);
		return PTR_ERR(info->psy_usb);
	}

	if (info->data->is_sc2721) {
		ret = sc2721_read_calib(info);
		if (ret) {
			dev_err(&pdev->dev, "failed to read cccv calibration: %d\n",
				ret);
			usb_unregister_notifier(info->usb_phy, &info->usb_notify);
			return ret;
		}

		mutex_lock(&sc2721_cv_lock);
		if (!sc2721_cv_info) {
			sc2721_cv_info = info;
			if (sc2721_cv_override_mv &&
			    !sc2721_cv_floor(sc2721_cv_override_mv, info->cv_cal,
					     &cv_e, &cv_c))
				dev_warn(&pdev->dev,
					 "CV override %u mV unreachable, lowest CV is %d mV\n",
					 sc2721_cv_override_mv,
					 sc2721_cv_v10(0, 0, info->cv_cal) / 10);
		} else {
			dev_warn(&pdev->dev,
				 "second SC2721 charger, CV override not applied to it\n");
		}
		mutex_unlock(&sc2721_cv_lock);
	}

	ret = sc2720_charger_hw_init(info);
	if (ret) {
		sc2721_cv_release(info);
		usb_unregister_notifier(info->usb_phy, &info->usb_notify);
		return ret;
	}

	sc2720_charger_detect_status(info);

	return 0;
}

static int sc2720_charger_remove(struct platform_device *pdev)
{
	struct sc2720_charger_info *info = platform_get_drvdata(pdev);

	sc2721_cv_release(info);
	usb_unregister_notifier(info->usb_phy, &info->usb_notify);

	return 0;
}

static const struct of_device_id sc2720_charger_of_match[] = {
	{ .compatible = "sprd,sc2720-charger", .data = &sc2720_charger_data },
	{ .compatible = "sprd,sc2721-charger", .data = &sc2721_charger_data },
	{ }
};

static struct platform_driver sc2720_charger_driver = {
	.driver = {
		.name = "sc2720-charger",
		.of_match_table = sc2720_charger_of_match,
	},
	.probe = sc2720_charger_probe,
	.remove = sc2720_charger_remove,
};

module_platform_driver(sc2720_charger_driver);

MODULE_DESCRIPTION("Spreadtrum SC2720/SC2721 Charger Driver");
MODULE_LICENSE("GPL v2");
