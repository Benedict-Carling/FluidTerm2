#include "SendGCode.h"
#include "FileDialog.h"
#include <string>
#include <fstream>
#include <iostream>

bool SendGCodeFile(SerialPort& comport) {
    std::string path;
    if (!showOpenFileDialog(path, "*.g;*.nc;*.gcode", "Open G-Code File")) {
        return false;
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        std::cout << "Could not open file: " << path << std::endl;
        return false;
    }

    std::string line;
    while (std::getline(file, line)) {
        // Remove any CR or LF
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        // Skip empty lines
        if (line.empty()) {
            continue;
        }

        // Send line to serial port
        line += '\n';
        comport.write(line);

        // Wait for 'ok' response
        bool gotOk = false;
        std::string response;
        while (!gotOk) {
            int ch = comport.timedRead(100);
            if (ch < 0) {
                continue;
            }
            
            char c = static_cast<char>(ch);
            response += c;
            std::cout << c;
            
            // Check if we got an 'ok' response
            if (response.find("ok") != std::string::npos || 
                response.find("error") != std::string::npos) {
                gotOk = true;
            }
            
            // Check for end of line
            if (c == '\n') {
                response = "";
            }
        }
    }

    return true;
} 