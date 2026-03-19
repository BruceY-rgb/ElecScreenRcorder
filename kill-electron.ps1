Get-Process | Where-Object { $_.Path -like '*Screen*' -or $_.Name -like '*electron*' } | Stop-Process -Force -ErrorAction SilentlyContinue
