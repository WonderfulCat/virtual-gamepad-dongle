# Virtual Gamepad Dongle

This project lets you create a virtual XInput-compatible game controller on Windows and programmatically send inputs through it. It is an alternative to kernel driver based solutions (ViGEmBus, USB/IP, etc.). Because it uses a hardware component - a cheap microcontroller board like the Raspberry Pi Pico - it doesn't require a kernel driver or even administrator privileges on your computer. From the computer's point of view the hardware is real.

You could use it to:

* make a non-Xbox controller visible to games that only use the XInput API
* merge multiple controllers into a single virtual one
* generate gamepad inputs using eye-tracking or speech input
* generate completely synthetic inputs to automate in-game actions

or do anything else where programmaticaly generated game controller inputs are useful.

(Please note that only some of the above is possible using the included software, others would require some additional effort.)

## How it works

When you flash a Pico board (or similar) with the included firmware, it starts working as an Xbox 360 controller, but instead of sending inputs based on the state of physical buttons or joysticks, it sends back inputs that it receives from the computer (on a second vendor-specific HID interface). The inputs are prepared by software running on the computer, the only thing the dongle does is it makes them look like they're coming from a real Xbox 360 controller.

## How to prepare the dongle

You're going to need a board with either the RP2040 or the RP2350 chip, like the Raspberry Pi Pico or Pico 2 or one of the many third party boards.

(The firmware is made with Zephyr, so you could compile a build for pretty much any board supported by Zephyr that has USB, but we have pre-built binaries for the RP2040 and RP2350.)

Hold the BOOTSEL button while connecting the board to your computer. A drive named "RPI-RP2" (or "RP2350") should appear. Download the [dongle-pico.uf2](https://github.com/jfedor2/virtual-gamepad-dongle/releases/latest/download/dongle-pico.uf2) file (or [dongle-pico2.uf2](https://github.com/jfedor2/virtual-gamepad-dongle/releases/latest/download/dongle-pico2.uf2) for RP2350 boards) and copy it to that drive. After it finishes copying the board is ready to use as a virtual gamepad dongle.

## Included software

We have some example software that shows how the dongle can be used to send synthetic inputs through the virtual controller.

Pre-built executables are included in the [latest release](https://github.com/jfedor2/virtual-gamepad-dongle/releases/latest).

* [any2xinput](any2xinput): a C++/SDL application that reads inputs from all connected controllers, merges them into one and sends them through the dongle, making them visible to apps using the XInput API. You can enable/disable individual controllers.
* [steam_controller](steam_controller): a Python application that reads inputs from a connected Steam Controller and sends them through the dongle, letting you use the controller in non-Steam games. You could use any2xinput to achieve the same result, but with this rumble also works.
* [basic_example](basic_example): a very simple Python application that shows how to generate completely synthetic inputs.