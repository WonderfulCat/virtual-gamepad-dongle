# Copyright (c) 2026 Jacek Fedorynski
# SPDX-License-Identifier: MIT

import pywinusb.hid
import time


class ControllerState:
    def __init__(self):
        self.a = False
        self.b = False
        self.x = False
        self.y = False
        self.left = False
        self.right = False
        self.up = False
        self.down = False
        self.lb = False
        self.rb = False
        self.ls = False
        self.rs = False
        self.back = False
        self.start = False
        self.guide = False
        self.lt = 0
        self.rt = 0
        self.lx = 0
        self.ly = 0
        self.rx = 0
        self.ry = 0

    def get_report(self):
        report = bytearray([0, 0, 20] + [0] * 18)

        if self.up:
            report[3] |= 0x01
        if self.down:
            report[3] |= 0x02
        if self.left:
            report[3] |= 0x04
        if self.right:
            report[3] |= 0x08
        if self.start:
            report[3] |= 0x10
        if self.back:
            report[3] |= 0x20
        if self.ls:
            report[3] |= 0x40
        if self.rs:
            report[3] |= 0x80

        if self.lb:
            report[4] |= 0x01
        if self.rb:
            report[4] |= 0x02
        if self.guide:
            report[4] |= 0x04
        if self.a:
            report[4] |= 0x10
        if self.b:
            report[4] |= 0x20
        if self.x:
            report[4] |= 0x40
        if self.y:
            report[4] |= 0x80

        report[5] = max(0, min(255, int(self.lt)))
        report[6] = max(0, min(255, int(self.rt)))

        lx = max(-32768, min(32767, int(self.lx)))
        report[7] = lx & 0xFF
        report[8] = (lx & 0xFF00) >> 8

        ly = max(-32768, min(32767, int(self.ly)))
        report[9] = ly & 0xFF
        report[10] = (ly & 0xFF00) >> 8

        rx = max(-32768, min(32767, int(self.rx)))
        report[11] = rx & 0xFF
        report[12] = (rx & 0xFF00) >> 8

        ry = max(-32768, min(32767, int(self.ry)))
        report[13] = ry & 0xFF
        report[14] = (ry & 0xFF00) >> 8

        return report


devices = pywinusb.hid.find_all_hid_devices()

dongle = None
for d in devices:
    if "hid#vid_2dc8&pid_3106&mi_01" in d.device_path:
        dongle = d
        dongle.open()
        break
else:
    raise Exception("dongle not found")

state = ControllerState()
state.lb = True

while True:
    state.lb = not state.lb
    state.rb = not state.rb

    dongle.send_output_report(state.get_report())
    time.sleep(0.25)
