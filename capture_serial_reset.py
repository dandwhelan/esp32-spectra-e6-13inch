
import serial
import time
import sys

port = 'COM7'
baud_rate = 115200

def reset_and_read(duration=30):
    try:
        ser = serial.Serial(port, baud_rate, timeout=0.1)
        print(f"Connected to {port}. Resetting via DTR/RTS...")
        
        # Toggle DTR and RTS to reset ESP32
        ser.dtr = False
        ser.rts = True
        time.sleep(0.1)
        ser.dtr = True
        ser.rts = False
        time.sleep(0.1)
        ser.dtr = False
        
        print("Reset complete. Capturing logs...")
        
        start_time = time.time()
        log_data = []
        
        while (time.time() - start_time) < duration:
            if ser.in_waiting > 0:
                line = ser.readline().decode('utf-8', errors='ignore').strip()
                if line:
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
    duration = 30
    if len(sys.argv) > 1:
        duration = int(sys.argv[1])
    reset_and_read(duration)
