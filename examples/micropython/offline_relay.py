# SPDX-License-Identifier: Apache-2.0
"""A captures, B takes custody, D archives. Each board opens only its own role."""
from micromeerschaum import provision
from mm_ble import Peripheral, deliver


def open_role(role, path, create=False):
    if role not in ("origin", "relay", "destination"):
        raise ValueError("unknown role")
    return provision("sensor", "temperature", path, origin=1, generation=5, destination=9,
                     custodian=None if role == "destination" else 2,
                     receiver=role != "origin", create=create)


def enrolled(device, node):
    # Fixed-address, previously enrolled, authenticated BLE bond on this board.
    def resolve(connection):
        if connection.bonded and connection.device == device:
            return node
    return resolve


async def receive_window(pipe, device, sender):
    peripheral = Peripheral(pipe, enrolled(device, sender))
    try:
        await peripheral.serve_once()
    finally:
        peripheral.close()


async def send_window(pipe, device, issuer):
    async with await device.connect() as connection:
        if not connection.encrypted:
            await connection.pair(bond=True, mitm=True, le_secure=True)
        authorize = enrolled(device, issuer)
        through = pipe.progress()["confirmed"]
        for _ in range(4):
            if through >= pipe.progress()["prefix"]:
                break
            next_through = await deliver(pipe, connection, authorize)
            if next_through <= through:
                raise OSError("receipt did not advance")
            through = next_through
        pipe.reclaim()
        return through
