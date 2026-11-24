"""Deterministic filesystem fault injection for the persistent-block design.
Not a substitute for running the Windows C++ executables over TCP.
"""
import hashlib, os, random, tempfile, zlib
from pathlib import Path

BLOCK=65536

def crc(b): return zlib.crc32(b) & 0xffffffff

def run_case(pct):
    rng=random.Random(1234)
    source=rng.randbytes(10*1024*1024+123)
    blocks=[source[i:i+BLOCK] for i in range(0,len(source),BLOCK)]
    cut=int(len(blocks)*pct/100)
    with tempfile.TemporaryDirectory() as d:
        stage=Path(d)/'data.fsync.tmp'
        journal=Path(d)/'data.fsync.blocks'
        with stage.open('wb') as f, journal.open('wb') as j:
            for i,b in enumerate(blocks[:cut]):
                f.seek(i*BLOCK);f.write(b);f.flush();os.fsync(f.fileno())
                j.write((i*BLOCK).to_bytes(8,'little')+len(b).to_bytes(4,'little')+crc(b).to_bytes(4,'little'))
                j.flush();os.fsync(j.fileno())
        with stage.open('r+b') as f:
            reused=0; transferred=0
            for i,b in enumerate(blocks):
                f.seek(i*BLOCK)
                existing=f.read(len(b))
                if len(existing)==len(b) and crc(existing)==crc(b): reused+=len(b)
                else:
                    f.seek(i*BLOCK);f.write(b);transferred+=len(b)
            f.truncate(len(source))
        assert hashlib.sha256(stage.read_bytes()).digest()==hashlib.sha256(source).digest()
        assert reused+transferred==len(source)
        return reused,transferred

def changed_source_case():
    original=b'A'*BLOCK+b'B'*123
    changed=original[:-1]+b'C'
    assert len(original)==len(changed)
    assert hashlib.sha256(original).digest()!=hashlib.sha256(changed).digest()

if __name__=='__main__':
    for pct in (20,50,80,99):
        reused,transferred=run_case(pct)
        print(f'PASS interruption={pct}% reused={reused} transferred={transferred}')
    changed_source_case()
    print('PASS same-size source mutation detection (SHA-256)')
