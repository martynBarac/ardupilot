#pragma once

#include <GCS_MAVLink/GCS.h>
#include "GCS_Mavlink.h"

class GCS_Plane : public GCS
{
    friend class Plane;  // for access to _chan in parameter declarations

public:

    // return GCS link at offset ofs
    GCS_MAVLINK_Plane *chan(const uint8_t ofs) override {
        if (ofs >= _num_gcs) {
            return nullptr;
        }
        return (GCS_MAVLINK_Plane *)_chan[ofs];
    }
    const GCS_MAVLINK_Plane *chan(const uint8_t ofs) const override {
        if (ofs >= _num_gcs) {
            return nullptr;
        }
        return (GCS_MAVLINK_Plane *)_chan[ofs];
    }

protected:

    uint8_t sysid_this_mav() const override;
    void update_vehicle_sensor_status_flags(void) override;
    uint32_t custom_mode() const override;
    MAV_TYPE frame_type() const override;

    GCS_MAVLINK_Plane *new_gcs_mavlink_backend(GCS_MAVLINK_Parameters &params,
                                               AP_HAL::UARTDriver &uart) override {
        return new GCS_MAVLINK_Plane(params, uart);
    }

    AP_GPS::GPS_Status min_status_for_gps_healthy() const override {
        // NO_FIX simply excludes NO_GPS
        return AP_GPS::GPS_OK_FIX_3D;
    }
};
