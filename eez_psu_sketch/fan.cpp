/*
 * EEZ PSU Firmware
 * Copyright (C) 2016-present, Envox d.o.o.
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
#include "temperature.h"
#include "scpi_psu.h"

#if EEZ_PSU_SELECTED_REVISION == EEZ_PSU_REVISION_R3B4

#include "fan.h"

namespace eez {
namespace psu {
namespace fan {

enum RpmMeasureState {
    RPM_MEASURE_STATE_START,
    RPM_MEASURE_STATE_MEASURE_T1,
    RPM_MEASURE_STATE_MEASURE_T2,
    RPM_MEASURE_STATE_MEASURED,
    RPM_MEASURE_STATE_FINISHED
};

////////////////////////////////////////////////////////////////////////////////

TestResult test_result = psu::TEST_FAILED;

static unsigned long g_testStartTime;

static int g_fanSpeedPWM = 0;
static float g_fanSpeed = FAN_MIN_PWM;

static unsigned long g_fanSpeedLastMeasuredTick = 0;
static unsigned long g_fanSpeedLastAdjustedTick = 0;

volatile int g_rpm = 0;

static int g_rpmMeasureInterruptNumber;
static volatile RpmMeasureState g_rpmMeasureState = RPM_MEASURE_STATE_FINISHED;
static unsigned long g_rpmMeasureT1;

////////////////////////////////////////////////////////////////////////////////

int dt_to_rpm(unsigned long dt) {
    dt *= 2; // duty cycle is 50%
    dt *= 2; // 2 impulse per revolution
    return (int)(60L * 1000 * 1000 / dt);
}

int pwm_to_rpm(int pwm) {
    return (int)util::remap((float)pwm, FAN_MIN_PWM, 0, FAN_MAX_PWM, FAN_NOMINAL_RPM);
}

////////////////////////////////////////////////////////////////////////////////

void finish_rpm_measure();
void rpm_measure_interrupt_handler();

void start_rpm_measure() {
    analogWrite(FAN_PWM, 255);
    delay(2);
    attachInterrupt(g_rpmMeasureInterruptNumber, rpm_measure_interrupt_handler, CHANGE);
    g_rpmMeasureState = RPM_MEASURE_STATE_START;

#ifdef EEZ_PSU_SIMULATOR
    g_rpmMeasureState = RPM_MEASURE_STATE_MEASURED;
    g_rpm = pwm_to_rpm(g_fanSpeedPWM);
#endif
}

void rpm_measure_interrupt_handler() {
    if (g_rpmMeasureState != RPM_MEASURE_STATE_FINISHED && g_rpmMeasureState != RPM_MEASURE_STATE_MEASURED) {
        // measure length of the square pulse
        int x = digitalRead(FAN_SENSE);
        if (g_rpmMeasureState == RPM_MEASURE_STATE_START && x) {
            g_rpmMeasureState = RPM_MEASURE_STATE_MEASURE_T1;
        } else if (g_rpmMeasureState == RPM_MEASURE_STATE_MEASURE_T1 && !x) {
            // start is when signal goes from 1 to 0
            g_rpmMeasureT1 = micros();
            g_rpmMeasureState = RPM_MEASURE_STATE_MEASURE_T2;
        } else if (g_rpmMeasureState == RPM_MEASURE_STATE_MEASURE_T2 && x) {
            // stop is when signal goes from 0 to 1
            unsigned long rpmMeasureT2 = micros();
            int rpm = dt_to_rpm(rpmMeasureT2 - g_rpmMeasureT1);
            if (rpm > (int)ceil(FAN_NOMINAL_RPM * 1.05)) {
                // invalid RPM
                //// measure again
                //g_rpmMeasureState = RPM_MEASURE_STATE_MEASURE_T1;
            } else {
                g_rpm = rpm;
            }
            g_rpmMeasureState = RPM_MEASURE_STATE_MEASURED;
        }
    }
}

void finish_rpm_measure() {
    if (g_rpmMeasureState == RPM_MEASURE_STATE_MEASURED) {
        analogWrite(FAN_PWM, g_fanSpeedPWM);
        g_rpmMeasureState = RPM_MEASURE_STATE_FINISHED;
        detachInterrupt(g_rpmMeasureInterruptNumber);
    }
}

////////////////////////////////////////////////////////////////////////////////

void init() {
    g_rpmMeasureInterruptNumber = digitalPinToInterrupt(FAN_SENSE);
    SPI_usingInterrupt(g_rpmMeasureInterruptNumber);
    //attachInterrupt(g_rpmMeasureInterruptNumber, rpm_measure_interrupt_handler, CHANGE);
}

void test_start() {
    if (OPTION_FAN) {
        analogWrite(FAN_PWM, FAN_MAX_PWM);
        g_testStartTime = millis();
    }
}

bool test() {
    if (OPTION_FAN) {
        unsigned long time_since_test_start = millis() - g_testStartTime;
        if (time_since_test_start < 250) {
            delay(300 - time_since_test_start);
        }

#ifdef EEZ_PSU_SIMULATOR
        int saved_fan_speed_pwm = g_fanSpeedPWM;
        g_fanSpeedPWM = FAN_MAX_PWM;
#endif

        start_rpm_measure();

#ifdef EEZ_PSU_SIMULATOR
        g_fanSpeedPWM = saved_fan_speed_pwm;
#endif

        for (int i = 0; i < 25 && g_rpmMeasureState != RPM_MEASURE_STATE_MEASURED; ++i) {
            delay(1);
        }

        if (g_rpmMeasureState == RPM_MEASURE_STATE_MEASURED) {
            finish_rpm_measure();
            test_result = psu::TEST_OK;
            DebugTraceF("Fan RPM: %d", g_rpm);
        } else {
            // measure timeout, interrupt measurement
            g_rpmMeasureState = RPM_MEASURE_STATE_MEASURED;
            g_rpm = 0;
            finish_rpm_measure();
            test_result = psu::TEST_FAILED;
        }
    } else {
        test_result = psu::TEST_SKIPPED;
    }

    if (test_result == psu::TEST_FAILED) {
        psu::generateError(SCPI_ERROR_FAN_TEST_FAILED);
        psu::setQuesBits(QUES_FAN, true);
        psu::limitMaxCurrent(MAX_CURRENT_LIMIT_CAUSE_FAN);
    } else {
        psu::unlimitMaxCurrent();
    }

    return test_result != psu::TEST_FAILED;
}

void tick(unsigned long tick_usec) {
    if (test_result != psu::TEST_OK) {
        return;
    }

    // adjust fan speed depending on max. channel temperature
    if (tick_usec - g_fanSpeedLastAdjustedTick >= FAN_SPEED_ADJUSTMENT_INTERVAL * 1000L) {
        float max_channel_temperature = temperature::getMaxChannelTemperature();
        //DebugTraceF("max_channel_temperature: %f", max_channel_temperature);

        float fanSpeedNew = util::remap(max_channel_temperature, FAN_MIN_TEMP, FAN_MIN_PWM, FAN_MAX_TEMP, FAN_MAX_PWM);
        if (fanSpeedNew >= FAN_MIN_PWM) {
            if (g_fanSpeed == 0) {
                g_fanSpeed = FAN_MIN_PWM;
            } 
            g_fanSpeed = g_fanSpeed + 0.1f * (fanSpeedNew - g_fanSpeed);
        } else {
            g_fanSpeed = 0;
        }

        int newFanSpeedPWM = (int)g_fanSpeed;
        if (newFanSpeedPWM < FAN_MIN_PWM) {
            newFanSpeedPWM = 0;
        } else if (newFanSpeedPWM > FAN_MAX_PWM) {
            newFanSpeedPWM = FAN_MAX_PWM;
        }

        if (newFanSpeedPWM != g_fanSpeedPWM) {
            g_fanSpeedPWM = newFanSpeedPWM;

            if (g_fanSpeedPWM > 0) {
                //DebugTraceF("fanSpeed PWM: %d", g_fanSpeedPWM);
    
                g_fanSpeedLastMeasuredTick = tick_usec;
            } else {
                //DebugTrace("fanSpeed OFF");
            }

            if (g_rpmMeasureState == RPM_MEASURE_STATE_FINISHED) {
                analogWrite(FAN_PWM, g_fanSpeedPWM);
            }
        }

        g_fanSpeedLastAdjustedTick = tick_usec;
    }

    // measure fan speed
#if FAN_OPTION_RPM_MEASUREMENT
    if (g_fanSpeedPWM != 0) {
        if (tick_usec - g_fanSpeedLastMeasuredTick >= FAN_SPEED_MEASURMENT_INTERVAL * 1000L) {
            g_fanSpeedLastMeasuredTick = tick_usec;
            start_rpm_measure();
        } else {
            RpmMeasureState rpmMeasureState = g_rpmMeasureState;
            if (rpmMeasureState == RPM_MEASURE_STATE_MEASURED) {
                finish_rpm_measure();
                //DebugTraceF("RPM=%d", g_rpm);
            } else if (rpmMeasureState != RPM_MEASURE_STATE_FINISHED) {
                if (tick_usec - g_fanSpeedLastMeasuredTick >= 50 * 1000L) {
                    // measure timeout, interrupt measurement
                    g_rpmMeasureState = RPM_MEASURE_STATE_MEASURED;
                    g_rpm = 0;
                    finish_rpm_measure();

                    if (g_fanSpeedPWM != 0) {
                        test_result = psu::TEST_FAILED;
                        psu::generateError(SCPI_ERROR_FAN_TEST_FAILED);
                        psu::setQuesBits(QUES_FAN, true);
                        psu::limitMaxCurrent(MAX_CURRENT_LIMIT_CAUSE_FAN);
                    }
                }
            }
        }
    } else {
        g_rpm = 0;
    }
#else
    g_rpm = 0;
#endif
}

}
}
} // namespace eez::psu::fan

#endif