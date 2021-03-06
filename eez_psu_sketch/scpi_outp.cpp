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
#include "scpi_psu.h"
#include "scpi_outp.h"

#include "calibration.h"
#include "channel_dispatcher.h"

namespace eez {
namespace psu {
namespace scpi {

////////////////////////////////////////////////////////////////////////////////

scpi_result_t scpi_outp_ModeQ(scpi_t *context) {
    Channel *channel = param_channel(context);
    if (!channel) {
        return SCPI_RES_ERR;
    }

    SCPI_ResultText(context, channel->getCvModeStr());

    return SCPI_RES_OK;
}

scpi_result_t scpi_outp_ProtectionClear(scpi_t * context) {
    Channel *channel = param_channel(context);
    if (!channel) {
        return SCPI_RES_ERR;
    }

    channel_dispatcher::clearProtection(*channel);

    return SCPI_RES_OK;
}

scpi_result_t scpi_outp_State(scpi_t * context) {
    bool enable;
    if (!SCPI_ParamBool(context, &enable, TRUE)) {
        return SCPI_RES_ERR;
    }

    Channel *channel = param_channel(context);
    if (!channel) {
        return SCPI_RES_ERR;
    }

    if (enable != channel->isOutputEnabled()) {
        if (enable) {
            if (channel_dispatcher::isTripped(*channel)) {
                SCPI_ErrorPush(context, SCPI_ERROR_CANNOT_EXECUTE_BEFORE_CLEARING_PROTECTION);
                return SCPI_RES_OK;
            }
        }
        else {
            if (calibration::isEnabled()) {
                SCPI_ErrorPush(context, SCPI_ERROR_CAL_OUTPUT_DISABLED);
            }
        }

        channel_dispatcher::outputEnable(*channel, enable);
    }

    return SCPI_RES_OK;
}

scpi_result_t scpi_outp_StateQ(scpi_t * context) {
    Channel *channel = param_channel(context);
    if (!channel) {
        return SCPI_RES_ERR;
    }

    SCPI_ResultBool(context, channel->isOutputEnabled());

    return SCPI_RES_OK;
}

scpi_result_t scpi_outp_TrackState(scpi_t * context) {
    if (channel_dispatcher::isCoupled()) {
        SCPI_ErrorPush(context, SCPI_ERROR_EXECUTE_ERROR_CHANNELS_ARE_COUPLED);
        return SCPI_RES_ERR;
    }

    bool enable;
    if (!SCPI_ParamBool(context, &enable, TRUE)) {
        return SCPI_RES_ERR;
    }

    Channel *channel = param_channel(context);
    if (!channel) {
        return SCPI_RES_ERR;
    }

    if (enable != channel_dispatcher::isTracked()) {
        channel_dispatcher::setType(enable ? channel_dispatcher::TYPE_TRACKED : channel_dispatcher::TYPE_NONE);
    }

    return SCPI_RES_OK;
}

scpi_result_t scpi_outp_TrackStateQ(scpi_t * context) {
    if (channel_dispatcher::isCoupled()) {
        SCPI_ErrorPush(context, SCPI_ERROR_EXECUTE_ERROR_CHANNELS_ARE_COUPLED);
        return SCPI_RES_ERR;
    }

    Channel *channel = param_channel(context);
    if (!channel) {
        return SCPI_RES_ERR;
    }

    SCPI_ResultBool(context, channel_dispatcher::isTracked());

    return SCPI_RES_OK;
}

scpi_result_t scpi_outp_ProtectionCouple(scpi_t * context) {
    bool enable;
    if (!SCPI_ParamBool(context, &enable, TRUE)) {
        return SCPI_RES_ERR;
    }

	if (!persist_conf::enableOutputProtectionCouple(enable)) {
        SCPI_ErrorPush(context, SCPI_ERROR_EXECUTION_ERROR);
        return SCPI_RES_ERR;
	}

    return SCPI_RES_OK;
}

scpi_result_t scpi_outp_ProtectionCoupleQ(scpi_t * context) {
    SCPI_ResultBool(context, persist_conf::isOutputProtectionCoupleEnabled());

    return SCPI_RES_OK;
}

}
}
} // namespace eez::psu::scpi