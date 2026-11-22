#!/usr/bin/env python3
"""Forward nRF GNSS serial text through a Raspberry Pi BLE UART service."""

import argparse
import logging
import threading
import time
from pathlib import Path

import serial
from bluezero import peripheral

SERVICE_UUID = "0000FFE0-0000-1000-8000-00805F9B34FB"
CHARACTERISTIC_UUID = "0000FFE1-0000-1000-8000-00805F9B34FB"


class GnssBleBridge:
    def __init__(self, serial_path: str, baud: int, adapter: str, name: str):
        self.serial_path = serial_path
        self.baud = baud
        self.adapter = adapter
        self.name = name
        self.characteristic = None
        self.connected = False
        self.stop_event = threading.Event()
        self.serial = None
        self.line_count = 0

    def read_value(self):
        return list(b"GNSS-RaspberryPi\r\n")

    def on_write(self, value, options):
        command = bytes(value).decode("utf-8", errors="replace")
        logging.info("BLE command received: %r", command)
        if self.serial:
            self.serial.write(command.encode("utf-8"))

    def on_notify(self, notifying, characteristic):
        self.connected = notifying
        # Bluezero supplies the live characteristic to this callback; its
        # add_characteristic() helper does not reliably return that object.
        self.characteristic = characteristic if notifying else None
        logging.info("BLE notifications %s", "enabled" if notifying else "disabled")

    def serial_loop(self):
        while not self.stop_event.is_set():
            try:
                with serial.Serial(self.serial_path, self.baud, timeout=1) as port:
                    self.serial = port
                    logging.info("Reading nRF GNSS from %s at %d baud", self.serial_path, self.baud)
                    while not self.stop_event.is_set():
                        # Read raw chunks instead of waiting for a line ending.
                        # This forwards arbitrary terminal text and binary data
                        # immediately, including payloads without CR/LF.
                        chunk = port.read(256)
                        if not chunk:
                            continue
                        if self.characteristic is None:
                            continue
                        self.line_count += 1
                        if self.line_count == 1 or self.line_count % 100 == 0:
                            logging.info("Forwarding serial chunk %d (%d bytes, notifications=%s)",
                                         self.line_count, len(chunk), self.connected)
                        payload = chunk
                        # BlueZ notifications are MTU-limited. Split long NMEA
                        # or debug lines while preserving their original bytes.
                        for offset in range(0, len(payload), 180):
                            self.characteristic.set_value(list(payload[offset:offset + 180]))
            except serial.SerialException as exc:
                self.serial = None
                # No GNSS bytes is normal. A transient port disconnect or
                # another opener should not produce a traceback or stop BLE.
                logging.warning("Serial input unavailable (%s); retrying in 2 seconds", exc)
                time.sleep(2)
            except Exception:
                self.serial = None
                logging.exception("Unexpected serial bridge error; retrying in 2 seconds")
                time.sleep(2)

    def run(self):
        ble = peripheral.Peripheral(
            adapter_address=self.adapter,
            local_name=self.name,
            appearance=0x0340,
        )
        ble.add_service(srv_id=1, uuid=SERVICE_UUID, primary=True)
        self.characteristic = ble.add_characteristic(
            srv_id=1,
            chr_id=1,
            uuid=CHARACTERISTIC_UUID,
            value=self.read_value(),
            notifying=False,
            flags=["read", "write", "write-without-response", "notify"],
            read_callback=self.read_value,
            write_callback=self.on_write,
            notify_callback=self.on_notify,
        )
        thread = threading.Thread(target=self.serial_loop, daemon=True)
        thread.start()
        logging.info("Advertising %s with FFE0/FFE1 on %s", self.name, self.adapter)
        try:
            ble.publish()
        finally:
            self.stop_event.set()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--serial", required=True)
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--adapter", default="DC:A6:32:10:EF:39")
    parser.add_argument("--name", default="GNSS-RaspberryPi")
    parser.add_argument("--log-level", default="INFO")
    args = parser.parse_args()
    logging.basicConfig(level=getattr(logging, args.log_level.upper()), format="%(asctime)s %(levelname)s %(message)s")
    GnssBleBridge(args.serial, args.baud, args.adapter, args.name).run()


if __name__ == "__main__":
    main()
