#!/usr/bin/env python3
"""Host side of the phone's USB Ethernet live log (doas instead of sudo).

  bonito-usblog.py LOGFILE

Waits for the phone's CDC-ECM link (host MAC 02:00:5e:77:ee:01, as in
tools/sweet-usbnet.sh), gives it 192.168.7.1/24 and appends every UDP datagram
the phone sends to port 5140 (its live kernel console) to LOGFILE.
"""
import socket, subprocess, sys, time

MAC = '02:00:5e:77:ee:01'


def iface():
    out = subprocess.run(['ip', '-o', 'link'], capture_output=True, text=True).stdout
    for line in out.splitlines():
        if MAC in line.lower():
            return line.split(': ')[1].split('@')[0]
    return None


def main():
    log = open(sys.argv[1], 'ab', buffering=0)
    while not (name := iface()):
        time.sleep(1)
    subprocess.run(['doas', 'ip', 'addr', 'replace', '192.168.7.1/24', 'dev', name], check=True)
    subprocess.run(['doas', 'ip', 'link', 'set', name, 'up'], check=True)
    print(f'{time.strftime("%T")} {name} up, listening on udp/5140', flush=True)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(('0.0.0.0', 5140))
    while True:
        data, _ = s.recvfrom(65536)
        log.write(data)


if __name__ == '__main__':
    main()
