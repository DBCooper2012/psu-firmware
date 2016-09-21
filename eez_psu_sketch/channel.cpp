/*
 * EEZ PSU Firmware
 * Copyright (C) 2015-present, Envox d.o.o.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
 
#include "psu.h"
#include "serial_psu.h"

#if OPTION_ETHERNET
#include "ethernet.h"
#endif

#include "bp.h"
#include "board.h"
#include "ioexp.h"
#include "adc.h"
#include "dac.h"
#include "bp.h"
#include "calibration.h"
#include "persist_conf.h"
#include "sound.h"
#include "profile.h"
#include "event_queue.h"

namespace eez {
namespace psu {

using namespace scpi;

////////////////////////////////////////////////////////////////////////////////

const char *CH_BOARD_REVISION_NAMES[] = {
    // CH_BOARD_REVISION_R4B43A
    "Power_r4b43a",
    // CH_BOARD_REVISION_R5B6B
    "Power_r5B6b",
    // CH_BOARD_REVISION_R5B9
    "Power_r5B9"
};

uint16_t CH_BOARD_REVISION_FEATURES[] = {
    // CH_BOARD_REVISION_R4B43A
    CH_FEATURE_VOLT | CH_FEATURE_CURRENT | CH_FEATURE_OE, 
    // CH_BOARD_REVISION_R5B6B
    CH_FEATURE_VOLT | CH_FEATURE_CURRENT | CH_FEATURE_POWER | CH_FEATURE_OE | CH_FEATURE_DPROG | CH_FEATURE_LRIPPLE | CH_FEATURE_RPROG,
    // CH_BOARD_REVISION_R5B9
    CH_FEATURE_VOLT | CH_FEATURE_CURRENT | CH_FEATURE_POWER | CH_FEATURE_OE | CH_FEATURE_DPROG | CH_FEATURE_LRIPPLE | CH_FEATURE_RPROG
};

////////////////////////////////////////////////////////////////////////////////

#define CHANNEL(INDEX, BOARD_REVISION, PINS, PARAMS) Channel(INDEX, BOARD_REVISION, PINS, PARAMS)
Channel channels[CH_MAX] = { CHANNELS };
#undef CHANNEL

////////////////////////////////////////////////////////////////////////////////

Channel &Channel::get(int channel_index) {
    return channels[channel_index];
}

////////////////////////////////////////////////////////////////////////////////

void Channel::Value::init(float def_step, float def_limit) {
    set = 0;
    mon_dac = 0;
    mon = 0;
    step = def_step;
	limit = def_limit;
}

////////////////////////////////////////////////////////////////////////////////

struct {
	unsigned OE_SAVED: 1;
	unsigned CH1_OE: 1;
	unsigned CH2_OE: 1;
} g_savedOE;

void Channel::saveAndDisableOE() {
	if (!g_savedOE.OE_SAVED) {
		if (CH_NUM > 0) {
			g_savedOE.CH1_OE = Channel::get(0).isOutputEnabled() ? 1 : 0;
			Channel::get(0).outputEnable(false);

			if (CH_NUM > 1) {
				g_savedOE.CH2_OE = Channel::get(1).isOutputEnabled() ? 1 : 0;
				Channel::get(1).outputEnable(false);
			}
		}
		g_savedOE.OE_SAVED = 1;
	}
}

void Channel::restoreOE() {
	if (g_savedOE.OE_SAVED) {
		if (CH_NUM > 0) {
			Channel::get(0).outputEnable(g_savedOE.CH1_OE ? true : false);
			if (CH_NUM > 1) {
				Channel::get(1).outputEnable(g_savedOE.CH2_OE ? true : false);
			}
		}
		g_savedOE.OE_SAVED = 0;
	}
}

////////////////////////////////////////////////////////////////////////////////

#ifdef EEZ_PSU_SIMULATOR

void Channel::Simulator::setLoadEnabled(bool value) {
    load_enabled = value;
}

bool Channel::Simulator::getLoadEnabled() {
    return load_enabled;
}

void Channel::Simulator::setLoad(float value) {
    load = value;
    profile::save();
}

float Channel::Simulator::getLoad() {
    return load;
}

void Channel::Simulator::setVoltProgExt(float value) {
	voltProgExt = value;
	profile::save();
}

float Channel::Simulator::getVoltProgExt() {
	return voltProgExt;
}

#endif

////////////////////////////////////////////////////////////////////////////////

Channel::Channel(
    uint8_t index_,
    uint8_t boardRevision_, uint8_t ioexp_iodir_, uint8_t ioexp_gpio_init_, uint8_t IO_BIT_OUT_SET_100_PERCENT_, uint8_t IO_BIT_OUT_EXT_PROG_,
    uint8_t isolator_pin_, uint8_t ioexp_pin_, uint8_t convend_pin_, uint8_t adc_pin_, uint8_t dac_pin_,
#if EEZ_PSU_SELECTED_REVISION == EEZ_PSU_REVISION_R1B9
    uint8_t bp_led_out_plus_, uint8_t bp_led_out_minus_, uint8_t bp_led_sense_plus_, uint8_t bp_led_sense_minus_, uint8_t bp_relay_sense_,
#elif EEZ_PSU_SELECTED_REVISION == EEZ_PSU_REVISION_R3B4
    uint8_t bp_led_out_, uint8_t bp_led_sense_, uint8_t bp_relay_sense_, uint8_t bp_led_prog_,
#endif
    uint8_t cc_led_pin_, uint8_t cv_led_pin_,
    float U_MIN_, float U_DEF_, float U_MAX_, float U_MIN_STEP_, float U_DEF_STEP_, float U_MAX_STEP_, float U_CAL_VAL_MIN_, float U_CAL_VAL_MID_, float U_CAL_VAL_MAX_, float U_CURR_CAL_,
    bool OVP_DEFAULT_STATE_, float OVP_MIN_DELAY_, float OVP_DEFAULT_DELAY_, float OVP_MAX_DELAY_,
    float I_MIN_, float I_DEF_, float I_MAX_, float I_MIN_STEP_, float I_DEF_STEP_, float I_MAX_STEP_, float I_CAL_VAL_MIN_, float I_CAL_VAL_MID_, float I_CAL_VAL_MAX_, float I_VOLT_CAL_,
    bool OCP_DEFAULT_STATE_, float OCP_MIN_DELAY_, float OCP_DEFAULT_DELAY_, float OCP_MAX_DELAY_,
    bool OPP_DEFAULT_STATE_, float OPP_MIN_DELAY_, float OPP_DEFAULT_DELAY_, float OPP_MAX_DELAY_, float OPP_MIN_LEVEL_, float OPP_DEFAULT_LEVEL_, float OPP_MAX_LEVEL_,
    float SOA_VIN_, float SOA_PREG_CURR_, float SOA_POSTREG_PTOT_, float PTOT_
    )
    :
    index(index_),
    boardRevision(boardRevision_), ioexp_iodir(ioexp_iodir_), ioexp_gpio_init(ioexp_gpio_init_),
    isolator_pin(isolator_pin_), ioexp_pin(ioexp_pin_), convend_pin(convend_pin_), adc_pin(adc_pin_), dac_pin(dac_pin_),
#if EEZ_PSU_SELECTED_REVISION == EEZ_PSU_REVISION_R1B9
    bp_led_out_plus(bp_led_out_plus_), bp_led_out_minus(bp_led_out_minus_), bp_led_sense_plus(bp_led_sense_plus_), bp_led_sense_minus(bp_led_sense_minus_), bp_relay_sense(bp_relay_sense_),
#elif EEZ_PSU_SELECTED_REVISION == EEZ_PSU_REVISION_R3B4
    bp_led_out(bp_led_out_), bp_led_sense(bp_led_sense_), bp_relay_sense(bp_relay_sense_), bp_led_prog(bp_led_prog_),
#endif
    cc_led_pin(cc_led_pin_), cv_led_pin(cv_led_pin_),
    U_MIN(U_MIN_), U_DEF(U_DEF_), U_MAX(U_MAX_), U_MIN_STEP(U_MIN_STEP_), U_DEF_STEP(U_DEF_STEP_), U_MAX_STEP(U_MAX_STEP_), U_CAL_VAL_MIN(U_CAL_VAL_MIN_), U_CAL_VAL_MID(U_CAL_VAL_MID_), U_CAL_VAL_MAX(U_CAL_VAL_MAX_), U_CURR_CAL(U_CURR_CAL_),
    OVP_DEFAULT_STATE(OVP_DEFAULT_STATE_), OVP_MIN_DELAY(OVP_MIN_DELAY_), OVP_DEFAULT_DELAY(OVP_DEFAULT_DELAY_), OVP_MAX_DELAY(OVP_MAX_DELAY_),
    I_MIN(I_MIN_), I_DEF(I_DEF_), I_MAX(I_MAX_), I_MIN_STEP(I_MIN_STEP_), I_DEF_STEP(I_DEF_STEP_), I_MAX_STEP(I_MAX_STEP_), I_CAL_VAL_MIN(I_CAL_VAL_MIN_), I_CAL_VAL_MID(I_CAL_VAL_MID_), I_CAL_VAL_MAX(I_CAL_VAL_MAX_), I_VOLT_CAL(I_VOLT_CAL_),
    OCP_DEFAULT_STATE(OCP_DEFAULT_STATE_), OCP_MIN_DELAY(OCP_MIN_DELAY_), OCP_DEFAULT_DELAY(OCP_DEFAULT_DELAY_), OCP_MAX_DELAY(OCP_MAX_DELAY_),
    OPP_DEFAULT_STATE(OPP_DEFAULT_STATE_), OPP_MIN_DELAY(OPP_MIN_DELAY_), OPP_DEFAULT_DELAY(OPP_DEFAULT_DELAY_), OPP_MAX_DELAY(OPP_MAX_DELAY_), OPP_MIN_LEVEL(OPP_MIN_LEVEL_), OPP_DEFAULT_LEVEL(OPP_DEFAULT_LEVEL_), OPP_MAX_LEVEL(OPP_MAX_LEVEL_),
    SOA_VIN(SOA_VIN_), SOA_PREG_CURR(SOA_PREG_CURR_), SOA_POSTREG_PTOT(SOA_POSTREG_PTOT_), PTOT(PTOT_),
    ioexp(*this, IO_BIT_OUT_SET_100_PERCENT_, IO_BIT_OUT_EXT_PROG_),
    adc(*this),
    dac(*this),
	onTimeCounter(index_)
{
}

void Channel::protectionEnter(ProtectionValue &cpv) {
    outputEnable(false);

    cpv.flags.tripped = 1;

    int bit_mask = reg_get_ques_isum_bit_mask_for_channel_protection_value(this, cpv);
    setQuesBits(bit_mask, true);

	int16_t eventId = event_queue::EVENT_ERROR_CH1_OVP_TRIPPED + 3 * (index - 1);

	if (IS_OVP_VALUE(this, cpv)) {
        doRemoteProgrammingEnable(false);
    } else if (IS_OCP_VALUE(this, cpv)) {
		eventId += 1;
	} else {
		eventId += 2;
	}

	event_queue::pushEvent(eventId);
}

void Channel::protectionCheck(ProtectionValue &cpv) {
    bool state;
    bool condition;
    float delay;
    
    if (IS_OVP_VALUE(this, cpv)) {
        state = flags.rprog_enabled || prot_conf.flags.u_state;
		//condition = flags.cv_mode && (!flags.cc_mode || fabs(i.mon - i.set) >= CHANNEL_VALUE_PRECISION) && (prot_conf.u_level <= u.set);
		condition = util::greaterOrEqual(u.mon, prot_conf.u_level, CHANNEL_VALUE_PRECISION);
        delay = prot_conf.u_delay;
        delay -= PROT_DELAY_CORRECTION;
    }
    else if (IS_OCP_VALUE(this, cpv)) {
        state = prot_conf.flags.i_state;
        //condition = flags.cc_mode && (!flags.cv_mode || fabs(u.mon - u.set) >= CHANNEL_VALUE_PRECISION);
		condition = util::greaterOrEqual(i.mon, i.set, CHANNEL_VALUE_PRECISION);
        delay = prot_conf.i_delay;
        delay -= PROT_DELAY_CORRECTION;
    }
    else {
        state = prot_conf.flags.p_state;
        condition = u.mon * i.mon > prot_conf.p_level;
        delay = prot_conf.p_delay;
    }

    if (state && isOutputEnabled() && condition) {
        if (delay > 0) {
            if (cpv.flags.alarmed) {
                if (micros() - cpv.alarm_started >= delay * 1000000UL) {
                    cpv.flags.alarmed = 0;

                    if (IS_OVP_VALUE(this, cpv)) {
                        DebugTraceF("OVP condition: CV_MODE=%d, CC_MODE=%d, I DIFF=%d mA, I MON=%d mA", (int)flags.cv_mode, (int)flags.cc_mode, (int)(fabs(i.mon - i.set) * 1000), (int)(i.mon * 1000));
                    }
                    else if (IS_OCP_VALUE(this, cpv)) {
                        DebugTraceF("OCP condition: CC_MODE=%d, CV_MODE=%d, U DIFF=%d mV", (int)flags.cc_mode, (int)flags.cv_mode, (int)(fabs(u.mon - u.set) * 1000));
                    }

                    protectionEnter(cpv);
                }
            }
            else {
                cpv.flags.alarmed = 1;
                cpv.alarm_started = micros();
            }
        }
        else {
            if (IS_OVP_VALUE(this, cpv)) {
                DebugTraceF("OVP condition: CV_MODE=%d, CC_MODE=%d, I DIFF=%d mA", (int)flags.cv_mode, (int)flags.cc_mode, (int)(fabs(i.mon - i.set) * 1000));
            }
            else if (IS_OCP_VALUE(this, cpv)) {
                DebugTraceF("OCP condition: CC_MODE=%d, CV_MODE=%d, U DIFF=%d mV", (int)flags.cc_mode, (int)flags.cv_mode, (int)(fabs(u.mon - u.set) * 1000));
            }

			protectionEnter(cpv);
        }
    }
    else {
        cpv.flags.alarmed = 0;
    }
}

////////////////////////////////////////////////////////////////////////////////

bool Channel::init() {
    bool result = true;

    bool last_save_enabled = profile::enableSave(false);

	for (int i = 0; i < 3; ++i) {
		if (i > 0) {
			DebugTraceF("ioexp.init failed (%d)", i);
		}
	    result = ioexp.init();
		if (result) {
			break;
		}
	}

    result &= adc.init();
    result &= dac.init();
	onTimeCounter.init();

    profile::enableSave(last_save_enabled);

    return result;
}

void Channel::onPowerDown() {
    bool last_save_enabled = profile::enableSave(false);

    outputEnable(false);
    doRemoteSensingEnable(false);
    if (getFeatures() & CH_FEATURE_RPROG) {
        doRemoteProgrammingEnable(false);
    }
    if (getFeatures() & CH_FEATURE_LRIPPLE) {
        doLowRippleEnable(false);
    }

    profile::enableSave(last_save_enabled);
}

void Channel::reset() {
    flags.output_enabled = 0;
    flags.dp_on = 0;
    flags.sense_enabled = 0;
    flags.rprog_enabled = 0;

    flags.cv_mode = 0;
    flags.cc_mode = 0;

    flags.power_ok = 0;

    ovp.flags.tripped = 0;
    ovp.flags.alarmed = 0;

    ocp.flags.tripped = 0;
    ocp.flags.alarmed = 0;

    opp.flags.tripped = 0;
    opp.flags.alarmed = 0;

    // CAL:STAT ON if valid calibrating data for both voltage and current exists in the nonvolatile memory, otherwise OFF.
    flags.cal_enabled = isCalibrationExists();

    // OUTP:PROT:CLE OFF
    // [SOUR[n]]:VOLT:PROT:TRIP? 0
    // [SOUR[n]]:CURR:PROT:TRIP? 0
    // [SOUR[n]]:POW:PROT:TRIP? 0
    clearProtection();

    // [SOUR[n]]:VOLT:SENS INTernal
    doRemoteSensingEnable(false);

    if (getFeatures() & CH_FEATURE_RPROG) {
        // [SOUR[n]]:VOLT:PROG INTernal
        doRemoteProgrammingEnable(false);
    }

    if (getFeatures() & CH_FEATURE_LRIPPLE) {
        doLowRippleEnable(false);
    }

    // [SOUR[n]]:VOLT:PROT:DEL 
    // [SOUR[n]]:VOLT:PROT:STAT
    // [SOUR[n]]:CURR:PROT:DEL
    // [SOUR[n]]:CURR:PROT:STAT 
    // [SOUR[n]]:POW:PROT[:LEV]
    // [SOUR[n]]:POW:PROT:DEL
    // [SOUR[n]]:POW:PROT:STAT -> set all to default
    clearProtectionConf();

    // [SOUR[n]]:CURR
    // [SOUR[n]]:CURR:STEP
    // [SOUR[n]]:VOLT
    // [SOUR[n]]:VOLT:STEP -> set all to default
    u.init(U_DEF_STEP, U_MAX);
    i.init(I_DEF_STEP, I_MAX);

	i_max_limit = NAN;
	p_limit = PTOT;
}

void Channel::clearCalibrationConf() {
    cal_conf.flags.u_cal_params_exists = 0;
    cal_conf.flags.i_cal_params_exists = 0;

    cal_conf.u.min.dac = cal_conf.u.min.val = cal_conf.u.min.adc = U_CAL_VAL_MIN;
    cal_conf.u.mid.dac = cal_conf.u.mid.val = cal_conf.u.mid.adc = (U_CAL_VAL_MIN + U_CAL_VAL_MAX) / 2;
    cal_conf.u.max.dac = cal_conf.u.max.val = cal_conf.u.max.adc = U_CAL_VAL_MAX;

    cal_conf.i.min.dac = cal_conf.i.min.val = cal_conf.i.min.adc = I_CAL_VAL_MIN;
    cal_conf.i.mid.dac = cal_conf.i.mid.val = cal_conf.i.mid.adc = (I_CAL_VAL_MIN + I_CAL_VAL_MAX) / 2;
    cal_conf.i.max.dac = cal_conf.i.max.val = cal_conf.i.max.adc = I_CAL_VAL_MAX;

    strcpy(cal_conf.calibration_date, "");
    strcpy(cal_conf.calibration_remark, CALIBRATION_REMARK_INIT);
}

void Channel::clearProtectionConf() {
    prot_conf.flags.u_state = OVP_DEFAULT_STATE;
    prot_conf.flags.i_state = OCP_DEFAULT_STATE;
    prot_conf.flags.p_state = OPP_DEFAULT_STATE;

    prot_conf.u_delay = OVP_DEFAULT_DELAY;
    prot_conf.u_level = U_MAX;
    prot_conf.i_delay = OCP_DEFAULT_DELAY;
    prot_conf.p_delay = OPP_DEFAULT_DELAY;
    prot_conf.p_level = OPP_DEFAULT_LEVEL;
}

bool Channel::test() {
	bool last_save_enabled = profile::enableSave(false);

    outputEnable(false);
    doRemoteSensingEnable(false);
    if (getFeatures() & CH_FEATURE_RPROG) {
        doRemoteProgrammingEnable(false);
    }
    if (getFeatures() & CH_FEATURE_LRIPPLE) {
        doLowRippleEnable(false);
    }

	ioexp.test();
    adc.test();
    dac.test();

    if (isOk()) {
        setVoltage(U_DEF);
        setCurrent(I_DEF);
    }

    profile::enableSave(last_save_enabled);
    profile::save();

	return isOk();
}

bool Channel::isPowerOk() {
    return flags.power_ok;
}

bool Channel::isTestFailed() {
    return ioexp.test_result == psu::TEST_FAILED ||
        adc.test_result == psu::TEST_FAILED ||
        dac.test_result == psu::TEST_FAILED;
}

bool Channel::isTestOk() {
    return ioexp.test_result == psu::TEST_OK &&
        adc.test_result == psu::TEST_OK &&
        dac.test_result == psu::TEST_OK;
}

bool Channel::isOk() {
    return psu::isPowerUp() && isPowerOk() && isTestOk();
}

void Channel::tick(unsigned long tick_usec) {
    ioexp.tick(tick_usec);
    adc.tick(tick_usec);
	onTimeCounter.tick(tick_usec);

    if (getFeatures() & CH_FEATURE_LRIPPLE) {
		lowRippleCheck(tick_usec);
	}

    // turn off DP after delay
    if (delayed_dp_off && tick_usec - delayed_dp_off_start >= DP_OFF_DELAY_PERIOD * 1000000L) {
        delayed_dp_off = false;
        doDpEnable(false);
    }

	// If channel output is off then test PWRGOOD here, otherwise it is tested in Channel::event method.
#if !CONF_SKIP_PWRGOOD_TEST
	if (!isOutputEnabled() && psu::isPowerUp()) {
		testPwrgood(ioexp.readGpio());
	}
#endif
}

float Channel::remapAdcDataToVoltage(int16_t adc_data) {
    return util::remap((float)adc_data, (float)AnalogDigitalConverter::ADC_MIN, U_MIN, (float)AnalogDigitalConverter::ADC_MAX, U_MAX);
}

float Channel::remapAdcDataToCurrent(int16_t adc_data) {
    return util::remap((float)adc_data, (float)AnalogDigitalConverter::ADC_MIN, I_MIN, (float)AnalogDigitalConverter::ADC_MAX, I_MAX);
}

int16_t Channel::remapVoltageToAdcData(float value) {
    float adc_value = util::remap(value, U_MIN, (float)AnalogDigitalConverter::ADC_MIN, U_MAX, (float)AnalogDigitalConverter::ADC_MAX);
    return (int16_t)util::clamp(adc_value, (float)(-AnalogDigitalConverter::ADC_MAX - 1), (float)AnalogDigitalConverter::ADC_MAX);
}

int16_t Channel::remapCurrentToAdcData(float value) {
    float adc_value = util::remap(value, I_MIN, (float)AnalogDigitalConverter::ADC_MIN, I_MAX, (float)AnalogDigitalConverter::ADC_MAX);
    return (int16_t)util::clamp(adc_value, (float)(-AnalogDigitalConverter::ADC_MAX - 1), (float)AnalogDigitalConverter::ADC_MAX);
}

float Channel::readingToCalibratedValue(Value *cv, float mon_reading) {
    if (flags.cal_enabled) {
        if (cv == &u) {
            mon_reading = util::remap(mon_reading, cal_conf.u.min.adc, cal_conf.u.min.val, cal_conf.u.max.adc, cal_conf.u.max.val);
        }
        else {
            mon_reading = util::remap(mon_reading, cal_conf.i.min.adc, cal_conf.i.min.val, cal_conf.i.max.adc, cal_conf.i.max.val);
        }
    }
    return mon_reading;
}

void Channel::valueAddReading(Value *cv, float value) {
    cv->mon = readingToCalibratedValue(cv, value);
}

void Channel::valueAddReadingDac(Value *cv, float value) {
    cv->mon_dac = readingToCalibratedValue(cv, value);
}

#if CONF_DEBUG
extern int16_t debug::u_mon[CH_MAX];
extern int16_t debug::u_mon_dac[CH_MAX];
extern int16_t debug::i_mon[CH_MAX];
extern int16_t debug::i_mon_dac[CH_MAX];
#endif

void Channel::adcDataIsReady(int16_t data) {
    switch (adc.start_reg0) {
    case AnalogDigitalConverter::ADC_REG0_READ_U_MON:
#if CONF_DEBUG
        debug::u_mon[index - 1] = data;
#endif
        valueAddReading(&u, remapAdcDataToVoltage(data));
        adc.start(AnalogDigitalConverter::ADC_REG0_READ_I_MON);
        break;

    case AnalogDigitalConverter::ADC_REG0_READ_I_MON:
#if CONF_DEBUG
        debug::i_mon[index - 1] = data;
#endif
	    valueAddReading(&i, remapAdcDataToCurrent(data));

		if (isOutputEnabled()) {
            adc.start(AnalogDigitalConverter::ADC_REG0_READ_U_MON);
        } else {
            u.mon = 0;
            i.mon = 0;
			adc.start(AnalogDigitalConverter::ADC_REG0_READ_U_SET);
        }
        break;

    case AnalogDigitalConverter::ADC_REG0_READ_U_SET:
#if CONF_DEBUG
        debug::u_mon_dac[index - 1] = data;
#endif
        valueAddReadingDac(&u, remapAdcDataToVoltage(data));
        adc.start(AnalogDigitalConverter::ADC_REG0_READ_I_SET);
        break;

    case AnalogDigitalConverter::ADC_REG0_READ_I_SET:
#if CONF_DEBUG
        debug::i_mon_dac[index - 1] = data;
#endif
        valueAddReadingDac(&i, remapAdcDataToCurrent(data));
        if (isOutputEnabled()) {
            adc.start(AnalogDigitalConverter::ADC_REG0_READ_U_MON);
        }
        break;
    }
}

void Channel::updateCcAndCvSwitch() {
#if EEZ_PSU_SELECTED_REVISION == EEZ_PSU_REVISION_R1B9
    board::cvLedSwitch(this, isCvMode());
    board::ccLedSwitch(this, isCcMode());
#elif EEZ_PSU_SELECTED_REVISION == EEZ_PSU_REVISION_R3B4
    bp::cvLedSwitch(this, isCvMode());
    bp::ccLedSwitch(this, isCcMode());
#endif
}

void Channel::setCcMode(bool cc_mode) {
    cc_mode = cc_mode && isOutputEnabled();

    if (cc_mode != flags.cc_mode) {
        flags.cc_mode = cc_mode;

        setOperBits(OPER_ISUM_CC, cc_mode);
        setQuesBits(QUES_ISUM_VOLT, cc_mode);
    }
}

void Channel::setCvMode(bool cv_mode) {
    cv_mode = cv_mode && isOutputEnabled();

    if (cv_mode != flags.cv_mode) {
        flags.cv_mode = cv_mode;

        updateCcAndCvSwitch();

        setOperBits(OPER_ISUM_CV, cv_mode);
        setQuesBits(QUES_ISUM_CURR, cv_mode);
    }
}

void Channel::event(uint8_t gpio, int16_t adc_data) {
    if (!psu::isPowerUp()) return;

#if !CONF_SKIP_PWRGOOD_TEST
	testPwrgood(gpio);
#endif

	if (boardRevision == CH_BOARD_REVISION_R5B9) {
		unsigned rpol = !(gpio & (1 << IOExpander::IO_BIT_IN_RPOL));

		if (rpol != flags.rpol) {
			flags.rpol = rpol;
			setQuesBits(QUES_ISUM_RPOL, flags.rpol ? true : false);
		}

		if (rpol && isOutputEnabled()) {
			outputEnable(false);
			event_queue::pushEvent(event_queue::EVENT_ERROR_CH1_REMOTE_SENSE_REVERSE_POLARITY_DETECTED + index - 1);
			return;
		}
	}

    adcDataIsReady(adc_data);

    setCvMode(gpio & (1 << IOExpander::IO_BIT_IN_CV_ACTIVE) ? true : false);
    setCcMode(gpio & (1 << IOExpander::IO_BIT_IN_CC_ACTIVE) ? true : false);
    updateCcAndCvSwitch();

	protectionCheck(ovp);
	protectionCheck(ocp);
	protectionCheck(opp);
}

void Channel::adcReadMonDac() {
    adc.start(AnalogDigitalConverter::ADC_REG0_READ_U_SET);
    delay(ADC_TIMEOUT_MS * 2);
}

void Channel::adcReadAll() {
    if (isOutputEnabled()) {
        adc.start(AnalogDigitalConverter::ADC_REG0_READ_U_SET);
        delay(ADC_TIMEOUT_MS * 3);
    }
    else {
        adc.start(AnalogDigitalConverter::ADC_REG0_READ_U_MON);
        delay(ADC_TIMEOUT_MS * 4);
    }
}

void Channel::doDpEnable(bool enable) {
    // DP bit is active low
    ioexp.changeBit(IOExpander::IO_BIT_OUT_DP_ENABLE, !enable);
    setOperBits(OPER_ISUM_DP_OFF, !enable);
    flags.dp_on = enable;
}

extern int g_trt;

void Channel::doOutputEnable(bool enable) {
    if (enable && !isOk()) {
        return;
    }

	noInterrupts();

	ioexp.disableWrite();

	flags.output_enabled = enable;
	ioexp.changeBit(IOExpander::IO_BIT_OUT_OUTPUT_ENABLE, enable);
	setOperBits(OPER_ISUM_OE_OFF, !enable);
	bp::switchOutput(this, enable);

	if (enable) {
		if (getFeatures() & CH_FEATURE_LRIPPLE) {
			outputEnableStartTime = micros();
			delayLowRippleCheck = true;
		}

		// enable DP
        delayed_dp_off = false;
		doDpEnable(true);
	} else {
		if (getFeatures() & CH_FEATURE_LRIPPLE) {
			doLowRippleEnable(false);
		}

		setCvMode(false);
        setCcMode(false);
        updateCcAndCvSwitch();

        if (calibration::isEnabled()) {
            calibration::stop();
        }

		// disable (delayed) DP
        delayed_dp_off = true;
        delayed_dp_off_start = micros();
	}

	interrupts();
	ioexp.enableWriteAndFlush();

	if (enable) {
		// start ADC conversion
		adc.start(AnalogDigitalConverter::ADC_REG0_READ_U_MON);

		onTimeCounter.start();
	} else {
		onTimeCounter.stop();
	}

}

void Channel::doRemoteSensingEnable(bool enable) {
    if (enable && !isOk()) {
        return;
    }
    flags.sense_enabled = enable;
    bp::switchSense(this, enable);
    setOperBits(OPER_ISUM_RSENS_ON, enable);
}

void Channel::doRemoteProgrammingEnable(bool enable) {
    if (enable && !isOk()) {
        return;
    }
    flags.rprog_enabled = enable;
    if (enable) {
        setVoltage(0);
        prot_conf.u_level = U_MAX;
		prot_conf.flags.u_state = true;
    }
    ioexp.changeBit(ioexp.IO_BIT_OUT_EXT_PROG, enable);
    bp::switchProg(this, enable);
    setOperBits(OPER_ISUM_RPROG_ON, enable);
}

bool Channel::isLowRippleAllowed(unsigned long tick_usec) {
	if (!isOutputEnabled()) {
		return false;
	}

	if (delayLowRippleCheck) {
		if (tick_usec - outputEnableStartTime < 100 * 1000L) {
			// If Auto low ripple is enabled, channel cannot enter low ripple mode
			// even if condition for that exist before pre-regulation is not activated
			// for a short period of time (100ms). Without this transition repetive
			// changing of channel's output mode with load connected WILL result with
			// post-regulator's power mosfet damage, and overheating of down-programmer mosfet!
			return false;
		}
		delayLowRippleCheck = false;
	}

	if (i.mon > SOA_PREG_CURR || i.mon > SOA_POSTREG_PTOT / (SOA_VIN - u.mon)) {
        return false;
    }

    if (i.mon * (SOA_VIN - u.mon) > SOA_POSTREG_PTOT) {
        return false;
    }

    return true;
}

void Channel::lowRippleCheck(unsigned long tick_usec) {
	if (isLowRippleAllowed(tick_usec)) {
		if (!flags.lripple_enabled) {
			if (flags.lripple_auto_enabled) {
				doLowRippleEnable(true);
			}
		}
	} else {
		if (flags.lripple_enabled) {
			doLowRippleEnable(false);
		}
	}
}

void Channel::doLowRippleEnable(bool enable) {
    flags.lripple_enabled = enable;
    ioexp.changeBit(ioexp.IO_BIT_OUT_SET_100_PERCENT, !enable);
}

void Channel::doLowRippleAutoEnable(bool enable) {
    if (enable && !isOk()) {
        return;
    }
    flags.lripple_auto_enabled = enable;
}

void Channel::update() {
	if (!isOk()) {
		return;
	}

	bool last_save_enabled = profile::enableSave(false);

    setVoltage(u.set);
    setCurrent(i.set);
    doOutputEnable(flags.output_enabled);
    doRemoteSensingEnable(flags.sense_enabled);
    if (getFeatures() & CH_FEATURE_RPROG) {
        doRemoteProgrammingEnable(flags.rprog_enabled);
    }

    profile::enableSave(last_save_enabled);
}

void Channel::outputEnable(bool enable) {
    if (enable != flags.output_enabled) {
        doOutputEnable(enable);
		event_queue::pushEvent((enable ? event_queue::EVENT_INFO_CH1_OUTPUT_ENABLED :
			event_queue::EVENT_INFO_CH1_OUTPUT_DISABLED) + index - 1);
		profile::save();
    }
}

bool Channel::isOutputEnabled() {
    return psu::isPowerUp() && flags.output_enabled;
}

void Channel::calibrationEnable(bool enable) {
    if (enable != flags.cal_enabled) {
	    flags.cal_enabled = enable;
		event_queue::pushEvent((enable ? event_queue::EVENT_INFO_CH1_CALIBRATION_ENABLED :
			event_queue::EVENT_WARNING_CH1_CALIBRATION_DISABLED) + index - 1);
		profile::save();
    }
}

bool Channel::isCalibrationEnabled() {
	return flags.cal_enabled;
}


void Channel::remoteSensingEnable(bool enable) {
    if (enable != flags.sense_enabled) {
        doRemoteSensingEnable(enable);
		event_queue::pushEvent((enable ? event_queue::EVENT_INFO_CH1_REMOTE_SENSE_ENABLED :
			event_queue::EVENT_INFO_CH1_REMOTE_SENSE_DISABLED) + index - 1);
        profile::save();
    }
}

bool Channel::isRemoteSensingEnabled() {
    return flags.sense_enabled;
}

void Channel::remoteProgrammingEnable(bool enable) {
    if (enable != flags.rprog_enabled) {
        doRemoteProgrammingEnable(enable);
		event_queue::pushEvent((enable ? event_queue::EVENT_INFO_CH1_REMOTE_PROG_ENABLED :
			event_queue::EVENT_INFO_CH1_REMOTE_PROG_DISABLED) + index - 1);
		profile::save();
    }
}

bool Channel::isRemoteProgrammingEnabled() {
    return flags.rprog_enabled;
}

bool Channel::lowRippleEnable(bool enable) {
    if (enable != flags.lripple_enabled) {
		if (enable && !isLowRippleAllowed(micros())) {
			return false;
		}
		doLowRippleEnable(enable);
    }
    return true;
}

bool Channel::isLowRippleEnabled() {
    return flags.lripple_enabled;
}

void Channel::lowRippleAutoEnable(bool enable) {
    if (enable != flags.lripple_auto_enabled) {
        doLowRippleAutoEnable(enable);
    }
}

bool Channel::isLowRippleAutoEnabled() {
    return flags.lripple_auto_enabled;
}

void Channel::setVoltage(float value) {
    u.set = value;
    u.mon_dac = 0;

    if (prot_conf.u_level < u.set) {
        prot_conf.u_level = u.set;
    }

    if (flags.cal_enabled) {
        value = util::remap(value, cal_conf.u.min.val, cal_conf.u.min.dac, cal_conf.u.max.val, cal_conf.u.max.dac);
    }
    dac.set_voltage(value);

    profile::save();
}

void Channel::setCurrent(float value) {
    i.set = value;
    i.mon_dac = 0;

    if (flags.cal_enabled) {
        value = util::remap(value, cal_conf.i.min.val, cal_conf.i.min.dac, cal_conf.i.max.val, cal_conf.i.max.dac);
    }
    dac.set_current(value);

    profile::save();
}

bool Channel::isCalibrationExists() {
    return cal_conf.flags.i_cal_params_exists && cal_conf.flags.u_cal_params_exists;
}

bool Channel::isTripped() {
    return ovp.flags.tripped ||
        ocp.flags.tripped ||
        opp.flags.tripped ||
        temperature::isChannelTripped(this);
}

void Channel::clearProtection() {
    ovp.flags.tripped = 0;
    ovp.flags.alarmed = 0;
    setQuesBits(QUES_ISUM_OVP, false);

    ocp.flags.tripped = 0;
    ocp.flags.alarmed = 0;
    setQuesBits(QUES_ISUM_OCP, false);

    opp.flags.tripped = 0;
    opp.flags.alarmed = 0;
    setQuesBits(QUES_ISUM_OPP, false);

	temperature::clearChannelProtection(this);
}

void Channel::disableProtection() {
	if (!isTripped()) {
		prot_conf.flags.u_state = 0;
		prot_conf.flags.i_state = 0;
		prot_conf.flags.p_state = 0;
		temperature::disableChannelProtection(this);
	}
}

void Channel::setQuesBits(int bit_mask, bool on) {
    reg_set_ques_isum_bit(&serial::scpi_context, this, bit_mask, on);
#if OPTION_ETHERNET
	if (ethernet::test_result == psu::TEST_OK) {
        reg_set_ques_isum_bit(&ethernet::scpi_context, this, bit_mask, on);
	}
#endif
}

void Channel::setOperBits(int bit_mask, bool on) {
    reg_set_oper_isum_bit(&serial::scpi_context, this, bit_mask, on);
#if OPTION_ETHERNET
	if (ethernet::test_result == psu::TEST_OK) {
        reg_set_oper_isum_bit(&ethernet::scpi_context, this, bit_mask, on);
	}
#endif
}

char *Channel::getCvModeStr() {
    if (isCvMode()) return "CV";
    else if (isCcMode()) return "CC";
    else return "UR";
}

const char *Channel::getBoardRevisionName() {
    return CH_BOARD_REVISION_NAMES[boardRevision];
}

uint16_t Channel::getFeatures() {
    return CH_BOARD_REVISION_FEATURES[boardRevision];
}

float Channel::getVoltageLimit() const {
	return u.limit;
}

float Channel::getVoltageMaxLimit() const {
	return U_MAX;
}

void Channel::setVoltageLimit(float limit) {
	u.limit = limit;
	if (u.set > u.limit) {
		setVoltage(u.limit);
	}
	profile::save();
}

float Channel::getCurrentLimit() const {
	return i.limit;
}

float Channel::getCurrentMaxLimit() const {
	float limit = psu::getCurrentMaxLimit();
	if (!util::isNaN(limit)) {
		return limit;
	}
	if (!util::isNaN(i_max_limit)) {
		return i_max_limit;
	}
	return I_MAX;
}

void Channel::setCurrentMaxLimit(float value) {
	if (!util::isNaN(value) && isOutputEnabled() && i.mon > value) {
		setCurrent(0);
	}

	if (i_max_limit != value) {
		i_max_limit = value;

		float max_limit = getCurrentMaxLimit();
		if (max_limit < i.limit) {
			setCurrentLimit(max_limit);
		}
	}
}

void Channel::setCurrentLimit(float limit) {
	i.limit = limit;
	if (i.set > i.limit) {
		setCurrent(i.limit);
	}
	profile::save();
}

float Channel::getPowerLimit() const {
	return p_limit;
}

float Channel::getPowerMaxLimit() const {
	return PTOT;
}

void Channel::setPowerLimit(float limit) {
	p_limit = limit;
	if (u.set * i.set > p_limit) {
		//setVoltage(p_limit / i.set);
		setCurrent(p_limit / u.set);
	}
	profile::save();
}

#if !CONF_SKIP_PWRGOOD_TEST
void Channel::testPwrgood(uint8_t gpio) {
    if (!(gpio & (1 << IOExpander::IO_BIT_IN_PWRGOOD))) {
        DebugTraceF("Ch%d PWRGOOD bit changed to 0", index);
        flags.power_ok = 0;
        psu::generateError(SCPI_ERROR_CH1_FAULT_DETECTED - (index - 1));
        psu::powerDownBySensor();
        return;
    }
}
#endif

}
} // namespace eez::psu