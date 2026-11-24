"""Portable deterministic fault-injection test of the staging/block reconciliation algorithm.
NOT a Windows TCP integration test. Run: python test_resume_fault_injection.py
"""
import hashlib, os, tempfile, zlib
from pathlib import Path
BLOCK=65536

def offsets(source, staged):
    out=[]
    for off in range(0,len(source),BLOCK):
        block=source[off:off+BLOCK]
        candidate=staged[off:off+len(block)]
        if len(candidate)!=len(block) or zlib.crc32(candidate)!=zlib.crc32(block):
            out.append(off)
    return out

def run():
    # Non-multiple block size to verify partial final block.
    source=bytes((i*37 + i//97)%256 for i in range(BLOCK*160+12345))
    print('source_bytes=',len(source),'blocks=',(len(source)+BLOCK-1)//BLOCK)
    for percent in (20,50,80,99):
        with tempfile.TemporaryDirectory() as d:
            stage=Path(d)/'data.fsync.tmp'; target=Path(d)/'data'
            cut=(len(source)*percent//100)//BLOCK*BLOCK
            stage.write_bytes(source[:cut])
            # Simulate process/session teardown and reopen the same stage.
            persisted=stage.read_bytes()
            need=offsets(source,persisted)
            transferred=sum(len(source[off:off+BLOCK]) for off in need)
            with stage.open('r+b') as f:
                for off in need:
                    f.seek(off);f.write(source[off:off+BLOCK])
            assert hashlib.sha256(stage.read_bytes()).digest()==hashlib.sha256(source).digest()
            os.replace(stage,target)
            assert target.read_bytes()==source
            assert transferred < len(source)
            print(f'PASS disconnect={percent}% persisted={cut} resent={transferred} reused={len(source)-transferred} sha256=OK')
    # Same-size different content
    changed=bytearray(source);changed[BLOCK*3+7]^=0x55
    assert offsets(changed,source)==[BLOCK*3]
    print('PASS same-size-change: only changed block requested')
    assert offsets(source,source)==[]
    print('PASS identical: zero blocks requested')
    assert offsets(source,source[:-12345])==[BLOCK*160]
    print('PASS partial-last-block: requested when missing')
    print('ALL 7 SIMULATION TESTS PASSED (NOT WINDOWS TCP TESTS)')
if __name__=='__main__':run()
