# Raspberry Pi GNSS BLE bridge

The Pi uses its built-in `hci0` controller as a BLE peripheral. It reads the
nRF9151 GNSS stream from USB serial and exposes the Android-compatible UART
service:

- Service: `0000FFE0-0000-1000-8000-00805F9B34FB`
- Characteristic: `0000FFE1-0000-1000-8000-00805F9B34FB`
- Properties: read, write, write without response, notify

Install dependencies on the Pi:

```bash
sudo apt install python3-pip python3-serial
python3 -m pip install --break-system-packages bluezero
```

Run manually:

```bash
python3 gnss_ble_bridge.py \
  --serial /dev/serial/by-id/usb-SEGGER_J-Link_001052045443-if00 \
  --baud 115200
```

Install the service after copying the script to `/opt/gnss`:

```bash
sudo install -D gnss_ble_bridge.py /opt/gnss/gnss_ble_bridge.py
sudo install gnss-ble-bridge.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now gnss-ble-bridge
sudo journalctl -u gnss-ble-bridge -f
```

The Android app scans for FFE0 rather than a device name, so the bridge can be
renamed without changing the app.
