# Linux ALSA Focusrite Scarlett2 USB Protocol Mixer Driver

## Overview

The Focusrite USB audio interfaces are class compliant, meaning that
they work “out of the box” on Linux as audio and MIDI interfaces
(although on Gen 3/4/Vocaster you need to disable MSD mode first for
full functionality). However, except for some of the smallest models,
this kernel driver is required to access the full range of features of
these interfaces.

This driver is an extension to ALSA, specifically to the
`snd_usb_audio` module.

## Supported Interfaces

The interfaces currently supported are:
- Scarlett 2nd Gen 6i6, 18i8, 18i20
- Scarlett 3rd Gen Solo, 2i2, 4i4, 8i6, 18i8, 18i20
- Scarlett 4th Gen Solo, 2i2, 4i4
- Clarett 2Pre, 4Pre, 8Pre USB
- Clarett+ 2Pre, 4Pre, 8Pre
- Vocaster One and Vocaster Two

## Minimum Kernel Version Required

This driver is already a part of the mainstream Linux kernel since
Linux kernel version 5.4. Depending on the particular interface you
have, you might have the driver for your interface already installed:

- Scarlett Gen 2: Supported since Linux 5.4 (bug fixes in Linux 5.14)
- Scarlett Gen 3: Supported since Linux 5.14
- Clarett+ 8Pre: Supported since Linux 6.1
- Clarett 2Pre/4Pre/8Pre USB, Clarett+ 2Pre/4Pre: Supported since
  Linux 6.7
- Scarlett Gen 4: Supported since Linux 6.8
- Vocaster: Submitted upstream, should appear in Linux 6.10

Note: From Linux 6.7 onwards, the driver is enabled by default and
this is the first version where the level meters work.

**It's recommended that you use at least 6.7, or install the
backported driver (see below).**

## Repository Purpose

This repository is a fork of the Linux kernel, used for:

1) Sharing the development code before it arrives in the mainstream
kernel.

2) Sharing backports for the `snd_usb_audio` module so that you don't
have to wait for Linux 6.8 to access the Scarlett Gen 4 support or
Linux 6.10 to access the Vocaster support.

## Building the Driver

If you want to build an entire kernel from source, check the
`scarlett2` branch. Otherwise, visit the
[Releases](https://github.com/geoffreybennett/scarlett-gen2/releases)
page to download a backport of just the `snd_usb_audio` module, which
can be built against your running kernel.

## Usage Instructions

To use the controls introduced by this driver, please refer to the
dedicated GUI application repository: [ALSA Scarlett
GUI](https://github.com/geoffreybennett/alsa-scarlett-gui).
