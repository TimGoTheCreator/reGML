"""Read-only debugging inventory for the legacy GameMaker package profile."""
import struct, sys
data = open(sys.argv[1], 'rb').read()
def u(p): return struct.unpack_from('<I', data, p)[0]
def i(p): return struct.unpack_from('<i', data, p)[0]
def s(p): return data[p:data.index(b'\0', p)].decode('utf8', errors='replace') if p else ''
chunks={}; at=8
while at<len(data):
    tag=data[at:at+4].decode(); size=u(at+4); chunks[tag]=at+8; at+=8+size
def table(at): return [u(at+4+n*4) for n in range(u(at))]
codes=table(chunks['CODE']); objects=table(chunks['OBJT']); rooms=table(chunks['ROOM'])
order_at=chunks['GEN8']+128
order=[u(order_at+4+n*4) for n in range(u(order_at))]
print('Room order:', [(r,s(u(rooms[r]))) for r in order[:8]])
for r in order[:3]:
    p=rooms[r]; print('ROOM',r,s(u(p)),'creation',i(p+32))
    for inst in table(u(p+48)):
        obj=i(inst+8); op=objects[obj]; print('INSTANCE',u(inst+12),obj,s(u(op)),'create',i(inst+16),'precreate',i(inst+36))
        evgroups=table(op+80+u(op+64)*8)
        for kind,group in enumerate(evgroups):
            for event in table(group):
                for action in table(event+4):
                    code=i(action+32); print(' EVENT',kind,u(event),code,s(u(codes[code])) if code>=0 else '')
