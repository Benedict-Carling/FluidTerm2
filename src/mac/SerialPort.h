#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <termios.h>
#include "Main.h"

class SerialPort {
private:
    std::thread* m_thread;
    std::atomic<bool> m_threadRunning;
    bool m_direct = false;

    static void ThreadFn(void* pvParam);

    speed_t m_baud;
    int m_parity;
    int m_stopBits;
    int m_dataBits;

    std::string m_commName;
    int m_fd; // File descriptor for the serial port

public:
    SerialPort();
    virtual ~SerialPort();

    std::string m_portName;

    bool reOpenPort();

    void setDirect();
    void setIndirect();
    int timedRead(uint32_t ms);
    int timedRead(uint8_t* buf, size_t len, uint32_t ms);
    int timedRead(char* buf, size_t len, uint32_t ms);

    void flushInput();

    void setTimeout(unsigned int ms);

    int write(const char* data, size_t dwSize);
    int write(std::string s) { return write(s.c_str(), s.length()); }
    void write(char data) { write(&data, 1); }
    bool Init(std::string szPortName = "/dev/tty.usbserial", speed_t dwBaudRate = B115200, int byParity = 0, int byStopBits = 1, int byByteSize = 8);
    void getMode(speed_t& dwBaudRate, int& byByteSize, int& byParity, int& byStopBits);
    bool setMode(speed_t dwBaudRate, int byByteSize, int byParity, int byStopBits);

    void setRts(bool on);
    void setDtr(bool on);
};

bool selectComPort(std::string& comName); 