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
#include "temp_sensor.h"
#include "scpi_regs.h"

namespace eez {
namespace psu {

using namespace scpi;

namespace temp_sensor {

#define TEMP_SENSOR(NAME, INSTALLED, PIN, CAL_POINTS, CH_NUM, QUES_REG_BIT, SCPI_ERROR) \
	TempSensor(#NAME, INSTALLED, PIN, CAL_POINTS, CH_NUM, QUES_REG_BIT, SCPI_ERROR)

TempSensor sensors[NUM_TEMP_SENSORS] = {
	TEMP_SENSORS
};

#undef TEMP_SENSOR

////////////////////////////////////////////////////////////////////////////////

TempSensor::TempSensor(const char *name_, int installed_, int pin_, float p1_volt_, float p1_cels_, float p2_volt_, float p2_cels_, int ch_num_, int ques_bit_, int scpi_error_)
	: name(name_)
	, installed(installed_)
	, pin(pin_)
	, p1_volt(p1_volt_)
	, p1_cels(p1_cels_)
	, p2_volt(p2_volt_)
	, p2_cels(p2_cels_)
	, ch_num(ch_num_)
	, ques_bit(ques_bit_)
	, scpi_error(scpi_error_)
{
}

void TempSensor::init() {
}

bool TempSensor::test() {
	if (installed) {
		if (read() > TEMP_SENSOR_MIN_VALID_TEMPERATURE) {
			test_result = psu::TEST_OK;
		} else {
			test_result = psu::TEST_FAILED;
		}
	} else {
		test_result = psu::TEST_SKIPPED;
	}

    if (test_result == psu::TEST_FAILED) {
		if (ch_num >= 0) {
			// set channel current max. limit to ERR_MAX_CURRENT if sensor is faulty
			Channel::get(ch_num).limitMaxCurrent(MAX_CURRENT_LIMIT_CAUSE_TEMPERATURE);
		}

		psu::generateError(scpi_error);
    } else {
		if (ch_num >= 0) {
			Channel::get(ch_num).unlimitMaxCurrent();
		}
	}

	return test_result != psu::TEST_FAILED;
}

float TempSensor::read() {
	if (installed) {
		float value = (float)analogRead(pin);
		value = util::remap(value, (float)MIN_ADC, (float)MIN_U, (float)MAX_ADC, (float)MAX_U);
		value = util::remap(value, p1_volt, p1_cels, p2_volt, p2_cels);

		if (value <= TEMP_SENSOR_MIN_VALID_TEMPERATURE) {
			test_result = psu::TEST_FAILED;

			if (ch_num >= 0) {
				// set channel current max. limit to ERR_MAX_CURRENT if sensor is faulty
				Channel::get(ch_num).limitMaxCurrent(MAX_CURRENT_LIMIT_CAUSE_TEMPERATURE);
			}

			psu::generateError(scpi_error);
		}

		return value;
	}

	return NAN;
}

}
}
} // namespace eez::psu::temp_sensor