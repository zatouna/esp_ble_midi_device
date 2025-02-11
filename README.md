# NimBLE GATT Server Example

## Overview

This repository provides an example of implementing a GATT server using the NimBLE stack on ESP32 devices. It extends the NimBLE Connection Example to demonstrate:

1. How to implement a GATT server using a GATT services table.
2. How to handle characteristic access requests:
   - Write access demonstrated by LED control.
   - Read and indicate access demonstrated by heart rate measurement (mocked).

## Features

- **GATT Services**: The example defines two primary GATT services:
  - **Heart Rate Service**: Includes a heart rate measurement characteristic with read and indicate permissions.
  - **Automation IO Service**: Includes an LED characteristic with write-only permission for controlling an LED.

- **Characteristic Access**:
  - **LED Control**: The LED characteristic allows writing to control an LED, turning it on or off based on the value written.
  - **Heart Rate Measurement**: The heart rate characteristic allows reading the current heart rate value and sending indications if subscribed.

- **BLE Advertising and Connection**: The example sets up BLE advertising and handles connection events, including advertising restart on disconnect.

- **Task Management**: A separate task updates the heart rate value periodically and sends indications if enabled.

## Setup and Usage

### Set Target

Before building the project, set the correct chip target using:
