# SPDX-License-Identifier: Apache-2.0
"""Application entry points for a provisioned device. Call start() once per boot."""
from micromeerschaum import ActionRegistry
from mm_session import DeviceSession, serve_usb

actions = ActionRegistry()


@actions.make_action("device label")
def device_label(words):
    if words:
        return False, "device label takes no arguments"
    return True, "room monitor"


def start(pipe, allow_writes=False):
    actions.expose(pipe, read=True, write=allow_writes)
    ops = ["catalog", "read", "actions", "complete", "action"]
    if allow_writes:
        ops.append("write")
    return DeviceSession(actions, ops, ("help", "show actions", "show pipes", "device label"))


def usb(session):
    serve_usb(session)


async def bluetooth_window(session, enrolled_device):
    from mm_action_ble import ManagementPeripheral
    service = ManagementPeripheral(session, (enrolled_device,))
    try:
        await service.serve_once()
    finally:
        service.close()
