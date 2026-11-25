"""An authenticated, enrolled node must also be this Pipe's delivery issuer."""
import asyncio
import sys
from micromeerschaum import provision
from mm_ble import deliver

class Bluetooth:
    UUID = str

class OtherPeer:
    encrypted = authenticated = bonded = True
    def is_connected(self):
        return True
    async def service(self, uuid, timeout_ms):
        raise AssertionError("attempted GATT discovery on the wrong peer")

sys.modules["bluetooth"] = Bluetooth
pipe = provision("sensor", "private", sys.argv[1] + "/ble.mm", 1, 2, 3, create=True)
assert pipe.sync({"tick": 1})[0]
async def test():
    try:
        await deliver(pipe, OtherPeer(), lambda connection: 4)
        raise AssertionError("accepted a different enrolled node")
    except ValueError:
        pass

asyncio.run(test())
assert pipe.progress()["confirmed"] == 0
pipe.close()
