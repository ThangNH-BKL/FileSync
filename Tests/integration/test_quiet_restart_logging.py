#!/usr/bin/env python3
"""Source-level regression checks for FileSync V5 quiet unchanged-file logging.

This verifies the wiring and non-regression guards; it does NOT execute the
Windows MFC applications or a real TCP transfer. Run with Python 3.
"""
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
SERVER = (ROOT / 'Common' / 'ClientSession.cpp').read_text(encoding='utf-8-sig')
CLIENT = (ROOT / 'Common' / 'ClientEngine.cpp').read_text(encoding='utf-8-sig')
CLIENT_HEADER = (ROOT / 'Common' / 'ClientEngine.h').read_text(encoding='utf-8-sig')
UI = (ROOT / 'FileClient' / 'CFClientDlg.cpp').read_text(encoding='utf-8-sig')
UI_HEADER = (ROOT / 'FileClient' / 'CFClientDlg.h').read_text(encoding='utf-8-sig')

def function(src, signature, end_next):
    start = src.index(signature)
    end = src.index(end_next, start + len(signature))
    return src[start:end]

s_start = function(SERVER, 'void ClientSession::TryStartNextFile()', 'void ClientSession::HandleFileDeferred(')
s_begin = function(SERVER, 'void ClientSession::HandleV5Begin(', 'void ClientSession::HandleV5Copy(')
s_same = function(SERVER, 'void ClientSession::HandleV5Unchanged(', '} // namespace fsyn')
c_ack = function(CLIENT, 'case MsgType::FILE_COMMITTED: {', 'case MsgType::SYNC_REQUEST_INIT: {')
c_same = function(CLIENT, 'if (size == v5_.baseSize &&', 'std::vector<ByteOp> operations;')
assert 'sync queue: started' not in s_start, 'Server must not log queued unchanged candidates'
assert 'sync queue: started' in s_begin, 'Changed-file start must be logged'
assert 'v5: unchanged ' not in s_same, 'Server must not log unchanged outcomes'
assert 'log_->Log(LogLevel::Info, "versioning: identical content saved as new file' in s_same, 'A genuinely new generation is logged even if bytes match'
assert 'ComputeFileSha256' in s_same, 'Server must still verify SHA-256'
assert 'SendCommitted(done,newVersion);' in s_same and 'SendCommitted(done,false);' in s_same, 'Server must ACK both unchanged and adopted legacy files'
assert 'NotifyTreeUpdate(done.relPath)' in s_same, 'Server Tree must still populate'
assert 'v5: unchanged, waiting Server ACK:' not in c_same, 'Client should be quiet on no-change'
assert 'awaitingUnchanged_ = true' in c_same, 'Client must track unchanged ACK'
assert 'const bool unchanged = awaitingUnchanged_' in c_ack
assert 'if (!unchanged)' in c_ack and 'OnFileDone' in c_ack, 'Only changed files reach UI file completion'
assert 'awaitingUnchanged_ = false;' in CLIENT, 'Unchanged state must be reset'
assert 'awaitingUnchanged_' in CLIENT_HEADER
assert 'SetTimer(kLogTimerId, 250' in UI and 'ON_WM_TIMER()' in UI
assert 'logQueue_.push_back(line)' in UI and 'i < 100' in UI
assert 'KillTimer(kLogTimerId)' in UI
assert 'std::deque<std::string> logQueue_' in UI_HEADER
assert 'session: tree end from' not in SERVER, 'Do not emit redundant snapshot progress on every restart'
assert 'v5: completed ' in SERVER and 'SHA256=PASS' in SERVER, 'Changed-file diagnostics preserved'
assert 'LogLevel::Warn' in SERVER and 'LogLevel::Warn' in CLIENT, 'Warnings preserved'
print('PASS: server unchanged files are silent but verified and acknowledged')
assert 'session: hello from' not in SERVER and 'session: closed for' not in SERVER, 'Normal session connect/disconnect must not add INFO noise'
print('PASS: routine server handshake/close is quiet')
print('PASS: client unchanged ACKs are silent, changed file commits are logged')
print('PASS: changed-file start/completion and warnings are retained')
print('PASS: client UI log batches limited to 100 lines / 250 ms')
