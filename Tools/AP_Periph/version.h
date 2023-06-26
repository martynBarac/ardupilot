#pragma once

#ifndef __AP_PERIPH_FW_VERSION__
#define __AP_PERIPH_FW_VERSION__

#define THISFIRMWARE "Ottano Carbopilot V4.3.4"

// the following line is parsed by the autotest scripts
#define FIRMWARE_VERSION 1,3,0,FIRMWARE_VERSION_TYPE_DEV

#ifndef FW_MAJOR
#define FW_MAJOR 1
#define FW_MINOR 3
#define FW_PATCH 0
#define FW_TYPE FIRMWARE_VERSION_TYPE_DEV
#endif

#endif
