# SPDX-License-Identifier: Apache-2.0
"""Exercise the management service from a second board over real NimBLE/aioble."""

# Provisioning and test assertions run locally on each board's test console.
# The central invokes management operations exclusively through the GATT link.
SERVER = """
from mm_action_ble import ManagementPeripheral
from mm_actions import ActionRegistry
from mm_session import DeviceSession
from micromeerschaum import provision
app = ActionRegistry()
@app.make_action('device echo')
def echo(args):
    return True, ' '.join(args)
@app.make_action('device fail')
def fail(args):
    return False, 'sample unavailable'
@app.make_action('device private')
def private(args):
    raise AssertionError('private action reached')
pipe = provision('sensor','room','/mm/room.mm',1,1,2,create=True)
app.expose(pipe,read=True,write=True)
session = DeviceSession(app,('catalog','read','write','action','actions','complete'),
                        ('help','device echo','device fail'))
peripheral = ManagementPeripheral(session,(other,))
"""

CLIENT = """
import json, collections, bluetooth
from mm_action_ble import SERVICE, RX, TX
async def call(rx,tx,n,op,**kw):
    req=dict(v=1,id=str(n),op=op)
    req.update(kw)
    data=b'@micro '+json.dumps(req).encode()+b'\\n'
    for i in range(0,len(data),17):
        await rx.write(data[i:i+17],response=True)
    data=b''
    while not data.endswith(b'\\n'):
        data+=await tx.notified(timeout_ms=6000)
    assert data.startswith(b'@micro ') and len(data)<=4096,data
    reply=json.loads(data[7:])
    assert reply['id']==str(n) and reply['v']==1,reply
    return reply

async def exercise(authorized):
    async with await other.connect() as c:
        if authorized and not c.encrypted:
            await c.pair(bond=True,mitm=True,le_secure=True)
        service=await c.service(bluetooth.UUID(SERVICE))
        rx=await service.characteristic(bluetooth.UUID(RX))
        tx=await service.characteristic(bluetooth.UUID(TX))
        tx._notify_queue=collections.deque((), 96)
        await tx.subscribe()
        key=['sensor','room',None]
        if not authorized:
            try:
                await call(rx,tx,1,'write',key=key,rows=[{'x':999}])
            except (OSError,aioble.GattError,asyncio.TimeoutError):
                return
            raise AssertionError('unbonded mutation accepted')
        assert c.encrypted and c.authenticated and c.bonded
        r=await call(rx,tx,2,'catalog')
        assert r['data']['items'][0]['key']==key,r
        r=await call(rx,tx,3,'write',key=key,rows=[{'x':23}])
        assert r['ok'],r
        r=await call(rx,tx,4,'read',key=key,limit=3)
        assert r['data']==[{'x':23}],r
        r=await call(rx,tx,5,'action',line='device echo a b')
        assert r['data']=='a b',r
        r=await call(rx,tx,6,'action',line='device fail')
        assert r['error']=='sample unavailable',r
        r=await call(rx,tx,7,'action',line='device private')
        assert not r['ok'],r
        r=await call(rx,tx,8,'complete',line='device ')
        assert r['data']==['device echo','device fail'],r
        r=await call(rx,tx,9,'write',key=['sensor','new',None],rows=[{'x':1}])
        assert not r['ok'],r
        r=await call(rx,tx,11,'register',key=['sensor','new',None])
        assert not r['ok'],r
        label='abc'*400
        r=await call(rx,tx,10,'action',line='device echo '+label)
        assert r['data']==label,r

async def check_removed():
    try:
        async with await other.connect() as c:
            await c.disconnected(timeout_ms=6000)
    except aioble.DeviceDisconnectedError:
        pass
"""


async def before_enrollment(sink, source, exchange):
    await sink.execute(SERVER)
    await source.execute(CLIENT)
    await exchange(sink, source,
                   "print('LISTENING')\nasyncio.run(peripheral.serve_once())\n"
                   "assert pipe.progress()['high']==0\nprint('UNBONDED WRITE DENIED')",
                   "asyncio.run(exercise(False))")


async def after_enrollment(sink, source, exchange):
    await exchange(sink, source,
                   "print('LISTENING')\nasyncio.run(peripheral.serve_once())\n"
                   "assert pipe.progress()['high']==1\nassert pipe.read_doc(1)=={'x':23}\n"
                   "print('MANAGEMENT OK')",
                   "asyncio.run(exercise(True))\nprint('CLIENT OK')")
    await exchange(sink, source,
                   "peripheral.peers=()\nprint('LISTENING')\nasyncio.run(peripheral.serve_once())\n"
                   "assert pipe.progress()['high']==1\nperipheral.close()\nprint('REMOVED PEER DENIED')",
                   "asyncio.run(check_removed())")
