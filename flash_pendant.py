#!/usr/bin/env python3
"""
ESP32-S3 Pendant Flasher

This script flashes ESP32-S3 pendant firmware through FluidNC's UART passthrough mode.
It handles the passthrough communication and uses esptool for the actual flashing.

Usage: python flash_pendant.py --port COM5 --firmware firmware.bin
"""

import argparse
import sys
import time
import os
import serial
import subprocess
import platform
import shutil

# Colors for terminal output
class Colors:
    HEADER = '\033[95m'
    BLUE = '\033[94m'
    GREEN = '\033[92m'
    YELLOW = '\033[93m'
    RED = '\033[91m'
    ENDC = '\033[0m'
    BOLD = '\033[1m'

def print_colored(message, color):
    """Print colored text to the terminal"""
    print(f"{color}{message}{Colors.ENDC}")

def find_esptool():
    """Find the esptool.py executable in the system path"""
    esptool_path = shutil.which("esptool.py")
    if esptool_path:
        return esptool_path
    
    esptool_path = shutil.which("esptool")
    if esptool_path:
        return esptool_path
    
    # Check if we're in a Python environment with esptool installed
    try:
        import esptool
        return "esptool.py"
    except ImportError:
        pass
    
    print_colored("Error: esptool.py not found in PATH", Colors.RED)
    print("Please install esptool.py with: pip install esptool")
    sys.exit(1)

def enter_passthrough_mode(ser, timeout=5):
    """Put FluidNC in passthrough mode"""
    print_colored("Entering passthrough mode...", Colors.BLUE)
    
    # Clear any pending data
    ser.reset_input_buffer()
    ser.reset_output_buffer()
    
    # Send passthrough command to FluidNC
    ser.write(b"$Uart/Passthrough=Uart1:120s\n")
    time.sleep(1)
    
    # Now prompt user to put the pendant in bootloader mode
    print_colored("Passthrough mode entered successfully", Colors.GREEN)
    print_colored("\nNOW put your ESP32-S3 pendant in bootloader mode:", Colors.YELLOW)
    print("1. Press and hold GPIO0 (BOOT button)")
    print("2. Press and release EN (RESET button)")
    print("3. Release GPIO0 (BOOT button)")
    
    input("Press Enter when you've done this...")
    return True
    

def exit_passthrough_mode(ser):
    """Exit passthrough mode by sending Ctrl+C"""
    print_colored("Exiting passthrough mode...", Colors.BLUE)
    
    # Send Ctrl+C to exit passthrough
    ser.write(b"\x03")
    time.sleep(0.5)
    
    # Clear any pending data
    ser.reset_input_buffer()
    
    # Send a newline to get a response from FluidNC
    ser.write(b"\n")
    time.sleep(0.5)
    
    # Read response to verify we're back in FluidNC mode
    response = ser.read(ser.in_waiting).decode('utf-8', 'ignore')
    if "ok" in response.lower() or "error:" in response.lower():
        print_colored("Successfully exited passthrough mode", Colors.GREEN)
        return True
    
    print_colored("Warning: Uncertain if passthrough mode was exited properly", Colors.YELLOW)
    return False

def flash_esp32s3(port, firmware_file):
    """Use esptool.py to flash the ESP32-S3 through passthrough mode"""
    
    # Find esptool.py
    esptool_path = find_esptool()
    
    # Construct esptool command
    cmd = [
        esptool_path,
        "--chip", "esp32s3",
        "--port", port,
        "--baud", "115200",
        "--before", "no_reset",
        "--after", "no_reset",
        "--no-stub",
        "write_flash",
        "0x0", firmware_file
    ]
    
    print_colored("Running esptool command:", Colors.BLUE)
    print(" ".join(cmd))
    print("")
    
    # Execute esptool command
    try:
        result = subprocess.run(cmd, check=True)
        if result.returncode == 0:
            print_colored("Firmware flashed successfully!", Colors.GREEN)
            return True
        else:
            print_colored(f"Failed to flash firmware! Return code: {result.returncode}", Colors.RED)
            return False
    except subprocess.CalledProcessError as e:
        print_colored(f"Error executing esptool: {str(e)}", Colors.RED)
        return False
    except Exception as e:
        print_colored(f"Unexpected error: {str(e)}", Colors.RED)
        return False

def main():
    """Main function for the pendant flasher script"""
    
    # Parse command line arguments
    parser = argparse.ArgumentParser(description='Flash ESP32-S3 pendant through FluidNC')
    parser.add_argument('--port', '-p', required=True, help='Serial port connected to FluidNC (e.g., COM3, /dev/ttyUSB0)')
    parser.add_argument('--firmware', '-f', required=True, help='Firmware binary file to flash')
    args = parser.parse_args()
    
    # Validate firmware file
    if not os.path.isfile(args.firmware):
        print_colored(f"Error: Firmware file '{args.firmware}' not found", Colors.RED)
        sys.exit(1)
    
    # Display banner
    print_colored("=" * 60, Colors.HEADER)
    print_colored("ESP32-S3 Pendant Flasher via FluidNC Passthrough", Colors.HEADER)
    print_colored("=" * 60, Colors.HEADER)
    print(f"Port: {args.port}")
    print(f"Firmware: {args.firmware}")
    print("")

    # Open serial connection to FluidNC
    try:
        ser = serial.Serial(args.port, 115200, timeout=1)
    except serial.SerialException as e:
        print_colored(f"Error opening serial port: {str(e)}", Colors.RED)
        sys.exit(1)
    
    try:
        # Enter passthrough mode
        if not enter_passthrough_mode(ser):
            print_colored("Failed to enter passthrough mode. Aborting.", Colors.RED)
            sys.exit(1)
        
        # Close serial connection to allow esptool.py to use the port
        ser.close()
        time.sleep(1)  # Give some time for the port to be released
        
        # Flash the ESP32-S3
        success = flash_esp32s3(args.port, args.firmware)
        
        if success:
            print_colored("\nFlashing completed successfully!", Colors.GREEN)
            print_colored("ESP32-S3 pendant should now be running the new firmware.", Colors.GREEN)
        else:
            print_colored("\nFlashing failed!", Colors.RED)
            print("Please check connections and try again.")
        
        # Reopen serial port to exit passthrough mode
        time.sleep(2)  # Give some time after flashing
        ser = serial.Serial(args.port, 115200, timeout=1)
        exit_passthrough_mode(ser)
        
    finally:
        # Ensure serial port is closed
        if ser.is_open:
            ser.close()
    
if __name__ == "__main__":
    main()