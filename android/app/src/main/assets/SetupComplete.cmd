@echo off
rem SetupComplete.cmd — runs as SYSTEM after specialize on first boot.
rem Installs Gunyah/VirtIO drivers collected by the Android builder.
if exist C:\Windows\Setup\Scripts\drivers.txt (
  for /f "usebackq delims=" %%I in ("C:\Windows\Setup\Scripts\drivers.txt") do (
    pnputil /add-driver "%%I" /install
  )
)
rem rdmapool: rebind handled by INF matching ACPI\RDMA0000 automatically via pnputil
exit /b 0
