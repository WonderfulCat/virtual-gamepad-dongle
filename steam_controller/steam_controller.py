# Copyright (c) 2026 Jacek Fedorynski
# SPDX-License-Identifier: MIT

import pywinusb.hid
import queue
import ctypes
import time
import struct
import win32gui
import threading


def get_handler(device, tag):
    def handler(data):
        event_queue.put((tag, data, device))

    return handler


def close_devices(devices):
    for d in devices:
        try:
            d.close()
        except:
            pass


def setup_devices():
    devices = pywinusb.hid.find_all_hid_devices()

    dongle = None
    for d in devices:
        if "hid#vid_cafe&pid_bafd&mi_01" in d.device_path:
            dongle = d
            dongle.set_raw_data_handler(get_handler(dongle, "dongle"))
            dongle.open()
            break

    steamcontrollers = []
    for d in devices:
        if d.vendor_id == 0x28DE and d.product_id in (0x1302, 0x1304):
            d.set_raw_data_handler(get_handler(d, "steamcontroller"))
            d.open()
            steamcontrollers.append(d)

    print(
        "Dongle {}, Steam Controller {}.".format(
            "OK" if dongle is not None else "not found",
            "OK" if steamcontrollers else "not found",
        )
    )
    return dongle, steamcontrollers


def main(event_queue):
    xusb_report = bytearray([0, 0, 20] + [0] * 18)
    c_xusb_report = (ctypes.c_ubyte * len(xusb_report)).from_buffer(xusb_report)

    rumble_report = bytearray([0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0])
    c_rumble_report = (ctypes.c_ubyte * len(rumble_report)).from_buffer(rumble_report)

    disable_lizard_mode_msg = bytearray([1, 0x87, 3, 9, 0, 0] + [0] * 58)
    c_disable_lizard_mode_msg = (ctypes.c_ubyte * 64).from_buffer(
        disable_lizard_mode_msg
    )

    next_disable_lizard_mode = time.monotonic_ns()
    next_rumble_heartbeat = None

    dongle, steamcontrollers = setup_devices()
    steamcontroller = None

    while True:
        now = time.monotonic_ns()
        if now >= next_disable_lizard_mode:
            if steamcontroller is not None:
                steamcontroller.send_feature_report(c_disable_lizard_mode_msg)
                next_disable_lizard_mode = now + 3_000_000_000
        if next_rumble_heartbeat is not None and now >= next_rumble_heartbeat:
            if steamcontroller is not None:
                steamcontroller.send_output_report(c_rumble_report)
            next_rumble_heartbeat = now + 40_000_000
        if next_rumble_heartbeat is not None:
            timeout = min(next_disable_lizard_mode, next_rumble_heartbeat) - now
        else:
            timeout = next_disable_lizard_mode - now

        if timeout < 0:
            timeout = 0

        try:
            msg, data, device = event_queue.get(timeout=timeout * 0.000000001)
            if msg == "steamcontroller":
                if device in steamcontrollers:
                    if device is not steamcontroller:
                        steamcontroller = device
                        next_rumble_heartbeat = now
                    steam_report = bytes(data[0:46])
                    if steam_report[0] == 69:
                        (
                            report_id,
                            seq_num,
                            buttons,
                            l2,
                            r2,
                            lx,
                            ly,
                            rx,
                            ry,
                            lpadx,
                            lpady,
                            lpressure,
                            rpadx,
                            rpady,
                            rpressure,
                            timestamp,
                            accelx,
                            accely,
                            accelz,
                            gyrox,
                            gyroy,
                            gyroz,
                        ) = struct.unpack("<BBLhhhhhhhhHhhHLhhhhhh", steam_report)
                        xusb_report[3] = (
                            (0x01 if buttons & (1 << 13) else 0)
                            | (0x02 if buttons & (1 << 10) else 0)
                            | (0x04 if buttons & (1 << 12) else 0)
                            | (0x08 if buttons & (1 << 11) else 0)
                            | (0x10 if buttons & (1 << 6) else 0)
                            | (0x20 if buttons & (1 << 14) else 0)
                            | (0x40 if buttons & (1 << 15) else 0)
                            | (0x80 if buttons & (1 << 5) else 0)
                        )
                        xusb_report[4] = (
                            (0x01 if buttons & (1 << 19) else 0)
                            | (0x02 if buttons & (1 << 9) else 0)
                            | (0x04 if buttons & (1 << 16) else 0)
                            | (0x10 if buttons & (1 << 0) else 0)
                            | (0x20 if buttons & (1 << 1) else 0)
                            | (0x40 if buttons & (1 << 2) else 0)
                            | (0x80 if buttons & (1 << 3) else 0)
                        )
                        xusb_report[5] = l2 >> 7
                        xusb_report[6] = r2 >> 7
                        xusb_report[7] = lx & 0xFF
                        xusb_report[8] = (lx & 0xFF00) >> 8
                        xusb_report[9] = ly & 0xFF
                        xusb_report[10] = (ly & 0xFF00) >> 8
                        xusb_report[11] = rx & 0xFF
                        xusb_report[12] = (rx & 0xFF00) >> 8
                        xusb_report[13] = ry & 0xFF
                        xusb_report[14] = (ry & 0xFF00) >> 8
                        if dongle is not None:
                            dongle.send_output_report(c_xusb_report)
            elif msg == "dongle":
                if device is dongle:
                    rumble_report[5] = data[4]
                    rumble_report[8] = data[5]
                    if steamcontroller is not None:
                        steamcontroller.send_output_report(c_rumble_report)
                        if data[4] != 0 or data[5] != 0:
                            next_rumble_heartbeat = now + 40_000_000
                        else:
                            next_rumble_heartbeat = None
            elif msg == "hardware_change":
                steamcontroller = None
                next_disable_lizard_mode = now
                close_devices([dongle] + steamcontrollers)
                dongle, steamcontrollers = setup_devices()
        except queue.Empty:
            pass
        except pywinusb.hid.HIDError:
            pass


WM_DEVICECHANGE = 0x0219
DBT_DEVNODES_CHANGED = 0x0007


def get_window_thread(event_queue):
    def window_thread():
        def wndproc(hwnd, msg, wparam, lparam):
            if msg == WM_DEVICECHANGE:
                if wparam == DBT_DEVNODES_CHANGED:
                    event_queue.put(("hardware_change", None, None))
            return win32gui.DefWindowProc(hwnd, msg, wparam, lparam)

        wc = win32gui.WNDCLASS()
        wc.lpszClassName = "VGD Monitor"
        wc.lpfnWndProc = wndproc
        class_atom = win32gui.RegisterClass(wc)
        hwnd = win32gui.CreateWindow(
            class_atom, "VGD Monitor", 0, 0, 0, 0, 0, 0, 0, 0, None
        )
        win32gui.PumpMessages()

    return window_thread


if __name__ == "__main__":
    event_queue = queue.Queue()

    t = threading.Thread(target=get_window_thread(event_queue), daemon=True)
    t.start()

    try:
        main(event_queue)
    except KeyboardInterrupt:
        pass
