/*
 *  max77803_charger.c
 *  Samsung max77803 Charger Driver
 *
 *  Copyright (C) 2012 Samsung Electronics
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/mfd/max77803.h>
#include <linux/mfd/max77803-private.h>
#ifdef CONFIG_USB_HOST_NOTIFY
#include <linux/host_notify.h>
#include <mach/usb3-drd.h>
#endif 

#define ENABLE 1
#define DISABLE 0

#define RECOVERY_DELAY		3000
#define RECOVERY_CNT		5
#define REDUCE_CURRENT_STEP	100 /* Fix noise rate (min = 100 , max = 700) */
#define MINIMUM_INPUT_CURRENT	400 /* Maximum amount allowed in usb2 (usb2 output = 500 mA) */

int SIOP_INPUT_LIMIT_CURRENT = 2100; /* Maximum amount allowed in Device (max = 2100 mA, if increase max => cpu very Damaged! ) */
int SIOP_CHARGING_LIMIT_CURRENT = 1900; /* Maximum Stable Charging allowed in Device (max = 2000mA, usually if screen off, else max = 1300mA)*/


struct max77803_charger_data {
	struct max77803_dev	*max77803;

	struct power_supply	psy_chg;

	struct workqueue_struct *wqueue;
	struct work_struct	chgin_work;
	struct delayed_work	isr_work;
	struct delayed_work	recovery_work;	/*  softreg recovery work */
	struct delayed_work	wpc_work;	/*  wpc detect work */
	struct delayed_work	chgin_init_work;	/*  chgin init work */

	/* mutex */
	struct mutex irq_lock;
	struct mutex ops_lock;

	/* wakelock */
	struct wake_lock recovery_wake_lock;
	struct wake_lock wpc_wake_lock;
	struct wake_lock chgin_wake_lock;

	unsigned int	is_charging;
	unsigned int	charging_type;
	unsigned int	battery_state;
	unsigned int	battery_present;
	unsigned int	cable_type;
	unsigned int	charging_current_max;
	unsigned int	charging_current;
	unsigned int	input_current_limit;
	unsigned int	vbus_state;
	int		status;
	int		siop_level;
	int uvlo_attach_flag;
	int uvlo_attach_cable_type;

	int		irq_bypass;
#if defined(CONFIG_CHARGER_MAX77803)
	int		irq_batp;
#else
	int		irq_therm;
#endif
	int		irq_battery;
	int		irq_chg;
#if defined(CONFIG_CHARGER_MAX77803)
	int		irq_wcin;
#endif
	int		irq_chgin;

	/* software regulation */
	bool		soft_reg_state;
	int		soft_reg_current;

	/* unsufficient power */
	bool		reg_loop_deted;

#if defined(CONFIG_CHARGER_MAX77803)
	/* wireless charge, w(wpc), v(vbus) */
	int		wc_w_gpio;
	int		wc_w_irq;
	int		wc_w_state;
	int		wc_v_gpio;
	int		wc_v_irq;
	int		wc_v_state;
	bool		wc_pwr_det;
#endif
	int		soft_reg_recovery_cnt;

	bool is_mdock;
	bool is_otg;
	int pmic_ver;
	int input_curr_limit_step;
	int wpc_input_curr_limit_step;
	int charging_curr_step;

	sec_battery_platform_data_t	*pdata;
};

static enum power_supply_property sec_charger_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_CURRENT_AVG,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_POWER_NOW,
#if defined(CONFIG_BATTERY_SWELLING)
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
#endif
};

static void max77803_charger_initialize(struct max77803_charger_data *charger);
static int max77803_get_vbus_state(struct max77803_charger_data *charger);
static int max77803_get_charger_state(struct max77803_charger_data *charger);
static void max77803_dump_reg(struct max77803_charger_data *charger)
{
	u8 reg_data;
	u32 reg_addr;
	pr_info("%s\n", __func__);

	for (reg_addr = 0xB0; reg_addr <= 0xC5; reg_addr++) {
		max77803_read_reg(charger->max77803->i2c, reg_addr, &reg_data);
		pr_info("max77803: c: 0x%02x(0x%02x)\n", reg_addr, reg_data);
	}
}

static int max77803_get_battery_present(struct max77803_charger_data *charger)
{
	u8 reg_data;

	if (max77803_read_reg(charger->max77803->i2c,
			MAX77803_CHG_REG_CHG_INT_OK, &reg_data) < 0) {
		/* Eventhough there is an error,
		   don't do power-off */
		return 1;
	}

	pr_debug("%s: CHG_INT_OK(0x%02x)\n", __func__, reg_data);

	reg_data = ((reg_data & MAX77803_BATP_OK) >> MAX77803_BATP_OK_SHIFT);

	return reg_data;
}

static void max77803_set_buck(struct max77803_charger_data *charger, int enable);
static void max77803_set_charger_state(struct max77803_charger_data *charger,
		int enable)
{
	u8 reg_data;
#if defined(CONFIG_SW_SELF_DISCHARGING)
	union power_supply_propval sdchg_state;
#endif

	max77803_read_reg(charger->max77803->i2c,
			MAX77803_CHG_REG_CHG_CNFG_00, &reg_data);

	if (enable)
		reg_data |= MAX77803_MODE_CHGR;
	else
		reg_data &= ~MAX77803_MODE_CHGR;

	pr_debug("%s: CHG_CNFG_00(0x%02x)\n", __func__, reg_data);
	max77803_write_reg(charger->max77803->i2c,
			MAX77803_CHG_REG_CHG_CNFG_00, reg_data);
#if defined(CONFIG_SW_SELF_DISCHARGING)
	psy_do_property("battery", get,
		POWER_SUPPLY_PROP_POWER_AVG, sdchg_state);
	if(sdchg_state.intval && !charger->is_charging)
		max77803_set_buck(charger, DISABLE);
#endif
}

static void max77803_set_buck(struct max77803_charger_data *charger,
		int enable)
{
	u8 reg_data;

	max77803_read_reg(charger->max77803->i2c,
		MAX77803_CHG_REG_CHG_CNFG_00, &reg_data);

	if (enable)
		reg_data |= MAX77803_MODE_BUCK;
	else
		reg_data &= ~MAX77803_MODE_BUCK;

	pr_debug("%s: CHG_CNFG_00(0x%02x)\n", __func__, reg_data);
	max77803_write_reg(charger->max77803->i2c,
		MAX77803_CHG_REG_CHG_CNFG_00, reg_data);
}

static void max77803_set_input_current(struct max77803_charger_data *charger,
		int cur)
{
	int set_current_reg, now_current_reg;
	int vbus_state, curr_step, delay;
	u8 set_reg, reg_data;
	int chg_state;

	mutex_lock(&charger->ops_lock);
	max77803_read_reg(charger->max77803->i2c,
		MAX77803_CHG_REG_CHG_INT_MASK, &reg_data);
	reg_data |= (1 << 6);
	max77803_write_reg(charger->max77803->i2c,
		MAX77803_CHG_REG_CHG_INT_MASK, reg_data);


	if (charger->cable_type == POWER_SUPPLY_TYPE_WPC) {
		set_reg = MAX77803_CHG_REG_CHG_CNFG_10;
		set_current_reg = cur / charger->wpc_input_curr_limit_step;
	} else {
		set_reg = MAX77803_CHG_REG_CHG_CNFG_09;
		set_current_reg = cur / charger->input_curr_limit_step;
	}

	if (cur <= 0) {
		max77803_write_reg(charger->max77803->i2c,
			set_reg, 0);
		max77803_set_buck(charger, DISABLE);
		goto exit;
	} else
		max77803_set_buck(charger, ENABLE);

	if (charger->cable_type == POWER_SUPPLY_TYPE_BATTERY)
		goto set_input_current;

	max77803_read_reg(charger->max77803->i2c,
		set_reg, &reg_data);
	if (reg_data == set_current_reg) {
		/* check uvlo  */
		while((set_current_reg > (MINIMUM_INPUT_CURRENT / charger->input_curr_limit_step)) && (set_current_reg < 255)) {
			vbus_state = max77803_get_vbus_state(charger);
			if (((vbus_state == 0x00) || (vbus_state == 0x01)) &&
				(charger->cable_type != POWER_SUPPLY_TYPE_WPC)) {
				/* UVLO */
				set_current_reg -= 5;
				if (set_current_reg < (MINIMUM_INPUT_CURRENT / charger->input_curr_limit_step))
					set_current_reg = (MINIMUM_INPUT_CURRENT / charger->input_curr_limit_step);
				max77803_write_reg(charger->max77803->i2c,
						set_reg, set_current_reg);
				pr_info("%s: set_current_reg(0x%02x)\n", __func__, set_current_reg);
				chg_state = max77803_get_charger_state(charger);
				if ((chg_state != POWER_SUPPLY_STATUS_CHARGING) &&
						(chg_state != POWER_SUPPLY_STATUS_FULL))
					break;
				msleep(50);
			} else
				break;
		}
		goto exit;
	}

	if (reg_data == 0) {
		if (charger->cable_type == POWER_SUPPLY_TYPE_WPC)
			now_current_reg = SOFT_CHG_START_CURR / charger->wpc_input_curr_limit_step;
		else
		now_current_reg = SOFT_CHG_START_CURR / charger->input_curr_limit_step;
		max77803_write_reg(charger->max77803->i2c,
			set_reg, now_current_reg);
		msleep(SOFT_CHG_START_DUR);
	} else
		now_current_reg = reg_data;

	if (cur <= 1000) {
		curr_step = 1;
		delay = 50;
	} else {
		if (charger->cable_type == POWER_SUPPLY_TYPE_WPC)
			curr_step = SOFT_CHG_START_CURR / charger->wpc_input_curr_limit_step;
		else
			curr_step = SOFT_CHG_START_CURR / charger->input_curr_limit_step;
		delay = SOFT_CHG_STEP_DUR;
	}
	now_current_reg += (curr_step);

	while (now_current_reg < set_current_reg &&
			charger->cable_type != POWER_SUPPLY_TYPE_BATTERY)
	{
		now_current_reg = min(now_current_reg, set_current_reg);
		max77803_write_reg(charger->max77803->i2c,
			set_reg, now_current_reg);
		msleep(delay);

		vbus_state = max77803_get_vbus_state(charger);
		if (((vbus_state == 0x00) || (vbus_state == 0x01)) &&
			!(charger->cable_type == POWER_SUPPLY_TYPE_WPC)) {
			/* UVLO */
			if (now_current_reg > (curr_step * 3))
				now_current_reg -= (curr_step * 3);
			/* current limit 300mA */
			if (now_current_reg < (MINIMUM_INPUT_CURRENT / charger->input_curr_limit_step))
				now_current_reg = (MINIMUM_INPUT_CURRENT / charger->input_curr_limit_step);
			curr_step /= 2;
			max77803_write_reg(charger->max77803->i2c,
					set_reg, now_current_reg);
			pr_info("%s: now_current_reg(0x%02x)\n", __func__, now_current_reg);
			chg_state = max77803_get_charger_state(charger);
			if ((chg_state != POWER_SUPPLY_STATUS_CHARGING) &&
					(chg_state != POWER_SUPPLY_STATUS_FULL))
				goto exit;
			if (curr_step < 2)
				goto exit;
			msleep(50);
		} else
			now_current_reg += (curr_step);
	}

set_input_current:
	pr_info("%s: reg_data(0x%02x), input(%d)\n",
		__func__, set_current_reg, cur);
	max77803_write_reg(charger->max77803->i2c,
		set_reg, set_current_reg);
exit:
	max77803_read_reg(charger->max77803->i2c,
		MAX77803_CHG_REG_CHG_INT_MASK, &reg_data);
	reg_data &= ~(1 << 6);
	max77803_write_reg(charger->max77803->i2c,
		MAX77803_CHG_REG_CHG_INT_MASK, reg_data);
	mutex_unlock(&charger->ops_lock);
}

static int max77803_get_input_current(struct max77803_charger_data *charger)
{
	u8 reg_data;
	int get_current = 0;

	if (charger->cable_type == POWER_SUPPLY_TYPE_WPC) {
		max77803_read_reg(charger->max77803->i2c,
			MAX77803_CHG_REG_CHG_CNFG_10, &reg_data);
		pr_info("%s: CHG_CNFG_10(0x%02x)\n", __func__, reg_data);
		get_current = reg_data * charger->wpc_input_curr_limit_step;
	} else {
		max77803_read_reg(charger->max77803->i2c,
			MAX77803_CHG_REG_CHG_CNFG_09, &reg_data);
		get_current = reg_data * charger->input_curr_limit_step;
		pr_info("%s: CHG_CNFG_09(0x%02x)\n", __func__, reg_data);
	}

	pr_debug("%s: get input current: %dmA\n", __func__, get_current);
	return get_current;
}

static void max77803_set_topoff_current(struct max77803_charger_data *charger,
		int cur, int timeout)
{
	u8 reg_data;

	if (cur >= 350)
		reg_data = 0x07;
	else if (cur >= 300)
		reg_data = 0x06;
	else if (cur >= 250)
		reg_data = 0x05;
	else if (cur >= 200)
		reg_data = 0x04;
	else if (cur >= 175)
		reg_data = 0x03;
	else if (cur >= 150)
		reg_data = 0x02;
	else if (cur >= 125)
		reg_data = 0x01;
	else
		reg_data = 0x00;

#if 0
	/* the unit of timeout is second*/
	timeout = timeout / 60;
	reg_data |= ((timeout / 10) << 3);
#else
	/* set top off timer to max(70 min): cut off will be done by kernel timer */
	reg_data |= (0x7 << 3);
#endif
	pr_info("%s: reg_data(0x%02x), topoff(%d), back-charging time(%d sec)\n", __func__, reg_data, cur, timeout);

	max77803_write_reg(charger->max77803->i2c,
		MAX77803_CHG_REG_CHG_CNFG_03, reg_data);
}

static void max77803_set_charge_current(struct max77803_charger_data *charger,
		int cur)
{
	u8 reg_data = 0;

	max77803_read_reg(charger->max77803->i2c,
		MAX77803_CHG_REG_CHG_CNFG_02, &reg_data);
	reg_data &= ~MAX77803_CHG_CC;

	if (!cur) {
		/* No charger */
		max77803_write_reg(charger->max77803->i2c,
			MAX77803_CHG_REG_CHG_CNFG_02, reg_data);
	} else {
#if defined(max77888_charger)
		reg_data |= ((cur / 40) << 0);
#else
		reg_data |= ((cur * 10 / charger->charging_curr_step) << 0);
#endif
		max77803_write_reg(charger->max77803->i2c,
				MAX77803_CHG_REG_CHG_CNFG_02, reg_data);
	}
	pr_info("%s: reg_data(0x%02x), charge(%d)\n",
		__func__, reg_data, cur);
}


static int max77803_get_charge_current(struct max77803_charger_data *charger)
{
	u8 reg_data;
	int get_current = 0;

	max77803_read_reg(charger->max77803->i2c,
		MAX77803_CHG_REG_CHG_CNFG_02, &reg_data);
	pr_debug("%s: CHG_CNFG_02(0x%02x)\n", __func__, reg_data);

	reg_data &= MAX77803_CHG_CC;
#if defined(max77888_charger)
	get_current = reg_data * 40;
#else
	get_current = reg_data * 333 / 10;
#endif
	pr_debug("%s: get charge current: %dmA\n", __func__, get_current);
	return get_current;
}


/* in soft regulation, current recovery operation */
static void max77803_recovery_work(struct work_struct *work)
{
	struct max77803_charger_data *charger = container_of(work,
						struct max77803_charger_data,
						recovery_work.work);
	u8 dtls_00, chgin_dtls;
	u8 dtls_01, chg_dtls;
	u8 dtls_02, byp_dtls;
	pr_debug("%s\n", __func__);

	wake_unlock(&charger->recovery_wake_lock);
	if ((!charger->is_charging) || mutex_is_locked(&charger->ops_lock) ||
			(charger->cable_type != POWER_SUPPLY_TYPE_MAINS))
		return;
	max77803_read_reg(charger->max77803->i2c,
				MAX77803_CHG_REG_CHG_DTLS_00, &dtls_00);
	max77803_read_reg(charger->max77803->i2c,
				MAX77803_CHG_REG_CHG_DTLS_01, &dtls_01);
	max77803_read_reg(charger->max77803->i2c,
				MAX77803_CHG_REG_CHG_DTLS_02, &dtls_02);

	chgin_dtls = ((dtls_00 & MAX77803_CHGIN_DTLS) >>
				MAX77803_CHGIN_DTLS_SHIFT);
	chg_dtls = ((dtls_01 & MAX77803_CHG_DTLS) >>
				MAX77803_CHG_DTLS_SHIFT);
	byp_dtls = ((dtls_02 & MAX77803_BYP_DTLS) >>
				MAX77803_BYP_DTLS_SHIFT);

	if ((charger->soft_reg_recovery_cnt < RECOVERY_CNT) && (
		(chgin_dtls == 0x3) && (chg_dtls != 0x8) && (byp_dtls == 0x0))) {
		pr_info("%s: try to recovery, cnt(%d)\n", __func__,
				(charger->soft_reg_recovery_cnt + 1));

		if (charger->siop_level < 100 &&
			charger->cable_type == POWER_SUPPLY_TYPE_MAINS &&
 			charger->charging_current_max > SIOP_INPUT_LIMIT_CURRENT) {
			pr_info("%s : LCD on status and revocer current\n", __func__);
			max77803_set_input_current(charger,
					SIOP_INPUT_LIMIT_CURRENT);
		} else {
			max77803_set_input_current(charger,
				charger->charging_current_max);
		}
	} else {
		pr_info("%s: fail to recovery, cnt(%d)\n", __func__,
				(charger->soft_reg_recovery_cnt + 1));

		pr_info("%s:  CHGIN(0x%x), CHG(0x%x), BYP(0x%x)\n",
				__func__, chgin_dtls, chg_dtls, byp_dtls);

		/* schedule softreg recovery wq */
		if (charger->soft_reg_recovery_cnt < RECOVERY_CNT) {
			wake_lock(&charger->recovery_wake_lock);
			queue_delayed_work(charger->wqueue, &charger->recovery_work,
				msecs_to_jiffies(RECOVERY_DELAY));
		} else {
			pr_info("%s: recovery cnt(%d) is over\n",
				__func__, RECOVERY_CNT);
		}
	}

	/* add recovery try count */
	charger->soft_reg_recovery_cnt++;
}

static void reduce_input_current(struct max77803_charger_data *charger, int cur)
{
	u8 set_reg;
	u8 set_value;
	unsigned int min_input_current = 0;

	if ((!charger->is_charging) || mutex_is_locked(&charger->ops_lock) ||
			(charger->cable_type == POWER_SUPPLY_TYPE_WPC))
		return;
	set_reg = MAX77803_CHG_REG_CHG_CNFG_09;
	min_input_current = MINIMUM_INPUT_CURRENT;

	if (!max77803_read_reg(charger->max77803->i2c,
				set_reg, &set_value)) {
		if ((set_value <= (min_input_current / charger->input_curr_limit_step)) ||
		    (set_value <= (cur / charger->input_curr_limit_step)))
			return;
		set_value -= (cur / charger->input_curr_limit_step);
		set_value = (set_value < (min_input_current / charger->input_curr_limit_step)) ?
			(min_input_current / charger->input_curr_limit_step) : set_value;
		max77803_write_reg(charger->max77803->i2c,
				set_reg, set_value);
		pr_info("%s: set current: reg:(0x%x), val:(0x%x)\n",
				__func__, set_reg, set_value);
	}
	if(charger->cable_type == POWER_SUPPLY_TYPE_MAINS) {
		/* schedule softreg recovery wq */
		cancel_delayed_work_sync(&charger->recovery_work);
		wake_lock(&charger->recovery_wake_lock);
		queue_delayed_work(charger->wqueue, &charger->recovery_work,
				msecs_to_jiffies(RECOVERY_DELAY));
	}
}

static int max77803_get_vbus_state(struct max77803_charger_data *charger)
{
	u8 reg_data;

	max77803_read_reg(charger->max77803->i2c,
		MAX77803_CHG_REG_CHG_DTLS_00, &reg_data);
	if (charger->cable_type == POWER_SUPPLY_TYPE_WPC)
		reg_data = ((reg_data & MAX77803_WCIN_DTLS) >>
			MAX77803_WCIN_DTLS_SHIFT);
	else
		reg_data = ((reg_data & MAX77803_CHGIN_DTLS) >>
			MAX77803_CHGIN_DTLS_SHIFT);

	switch (reg_data) {
	case 0x00:
		pr_info("%s: VBUS is invalid. CHGIN < CHGIN_UVLO\n",
			__func__);
		break;
	case 0x01:
		pr_info("%s: VBUS is invalid. CHGIN < MBAT+CHGIN2SYS" \
			"and CHGIN > CHGIN_UVLO\n", __func__);
		break;
	case 0x02:
		pr_info("%s: VBUS is invalid. CHGIN > CHGIN_OVLO",
			__func__);
		break;
	case 0x03:
		pr_info("%s: VBUS is valid. CHGIN < CHGIN_OVLO", __func__);
		break;
	default:
		break;
	}

	return reg_data;
}

static int max77803_get_charger_state(struct max77803_charger_data *charger)
{
	int state;
	u8 reg_data;

	max77803_read_reg(charger->max77803->i2c,
		MAX77803_CHG_REG_CHG_DTLS_01, &reg_data);
	reg_data = ((reg_data & MAX77803_CHG_DTLS) >> MAX77803_CHG_DTLS_SHIFT);
	pr_info("%s: CHG_DTLS : 0x%2x\n", __func__, reg_data);

	switch (reg_data) {
	case 0x0:
	case 0x1:
	case 0x2:
		state = POWER_SUPPLY_STATUS_CHARGING;
		break;
	case 0x3:
	case 0x4:
		state = POWER_SUPPLY_STATUS_FULL;
		break;
	case 0x5:
	case 0x6:
	case 0x7:
		state = POWER_SUPPLY_STATUS_NOT_CHARGING;
		break;
	case 0x8:
	case 0xA:
	case 0xB:
		state = POWER_SUPPLY_STATUS_DISCHARGING;
		break;
	default:
		state = POWER_SUPPLY_STATUS_UNKNOWN;
		break;
	}

	return state;
}

static int max77803_get_health_state(struct max77803_charger_data *charger)
{
	int state;
	int vbus_state;
	u8 chg_dtls_00, chg_dtls_01, chg_dtls, reg_data;
	u8 chg_cnfg_00, chg_cnfg_01 ,chg_cnfg_02, chg_cnfg_04, chg_cnfg_09, chg_cnfg_12;
#if defined(CONFIG_CHAGALL)
	/* watchdog clear */
	max77803_update_reg(charger->max77803->i2c, MAX77803_CHG_REG_CHG_CNFG_06,
				MAX77803_WDTCLR, MAX77803_WDTCLR);
#endif
	max77803_read_reg(charger->max77803->i2c,
		MAX77803_CHG_REG_CHG_DTLS_01, &reg_data);
	reg_data = ((reg_data & MAX77803_BAT_DTLS) >> MAX77803_BAT_DTLS_SHIFT);

	switch (reg_data) {
	case 0x00:
		pr_info("%s: No battery and the charger is suspended\n",
			__func__);
		state = POWER_SUPPLY_HEALTH_UNSPEC_FAILURE;
		break;
	case 0x01:
		pr_info("%s: battery is okay "
			"but its voltage is low(~VPQLB)\n", __func__);
		state = POWER_SUPPLY_HEALTH_GOOD;
		break;
	case 0x02:
		pr_info("%s: battery dead\n", __func__);
		state = POWER_SUPPLY_HEALTH_DEAD;
		break;
	case 0x03:
		state = POWER_SUPPLY_HEALTH_GOOD;
		break;
	case 0x04:
		pr_info("%s: battery is okay" \
			"but its voltage is low\n", __func__);
		state = POWER_SUPPLY_HEALTH_GOOD;
		break;
	case 0x05:
		pr_info("%s: battery ovp\n", __func__);
		state = POWER_SUPPLY_HEALTH_OVERVOLTAGE;
		break;
	default:
		pr_info("%s: battery unknown : 0x%d\n", __func__, reg_data);
		state = POWER_SUPPLY_HEALTH_UNKNOWN;
		break;
	}

	pr_info("%s: CHG_DTLS(0x%x), \n", __func__, reg_data);

	/* VBUS OVP state return battery OVP state */
	vbus_state = max77803_get_vbus_state(charger);

	if (state == POWER_SUPPLY_HEALTH_GOOD) {
		union power_supply_propval value;
		psy_do_property("battery", get,
				POWER_SUPPLY_PROP_HEALTH, value);
		max77803_read_reg(charger->max77803->i2c,
				MAX77803_CHG_REG_CHG_DTLS_01, &chg_dtls);
		chg_dtls = ((chg_dtls & MAX77803_CHG_DTLS) >>
				MAX77803_CHG_DTLS_SHIFT);
		max77803_read_reg(charger->max77803->i2c,
				MAX77803_CHG_REG_CHG_CNFG_00, &chg_cnfg_00);

		/* print the log at the abnormal case */
		if((charger->is_charging == 1) && (chg_dtls & 0x08)) {
			max77803_read_reg(charger->max77803->i2c,
				MAX77803_CHG_REG_CHG_DTLS_00, &chg_dtls_00);
			max77803_read_reg(charger->max77803->i2c,
				MAX77803_CHG_REG_CHG_DTLS_01, &chg_dtls_01);
			max77803_read_reg(charger->max77803->i2c,
				MAX77803_CHG_REG_CHG_CNFG_01, &chg_cnfg_01);
			max77803_read_reg(charger->max77803->i2c,
				MAX77803_CHG_REG_CHG_CNFG_02, &chg_cnfg_02);
			max77803_read_reg(charger->max77803->i2c,
				MAX77803_CHG_REG_CHG_CNFG_04, &chg_cnfg_04);
			max77803_read_reg(charger->max77803->i2c,
				MAX77803_CHG_REG_CHG_CNFG_09, &chg_cnfg_09);
			max77803_read_reg(charger->max77803->i2c,
				MAX77803_CHG_REG_CHG_CNFG_12, &chg_cnfg_12);

			pr_info("%s: CHG_DTLS_00(0x%x), CHG_DTLS_01(0x%x), CHG_CNFG_00(0x%x)\n",
					__func__, chg_dtls_00, chg_dtls_01, chg_cnfg_00);
			pr_info("%s:  CHG_CNFG_01(0x%x), CHG_CNFG_02(0x%x), CHG_CNFG_04(0x%x)\n",
					__func__, chg_cnfg_01, chg_cnfg_02, chg_cnfg_04);
			pr_info("%s:  CHG_CNFG_09(0x%x), CHG_CNFG_12(0x%x)\n",
					__func__, chg_cnfg_09, chg_cnfg_12);
		}

		/* OVP is higher priority */
		if (vbus_state == 0x02) { /* CHGIN_OVLO */
			pr_info("%s: vbus ovp\n", __func__);
			state = POWER_SUPPLY_HEALTH_OVERVOLTAGE;
		} else if (((vbus_state == 0x0) || (vbus_state == 0x01)) &&(chg_dtls & 0x08) && \
				(chg_cnfg_00 & MAX77803_MODE_BUCK) && \
				(chg_cnfg_00 & MAX77803_MODE_CHGR) && \
				(charger->cable_type != POWER_SUPPLY_TYPE_WPC)) {
			pr_info("%s: vbus is under\n", __func__);
			state = POWER_SUPPLY_HEALTH_UNDERVOLTAGE;
		} else if((value.intval == POWER_SUPPLY_HEALTH_UNDERVOLTAGE) && \
				!((vbus_state == 0x0) || (vbus_state == 0x01))){
			max77803_set_input_current(charger,
					charger->charging_current_max);
		}
	}

	return state;
}

static bool max77803_charger_unlock(struct max77803_charger_data *chg_data)
{
	struct i2c_client *i2c = chg_data->max77803->i2c;
	u8 reg_data;
	u8 chgprot;
	int retry_cnt = 0;
	bool need_init = false;
	pr_debug("%s\n", __func__);

	max77803_read_reg(i2c, MAX77803_CHG_REG_CHG_CNFG_06, &reg_data);
	chgprot = ((reg_data & 0x0C) >> 2);

	if (chgprot == 0x03) {
		pr_info("%s: unlocked state, return\n", __func__);
		need_init = false;
		goto unlock_finish;
	}

	do {
		max77803_write_reg(i2c, MAX77803_CHG_REG_CHG_CNFG_06,
					(0x03 << 2));

		max77803_read_reg(i2c, MAX77803_CHG_REG_CHG_CNFG_06, &reg_data);
		chgprot = ((reg_data & 0x0C) >> 2);

		if (chgprot != 0x03) {
			pr_err("%s: unlock err, chgprot(0x%x), retry(%d)\n",
					__func__, chgprot, retry_cnt);
			msleep(100);
		} else {
			pr_info("%s: unlock success, chgprot(0x%x)\n",
							__func__, chgprot);
			need_init = true;
			break;
		}
	} while ((chgprot != 0x03) && (++retry_cnt < 10));

unlock_finish:
	return need_init;
}

static void max77803_charger_initialize(struct max77803_charger_data *charger)
{
	u8 reg_data;
	pr_debug("%s\n", __func__);

	/* unlock charger setting protect */
	reg_data = (0x03 << 2);
	max77803_write_reg(charger->max77803->i2c,
		MAX77803_CHG_REG_CHG_CNFG_06, reg_data);

	/*
	 * fast charge timer disable
	 * restart threshold disable
	 * pre-qual charge enable(default)
	 */
	reg_data = (0x0 << 0) | (0x03 << 4);
	max77803_write_reg(charger->max77803->i2c,
		MAX77803_CHG_REG_CHG_CNFG_01, reg_data);

	/*
	 * charge current 466mA(default)
	 * (max77888: 480mA(default))
	 * otg current limit 900mA
	 * (max77888: 350mA/1250mA)
	 */
	max77803_read_reg(charger->max77803->i2c,
			MAX77803_CHG_REG_CHG_CNFG_02, &reg_data);
#if !defined(CONFIG_CHAGALL)/* 350mA for Chagall */
	reg_data |= (1 << 7);
#endif
	max77803_write_reg(charger->max77803->i2c,
		MAX77803_CHG_REG_CHG_CNFG_02, reg_data);

	/*
	 * top off current 100mA
	 * top off timer 40min
	 */
	reg_data = (0x04 << 3);
	max77803_write_reg(charger->max77803->i2c,
		MAX77803_CHG_REG_CHG_CNFG_03, reg_data);
#if defined(CONFIG_CHAGALL)
	/* Watchdog Enable */
	max77803_update_reg(charger->max77803->i2c, MAX77803_CHG_REG_CHG_CNFG_00,
				MAX77803_WDTEN, MAX77803_WDTEN);
#endif
	/*
	 * cv voltage 4.2V or 4.35V
	 * MINVSYS 3.6V(default)
	 */
#if defined(max77888_charger)
	reg_data = (0xD9 << 0);
#else
	reg_data = (0xDD << 0);
#endif

	/*
	pr_info("%s: battery cv voltage %s, (sysrev %d)\n", __func__,
		(((reg_data & MAX77803_CHG_PRM_MASK) == \
		(0x1D << MAX77803_CHG_PRM_SHIFT)) ? "4.35V" : "4.2V"),
		system_rev);
	*/
	max77803_write_reg(charger->max77803->i2c,
		MAX77803_CHG_REG_CHG_CNFG_04, reg_data);

	max77803_dump_reg(charger);
}

#if defined(CONFIG_BATTERY_SWELLING)
static u8 max77803_get_float_voltage_data(int float_voltage)
{
#if defined(max77888_charger)
	u8 data = 0x13;

	if (float_voltage >= 4500)
		data = 0x1f;
	else
		data = (float_voltage - 3725) / 25;
	return data;
#else
	int voltage = 3650;
	int i;

	for (i = 0; voltage <= 4400; i++) {
		if (float_voltage <= voltage)
			break;
		voltage += 25;
	}

	if (float_voltage <= 4340)
		return i;
	else
		return (i+1);
#endif
}

static void max77803_set_float_voltage(struct max77803_charger_data *charger, int float_voltage)
{
	u8 reg_data = 0;

	reg_data = max77803_get_float_voltage_data(float_voltage);
	max77803_update_reg(charger->max77803->i2c, MAX77803_CHG_REG_CHG_CNFG_04,
			(reg_data << CHG_CNFG_04_CHG_CV_PRM_SHIFT),
			CHG_CNFG_04_CHG_CV_PRM_MASK);
	max77803_read_reg(charger->max77803->i2c, MAX77803_CHG_REG_CHG_CNFG_04, &reg_data);
	pr_info("%s: battery cv voltage 0x%x\n", __func__, reg_data);
}

static u8 max77803_get_float_voltage(struct max77803_charger_data *charger)
{
	u8 reg_data = 0;

	max77803_read_reg(charger->max77803->i2c, MAX77803_CHG_REG_CHG_CNFG_04, &reg_data);
	reg_data &= 0x1F;
	pr_info("%s: battery cv voltage 0x%x\n", __func__, reg_data);
	return reg_data;
}
#endif

static void check_charger_unlock_state(struct max77803_charger_data *chg_data)
{
	bool need_reg_init = false;
	pr_debug("%s\n", __func__);

	need_reg_init = max77803_charger_unlock(chg_data);
	if (need_reg_init) {
		pr_err("%s: charger locked state, reg init\n", __func__);
		max77803_charger_initialize(chg_data);
	}
}

static int sec_chg_get_property(struct power_supply *psy,
		enum power_supply_property psp,
		union power_supply_propval *val)
{
	struct max77803_charger_data *charger =
		container_of(psy, struct max77803_charger_data, psy_chg);
	u8 reg_data;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = POWER_SUPPLY_TYPE_BATTERY;
		if (max77803_read_reg(charger->max77803->i2c,
			MAX77803_CHG_REG_CHG_INT_OK, &reg_data) == 0) {
			if (reg_data & MAX77803_CHGIN_OK)
				val->intval = POWER_SUPPLY_TYPE_MAINS;
			else if (reg_data & MAX77803_WCIN_OK) {
				val->intval = POWER_SUPPLY_TYPE_WPC;
				charger->wc_w_state = 1;
			}
		}
		break;
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = max77803_get_charger_state(charger);
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		val->intval = max77803_get_health_state(charger);
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		val->intval = max77803_get_input_current(charger);
		break;
	case POWER_SUPPLY_PROP_CURRENT_AVG:
		val->intval = charger->charging_current;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		val->intval = max77803_get_charge_current(charger);
		break;
#if defined(CONFIG_BATTERY_SWELLING)
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		val->intval = max77803_get_float_voltage(charger);
		break;
#endif
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		if (!charger->is_charging)
		val->intval = POWER_SUPPLY_CHARGE_TYPE_NONE;
		else
			val->intval = POWER_SUPPLY_CHARGE_TYPE_FAST;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = max77803_get_battery_present(charger);
		break;
	case POWER_SUPPLY_PROP_POWER_NOW:
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int sec_chg_set_property(struct power_supply *psy,
		enum power_supply_property psp,
		const union power_supply_propval *val)
{
	struct max77803_charger_data *charger =
		container_of(psy, struct max77803_charger_data, psy_chg);
	union power_supply_propval value;
	int set_charging_current, set_charging_current_max;
	const int usb_charging_current = charger->pdata->charging_current[
		POWER_SUPPLY_TYPE_USB].fast_charging_current;
	const int wpc_charging_current = charger->pdata->charging_current[
		POWER_SUPPLY_TYPE_WPC].input_current_limit;
	u8 en_chg_cnfg_00;
	u8 dis_chg_cnfg_00;
	u8 mask_chg_cnfg_00;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		charger->status = val->intval;
		break;
	/* val->intval : type */
	case POWER_SUPPLY_PROP_ONLINE:
		/* check and unlock */
		check_charger_unlock_state(charger);

		if (val->intval == POWER_SUPPLY_TYPE_POWER_SHARING) {
			psy_do_property("ps", get,
				POWER_SUPPLY_PROP_STATUS, value);
#if defined (max77888_charger)
			mask_chg_cnfg_00 = CHG_CNFG_00_CHG_MASK
				| CHG_CNFG_00_OTG_MASK
				| CHG_CNFG_00_BUCK_MASK
				| CHG_CNFG_00_BOOST_MASK
				| CHG_CNFG_00_DIS_MUIC_CTRL_MASK;
			dis_chg_cnfg_00 = CHG_CNFG_00_BUCK_MASK;
#else
			mask_chg_cnfg_00 = CHG_CNFG_00_OTG_MASK
				| CHG_CNFG_00_BOOST_MASK
				| CHG_CNFG_00_DIS_MUIC_CTRL_MASK;
			dis_chg_cnfg_00 = 0;
#endif
			en_chg_cnfg_00 = CHG_CNFG_00_OTG_MASK
				| CHG_CNFG_00_BOOST_MASK
				| CHG_CNFG_00_DIS_MUIC_CTRL_MASK;

			if (value.intval) {
#if defined(CONFIG_CHAGALL)
				max77803_update_reg(charger->max77803->i2c, MAX77803_CHG_REG_CHG_CNFG_02,
					(1 << 7), (1 << 7));
#endif
				max77803_update_reg(charger->max77803->i2c, MAX77803_CHG_REG_CHG_CNFG_00,
					en_chg_cnfg_00, mask_chg_cnfg_00);

				pr_info("%s: ps enable\n", __func__);
			} else {
#if defined(CONFIG_CHAGALL)
				max77803_update_reg(charger->max77803->i2c, MAX77803_CHG_REG_CHG_CNFG_02,
					0, (1 << 7));
#endif
				max77803_update_reg(charger->max77803->i2c, MAX77803_CHG_REG_CHG_CNFG_00,
					dis_chg_cnfg_00, mask_chg_cnfg_00);

				pr_info("%s: ps disable\n", __func__);
			}
			break;
		}

		charger->cable_type = val->intval;
		if (val->intval == POWER_SUPPLY_TYPE_OTG)
			break;

		psy_do_property("battery", get,
				POWER_SUPPLY_PROP_HEALTH, value);
		if (val->intval == POWER_SUPPLY_TYPE_BATTERY) {
			charger->is_charging = false;
			charger->soft_reg_recovery_cnt = 0;
			charger->is_mdock = false;
			charger->is_otg = false;
			set_charging_current = 0;
			set_charging_current_max =
				charger->pdata->charging_current[
				POWER_SUPPLY_TYPE_USB].input_current_limit;

			if (charger->wc_w_state) {
				cancel_delayed_work_sync(&charger->wpc_work);
				/* recheck after cancel_delayed_work_sync */
				if (charger->wc_w_state) {
					wake_lock(&charger->wpc_wake_lock);
					queue_delayed_work(charger->wqueue, &charger->wpc_work,
							msecs_to_jiffies(500));
					charger->wc_w_state = 0;
				}
			}
		} else {
			pr_info("%s: cable type = %d\n", __func__, charger->cable_type);
			charger->is_charging = true;

			if ((charger->cable_type == POWER_SUPPLY_TYPE_USB)
				&& (charger->pdata->is_hc_usb)) {
				pr_info("%s: high current usb setting\n", __func__);

				charger->charging_current = charger->pdata->charging_current[
					POWER_SUPPLY_TYPE_MAINS].fast_charging_current;
				charger->charging_current_max =	charger->pdata->charging_current[
						POWER_SUPPLY_TYPE_MAINS].input_current_limit;
			}

			if (charger->cable_type == POWER_SUPPLY_TYPE_SMART_NOTG)
				charger->is_otg = false;
			else if (charger->cable_type == POWER_SUPPLY_TYPE_SMART_OTG)
				charger->is_otg = true;
			if (charger->cable_type == POWER_SUPPLY_TYPE_MDOCK_TA)
				charger->is_mdock = true;

			if(charger->is_mdock){
				if(charger->is_otg){
					charger->charging_current = charger->pdata->charging_current[
					POWER_SUPPLY_TYPE_MDOCK_TA].fast_charging_current - 300;
					charger->charging_current_max = charger->pdata->charging_current[
					POWER_SUPPLY_TYPE_MDOCK_TA].input_current_limit - 300;
				}else{
					charger->charging_current = charger->pdata->charging_current[
					POWER_SUPPLY_TYPE_MDOCK_TA].fast_charging_current;
					charger->charging_current_max = charger->pdata->charging_current[
					POWER_SUPPLY_TYPE_MDOCK_TA].input_current_limit;
				}
			}

			/* decrease the charging current according to siop level */
			set_charging_current =
				charger->charging_current * charger->siop_level / 100;
			if (set_charging_current > 0 &&
					set_charging_current < usb_charging_current)
				set_charging_current = usb_charging_current;
			if (val->intval == POWER_SUPPLY_TYPE_WPC)
				set_charging_current_max = wpc_charging_current;
			else
				set_charging_current_max =
					charger->charging_current_max;

			if (charger->siop_level < 100 &&
				val->intval == POWER_SUPPLY_TYPE_MAINS) {
				if (set_charging_current_max > SIOP_INPUT_LIMIT_CURRENT)
					set_charging_current_max = SIOP_INPUT_LIMIT_CURRENT;
				if (set_charging_current > SIOP_CHARGING_LIMIT_CURRENT)
					set_charging_current = SIOP_CHARGING_LIMIT_CURRENT;
			}
		}

		if (charger->pdata->full_check_type_2nd == SEC_BATTERY_FULLCHARGED_CHGPSY) {
			union power_supply_propval chg_mode;
			psy_do_property("battery", get, POWER_SUPPLY_PROP_CHARGE_NOW, chg_mode);

			if (chg_mode.intval == SEC_BATTERY_CHARGING_2ND) {
				max77803_set_charger_state(charger, 0);
				max77803_set_topoff_current(charger,
							    charger->pdata->charging_current[
								    charger->cable_type].full_check_current_2nd,
							    (70 * 60));
			} else {
				max77803_set_topoff_current(charger,
							    charger->pdata->charging_current[
								    charger->cable_type].full_check_current_1st,
							    (70 * 60));
			}
		} else {
			max77803_set_topoff_current(charger,
				charger->pdata->charging_current[
				val->intval].full_check_current_1st,
				charger->pdata->charging_current[
				val->intval].full_check_current_2nd);
		}

		max77803_set_charger_state(charger, charger->is_charging);
		/* if battery full, only disable charging  */
		if ((charger->status == POWER_SUPPLY_STATUS_CHARGING) ||
				(charger->status == POWER_SUPPLY_STATUS_DISCHARGING) ||
				(value.intval == POWER_SUPPLY_HEALTH_UNSPEC_FAILURE)) {
			/* current setting */
			max77803_set_charge_current(charger,
				set_charging_current);
			/* if battery is removed, disable input current and reenable input current
			  *  to enable buck always */
			if (value.intval == POWER_SUPPLY_HEALTH_UNSPEC_FAILURE)
				max77803_set_input_current(charger, 0);
			else
				max77803_set_input_current(charger,
					set_charging_current_max);
		}
		break;
	/* val->intval : input charging current */
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		charger->charging_current_max = val->intval;
		break;
	/*  val->intval : charging current */
	case POWER_SUPPLY_PROP_CURRENT_AVG:
		charger->charging_current = val->intval;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		charger->siop_level = val->intval;
		if (charger->is_charging) {
			/* decrease the charging current according to siop level */
			int current_now =
				charger->charging_current * val->intval / 100;

			/* do forced set charging current */
			if (current_now > 0 &&
					current_now < usb_charging_current)
				current_now = usb_charging_current;

			if (charger->cable_type == POWER_SUPPLY_TYPE_MAINS) {
				if (charger->siop_level < 100 ) {
					set_charging_current_max =
						SIOP_INPUT_LIMIT_CURRENT;
				} else
					set_charging_current_max =
						charger->charging_current_max;

				if (charger->siop_level < 100 &&
						current_now > SIOP_CHARGING_LIMIT_CURRENT)
					current_now = SIOP_CHARGING_LIMIT_CURRENT;
				max77803_set_input_current(charger,
					set_charging_current_max);
			}

			max77803_set_charge_current(charger, current_now);

		}
		break;
#if defined(CONFIG_BATTERY_SWELLING)
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		pr_info("%s: float voltage(%d)\n", __func__, val->intval);
		max77803_set_float_voltage(charger, val->intval);
		break;
#endif
	case POWER_SUPPLY_PROP_POWER_NOW:
		max77803_set_charge_current(charger,
				val->intval);
		max77803_set_input_current(charger,
				val->intval);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static void sec_chg_isr_work(struct work_struct *work)
{
	struct max77803_charger_data *charger =
		container_of(work, struct max77803_charger_data, isr_work.work);

	union power_supply_propval val;

	if (charger->pdata->full_check_type ==
			SEC_BATTERY_FULLCHARGED_CHGINT) {

		val.intval = max77803_get_charger_state(charger);

		switch (val.intval) {
		case POWER_SUPPLY_STATUS_DISCHARGING:
			pr_err("%s: Interrupted but Discharging\n", __func__);
			break;

		case POWER_SUPPLY_STATUS_NOT_CHARGING:
			pr_err("%s: Interrupted but NOT Charging\n", __func__);
			break;

		case POWER_SUPPLY_STATUS_FULL:
			pr_info("%s: Interrupted by Full\n", __func__);
			psy_do_property("battery", set,
				POWER_SUPPLY_PROP_STATUS, val);
			break;

		case POWER_SUPPLY_STATUS_CHARGING:
			pr_err("%s: Interrupted but Charging\n", __func__);
			break;

		case POWER_SUPPLY_STATUS_UNKNOWN:
		default:
			pr_err("%s: Invalid Charger Status\n", __func__);
			break;
		}
	}

	if (charger->pdata->ovp_uvlo_check_type ==
			SEC_BATTERY_OVP_UVLO_CHGINT) {

		val.intval = max77803_get_health_state(charger);

		switch (val.intval) {
		case POWER_SUPPLY_HEALTH_OVERHEAT:
		case POWER_SUPPLY_HEALTH_COLD:
			pr_err("%s: Interrupted but Hot/Cold\n", __func__);
			break;

		case POWER_SUPPLY_HEALTH_DEAD:
			pr_err("%s: Interrupted but Dead\n", __func__);
			break;

		case POWER_SUPPLY_HEALTH_OVERVOLTAGE:
		case POWER_SUPPLY_HEALTH_UNDERVOLTAGE:
			pr_info("%s: Interrupted by OVP/UVLO\n", __func__);
			psy_do_property("battery", set,
				POWER_SUPPLY_PROP_HEALTH, val);
			break;

		case POWER_SUPPLY_HEALTH_UNSPEC_FAILURE:
			pr_err("%s: Interrupted but Unspec\n", __func__);
			break;

		case POWER_SUPPLY_HEALTH_GOOD:
			pr_err("%s: Interrupted but Good\n", __func__);
			break;

		case POWER_SUPPLY_HEALTH_UNKNOWN:
		default:
			pr_err("%s: Invalid Charger Health\n", __func__);
			break;
		}
	}
}

static irqreturn_t sec_chg_irq_thread(int irq, void *irq_data)
{
	struct max77803_charger_data *charger = irq_data;

	pr_info("%s: Charger interrupt occured\n", __func__);

	if ((charger->pdata->full_check_type ==
				SEC_BATTERY_FULLCHARGED_CHGINT) ||
			(charger->pdata->ovp_uvlo_check_type ==
			 SEC_BATTERY_OVP_UVLO_CHGINT))
		schedule_delayed_work(&charger->isr_work, 0);

	return IRQ_HANDLED;
}

#if defined(CONFIG_CHARGER_MAX77803)
static void wpc_detect_work(struct work_struct *work)
{
	struct max77803_charger_data *chg_data = container_of(work,
						struct max77803_charger_data,
						wpc_work.work);
	int wc_w_state;
	union power_supply_propval value;
	u8 reg_data;
	pr_info("%s\n", __func__);
	wake_unlock(&chg_data->wpc_wake_lock);

	/*get status of cable*/
	psy_do_property("battery", get,
		POWER_SUPPLY_PROP_ONLINE, value);
	if ((value.intval != POWER_SUPPLY_TYPE_BATTERY) &&
			(value.intval != POWER_SUPPLY_TYPE_WPC)) {
		return;
	}
	/* check and unlock */
	check_charger_unlock_state(chg_data);

	max77803_read_reg(chg_data->max77803->i2c,
			MAX77803_CHG_REG_CHG_INT_OK, &reg_data);
	wc_w_state = (reg_data & MAX77803_WCIN_OK)
				>> MAX77803_WCIN_OK_SHIFT;
	if ((chg_data->wc_w_state == 0) && (wc_w_state == 1)) {
		value.intval = POWER_SUPPLY_TYPE_WPC<<ONLINE_TYPE_MAIN_SHIFT;
		psy_do_property("battery", set,
				POWER_SUPPLY_PROP_ONLINE, value);
		pr_info("%s: wpc activated, set V_INT as PN\n",
				__func__);
	} else if ((chg_data->wc_w_state == 1) && (wc_w_state == 0)) {
		if (!chg_data->is_charging)
			max77803_set_charger_state(chg_data, true);
		max77803_read_reg(chg_data->max77803->i2c,
				MAX77803_CHG_REG_CHG_DTLS_01, &reg_data);
		reg_data = ((reg_data & MAX77803_CHG_DTLS) >> MAX77803_CHG_DTLS_SHIFT);
		pr_info("%s: reg_data: 0x%x, charging: %d\n", __func__,
			reg_data, chg_data->is_charging);
		if (!chg_data->is_charging)
			max77803_set_charger_state(chg_data, false);
		if (reg_data != 0x08) {
			pr_info("%s: wpc uvlo, but charging\n",	__func__);
			wake_lock(&chg_data->wpc_wake_lock);
			queue_delayed_work(chg_data->wqueue, &chg_data->wpc_work,
					msecs_to_jiffies(500));
			return;
		} else {
			value.intval =
				POWER_SUPPLY_TYPE_BATTERY<<ONLINE_TYPE_MAIN_SHIFT;
			psy_do_property("battery", set,
					POWER_SUPPLY_PROP_ONLINE, value);
			pr_info("%s: wpc deactivated, set V_INT as PD\n",
					__func__);
		}
	}
	pr_info("%s: w(%d to %d)\n", __func__,
			chg_data->wc_w_state, wc_w_state);

	chg_data->wc_w_state = wc_w_state;
}

static irqreturn_t wpc_charger_irq(int irq, void *data)
{
	struct max77803_charger_data *chg_data = data;

	cancel_delayed_work_sync(&chg_data->wpc_work);
	wake_lock(&chg_data->wpc_wake_lock);
	if (chg_data->wc_w_state)
		queue_delayed_work(chg_data->wqueue, &chg_data->wpc_work,
			msecs_to_jiffies(500));
	else
		queue_delayed_work(chg_data->wqueue, &chg_data->wpc_work,
			msecs_to_jiffies(0));
	return IRQ_HANDLED;
}
#elif defined(CONFIG_WIRELESS_CHARGING)
static irqreturn_t wpc_charger_irq(int irq, void *data)
{
	struct max77803_charger_data *chg_data = data;
	int wc_w_state;
	union power_supply_propval value;
	pr_info("%s: irq(%d)\n", __func__, irq);

	/* check and unlock */
	check_charger_unlock_state(chg_data);

	wc_w_state = !gpio_get_value(chg_data->wc_w_gpio);
	if ((chg_data->wc_w_state == 0) && (wc_w_state == 1)) {
		value.intval = POWER_SUPPLY_TYPE_WPC<<ONLINE_TYPE_MAIN_SHIFT;
		psy_do_property("battery", set,
				POWER_SUPPLY_PROP_ONLINE, value);
		pr_info("%s: wpc activated, set V_INT as PN\n",
				__func__);
	} else if ((chg_data->wc_w_state == 1) && (wc_w_state == 0)) {
		value.intval =
			POWER_SUPPLY_TYPE_BATTERY<<ONLINE_TYPE_MAIN_SHIFT;
		psy_do_property("battery", set,
				POWER_SUPPLY_PROP_ONLINE, value);
		pr_info("%s: wpc deactivated, set V_INT as PD\n",
				__func__);
	}
	pr_info("%s: w(%d to %d)\n", __func__,
			chg_data->wc_w_state, wc_w_state);

	chg_data->wc_w_state = wc_w_state;

	return IRQ_HANDLED;
}
#endif

static irqreturn_t max77803_bypass_irq(int irq, void *data)
{
	struct max77803_charger_data *chg_data = data;
	u8 dtls_02;
	u8 byp_dtls;
	u8 chgin_dtls, chg_dtls, reg_data;
	u8 chg_cnfg_00;
	u8 vbus_state;

	union power_supply_propval value;

#ifdef CONFIG_USB_HOST_NOTIFY
	 struct host_notifier_platform_data *host_noti_pdata =
	   host_notifier_device.dev.platform_data;
#endif


	pr_info("%s: irq(%d)\n", __func__, irq);

	/* check and unlock */
	check_charger_unlock_state(chg_data);

	/* Due to timing issue, 0xB5 reg should be read at first to detect overcurrent limit.
	*  If 0xB5's read after 0XB3, 0xB4, it's value is 0x00 even for the overcurrent limit case.
	*/
	max77803_read_reg(chg_data->max77803->i2c,
				MAX77803_CHG_REG_CHG_DTLS_02,
				&dtls_02);
	pr_info("%s: CHG_DTLS_02(0xb5) = 0x%x\n", __func__, dtls_02);

	max77803_read_reg(chg_data->max77803->i2c,
				MAX77803_CHG_REG_CHG_DTLS_00,
				&chgin_dtls);
	max77803_read_reg(chg_data->max77803->i2c,
				MAX77803_CHG_REG_CHG_DTLS_01, &chg_dtls);
	chgin_dtls = ((chgin_dtls & MAX77803_CHGIN_DTLS) >>
				MAX77803_CHGIN_DTLS_SHIFT);
	chg_dtls = ((chg_dtls & MAX77803_CHG_DTLS) >>
				MAX77803_CHG_DTLS_SHIFT);

	byp_dtls = ((dtls_02 & MAX77803_BYP_DTLS) >>
				MAX77803_BYP_DTLS_SHIFT);
	pr_info("%s: BYP_DTLS(0x%02x), chgin_dtls(0x%02x), chg_dtls(0x%02x)\n",
		__func__, byp_dtls, chgin_dtls, chg_dtls);
	vbus_state = max77803_get_vbus_state(chg_data);

	if (byp_dtls & 0x1) {
		pr_info("%s: bypass overcurrent limit\n", __func__);
#ifdef CONFIG_USB_HOST_NOTIFY
		  host_state_notify(&host_noti_pdata->ndev,
			 NOTIFY_HOST_OVERCURRENT);
#endif
		/* disable the register values just related to OTG and
		   keep the values about the charging */
		max77803_read_reg(chg_data->max77803->i2c,
			MAX77803_CHG_REG_CHG_CNFG_00, &chg_cnfg_00);
		chg_cnfg_00 &= ~(CHG_CNFG_00_OTG_MASK
				| CHG_CNFG_00_BOOST_MASK
				| CHG_CNFG_00_DIS_MUIC_CTRL_MASK);
		max77803_write_reg(chg_data->max77803->i2c,
					MAX77803_CHG_REG_CHG_CNFG_00,
					chg_cnfg_00);
	}

	if ((byp_dtls & 0x8) && (vbus_state < 0x03))
		reduce_input_current(chg_data, REDUCE_CURRENT_STEP);

	return IRQ_HANDLED;
}
bool unstable_power_detection = true;

static void max77803_chgin_isr_work(struct work_struct *work)
{
	struct max77803_charger_data *charger = container_of(work,
				struct max77803_charger_data, chgin_work);
	u8 chgin_dtls, chg_dtls, reg_data;
	u8 prev_chgin_dtls = 0xff;
	int battery_health;
	union power_supply_propval value;
	int stable_count = 0;

	wake_lock(&charger->chgin_wake_lock);
	max77803_read_reg(charger->max77803->i2c,
		MAX77803_CHG_REG_CHG_INT_MASK, &reg_data);
	reg_data |= (1 << 6);
	max77803_write_reg(charger->max77803->i2c,
		MAX77803_CHG_REG_CHG_INT_MASK, reg_data);

	while (1) {
		psy_do_property("battery", get,
				POWER_SUPPLY_PROP_HEALTH, value);
		battery_health = value.intval;

	max77803_read_reg(charger->max77803->i2c,
				MAX77803_CHG_REG_CHG_DTLS_00,
				&chgin_dtls);
		chgin_dtls = ((chgin_dtls & MAX77803_CHGIN_DTLS) >>
				MAX77803_CHGIN_DTLS_SHIFT);
		max77803_read_reg(charger->max77803->i2c,
				MAX77803_CHG_REG_CHG_DTLS_01, &chg_dtls);
		chg_dtls = ((chg_dtls & MAX77803_CHG_DTLS) >>
				MAX77803_CHG_DTLS_SHIFT);
		if (prev_chgin_dtls == chgin_dtls)
			stable_count++;
		else
			stable_count = 0;
		if (stable_count > 10 || !unstable_power_detection) {
			pr_info("%s: irq(%d), chgin(0x%x), prev 0x%x\n",
					__func__, charger->irq_chgin,
					chgin_dtls, prev_chgin_dtls);

			psy_do_property("battery", get,
				POWER_SUPPLY_PROP_STATUS, value);

		if (charger->is_charging) {
			if ((chgin_dtls == 0x02) && \
				(battery_health == POWER_SUPPLY_HEALTH_GOOD)) {
				pr_info("%s: charger is over voltage\n",
						__func__);
				value.intval = POWER_SUPPLY_HEALTH_OVERVOLTAGE;
				psy_do_property("battery", set,
					POWER_SUPPLY_PROP_HEALTH, value);
			} else if ((battery_health == \
					POWER_SUPPLY_HEALTH_OVERVOLTAGE) &&
					(chgin_dtls != 0x02)){
				pr_info("%s: charger is good\n", __func__);
				value.intval = POWER_SUPPLY_HEALTH_GOOD;
				psy_do_property("battery", set,
					POWER_SUPPLY_PROP_HEALTH, value);
			}
			}
			break;
		}

		if (charger->is_charging) {
			/* reduce only at CC MODE */
			if (((chgin_dtls == 0x0) || (chgin_dtls == 0x01)) &&
					(chg_dtls == 0x01) && (stable_count > 2))
				reduce_input_current(charger, REDUCE_CURRENT_STEP);
		}
		prev_chgin_dtls = chgin_dtls;
		msleep(100);
	}
	max77803_read_reg(charger->max77803->i2c,
		MAX77803_CHG_REG_CHG_INT_MASK, &reg_data);
	reg_data &= ~(1 << 6);
	max77803_write_reg(charger->max77803->i2c,
		MAX77803_CHG_REG_CHG_INT_MASK, reg_data);
	wake_unlock(&charger->chgin_wake_lock);
}

static irqreturn_t max77803_chgin_irq(int irq, void *data)
{
	struct max77803_charger_data *charger = data;
	queue_work(charger->wqueue, &charger->chgin_work);

	return IRQ_HANDLED;
}

/* register chgin isr after sec_battery_probe */
static void max77803_chgin_init_work(struct work_struct *work)
{
	struct max77803_charger_data *charger = container_of(work,
						struct max77803_charger_data,
						chgin_init_work.work);
	int ret;

	pr_info("%s \n", __func__);
	ret = request_threaded_irq(charger->irq_chgin, NULL,
			max77803_chgin_irq, 0, "chgin-irq", charger);
	if (ret < 0) {
		pr_err("%s: fail to request chgin IRQ: %d: %d\n",
				__func__, charger->irq_chgin, ret);
	}
}

static __devinit int max77803_charger_probe(struct platform_device *pdev)
{
	struct max77803_dev *iodev = dev_get_drvdata(pdev->dev.parent);
	struct max77803_platform_data *pdata = dev_get_platdata(iodev->dev);
	struct max77803_charger_data *charger;
	int ret = 0;
	u8 reg_data;

	pr_info("%s: MAX77803 Charger driver probe\n", __func__);

	charger = kzalloc(sizeof(*charger), GFP_KERNEL);
	if (!charger)
		return -ENOMEM;

	charger->max77803 = iodev;
	charger->pdata = pdata->charger_data;
	charger->siop_level = 100;

	platform_set_drvdata(pdev, charger);

	charger->psy_chg.name           = "sec-charger";
	charger->psy_chg.type           = POWER_SUPPLY_TYPE_UNKNOWN;
	charger->psy_chg.get_property   = sec_chg_get_property;
	charger->psy_chg.set_property   = sec_chg_set_property;
	charger->psy_chg.properties     = sec_charger_props;
	charger->psy_chg.num_properties = ARRAY_SIZE(sec_charger_props);

	mutex_init(&charger->ops_lock);

	if (charger->pdata->chg_gpio_init) {
		if (!charger->pdata->chg_gpio_init()) {
			pr_err("%s: Failed to Initialize GPIO\n", __func__);
			goto err_free;
		}
	}

	max77803_charger_initialize(charger);

	if (max77803_read_reg(charger->max77803->i2c, MAX77803_PMIC_REG_PMIC_ID1, &reg_data) < 0) {
		pr_err("device not found on this channel (this is not an error)\n");
		ret = -ENODEV;
		goto err_free;
	} else {
		charger->pmic_ver = (reg_data & 0xf);
		pr_info("%s: device found: ver.0x%x\n", __func__,
				charger->pmic_ver);
	}

#if defined(max77888_charger)
	charger->input_curr_limit_step = 25;
	charger->wpc_input_curr_limit_step = 20;
	charger->charging_curr_step= 400;  // 0.1mA unit
#else
	if (charger->pmic_ver == 0x04) {
		charger->input_curr_limit_step = 25;
		charger->wpc_input_curr_limit_step = 20;
		charger->charging_curr_step= 400;  // 0.1mA unit
	} else {
		charger->input_curr_limit_step = 20;
		charger->wpc_input_curr_limit_step = 20;
		charger->charging_curr_step= 333;  // 0.1mA unit
	}
#endif

	charger->wqueue =
	    create_singlethread_workqueue(dev_name(&pdev->dev));
	if (!charger->wqueue) {
		pr_err("%s: Fail to Create Workqueue\n", __func__);
		goto err_free;
	}
	wake_lock_init(&charger->chgin_wake_lock, WAKE_LOCK_SUSPEND,
            "charger-chgin");
	INIT_WORK(&charger->chgin_work, max77803_chgin_isr_work);
	INIT_DELAYED_WORK(&charger->chgin_init_work, max77803_chgin_init_work);
	wake_lock_init(&charger->recovery_wake_lock, WAKE_LOCK_SUSPEND,
					       "charger-recovery");
	INIT_DELAYED_WORK(&charger->recovery_work, max77803_recovery_work);
	wake_lock_init(&charger->wpc_wake_lock, WAKE_LOCK_SUSPEND,
					       "charger-wpc");
	INIT_DELAYED_WORK(&charger->wpc_work, wpc_detect_work);
	ret = power_supply_register(&pdev->dev, &charger->psy_chg);
	if (ret) {
		pr_err("%s: Failed to Register psy_chg\n", __func__);
		goto err_power_supply_register;
	}

	if (charger->pdata->chg_irq) {
		INIT_DELAYED_WORK_DEFERRABLE(
				&charger->isr_work, sec_chg_isr_work);
		ret = request_threaded_irq(charger->pdata->chg_irq,
				NULL, sec_chg_irq_thread,
				charger->pdata->chg_irq_attr,
				"charger-irq", charger);
		if (ret) {
			pr_err("%s: Failed to Reqeust IRQ\n", __func__);
			goto err_irq;
		}
	}
#if defined(CONFIG_WIRELESS_CHARGING)
	charger->wc_w_irq = pdata->irq_base + MAX77803_CHG_IRQ_WCIN_I;
	ret = request_threaded_irq(charger->wc_w_irq,
			NULL, wpc_charger_irq,
			IRQF_TRIGGER_FALLING,
			"wpc-int", charger);
	if (ret) {
		pr_err("%s: Failed to Reqeust IRQ\n", __func__);
		goto err_wc_irq;
	}
	max77803_read_reg(charger->max77803->i2c,
			MAX77803_CHG_REG_CHG_INT_OK, &reg_data);
	charger->wc_w_state = (reg_data & MAX77803_WCIN_OK)
				>> MAX77803_WCIN_OK_SHIFT;
#elif defined(CONFIG_CHARGER_MAX77803)
	charger->wc_w_gpio = pdata->wc_irq_gpio;
	if (charger->wc_w_gpio) {
		charger->wc_w_irq = gpio_to_irq(charger->wc_w_gpio);
		ret = gpio_request(charger->wc_w_gpio, "wpc_charger-irq");
		if (ret < 0) {
			pr_err("%s: failed requesting gpio %d\n", __func__,
				charger->wc_w_gpio);
			goto err_wc_irq;
		}
		ret = request_threaded_irq(charger->wc_w_irq,
				NULL, wpc_charger_irq,
				IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING |
				IRQF_ONESHOT,
				"wpc-int", charger);
		if (ret) {
			pr_err("%s: Failed to Reqeust IRQ\n", __func__);
			goto err_wc_irq;
		}
		enable_irq_wake(charger->wc_w_irq);
		charger->wc_w_state = !gpio_get_value(charger->wc_w_gpio);
	}
#endif

	charger->irq_chgin = pdata->irq_base + MAX77803_CHG_IRQ_CHGIN_I;
	/* enable chgin irq after sec_battery_probe */
	queue_delayed_work(charger->wqueue, &charger->chgin_init_work,
			msecs_to_jiffies(3000));

	charger->irq_bypass = pdata->irq_base + MAX77803_CHG_IRQ_BYP_I;
	ret = request_threaded_irq(charger->irq_bypass, NULL,
			max77803_bypass_irq, 0, "bypass-irq", charger);
	if (ret < 0)
		pr_err("%s: fail to request bypass IRQ: %d: %d\n",
				__func__, charger->irq_bypass, ret);

	return 0;
err_wc_irq:
	free_irq(charger->pdata->chg_irq, NULL);
err_irq:
	power_supply_unregister(&charger->psy_chg);
err_power_supply_register:
	destroy_workqueue(charger->wqueue);
err_free:
	mutex_destroy(&charger->ops_lock);
	kfree(charger);

	return ret;

}

static int __devexit max77803_charger_remove(struct platform_device *pdev)
{
	struct max77803_charger_data *charger =
				platform_get_drvdata(pdev);

	destroy_workqueue(charger->wqueue);
	free_irq(charger->wc_w_irq, NULL);
	free_irq(charger->pdata->chg_irq, NULL);
	power_supply_unregister(&charger->psy_chg);
	kfree(charger);

	return 0;
}

#if defined CONFIG_PM
static int max77803_charger_suspend(struct device *dev)
{
	return 0;
}

static int max77803_charger_resume(struct device *dev)
{
	return 0;
}
#else
#define max77803_charger_suspend NULL
#define max77803_charger_resume NULL
#endif

static void max77803_charger_shutdown(struct device *dev)
{
	struct max77803_charger_data *charger =
				dev_get_drvdata(dev);
	u8 reg_data;

	pr_info("%s: MAX77803 Charger driver shutdown\n", __func__);
	if (!charger->max77803->i2c) {
		pr_err("%s: no max77803 i2c client\n", __func__);
		return;
	}
	reg_data = 0x04;
	max77803_write_reg(charger->max77803->i2c,
		MAX77803_CHG_REG_CHG_CNFG_00, reg_data);
#if defined(max77888_charger)
	reg_data = 0x14;
#else
	reg_data = 0x19;
#endif
	max77803_write_reg(charger->max77803->i2c,
		MAX77803_CHG_REG_CHG_CNFG_09, reg_data);
	reg_data = 0x19;
	max77803_write_reg(charger->max77803->i2c,
		MAX77803_CHG_REG_CHG_CNFG_10, reg_data);
	pr_info("func:%s \n", __func__);
}

static SIMPLE_DEV_PM_OPS(max77803_charger_pm_ops, max77803_charger_suspend,
		max77803_charger_resume);

static struct platform_driver max77803_charger_driver = {
	.driver = {
		.name = "max77803-charger",
		.owner = THIS_MODULE,
		.pm = &max77803_charger_pm_ops,
		.shutdown = max77803_charger_shutdown,
	},
	.probe = max77803_charger_probe,
	.remove = __devexit_p(max77803_charger_remove),
};

static int __init max77803_charger_init(void)
{
	pr_info("func:%s\n", __func__);
	return platform_driver_register(&max77803_charger_driver);
}
module_init(max77803_charger_init);

static void __exit max77803_charger_exit(void)
{
	platform_driver_register(&max77803_charger_driver);
}

module_exit(max77803_charger_exit);

MODULE_DESCRIPTION("max77803 charger driver");
MODULE_AUTHOR("Samsung Electronics");
MODULE_LICENSE("GPL");
