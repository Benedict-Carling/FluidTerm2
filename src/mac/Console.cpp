#include "Console.h"
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <stdio.h>
#include <iostream>

static struct termios original_termios;
static bool termios_saved = false;

void editModeOn() {
    if (!termios_saved) {
        tcgetattr(STDIN_FILENO, &original_termios);
        termios_saved = true;
    }
    
    struct termios raw = original_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

void editModeOff() {
    if (termios_saved) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_termios);
    }
}

bool setConsoleModes() {
    if (!termios_saved) {
        tcgetattr(STDIN_FILENO, &original_termios);
        termios_saved = true;
    }
    
    struct termios raw = original_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    
    return true;
}

bool setConsoleColor() {
    // Not necessary on macOS terminals as they typically support colors by default
    return true;
}

void restoreConsoleModes() {
    if (termios_saved) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_termios);
    }
}

int getConsoleChar() {
    char c;
    if (read(STDIN_FILENO, &c, 1) == 1) {
        return c;
    }
    return -1;
}

bool availConsoleChar() {
    fd_set readfds;
    struct timeval tv;
    
    FD_ZERO(&readfds);
    FD_SET(STDIN_FILENO, &readfds);
    
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    
    int result = select(STDIN_FILENO + 1, &readfds, NULL, NULL, &tv);
    
    return result > 0;
}

void clearScreen() {
    // ANSI escape sequence to clear screen and position cursor at top-left
    std::cout << "\033[2J\033[1;1H";
} 