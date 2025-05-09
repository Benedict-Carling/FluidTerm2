#include "FileDialog.h"
#include <string>
#include <iostream>
#include <libgen.h>

const char* fileTail(const char* path) {
    static std::string result;
    char* pathCopy = strdup(path);
    const char* filename = basename(pathCopy);
    result = std::string(filename);
    free(pathCopy);
    
    return result.c_str();
}

// Simple console-based file dialog for macOS
bool showOpenFileDialog(std::string& fileName, const char* filter, const char* title) {
    if (title) {
        std::cout << title << std::endl;
    } else {
        std::cout << "Open File" << std::endl;
    }
    
    if (filter) {
        std::cout << "Filter: " << filter << std::endl;
    }
    
    std::cout << "Enter file path: ";
    std::getline(std::cin, fileName);
    
    return !fileName.empty();
}

// Simple console-based save dialog for macOS
bool showSaveFileDialog(std::string& fileName, const char* filter, const char* title) {
    if (title) {
        std::cout << title << std::endl;
    } else {
        std::cout << "Save File" << std::endl;
    }
    
    if (filter) {
        std::cout << "Filter: " << filter << std::endl;
    }
    
    std::cout << "Enter file path to save: ";
    std::getline(std::cin, fileName);
    
    return !fileName.empty();
} 