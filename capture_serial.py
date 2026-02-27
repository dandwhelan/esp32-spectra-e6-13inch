
import serial
import time
import sys

# Windows COM7
port = 'COM7'
baud_rate = 115200

def read_serial_log(duration=15):
    try:
        ser = serial.Serial(port, baud_rate, timeout=1)
        print(f"Connected to {port} at {baud_rate} baud.")
        
        start_time = time.time()
        log_data = []
        
        while (time.time() - start_time) < duration:
            if ser.in_waiting > 0:
                line = ser.readline().decode('utf-8', errors='ignore').strip()
                print(line)
                log_data.append(line)
            time.sleep(0.01)
            
        ser.close()
        
        with open('serial_log_capture.txt', 'w') as f:
            f.write('\n'.join(log_data))
            
        print("Done capturing logs.")
        
    except serial.SerialException as e:
        print(f"Error opening port {port}: {e}")

if __name__ == "__main__":
    duration = 15
    if len(sys.argv) > 1:
        duration = int(sys.argv[1])
    read_serial_log(duration)
