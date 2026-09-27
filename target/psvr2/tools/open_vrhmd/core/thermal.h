#ifndef OPEN_VRHMD_THERMAL_H
#define OPEN_VRHMD_THERMAL_H

#include <stdint.h>

#define THERMAL_IOC_TYPE 'T'
struct thermal_get_from_id {
  int32_t in_bank_id;    /* [in] bank 0 */
  int32_t in_sensor_id;  /* [in] sensor 0-7 */
  int32_t out_temperature; /* [out] millidegrees C */
};
#define THERMAL_GET_FROM_ID_CMD 0x800C5401

#ifndef I2C_RDWR
#define I2C_RDWR 0x0707
#endif
struct i2c_msg_user {
  uint16_t addr;
  uint16_t flags;
  uint16_t len;
  uint8_t *buf;
};
struct i2c_rdwr_data {
  struct i2c_msg_user *msgs;
  uint32_t nmsgs;
};

int fan_ctrl_init(void);
int fan_ctrl_teardown(void);


_Static_assert(sizeof(struct thermal_get_from_id) == 12, "thermal request size");
#endif
