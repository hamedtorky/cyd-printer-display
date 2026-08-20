# CYD Printer Display

Read-only Klipper status display for the ESP32-2432S028R Cheap Yellow Display.

The firmware polls Moonraker every 1.5 seconds and displays:

- Klipper and print state
- Nozzle and bed actual/target temperatures
- Current filename
- Print progress
- Elapsed print time
- Wi-Fi signal and Moonraker connection state

The interface uses a black, white, red, and yellow theme. Static labels and
frames are drawn once; each dynamic region is redrawn only when its displayed
value changes, avoiding full-screen refresh flicker.

It does not send G-code or printer-control requests.

## Network setup

Wi-Fi credentials are not compiled into the firmware. On first boot, the screen
shows `Printer-Display-Setup`. Join that temporary Wi-Fi network from a phone or
computer, open `192.168.4.1`, and select the Wi-Fi network used by the printer.
The ESP32 stores those credentials locally.

The default Moonraker address in `include/config.h` is `printer.lan:7125`. If
local DNS does not resolve that name, use `192.168.1.138` for `MOONRAKER_HOST`.

If Moonraker requires authentication, set `MOONRAKER_API_KEY`. Leave it empty
when the display is on a trusted local network.

## Build

```sh
pio run
```

## Upload

The confirmed CYD serial port on this Mac is `/dev/cu.usbserial-13140`.

```sh
pio run --target upload
pio device monitor
```

Uploading replaces the firmware currently stored on the ESP32. Building does
not modify the device.
