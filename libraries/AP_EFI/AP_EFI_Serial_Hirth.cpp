/*
   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/


#include <AP_HAL/AP_HAL.h>

#include "AP_EFI_Serial_Hirth.h"

#if HAL_EFI_ENABLED
#include <AP_SerialManager/AP_SerialManager.h>
#include "SRV_Channel/SRV_Channel.h"


extern const AP_HAL::HAL& hal;

/**
 * @brief Constructor with port initialization
 * 
 * @param _frontend 
 */
AP_EFI_Serial_Hirth::AP_EFI_Serial_Hirth(AP_EFI &_frontend) : AP_EFI_Backend(_frontend) {
    port = AP::serialmanager().find_serial(AP_SerialManager::SerialProtocol_EFI, 0);
    throttle_scaling_factor = (get_throttle_max() - get_throttle_idle()) / 100;
    fuel_avg_config = get_ecu_fcr_average_count();
    fuel_avg_count = 0;
}


/**
 * @brief checks for response from or makes requests to Hirth ECU periodically
 * 
 */
void AP_EFI_Serial_Hirth::update() {
    bool status = false;
    uint32_t now = AP_HAL::millis();

    //log the delay in loop interval
    internal_state.loop_cnt = now - last_loop_ms;
    last_loop_ms = now;

    if ((port != nullptr)) {

        //waiting for response
        if (waiting_response) {

            // reset request if not response for SERIAL_WAIT_TIMEOUT-ms
            if (now - last_response_ms > SERIAL_WAIT_TIMEOUT) {
                waiting_response = false;
                last_response_ms = now;

                port->discard_input();

                internal_state.ack_fail_cnt++;
                if(data_send == 1) {internal_state.ack_s1++;}
                if(data_send == 2) {internal_state.ack_s2++;}
                if(data_send == 3) {internal_state.ack_s3++;}
                if(data_send == 5) {internal_state.ack_thr++;}
            }
            else{

                num_bytes = port->available();

                // if already requested
                if (num_bytes >= expected_bytes) {
                    
                    // read data from buffer
                    uint8_t computed_checksum = 0;
                    computed_checksum += res_data.quantity = port->read();
                    computed_checksum += res_data.code = port->read();

                    if (res_data.code == req_data.code) {
                        for (int i = 0; i < (res_data.quantity - QUANTITY_REQUEST_STATUS); i++) {
                            computed_checksum += raw_data[i] = port->read();
                        }
                    }

                    res_data.checksum = port->read();
                    computed_checksum = computed_checksum % BYTE_RANGE_MAX;
                    if (res_data.checksum != (BYTE_RANGE_MAX - computed_checksum)) {
                        internal_state.crc_fail_cnt++;
                        port->discard_input();
                    }
                    else {
                        internal_state.uptime = now - last_uptime;
                        last_uptime = now;
                        internal_state.last_updated_ms = now;
                        decode_data();
                        copy_to_frontend();
                    }

                    waiting_response = false;
                }
            }
        }
        
        // sending cmd
        if(!waiting_response) {

            // get new throttle value
            new_throttle = (uint16_t)SRV_Channels::get_output_scaled(SRV_Channel::k_throttle);

            // check for change or timeout for throttle value
            if ((new_throttle != old_throttle) || (now - last_req_send_throttle > 500)) {
                // if new throttle value, send throttle request
                // also send new throttle value, only when ARMED
                if (hal.util->persistent_data.armed) {
                    status = send_target_values((new_throttle * throttle_scaling_factor) + get_throttle_idle());
                }
                else{
                    status = send_target_values(0);
                }

                old_throttle = new_throttle;
                internal_state.k_throttle = new_throttle;
                last_req_send_throttle = now;
            }
            else {
                // request Status request, if no throttle commands
                status = send_request_status();
            }      

            if (status == true) {
                waiting_response = true;
                last_response_ms = now;
            }
        }
    }
}


/**
 * @brief updates the current quantity that will be expected
 * 
 */
void AP_EFI_Serial_Hirth::get_quantity() {
    switch (req_data.code)
    {
    case CODE_REQUEST_STATUS_1:
        expected_bytes = QUANTITY_RESPONSE_STATUS_1;
        break;
    case CODE_REQUEST_STATUS_2:
        expected_bytes = QUANTITY_RESPONSE_STATUS_2;
        break;
    case CODE_REQUEST_STATUS_3:
        expected_bytes = QUANTITY_RESPONSE_STATUS_3;
        break;
    case CODE_SET_VALUE:
        expected_bytes = QUANTITY_ACK_SET_VALUES;
        break;
    }
}


/**
 * @brief sends the new throttle command to Hirth ECU
 * 
 * @param thr - new throttle value given by SRV_Channel::k_throttle
 * @return true - if success
 * @return false - currently not implemented
 */
bool AP_EFI_Serial_Hirth::send_target_values(uint16_t thr) {
    int idx = 0;
    uint8_t computed_checksum = 0;

    // clear buffer 
    for (size_t i = 0; i < BYTE_RANGE_MAX; i++)
    {
        raw_data[i] = 0;
    }
    
    // Get throttle value from parameters config
    uint16_t throttle = thr * THROTTLE_POSITION_FACTOR; // * get_throttle_scale();

    // set Quantity + Code + "20 bytes of records to set" + Checksum
    computed_checksum += raw_data[idx++] = req_data.quantity = QUANTITY_SET_VALUE;
    computed_checksum += raw_data[idx++] = req_data.code = CODE_SET_VALUE;
    computed_checksum += raw_data[idx++] = throttle & 0xFF;
    computed_checksum += raw_data[idx++] = (throttle >> 0x08) & 0xFF;

    for (; idx < QUANTITY_SET_VALUE - 2; idx++) {
        // 0 has no impact on checksum
        raw_data[idx] = 0;
    }
    //checksum calculation
    computed_checksum = computed_checksum % BYTE_RANGE_MAX;
    raw_data[QUANTITY_SET_VALUE - 1] = (BYTE_RANGE_MAX - computed_checksum);

    //debug
    internal_state.packet_sent = 5;
    data_send = 5;
    
    expected_bytes = QUANTITY_ACK_SET_VALUES;
    //write data
    port->write(raw_data, QUANTITY_SET_VALUE);

    return true;
}


/**
 * @brief cyclically sends different Status requests to Hirth ECU
 * 
 * @return true - when successful
 * @return false  - not implemented
 */
bool AP_EFI_Serial_Hirth::send_request_status() {

    bool status = false;

    switch (req_data.code)
    {
    case CODE_REQUEST_STATUS_1:
        req_data.quantity = QUANTITY_REQUEST_STATUS;
        req_data.code = CODE_REQUEST_STATUS_2;
        req_data.checksum = CHECKSUM_REQUEST_STATUS_2;
        expected_bytes = QUANTITY_RESPONSE_STATUS_2;
        internal_state.packet_sent = 2;
        data_send =2;
        break;
    case CODE_REQUEST_STATUS_2:
        req_data.quantity = QUANTITY_REQUEST_STATUS;
        req_data.code = CODE_REQUEST_STATUS_3;
        req_data.checksum = CHECKSUM_REQUEST_STATUS_3;
        expected_bytes = QUANTITY_RESPONSE_STATUS_3;
        internal_state.packet_sent = 3;
        data_send =3;
        break;
    case CODE_REQUEST_STATUS_3:
        req_data.quantity = QUANTITY_REQUEST_STATUS;
        req_data.code = CODE_REQUEST_STATUS_1;
        req_data.checksum = CHECKSUM_REQUEST_STATUS_1;
        expected_bytes = QUANTITY_RESPONSE_STATUS_1;
        internal_state.packet_sent = 1;
        data_send =1;
        break;
    default:
        req_data.quantity = QUANTITY_REQUEST_STATUS;
        req_data.code = CODE_REQUEST_STATUS_1;
        req_data.checksum = CHECKSUM_REQUEST_STATUS_1;
        expected_bytes = QUANTITY_RESPONSE_STATUS_1;
        internal_state.packet_sent = 1;
        data_send =1;
        break;
    }
    //memcpy(&raw_data, 0, sizeof(raw_data));
    raw_data[0] = req_data.quantity;
    raw_data[1] = req_data.code;
    raw_data[2] = req_data.checksum;

    port->write(raw_data, QUANTITY_REQUEST_STATUS);

    status = true;

    return status;
}


/**
 * @brief parses the response from Hirth ECU and updates the internal state instance
 * 
 */
void AP_EFI_Serial_Hirth::decode_data() {
    int engine_status = 0;

    int excess_temp_error = 0;

    switch (res_data.code)
    {
    case CODE_REQUEST_STATUS_1:
        engine_status = (raw_data[8] | raw_data[9] << 0x08);

        //EFI2 Log
        internal_state.engine_state = (Engine_State)engine_status;
        internal_state.save_in_flash = raw_data[1] | raw_data[2] << 0x08;
        internal_state.engine_temperature_sensor_status = raw_data[82] & 0x01;
        internal_state.air_temperature_sensor_status = raw_data[82] & 0x02;
        internal_state.air_pressure_sensor_status = raw_data[82] & 0x04;
        internal_state.throttle_sensor_status = raw_data[82] & 0x08;
        internal_state.thr_pos = (raw_data[72] | raw_data[73] << 0x08);
        internal_state.air_temp = (raw_data[78] | raw_data[79] << 0x08);
        internal_state.eng_temp = (raw_data[74] | raw_data[75] << 0x08);
        //add in ADC voltage of MAP sensor > convert to MAP in kPa
        internal_state.converted_map = (raw_data[50] | raw_data[51] << 0x08) * ADC_CALIBRATION * MAP_HPA_PER_VOLT_FACTOR * HPA_TO_KPA; 
        //#define ADC_CALIBRATION 5.0/1024.0
        //#define MAP_HPA_PER_VOLT_FACTOR 248.2673
        //#define HPA_TO_KPA 0.1
        internal_state.battery_voltage = (raw_data[76] | raw_data[77] << 0x08) * VOLTAGE_RESOLUTION;

        //EFI1 Log
        internal_state.engine_speed_rpm = (raw_data[10] | raw_data[11] << 0x08);
        internal_state.cylinder_status->injection_time_ms = ((raw_data[32] | raw_data[33] << 0x08)) * INJECTION_TIME_RESOLUTION;
        internal_state.cylinder_status->ignition_timing_deg = (raw_data[34] | raw_data[35] << 0x08);
        //air temperature
        internal_state.cylinder_status->cylinder_head_temperature = (raw_data[78] | raw_data[79] << 0x08) + KELVIN_CONVERSION_CONSTANT;
        //engine temperature
        internal_state.cylinder_status->exhaust_gas_temperature = (raw_data[74] | raw_data[75] << 0x08) + KELVIN_CONVERSION_CONSTANT;
        internal_state.crankshaft_sensor_status = ((raw_data[82] & CRANK_SHAFT_SENSOR_OK) == CRANK_SHAFT_SENSOR_OK) ? Crankshaft_Sensor_Status::OK : Crankshaft_Sensor_Status::ERROR;     

        break;

    case CODE_REQUEST_STATUS_2:

        fuel_consumption_rate_raw = (raw_data[52] | raw_data[53] << 0x08) / FUEL_CONSUMPTION_RESOLUTION;
        internal_state.fuel_consumption_rate_raw = get_avg_fuel_consumption_rate(fuel_consumption_rate_raw);
        internal_state.fuel_consumption_rate_cm3pm = (fuel_consumption_rate_raw * get_ecu_fcr_slope()) + get_ecu_fcr_offset();

        total_fuel_consumed = total_fuel_consumed + internal_state.fuel_consumption_rate_cm3pm;
        internal_state.total_fuel_consumed = total_fuel_consumed;

        internal_state.throttle_position_percent = (raw_data[62] | raw_data[63] << 0x08) / THROTTLE_POSITION_RESOLUTION;
        
        //EFI2 Log
        // internal_state.no_of_log_data = raw_data[52] | raw_data[53] << 0x08 | raw_data[53] << 0x08 | raw_data[53] << 0x08
        break;

    case CODE_REQUEST_STATUS_3: // TBD - internal state addition
        excess_temp_error = (raw_data[46] | raw_data[47] << 0x08);

        //EFI2 Log
        internal_state.CHT_1_error_excess_temperature_status = (excess_temp_error & 0x01) || (excess_temp_error & 0x02) || (excess_temp_error & 0x04);
        internal_state.CHT_2_error_excess_temperature_status = (excess_temp_error & 0x08) || (excess_temp_error & 0x10) || (excess_temp_error & 0x20);
        internal_state.EGT_1_error_excess_temperature_status = (excess_temp_error & 0x40) || (excess_temp_error & 0x80) || (excess_temp_error & 0x100);
        internal_state.EGT_2_error_excess_temperature_status = (excess_temp_error & 0x200) || (excess_temp_error & 0x400) || (excess_temp_error & 0x800);
        internal_state.cht1_temp = (raw_data[16] | raw_data[17] << 0x08);
        internal_state.cht2_temp = (raw_data[18] | raw_data[19] << 0x08);
        internal_state.egt1_temp = (raw_data[20] | raw_data[21] << 0x08);
        internal_state.egt2_temp = (raw_data[22] | raw_data[23] << 0x08);
        break;
        
    // case CODE_SET_VALUE:
    //     // Do nothing for now
    //     break;
    }
}


float AP_EFI_Serial_Hirth::get_avg_fuel_consumption_rate(float fuel_consumed) {
    fuel_avg_count = (fuel_avg_count) % fuel_avg_config;
    float avg_fuel_consumed = 0;

    instance_fuel_reading[fuel_avg_count++] = fuel_consumed;
    for (int i = 0; i < fuel_avg_config; i++) {
        avg_fuel_consumed += instance_fuel_reading[i];
    }
    
    return (avg_fuel_consumed / fuel_avg_config);
}

#endif // HAL_EFI_ENABLED