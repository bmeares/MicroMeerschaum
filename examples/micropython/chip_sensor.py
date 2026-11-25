# SPDX-License-Identifier: Apache-2.0
"""Install this module on the ESP32-S3 for a source-driven Pipe."""
import esp32
import time


def register(pipe, **kw):
    return {"sensor": "ESP32-S3 die temperature"}


def fetch(pipe, **kw):
    return {"tick": time.ticks_ms(), "celsius": esp32.mcu_temperature()}
