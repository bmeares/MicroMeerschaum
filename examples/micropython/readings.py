# SPDX-License-Identifier: Apache-2.0
"""Install with device_identity.py on the MM_ESP32_S3 build."""
import _mmstorage
from micromeerschaum import provision
from device_identity import ORIGIN, GENERATION, DESTINATION


def open_readings(first_boot=False):
    _mmstorage.mount(first_boot)
    return provision("plugin:chip_sensor", "temperature", "/mm/TEMP.MM",
                     ORIGIN, GENERATION, DESTINATION, create=first_boot)
