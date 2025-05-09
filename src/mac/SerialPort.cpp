#include "SerialPort.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <iostream>
#include <termios.h>
#include <dirent.h>
#include <vector>
#include <algorithm>
#include <IOKit/serial/ioss.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/serial/IOSerialKeys.h>

SerialPort::SerialPort() : 
    m_thread(nullptr),
    m_threadRunning(false),
    m_fd(-1),
    m_baud(B115200),
    m_parity(0),
    m_stopBits(1),
    m_dataBits(8) {
}

SerialPort::~SerialPort() {
    if (m_threadRunning) {
        m_threadRunning = false;
        if (m_thread) {
            m_thread->join();
            delete m_thread;
            m_thread = nullptr;
        }
    }
    
    if (m_fd >= 0) {
        close(m_fd);
        m_fd = -1;
    }
}

void SerialPort::ThreadFn(void* pvParam) {
    SerialPort* pThis = static_cast<SerialPort*>(pvParam);
    
    if (!pThis) {
        return;
    }
    
    const size_t bufferSize = 1024;
    char buffer[bufferSize];
    
    while (pThis->m_threadRunning) {
        if (pThis->m_fd < 0) {
            usleep(100 * 1000); // 100ms
            continue;
        }
        
        if (pThis->m_direct) {
            usleep(10 * 1000); // 10ms
            continue;
        }
        
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(pThis->m_fd, &readfds);
        
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 100 * 1000; // 100ms
        
        int res = select(pThis->m_fd + 1, &readfds, NULL, NULL, &timeout);
        
        if (res > 0 && FD_ISSET(pThis->m_fd, &readfds)) {
            int bytesRead = read(pThis->m_fd, buffer, bufferSize - 1);
            
            if (bytesRead > 0) {
                buffer[bytesRead] = 0;
                std::cout << buffer;
                std::cout.flush();
            }
        }
    }
}

bool SerialPort::reOpenPort() {
    if (m_fd >= 0) {
        close(m_fd);
        m_fd = -1;
    }
    
    m_fd = open(m_commName.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (m_fd < 0) {
        return false;
    }
    
    struct termios options;
    tcgetattr(m_fd, &options);
    
    // Set baud rate
    cfsetispeed(&options, m_baud);
    cfsetospeed(&options, m_baud);
    
    // No parity (8N1)
    options.c_cflag &= ~PARENB;
    options.c_cflag &= ~CSTOPB;
    options.c_cflag &= ~CSIZE;
    options.c_cflag |= CS8;
    
    // Raw input, no echo, no signals
    options.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    
    // Raw output
    options.c_oflag &= ~OPOST;
    
    // Set options
    tcsetattr(m_fd, TCSANOW, &options);
    
    return true;
}

void SerialPort::setDirect() {
    m_direct = true;
}

void SerialPort::setIndirect() {
    m_direct = false;
}

int SerialPort::timedRead(uint32_t ms) {
    if (m_fd < 0) {
        return -1;
    }
    
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(m_fd, &readfds);
    
    struct timeval timeout;
    timeout.tv_sec = ms / 1000;
    timeout.tv_usec = (ms % 1000) * 1000;
    
    int res = select(m_fd + 1, &readfds, NULL, NULL, &timeout);
    
    if (res > 0) {
        unsigned char buffer[1];
        int bytesRead = read(m_fd, buffer, 1);
        
        if (bytesRead == 1) {
            return buffer[0];
        }
    }
    
    return -1;
}

int SerialPort::timedRead(uint8_t* buf, size_t len, uint32_t ms) {
    if (m_fd < 0 || !buf || len == 0) {
        return -1;
    }
    
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(m_fd, &readfds);
    
    struct timeval timeout;
    timeout.tv_sec = ms / 1000;
    timeout.tv_usec = (ms % 1000) * 1000;
    
    int res = select(m_fd + 1, &readfds, NULL, NULL, &timeout);
    
    if (res > 0) {
        return read(m_fd, buf, len);
    }
    
    return -1;
}

int SerialPort::timedRead(char* buf, size_t len, uint32_t ms) {
    return timedRead(reinterpret_cast<uint8_t*>(buf), len, ms);
}

void SerialPort::flushInput() {
    if (m_fd >= 0) {
        tcflush(m_fd, TCIFLUSH);
    }
}

void SerialPort::setTimeout(unsigned int ms) {
    if (m_fd >= 0) {
        struct termios options;
        tcgetattr(m_fd, &options);
        
        options.c_cc[VTIME] = ms / 100; // In tenths of a second
        options.c_cc[VMIN] = 0;
        
        tcsetattr(m_fd, TCSANOW, &options);
    }
}

int SerialPort::write(const char* data, size_t dwSize) {
    if (m_fd < 0 || !data) {
        return -1;
    }
    
    return ::write(m_fd, data, dwSize);
}

bool SerialPort::Init(std::string szPortName, speed_t dwBaudRate, int byParity, int byStopBits, int byByteSize) {
    m_commName = szPortName;
    m_portName = szPortName;
    m_baud = dwBaudRate;
    m_parity = byParity;
    m_stopBits = byStopBits;
    m_dataBits = byByteSize;
    
    if (!reOpenPort()) {
        return false;
    }
    
    m_threadRunning = true;
    m_thread = new std::thread(ThreadFn, this);
    
    return true;
}

void SerialPort::getMode(speed_t& dwBaudRate, int& byByteSize, int& byParity, int& byStopBits) {
    dwBaudRate = m_baud;
    byByteSize = m_dataBits;
    byParity = m_parity;
    byStopBits = m_stopBits;
}

bool SerialPort::setMode(speed_t dwBaudRate, int byByteSize, int byParity, int byStopBits) {
    m_baud = dwBaudRate;
    m_dataBits = byByteSize;
    m_parity = byParity;
    m_stopBits = byStopBits;
    
    if (m_fd < 0) {
        return false;
    }
    
    struct termios options;
    tcgetattr(m_fd, &options);
    
    // Set baud rate
    cfsetispeed(&options, m_baud);
    cfsetospeed(&options, m_baud);
    
    // Set character size
    options.c_cflag &= ~CSIZE;
    switch (byByteSize) {
        case 5: options.c_cflag |= CS5; break;
        case 6: options.c_cflag |= CS6; break;
        case 7: options.c_cflag |= CS7; break;
        default: options.c_cflag |= CS8; break;
    }
    
    // Set parity
    if (byParity == 0) {
        options.c_cflag &= ~PARENB;
    } else {
        options.c_cflag |= PARENB;
        if (byParity == 1) { // Odd
            options.c_cflag |= PARODD;
        } else { // Even
            options.c_cflag &= ~PARODD;
        }
    }
    
    // Set stop bits
    if (byStopBits == 2) {
        options.c_cflag |= CSTOPB;
    } else {
        options.c_cflag &= ~CSTOPB;
    }
    
    tcsetattr(m_fd, TCSANOW, &options);
    
    return true;
}

void SerialPort::setRts(bool on) {
    if (m_fd < 0) {
        return;
    }
    
    int flags;
    ioctl(m_fd, TIOCMGET, &flags);
    
    if (on) {
        flags |= TIOCM_RTS;
    } else {
        flags &= ~TIOCM_RTS;
    }
    
    ioctl(m_fd, TIOCMSET, &flags);
}

void SerialPort::setDtr(bool on) {
    if (m_fd < 0) {
        return;
    }
    
    int flags;
    ioctl(m_fd, TIOCMGET, &flags);
    
    if (on) {
        flags |= TIOCM_DTR;
    } else {
        flags &= ~TIOCM_DTR;
    }
    
    ioctl(m_fd, TIOCMSET, &flags);
}

// Helper functions to get a list of available serial ports on macOS
static std::vector<std::string> getSerialPorts() {
    std::vector<std::string> result;
    
    // Create matching dictionary for serial ports
    CFMutableDictionaryRef matchingDict = IOServiceMatching(kIOSerialBSDServiceValue);
    if (!matchingDict) {
        return result;
    }
    
    // Filter to include all types of serial devices
    CFDictionarySetValue(matchingDict, 
                        CFSTR(kIOSerialBSDTypeKey),
                        CFSTR(kIOSerialBSDAllTypes));
    
    // Get iterator for matching services
    io_iterator_t matchingServices;
    kern_return_t kernResult = IOServiceGetMatchingServices(kIOMasterPortDefault, matchingDict, &matchingServices);
    if (KERN_SUCCESS != kernResult) {
        return result;
    }
    
    // Iterate through all serial devices
    io_object_t usbDevice;
    while ((usbDevice = IOIteratorNext(matchingServices))) {
        CFTypeRef bsdPathAsCFString = IORegistryEntryCreateCFProperty(usbDevice,
                                                                    CFSTR(kIOCalloutDeviceKey),
                                                                    kCFAllocatorDefault,
                                                                    0);
        if (bsdPathAsCFString) {
            char path[PATH_MAX];
            Boolean bResult = CFStringGetCString((CFStringRef)bsdPathAsCFString,
                                               path,
                                               PATH_MAX,
                                               kCFStringEncodingUTF8);
            CFRelease(bsdPathAsCFString);
            
            if (bResult) {
                result.push_back(std::string(path));
            }
        }
        
        IOObjectRelease(usbDevice);
    }
    
    IOObjectRelease(matchingServices);
    
    return result;
}

bool selectComPort(std::string& comName) {
    std::vector<std::string> ports = getSerialPorts();
    
    if (ports.empty()) {
        return false;
    }
    
    std::cout << "Available serial ports:" << std::endl;
    for (size_t i = 0; i < ports.size(); i++) {
        std::cout << i + 1 << ": " << ports[i] << std::endl;
    }
    
    std::cout << "Select a port (1-" << ports.size() << "): ";
    int selection = 0;
    std::cin >> selection;
    
    if (selection < 1 || selection > static_cast<int>(ports.size())) {
        return false;
    }
    
    comName = ports[selection - 1];
    return true;
} 