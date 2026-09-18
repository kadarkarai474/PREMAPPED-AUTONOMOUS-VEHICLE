import socket
import msvcrt
import sys
import time

UDP_IP = "192.168.4.1"  
UDP_PORT = 4210
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

def send_command(cmd):
    try:
        sock.sendto(cmd.encode(), (UDP_IP, UDP_PORT))
        print(f" -> Sent: {cmd}")
    except Exception:
        pass

print("="*50)
print("  STRICT TIMING COMMAND CENTER (KEYBOARD)")
print("="*50)
print("  [W] : FORWARD         [X] : START PRE-MAP")
print("  [S] : BACKWARD        [C] : STOP PRE-MAP")
print("  [A] : TURN LEFT       [P] : PLAY (REPEAT)")
print("  [D] : TURN RIGHT")
print("\n  [SPACEBAR] : STOP     [Q] : QUIT PROGRAM")
print("="*50)

while True:
    if msvcrt.kbhit():
        key = msvcrt.getch().decode('utf-8').lower()
        
        # Drive Controls (Tap once to move, tap Space to stop)
        if key == 'w': send_command('F')
        elif key == 's': send_command('B')
        elif key == 'a': send_command('L')
        elif key == 'd': send_command('R')
        elif key == ' ': send_command('S')
        
        # Mapping Controls
        elif key == 'x': send_command('X') # Starts stopwatch and tracking
        elif key == 'c': send_command('x') # Stops and saves to memory
        elif key == 'p': send_command('Y') # Plays identical timings
        
        elif key == 'q': sys.exit()
        
        time.sleep(0.05) # Prevents spamming the network