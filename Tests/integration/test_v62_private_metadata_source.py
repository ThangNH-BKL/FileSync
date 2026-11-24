#!/usr/bin/env python3
"""Source-shape regression guard, not an integrated Windows runtime test."""
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
STORE=(ROOT/'Common/FileVersionStore.h').read_text(encoding='utf-8-sig')
SERVER=(ROOT/'Common/ClientSession.cpp').read_text(encoding='utf-8-sig')
CLIENT=(ROOT/'Common/ClientEngine.cpp').read_text(encoding='utf-8-sig')
assert 'FSYN-VERSION-2' in STORE
assert 'StateManifestPath' in STORE and 'Sha256::HashBuffer' in STORE
assert '.fsync_state' in STORE
assert 'baseline.log' in STORE and 'versions\\' in STORE
assert 'keyHex,false' in STORE and 'IsContained(old,entries)' in STORE
assert 'const std::wstring legacySidecar = logical + L".fsync.versions";' in STORE
assert 'Save(manifest,entries,keyHex)' in STORE and 'Save(legacySidecar' not in STORE
assert 'DeleteFileW(legacySidecar.c_str())' in STORE
assert 'JournalPath(' in STORE and 'journals\\' in STORE
assert 'if (!unchanged)' in CLIENT and 'awaitingUnchanged_ = true' in CLIENT
assert 'SendCommitted(done,newVersion)' in SERVER and 'SendCommitted(done,false)' in SERVER
assert 'session: hello from' not in SERVER and 'session: closed for' not in SERVER
assert 'v5: completed ' in SERVER and 'SHA256=PASS' in SERVER
assert 'LogLevel::Warn' in SERVER and 'LogLevel::Error' in SERVER
print('PASS private metadata path, V2 manifest format, migration and fail-closed shape')
print('PASS no new adjacent version sidecar written')
print('PASS unchanged file ACK+SHA verification retained and logging quiet')
print('PASS changed/new version and warning/error logs retained')
