/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_I2C_BD71828_H
#define HW_I2C_BD71828_H

#include "hw/i2c/i2c.h"

#define TYPE_BD71828 "bd71828"
OBJECT_DECLARE_SIMPLE_TYPE(BD71828State, BD71828)

/* Press and release the machine's PMIC power button (/machine/pmic). */
void bd71828_power_button_click(void);

#endif
