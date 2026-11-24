# Đo hiệu năng FileSync V8.3

Chạy `Measure-FileSync.ps1` ở PowerShell riêng cùng máy với hai EXE. Nếu FileClient/FileServer đặt khác máy, chạy monitor riêng cho mỗi máy (điều chỉnh PID tạm; tiến trình không tồn tại sẽ có warning).

1. `Get-Process FileClient,FileServer | Select ProcessName,Id` lấy PID thực tế.
2. `powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Validation\Measure-FileSync.ps1 -ClientPid 1234 -ServerPid 5678 -Seconds 120 -OutputCsv .\sample_v83.csv`
3. Lưu cả CSV và `client_conn.log` + `server_conn.log`. Bật `FILESYNC_PERF_METRICS=1` trên CẢ HAI tiến trình để nhận breakdown.
4. Báo số file đã ACK (không dùng số file đã gửi), thời gian elapsed, CPU trung bình và đỉnh RAM.

**Lưu ý:** `ProcessIoReadMiBps`/`ProcessIoWriteMiBps` phản ánh I/O counters Windows của tiến trình, KHÔNG thể đại diện riêng cho tốc độ network hay tốc độ vật lý SSD. Muốn thấy IOPS/latency thực, dùng Windows Performance Monitor counters cho ổ vật lý/logic hoặc Windows Performance Recorder (WPR) trong môi trường có quyền.

Mục tiêu là khoanh vùng nguyên nhân, không hứa tăng tốc cố định.
