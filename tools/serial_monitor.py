#!/usr/bin/env python3
"""Persistent serial logger for the SpoolWisp ESP32-S3 board.

Usage:
    python3 tools/serial_monitor.py [port] [logfile]

Opens the board's UART bridge (CH9102) at 115200, pulses the auto-reset
circuit so the application starts from a clean boot, then appends every byte
received to the log file and to stdout.
"""
import glob
import os
import sys
import time

import serial


def find_port():
    env = os.environ.get("SPOOLWISP_PORT")
    if env:
        return env
    patterns = (
        "/dev/cu.usbmodem*",      # macOS native USB / CDC
        "/dev/cu.usbserial*",     # macOS UART bridge
        "/dev/cu.wchusbserial*",  # macOS WCH bridge
        "/dev/ttyUSB*",           # Linux UART bridge
        "/dev/ttyACM*",           # Linux CDC
    )
    for pattern in patterns:
        hits = sorted(p for p in glob.glob(pattern) if "Bluetooth" not in p)
        if hits:
            return hits[0]
    raise SystemExit("no serial port found; pass the port as the first argument")


port = sys.argv[1] if len(sys.argv) > 1 else find_port()
log_path = sys.argv[2] if len(sys.argv) > 2 else os.environ.get(
    "SPOOLWISP_LOG", "firmware/.serial.log")

os.makedirs(os.path.dirname(os.path.abspath(log_path)), exist_ok=True)
log = open(log_path, "ab", buffering=0)
log.write(("\n===== monitor start %s =====\n" %
           time.strftime("%Y-%m-%d %H:%M:%S")).encode())

while True:
    try:
        ser = serial.Serial(port, 115200, timeout=0.3)
        break
    except Exception as exc:  # port busy / replugged
        print("open retry: %s" % exc, file=sys.stderr, flush=True)
        time.sleep(1)

# ESP32 auto-reset: IO0 high, EN low pulse -> normal boot
try:
    ser.setDTR(False)
    ser.setRTS(True)
    time.sleep(0.15)
    ser.setRTS(False)
except Exception as exc:
    print("reset warning: %s" % exc, file=sys.stderr, flush=True)

while True:
    try:
        data = ser.read(8192)
    except Exception as exc:
        print("read error: %s" % exc, file=sys.stderr, flush=True)
        break
    if data:
        log.write(data)
        sys.stdout.write(data.decode("utf-8", "replace"))
        sys.stdout.flush()
