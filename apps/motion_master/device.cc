#include "device.h"

Device::Device(uint16_t slavePosition, mm::comm::FieldbusDriver& driver)
    : slavePosition_(slavePosition), driver_(driver) {}

uint16_t Device::slavePosition() const { return slavePosition_; }
