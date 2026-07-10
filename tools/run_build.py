#!/usr/bin/env python3
"""
Build-Wrapper: Patched locale.getdefaultlocale() fuer Python 3.14
und ruft dann idf.py build direkt auf.
"""
import sys
import os

# Patch: getdefaultlocale soll UTF-8 liefern - BEVOR irgendwas importiert wird
import locale
locale.getdefaultlocale = lambda: ('de_DE', 'UTF-8')

os.environ['PYTHONUTF8'] = '1'
os.environ['IDF_PY_BUILD_JOBS'] = '6'

# In Projektverzeichnis wechseln
os.chdir(os.path.join(os.path.dirname(__file__), '..'))

idf_py = os.path.join(os.environ.get('IDF_PATH', ''), 'tools', 'idf.py')
if not os.path.exists(idf_py):
    print(f"FEHLER: idf.py nicht gefunden unter {idf_py}")
    sys.exit(1)

print(f"Starte Build von {os.getcwd()}...")

# idf.py direkt ausfuehren (Patch bleibt aktiv)
sys.argv = [idf_py, 'build']
with open(idf_py, 'rb') as f:
    code = compile(f.read(), idf_py, 'exec')
exec(code, {'__name__': '__main__', '__file__': idf_py})
