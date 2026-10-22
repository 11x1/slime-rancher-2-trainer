sr2mods INJECTION TEST - version.dll
=====================================

What it is
----------
A minimal test build of the sr2mods version.dll proxy. It only checks that
Slime Rancher 2 loads a version.dll placed next to SlimeRancher2.exe.

It does NOT mod the game: no IL2CPP calls, no hooks, no calls into
GameAssembly.dll. It forwards all 17 version.dll exports to the real
C:\Windows\System32\version.dll and writes a log file. No message boxes.

NOTE: this is a DIFFERENT build from the full mod's version.dll (dist\Release).
Only one version.dll can be in the game folder at a time. Remove this test
DLL (or overwrite it) before installing the full mod.

Install
-------
1. Close the game.
2. Copy version.dll from this folder into
     C:\Program Files (x86)\Steam\steamapps\common\Slime Rancher 2\
   (the folder that contains SlimeRancher2.exe).
   version.pdb is optional (debug symbols only).
3. Launch Slime Rancher 2 through Steam as usual and wait until the main menu.
4. Open sr2mods_injecttest.log in that same game folder.
   (If the game folder is not writable, the log goes to %TEMP% instead:
    %TEMP%\sr2mods_injecttest.log)

Reading the log
---------------
- host exe / host PID      : the game process that loaded the DLL
- proxy dll                : full path of this version.dll
- real dll                 : the System32 version.dll calls are forwarded to
- forwarding self-test: OK : the forwarded API works (shows the game's file version)
- UnityPlayer.dll / GameAssembly.dll loaded at +N ms : time since the test
  thread started, plus base address
- "INJECTION OK"           : success - the proxy loads and works

No log file at all = the DLL was not loaded (wrong folder, or the game did not
pick it up). Each launch appends a new block to the log.

Uninstall
---------
Delete these from the game folder:
  version.dll
  sr2mods_injecttest.log
(and version.pdb if you copied it).
