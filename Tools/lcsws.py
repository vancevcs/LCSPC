#!/usr/bin/env python3
"""Minimal synchronous client for PPSSPP's WebSocket debugger, for LCS address work.

Start PPSSPP with --debugger=1337, then:

  python Tools/lcsws.py status                       game.status and cpu.status
  python Tools/lcsws.py read 0x08b5e174 [u8|u16|u32|f32] [count]
  python Tools/lcsws.py watch 0x08b5e174 [type] [seconds]   one read per second
  python Tools/lcsws.py write 0x08b5e148 f32 1.0
  python Tools/lcsws.py press cross [frames]         hold a PSP button, then release

Protocol: docs/WebSocketDebugger.md. Needs `pip install websockets` (13+, for the sync client).
Button injection is the only input this uses; it never sends Windows keystrokes.
"""
import itertools
import json
import struct
import sys
import time

from websockets.sync.client import connect

SUBPROTOCOL = 'debugger.ppsspp.org'
_tickets = itertools.count(1)


class Debugger:
    def __init__(self, port=1337):
        self.ws = connect(f'ws://127.0.0.1:{port}/debugger', subprotocols=[SUBPROTOCOL], max_size=None)

    def call(self, event, timeout=5.0, **params):
        ticket = next(_tickets)
        self.ws.send(json.dumps(dict(event=event, ticket=ticket, **params)))
        deadline = time.time() + timeout
        while True:
            msg = json.loads(self.ws.recv(timeout=max(0.01, deadline - time.time())))
            if msg.get('ticket') == ticket:
                if msg.get('event') == 'error':
                    raise RuntimeError(msg.get('message'))
                return msg

    def read(self, addr, kind='u32'):
        if kind == 'f32':
            raw = self.call('memory.read_u32', address=addr)['uintValue']
            return struct.unpack('<f', struct.pack('<I', raw))[0]
        return self.call(f'memory.read_{kind}', address=addr)['uintValue']

    def write(self, addr, kind, value):
        if kind == 'f32':
            value = struct.unpack('<I', struct.pack('<f', float(value)))[0]
            kind = 'u32'
        return self.call(f'memory.write_{kind}', address=addr, value=int(value))

    def press(self, button, frames=3):
        self.call('input.buttons.send', buttons={button: True})
        time.sleep(frames / 60.0)
        self.call('input.buttons.send', buttons={button: False})

    def close(self):
        self.ws.close()


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    dbg = Debugger()
    try:
        cmd = args[0]
        if cmd == 'status':
            print(json.dumps(dbg.call('game.status')))
            print(json.dumps(dbg.call('cpu.status')))
        elif cmd in ('read', 'watch'):
            addr = int(args[1], 0)
            kind = args[2] if len(args) > 2 else 'u32'
            if cmd == 'read':
                count = int(args[3]) if len(args) > 3 else 1
                step = {'u8': 1, 'u16': 2}.get(kind, 4)
                for i in range(count):
                    print(f'{addr + i * step:#010x}  {dbg.read(addr + i * step, kind)}')
            else:
                seconds = int(args[3]) if len(args) > 3 else 5
                for _ in range(seconds):
                    print(f'{time.strftime("%H:%M:%S")}  {dbg.read(addr, kind)}')
                    time.sleep(1.0)
        elif cmd == 'write':
            print(dbg.write(int(args[1], 0), args[2], args[3]))
        elif cmd == 'press':
            dbg.press(args[1], int(args[2]) if len(args) > 2 else 3)
        else:
            sys.exit(f'unknown command {cmd}')
    finally:
        dbg.close()


if __name__ == '__main__':
    main()
