"""Exercise native gameplay with a synthetic local Steam peer.

This is a backend smoke test, not a complete vanilla client implementation.
Join gates use diagnostic payloads accepted by the research listener. The
paint command and returned NetObj state use recovered game wire formats.
"""
import argparse
import ctypes as C
import json
import os
from pathlib import Path
import struct
import sys
import time
import lz4.block
import identity_probe as identity
import listener_probe as network
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'probe'))
from inspect_world_sync import net_objects, aggregate_children

def paint_command(body, shape, color):
    host = struct.pack('<IIB6iI', body, shape, 4, 0, 0, 0, 0, 0, 0, color)
    return b'\x24' + host[::-1]

def run(dll, remote_id, body, shape, color, timeout):
    if os.name != 'nt' or C.sizeof(C.c_void_p) != 8: raise RuntimeError('Windows x64 required')
    result = {'format':'scrapds.synthetic-gameplay-smoke.v1', 'vanilla_client_test':False,
              'server_id':str(remote_id), 'body':body, 'shape':shape, 'color_abgr':color,
              'received_ids':[], 'paint_sent':False, 'native_state_confirmed':False}
    old = {k:os.environ.get(k) for k in ['SteamAppId','SteamGameId']}
    os.environ.update(SteamAppId='387990',SteamGameId='387990')
    functions={};initialized=False;connection=0;sockets=None;group=0;directory=None
    try:
        directory=os.add_dll_directory(str(dll.parent)); library=C.CDLL(str(dll))
        signatures=dict(identity.SIGNATURES);signatures.update(network.SIGNATURES)
        signatures.update({
            'SteamAPI_Init':(C.c_bool,[]), 'SteamAPI_Shutdown':(None,[]),
            'SteamAPI_GetHSteamPipe':(C.c_int32,[]),
            'SteamAPI_SteamNetworkingSockets_SteamAPI_v012':(C.c_void_p,[]),
            'SteamAPI_ISteamNetworkingSockets_ConnectP2P':(C.c_uint32,[C.c_void_p,C.POINTER(network.NetworkIdentity),C.c_int,C.c_int,C.c_void_p]),
            'SteamAPI_ISteamNetworkingSockets_SendMessageToConnection':(C.c_int,[C.c_void_p,C.c_uint32,C.c_void_p,C.c_uint32,C.c_int,C.POINTER(C.c_int64)]),
            'SteamAPI_ISteamNetworkingSockets_FlushMessagesOnConnection':(C.c_int,[C.c_void_p,C.c_uint32]),
        })
        for name,(restype,args) in signatures.items():
            f=getattr(library,name);f.restype=restype;f.argtypes=args;functions[name]=f
        initialized=bool(functions['SteamAPI_Init']())
        if not initialized:raise RuntimeError('Steam user initialization failed')
        sockets=functions['SteamAPI_SteamNetworkingSockets_SteamAPI_v012']()
        own=network.NetworkIdentity()
        if not sockets or not functions['SteamAPI_ISteamNetworkingSockets_GetIdentity'](sockets,C.byref(own)) or own.steam_id() == 0:raise RuntimeError('Unexpected local Steam identity')
        functions['SteamAPI_ManualDispatch_Init']();pipe=functions['SteamAPI_GetHSteamPipe']()
        remote=network.NetworkIdentity();remote.kind=16;remote.size=8
        for i,b in enumerate(struct.pack('<Q',remote_id)):remote.data[i]=b
        functions['SteamAPI_ISteamNetworkingSockets_InitAuthentication'](sockets)
        connection=functions['SteamAPI_ISteamNetworkingSockets_ConnectP2P'](sockets,C.byref(remote),0,0,None)
        group=functions['SteamAPI_ISteamNetworkingSockets_CreatePollGroup'](sockets)
        if not connection or not group or not functions['SteamAPI_ISteamNetworkingSockets_SetConnectionPollGroup'](sockets,connection,group):raise RuntimeError('Cannot prepare synthetic connection')
        def send(payload,compressed=False):
            wire=payload[:1]+lz4.block.compress(payload[1:],store_size=False) if compressed and len(payload)>1 else payload
            data=C.create_string_buffer(wire);number=C.c_int64()
            code=functions['SteamAPI_ISteamNetworkingSockets_SendMessageToConnection'](sockets,connection,data,len(wire),8,C.byref(number))
            if code!=1:raise RuntimeError(f'SNS send failed: {code}')
            functions['SteamAPI_ISteamNetworkingSockets_FlushMessagesOnConnection'](sockets,connection)
        joined=False;sent_join=False;deadline=time.monotonic()+timeout
        while time.monotonic()<deadline:
            functions['SteamAPI_ManualDispatch_RunFrame'](pipe)
            callback=identity.CallbackMsg()
            while functions['SteamAPI_ManualDispatch_GetNextCallback'](pipe,C.byref(callback)):
                try:
                    if callback.callback==1221 and callback.parameter:
                        event=network.parse_status(C.string_at(callback.parameter,callback.size))
                        if event['state'] in [4,5]:raise RuntimeError('Connection ended: '+event['end_debug'])
                finally:functions['SteamAPI_ManualDispatch_FreeLastCallback'](pipe)
            messages=(C.POINTER(network.Message)*32)()
            count=functions['SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnPollGroup'](sockets,group,messages,32)
            if count<0 or count>32:raise RuntimeError('SNS receive failed')
            try:
                for i in range(count):
                    message=messages[i].contents
                    if message.size<1 or message.size>16*1024*1024:raise RuntimeError('Invalid message size')
                    wire=C.string_at(message.data,message.size)
                    plain=wire if len(wire)==1 else wire[:1]+lz4.block.decompress(wire[1:],uncompressed_size=16*1024*1024)
                    packet_id=plain[0]
                    if len(result['received_ids'])<256:result['received_ids'].append(packet_id)
                    if packet_id==3:send(b'\x04\x00'+bytes(8))
                    elif packet_id==5:send(b'\x01')
                    elif packet_id==2 and not sent_join:send(b'\x06\x00',True);sent_join=True
                    elif packet_id==7:send(b'\x09\x00',True)
                    elif packet_id==10 and not joined:
                        joined=True;send(paint_command(body,shape,color),True);result['paint_sent']=True
                    elif packet_id in [21,22,29] and result['paint_sent']:
                        pending=[plain]
                        while pending:
                            p=pending.pop()
                            if p[0]==29:pending.extend(b for _,b in aggregate_children(p));continue
                            if p[0] not in [21,22]:continue
                            _,records=net_objects(p)
                            for record in records:
                                payload=record['payload']
                                if record['operation']==3 and record['type']==1 and record['id']==shape and len(payload)>=16 and int.from_bytes(payload[12:16],'big')==color:
                                    result['native_state_confirmed']=True
                                    result['state_packet_tick']=int.from_bytes(p[1:5],'big')
            finally:
                for i in range(count):functions['SteamAPI_SteamNetworkingMessage_t_Release'](messages[i])
            if result['native_state_confirmed']:break
            time.sleep(.01)
        result['status']='native_paint_confirmed' if result['native_state_confirmed'] else 'timeout'
        return result
    finally:
        if sockets and connection:functions['SteamAPI_ISteamNetworkingSockets_CloseConnection'](sockets,connection,1000,b'synthetic gameplay test complete',False)
        if sockets and group:functions['SteamAPI_ISteamNetworkingSockets_DestroyPollGroup'](sockets,group)
        if initialized:functions['SteamAPI_Shutdown']()
        if directory:directory.close()
        for k,v in old.items():
            if v is None:os.environ.pop(k,None)
            else:os.environ[k]=v

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--dll',type=Path,required=True);p.add_argument('--ready',type=Path,required=True)
    p.add_argument('--body',type=int,required=True);p.add_argument('--shape',type=int,required=True)
    p.add_argument('--color',type=lambda s:int(s,0),required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--timeout',type=float,default=30)
    a=p.parse_args()
    if a.output.exists():p.error('output must be new')
    if not 0<a.timeout<=60 or not all(0<=v<=0xffffffff for v in [a.body,a.shape,a.color]):p.error('invalid timeout or uint32 field')
    ready=json.loads(a.ready.read_text());remote=int(ready['steam_id'])
    if ((remote>>52)&15)!=4 or remote>>56!=1:p.error('ready file does not describe an anonymous public GameServer')
    try:r=run(a.dll.resolve(strict=True),remote,a.body,a.shape,a.color,a.timeout)
    except Exception as e:r={'status':'probe_error','error':str(e),'vanilla_client_test':False}
    with a.output.open('x',encoding='utf8') as f:json.dump(r,f,indent=2)
    print(json.dumps(r));sys.exit(0 if r['status']=='native_paint_confirmed' else 1)
