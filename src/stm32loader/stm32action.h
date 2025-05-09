#pragma once
#ifdef _WIN32
#include "../windows/SerialPort.h"
#else
#include "../mac/SerialPort.h"
#endif
#include <string>

int stm32action(SerialPort& port, std::string cmd);
