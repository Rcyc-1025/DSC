
import subprocess, sys, os
os.chdir(r"D:\workplace\Sai")
p = subprocess.run([r"build\aic.exe","verify",".",r"build\verify"], capture_output=True)
print("rc", p.returncode, "out", len(p.stdout), "err", len(p.stderr))
open(r"build\verify.log","wb").write(p.stdout)
