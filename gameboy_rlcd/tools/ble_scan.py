#!/usr/bin/env python3
"""List what is advertising over Bluetooth Low Energy nearby.

The board can only talk to BLE HID (HOGP) gamepads -- the ESP32-S3 has no
classic Bluetooth radio at all -- so a pad that only speaks classic Bluetooth
will never appear to it. This scans with the host's own Bluetooth stack, which
is an independent check: if a pad does not show up here either, while its
pairing LED is blinking, it is not advertising over BLE.

It also flags anything carrying the HID service or a gamepad appearance, which
is what the board looks for.

    pip install bleak
    python3 ble_scan.py [seconds]

On macOS the first run asks for Bluetooth permission for your terminal.
"""
import asyncio
import sys
import time

from bleak import BleakScanner

HID_SERVICE = "00001812-0000-1000-8000-00805f9b34fb"


def describe(device, adv):
    """One line describing a device, plus whether it looks like a HID pad."""
    name = adv.local_name or device.name or "(no name)"

    services = [str(u).lower() for u in (adv.service_uuids or [])]
    is_hid = HID_SERVICE in services

    tag = "  <-- HID" if is_hid else ""

    extra = ""
    if services:
        extra += f" services={','.join(s[4:8] for s in services)}"
    if adv.manufacturer_data:
        extra += " mfr=" + ",".join(f"0x{k:04x}" for k in adv.manufacturer_data)

    return f"{name:<28} {device.address} rssi={adv.rssi:>4}{extra}{tag}", is_hid


async def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 20.0
    seen = {}
    pads = []

    def on_detection(device, adv):
        key = device.address
        line, looks_like_pad = describe(device, adv)
        if looks_like_pad and key not in pads:
            pads.append(key)
        # Re-print when a device first appears or its name becomes known.
        prev = seen.get(key)
        if prev != line:
            seen[key] = line
            print(f"[{time.strftime('%H:%M:%S')}] {line}", flush=True)

    print(f"Scanning for {seconds:.0f} s. Put the gamepad in pairing mode now.\n")
    scanner = BleakScanner(detection_callback=on_detection)
    await scanner.start()
    await asyncio.sleep(seconds)
    await scanner.stop()

    print(f"\n{len(seen)} device(s) seen over BLE.")
    if pads:
        print("BLE HID gamepad candidate(s):")
        for key in pads:
            print(f"  {seen[key]}")
    else:
        print("No BLE HID gamepad was advertising. If the pad's pairing LED was")
        print("blinking during this scan, it is not a BLE device -- it is almost")
        print("certainly classic Bluetooth, which this board cannot use.")


if __name__ == "__main__":
    asyncio.run(main())
