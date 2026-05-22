#include "device_manager.h"

DeviceManager::DeviceManager(mm::comm::FieldbusDriver& driver) : driver_(driver) {}

std::expected<void, std::string> DeviceManager::init() { return driver_.init(); }

void DeviceManager::pdoExchange() { driver_.exchangeProcessData(); }
