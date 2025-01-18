# Linux ALSA Focusrite Control/Mixer Drivers

## Overview

Focusrite USB audio interfaces are class compliant, meaning they work
“out of the box” on Linux as audio and MIDI interfaces\*.

However, to access device-specific features (like direct monitoring,
hardware mixing, custom routing, and level meters), most models need
additional driver support.

This repository provides two drivers that extend the Linux kernel’s
audio subsystem (ALSA), adding controls to make those features
available:

- **Scarlett2**: A mature kernel driver supporting nearly every
  Focusrite USB device from the 2nd Gen Scarlett range onwards.

- **FCP**: A new hybrid kernel/user-space driver with support for the
  new big 4th Gen Scarlett models (16i16, 18i16, and 18i20).

Note:

- Scarlett 1st Gen devices are supported by the Scarlett driver which
  has been in the Linux kernel since 2014.

- Scarlett 2nd Gen Solo, 2i2, and 2i4 have no software-accessible
  controls, so no need for any special driver. All device-specific
  functionality is accessible from the front panel.

\* Scarlett 3rd and 4th Gen and Vocaster interfaces need MSD (Mass
Storage Device/“Easy Start”) mode disabled for full functionality.
This will be done automatically when you do a firmware update.

## Supported Interfaces

### Scarlett2 Driver

Scarlett Series:
- 2nd Gen: 6i6, 18i8, 18i20
- 3rd Gen: Solo, 2i2, 4i4, 8i6, 18i8, 18i20
- 4th Gen: Solo, 2i2, 4i4

Clarett USB and Clarett+ Series:
- 2Pre, 4Pre, 8Pre

Vocaster Series:
- One, Two

### FCP Driver

Scarlett 4th Gen:
- 16i16, 18i16, 18i20

## Kernel Version Requirements

If your device is supported by the Scarlett2 driver and your Linux
kernel meets the minimum version listed below, you don’t need to
install this driver as you already have it; jump ahead to the [Setup
Instructions](#setup-instructions).

The `scarlett2` driver is already included in the Linux kernel,
supporting these models:

- Scarlett 2nd Gen: Since Linux 5.4 (bug fixes in 5.14 and 6.7)
- Scarlett 3rd Gen: Since Linux 5.14 (bug fixes in 6.7)
- Scarlett 4th Gen (Solo/2i2/4i4): Since Linux 6.8
- Clarett+ 8Pre: Since Linux 6.1 (bug fixes in 6.7)
- Clarett USB and Clarett+ (2Pre/4Pre/8Pre): Since Linux 6.7
- Vocaster: Since Linux 6.10

The `fcp` driver is currently only available in this repository and in
the ALSA `for-next` branch.

## Repository Purpose

This repository is a fork of the Linux kernel, used for:

1. Sharing development code before it arrives in the mainstream kernel

2. Sharing backports of the `scarlett2` and `fcp` drivers so you don’t
   have to wait for your distribution to update to the kernel version
   you need.

## Installation Options

1. Module Installation (recommended):

    - Download from
      [Releases](https://github.com/geoffreybennett/linux-fcp/releases)
    - Build and install the `snd_usb_audio` module following the
      instructions in the enclosed `README.md`.

2. Full Kernel Build:

    - Check out the `fcp` branch
    - Build the kernel normally

## Setup Instructions

### Scarlett2 Driver

- Install the
  [firmware](https://github.com/geoffreybennett/scarlett2-firmware).

- Install and run [ALSA Scarlett
  GUI](https://github.com/geoffreybennett/alsa-scarlett-gui) to update
  your firmware and configure your device.

### FCP Driver

- Contact the author by email for assistance while this is under
  development.

- Install the
  [firmware](https://github.com/geoffreybennett/scarlett4-firmware).

- Install the `fcp-server` user-space driver and the `fcp-firmware`
  firmware update utility from the
  [fcp-support](https://github.com/geoffreybennett/fcp-support) repo.

- Update your firmware with `fcp-tool`.

- Install and run [ALSA Scarlett
  GUI](https://github.com/geoffreybennett/alsa-scarlett-gui) to
  configure your device (use the g4-big branch for now).

## Support

Please report any issues with the kernel driver to:

- https://github.com/geoffreybennett/linux-fcp/issues

If using the FCP driver, please report any issues with the user-space
driver or tools to:

- https://github.com/geoffreybennett/fcp-support/issues

## Contact

- Author: Geoffrey D. Bennett
- Email: g@b4.vu
- GitHub: https://github.com/geoffreybennett

## Donations

This software, including the driver, tools, and GUI is Free Software
that I’ve independently developed using my own resources. It
represents hundreds of hours of development work.

If you find this software valuable, please consider making a donation.
Your show of appreciation, more than the amount itself, motivates me
to continue improving these tools.

You can donate via:

- LiberaPay: https://liberapay.com/gdb
- PayPal: https://paypal.me/gdbau
- Zelle: g@b4.vu

## Disclaimer Third Parties

Focusrite, Scarlett, Clarett, and Vocaster are trademarks or
registered trademarks of Focusrite Audio Engineering Limited in
England, USA, and/or other countries. Use of these trademarks does not
imply any affiliation or endorsement of this software.
