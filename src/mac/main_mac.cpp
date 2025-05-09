#include <iostream>
#include <fstream>
#include <stdio.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include "Main.h"
#include "Colorize.h"
#include "SerialPort.h"
#include "FileDialog.h"
#include "Xmodem.h"
#include "Console.h"
#include "SendGCode.h"

static void errorExit(const char* msg) {
    std::cerr << msg << std::endl;
    std::cerr << "..press any key to continue" << std::endl;
    getConsoleChar();

    // Restore input mode on exit.
    restoreConsoleModes();
    exit(1);
}

static SerialPort comport;

static void okayExit(const char* msg) {
    comport.write("\x0c");  // Send CTRL-L to exit FluidNC echo mode
    std::cerr << msg << std::endl;
    Sleep(1000);

    // Restore input mode on exit.
    restoreConsoleModes();
    exit(0);
}

static void enableFluidEcho() {
    comport.write("\x1b[C");  // Send right-arrow to enter FluidNC echo mode
}

void resetFluidNC() {
    std::cout << "Resetting MCU" << std::endl;
    comport.setRts(true);
    Sleep(500);
    comport.setRts(false);
    Sleep(4000);
    enableFluidEcho();
}

static const char* getSaveName(const char* proposal) {
    editModeOn();

    static std::string saveName;

    std::cout << "FluidNC filename [" << proposal << "]: ";
    std::getline(std::cin, saveName);

    if (saveName.length() == 0) {
        saveName = proposal;
    }

    editModeOff();

    return saveName.c_str();
}

struct cmd {
    const char* code;
    uint8_t     value;
    const char* help;
} realtime_commands[] = {
    { "sd", 0x84, "Safety Door" },
    { "jc", 0x85, "JogCancel" },
    { "dr", 0x86, "DebugReport" },
    { "m0", 0x87, "Macro0" },
    { "m1", 0x88, "Macro1" },
    { "m2", 0x89, "Macro2" },
    { "m3", 0x8a, "Macro3" },
    { "fr", 0x90, "FeedOvrReset" },
    { "f>", 0x91, "FeedOvrCoarsePlus" },
    { "f<", 0x92, "FeedOvrCoarseMinus" },
    { "f+", 0x93, "FeedOvrFinePlus" },
    { "f-", 0x94, "FeedOvrFineMinus" },
    { "rr", 0x95, "RapidOvrReset" },
    { "rm", 0x96, "RapidOvrMedium" },
    { "rl", 0x97, "RapidOvrLow" },
    { "rx", 0x98, "RapidOvrExtraLow" },
    { "sr", 0x99, "SpindleOvrReset" },
    { "s>", 0x9A, "SpindleOvrCoarsePlus" },
    { "s<", 0x9B, "SpindleOvrCoarseMinus" },
    { "s+", 0x9C, "SpindleOvrFinePlus" },
    { "s-", 0x9D, "SpindleOvrFineMinus" },
    { "ss", 0x9E, "SpindleOvrStop" },
    { "ft", 0xA0, "CoolantFloodOvrToggle" },
    { "mt", 0xA1, "CoolantMistOvrToggle" },
    { NULL, 0, NULL },
};

char get_character() {
    int res = getConsoleChar();
    if (res < 0) {
        errorExit("Input error");
    }
    return res;
}

void sendOverride() {
    std::cout << "Enter 2-character code - xx for help: ";

    char c[3];
    c[0] = tolower(get_character());
    std::cout << c[0];
    c[1] = tolower(get_character());
    std::cout << c[1] << ' ';
    c[2] = '\0';

    for (struct cmd* p = realtime_commands; p->code; p++) {
        if (!strcmp(c, p->code)) {
            char ch = p->value;
            std::cout << '<' << p->help << '>' << std::endl;
            comport.write(&ch, 1);
            return;
        }
    }
    std::cout << std::endl << "The codes are:" << std::endl;
    for (struct cmd* p = realtime_commands; p->code; p++) {
        std::cout << p->code << " " << p->help << std::endl;
    }
}

off_t fileSize(const char* name) {
    struct stat st;
    if (stat(name, &st) != 0) {
        return -1;
    }
    return st.st_size;
}

void uploadFile(const std::string& path, const std::string& remoteName) {
    std::ifstream infile(path, std::ifstream::in | std::ifstream::binary);
    if (infile.fail()) {
        std::cout << "Can't open " << path << std::endl;
        return;
    }
    
    int size = fileSize(path.c_str());
    std::cout << "XModem Upload " << path << " " << remoteName << std::endl;

    std::string msg = "$Xmodem/Receive=";
    msg += remoteName;
    msg += '\n';
    comport.setDirect();
    comport.write(msg);
    int ch;
    while (true) {
        ch = comport.timedRead(1);

        if (ch == -1) {
        } else if (ch == 0x18 || ch == 0x04) {
            // 0x18 is the correct cancel character but older FluidNC versions use 0x04
            std::cout << "FluidNC cancelled the upload" << std::endl;
            comport.setIndirect();
            break;
        } else if (ch == 'C') {
            int ret = xmodemTransmit(comport, infile);
            comport.flushInput();
            comport.setIndirect();
            if (ret < 0) {
                std::cout << "Returned " << ret << std::endl;
            }
            break;
        } else if (ch == '$') {
            std::cout << (char)ch;
            // FluidNC is echoing the line
            do {
                ch = comport.timedRead(1);
                if (ch != -1) {
                    std::cout << (char)ch;
                }
            } while (ch != '\n');
        } else if (ch == '\n') {
            std::cout << (char)ch;
        } else if (ch == 'e') {
            // Probably an "error:N" message
            std::cout << (char)ch;
            comport.setIndirect();
            break;
        }
    }
}

const char* uploadpath = nullptr;

int main(int argc, char** argv) {
    std::string comName;
    std::string uploadName;
    std::string remoteName;

    opterr = 0;
    int c;
    while ((c = getopt(argc, argv, "p:u:r:")) != -1) {
        switch (c) {
            case 'p':
                comName = optarg;
                break;
            case 'u':
                uploadName = optarg;
                break;
            case 'r':
                remoteName = optarg;
                break;
            case '?':
                if (optopt == 'p' || optopt == 'u' || optopt == 'd')
                    fprintf(stderr, "Option -%c requires an argument.\n", optopt);
                else if (isprint(optopt))
                    fprintf(stderr, "Unknown option `-%c'.\n", optopt);
                else
                    fprintf(stderr, "Unknown option character `\\x%x'.\n", optopt);
                return 1;
            default:
                abort();
        }
    }
    for (int index = optind; index < argc; index++)
        printf("Non-option argument %s\n", argv[index]);

    editModeOn();
    if (comName.length() == 0 && !selectComPort(comName)) {
        editModeOff();
        errorExit("No COM port found");
    }
    editModeOff();

    // Start a thread to read the serial port and send to the console
    if (!comport.Init(comName.c_str(), B115200)) {
        std::string errorstr("Cannot open ");
        errorstr += comName;
        errorExit(errorstr.c_str());
    }

    if (uploadName.length()) {
        if (remoteName.length()) {
            if (remoteName.back() == '/') {
                remoteName += fileTail(uploadName.c_str());
            }
        } else {
            remoteName = getSaveName(fileTail(uploadName.c_str()));
        }
        uploadFile(uploadName, remoteName);
        okayExit("Upload complete");
    }

    bool done = false;
    std::string line;
    bool edit = true;
    bool realtime = true;

    comport.setTimeout(100);

    while (!done) {
        if (edit) {
            editModeOn();
            std::cout << "--: ";
            std::getline(std::cin, line);
            editModeOff();
            if (line == "quit") {
                return 0;
            } else if (line == "echo") {
                enableFluidEcho();
            } else if (line == "reset") {
                resetFluidNC();
            } else if (line == "rt") {
                realtime = true;
            } else if (line == "nort") {
                realtime = false;
            } else if (line == "upload") {
                std::string path;
                if (showOpenFileDialog(path, "*.g;*.nc;*.gcode", "Open G-Code File")) {
                    std::string remoteName = getSaveName(fileTail(path.c_str()));
                    uploadFile(path, remoteName);
                }
            } else if (line == "load") {
                std::string path;
                if (showOpenFileDialog(path, "*.bin", "Open firmware.bin")) {
                    continue;  // Not implemented for macOS yet
                }
            } else if (line.length() >= 2 && line[0] == '$' && line[1] == '<') {
                edit = false;
                SendGCodeFile(comport);
                edit = true;
            } else if (line.length() >= 2 && line[0] == '$' && line[1] == '>') {
                // Send lines from the terminal screen
                continue;  // Not implemented for macOS yet
            } else if (line.length() > 0) {
                line += '\n';
                comport.write(line.c_str(), line.length());
            }
        }

        // Check for console input
        if (realtime && availConsoleChar()) {
            char ch = get_character();
            if (ch == '~') {
                sendOverride();
            } else if (ch == 0x1b || ch == 0x03) {  // ESC or CTRL-C
                comport.write("\x18");  // Cancel
            } else {
                if (ch == '!') {  // Feed hold
                    comport.write("!");
                } else if (ch == '?') {  // Status report
                    comport.write("?");
                } else if (ch == '`') {  // Enter edit mode
                }
            }
        }
    }

    return 0;
} 