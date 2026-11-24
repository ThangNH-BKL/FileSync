# Portable core smoke test (without MFC)

From `FileSync/Tests/Portable` on Linux/macOS with Clang:

```bash
clang++ -std=c++17 -O2 -Wall -Wextra -Werror -pthread test_v7_native.cpp -o test_v7_native
./test_v7_native
```

On Windows with Visual Studio Developer PowerShell:

```powershell
cl /std:c++17 /EHsc /W4 test_v7_native.cpp /Fe:test_v7_native.exe
.\test_v7_native.exe
```

This uses temporary files, tests only native C++ metadata/pacing/inventory primitives. It does NOT establish integration correctness, MSVC Common library build, actual TCP/IOCP, USN journal, or 100m records throughput.
