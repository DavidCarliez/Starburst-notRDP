# notRDP for Starburst (Mythic)

A hidden-desktop module for the [Whispergate/Starburst](https://github.com/Whispergate/Starburst) PIC shellcode agent. `notrdp` creates an invisible alternate Windows desktop inside the agent's session, launches `explorer.exe` and a persistent shell on it, and serves screenshots plus mouse/keyboard input over normal Mythic tasking. No RDP, no VNC, no extra listening service on the target.

Ported from [dagowda/notRDP](https://github.com/dagowda/notRDP), a Havoc C2 plugin by Dhanush Arvind (MIT). The hidden-desktop technique, the input model, and the workflow originate there. This version re-implements them as a Starburst PIC C++ module driven by Mythic tasking instead of BOFs and a browser streaming viewer.

```text
Payload Type : starburst (PIC shellcode, x64)
Commands     : notrdp [start|shot|input|stop]
Windows      : 10/11 (tested on Win11 25H2)
```

## How it works

1. `start`: `CreateDesktopW(L"StarburstHidden")` with an explicit `SECURITY_ATTRIBUTES` holding a NULL DACL, so child processes and later agent instances can always open the desktop. Launches `explorer.exe` and `cmd.exe /k` on it via `STARTUPINFOW.lpDesktop` and reports both PIDs in the task response.
2. `shot`: opens the hidden desktop with `DESKTOP_SWITCHDESKTOP`, makes it the active desktop for ~0.7 s (DWM composites only the active desktop; without this step the capture is black on Win10/11), captures with `GetDC(NULL)` + `BitBlt`, and streams the 24-bit BMP back through Mythic's chunked file-transfer flow.
3. `input`: `WindowFromPoint` at the given screen coordinates on the hidden desktop, `ScreenToClient`, then `PostMessage` mouse-button and key sequences. No cursor movement, no focus stealing.
4. `stop`: terminates the shell and explorer children, closes the desktop, frees agent state.

## Install

Apply the integration patch against Starburst `b0365af`, then rebuild the agent container:

```bash
git clone https://github.com/Whispergate/Starburst
cd Starburst
git apply /path/to/patches/starburst-notrdp.patch   # config, commands, dispatch, table, instance init, translator
./mythic-cli install folder . --force
```

The patch also adds `agent_code/src/commands/cmd_notrdp.cc` and `agent_functions/notrdp.py`. The container compiles the module automatically with `x86_64-w64-mingw32-gcc`; the usual Starburst constraints apply (no CRT, no .bss, FNV-1a hash-based API resolution, see the Starburst README).

**Payload note:** include the `download` and `download_resp` commands in the payload. Mythic 3.4 has no automatic download-response tasking: the frame BMP arrives as a Mythic file transfer keyed by the `download_resp` translation injection, and a payload without those commands stalls at chunk 0.

## Usage

| Task | Effect |
|---|---|
| `notrdp start` | create hidden desktop, explorer, persistent shell |
| `notrdp shot` | capture a BMP of the hidden desktop (arrives as a file response) |
| `notrdp input <x> <y> <action> [key]` | inject mouse/keyboard on the hidden desktop |
| `notrdp stop` | tear down and free state |

`input` action codes: `0` move, `1` left-click, `2` right-click, `3` double-click, `4` left-down, `5` left-up, `6` right-down, `7` right-up, `10` key-press. `key` is a virtual-key code, for example `13` for Enter, `0x74` for F5.

Typical loop: look, click, look again.

```text
notrdp start
notrdp shot                     # frame 1: find what to click
notrdp input 400 300 1          # left-click at (400,400)
notrdp shot                     # see the result
notrdp input 100 100 10 13      # press Enter
notrdp stop
```

Frame latency is one tasking round-trip. Lower the agent sleep (for example `sleep 5`) for faster iteration. Each frame is a ~7 MB BMP at 1852x1316; adjust the capture size in `cmd_notrdp.cc` if you want smaller frames.

## Differences from the original

| | [dagowda/notRDP](https://github.com/dagowda/notRDP) (Havoc) | this module (Starburst) |
|---|---|---|
| Delivery | BOFs inside the Havoc Demon | PIC C++ module compiled into the Starburst shellcode |
| View | browser viewer with streaming JPEG (interactive) | on-demand BMP snapshots via tasking |
| Capture | `PrintWindow` window compositing | desktop-surface `BitBlt` plus a short `SwitchDesktop` render blip |
| Shell | kills the running explorer first, restarts one on the hidden desktop | non-destructive: leaves the user's shell alone, adds a second one on the hidden desktop |
| Access | grants ACLs after creation | NULL DACL applied at creation, so agent restarts and children can always re-open |

Both inject input the same way (`WindowFromPoint` + `PostMessage`), so the focus, UAC, and modality quirks apply identically: synthetic input does not move the real cursor or raise windows.

## Operational notes

- Run inside a logged-on user session. An agent running as SYSTEM in session 0 captures black frames (session-0 desktops do not render) and explorer self-exits on secondary desktops there.
- Each `shot` makes the hidden desktop the active desktop for ~0.7 s. A user watching the console will see it flicker.
- Re-running `start` after `stop` (or after an agent restart) adopts an existing desktop instead of failing. A desktop leaked by a crashed agent is adopted the same way.
- Module settings live at the top of `cmd_notrdp.cc`: desktop name, blip duration, console creation flags.

## Attribution

- [dagowda/notRDP](https://github.com/dagowda/notRDP) by Dhanush Arvind, the original Havoc C2 hidden-desktop plugin (MIT). This module is a port of its concept: hidden desktop, explorer on it, screenshot plus PostMessage input.
- [Whispergate/Starburst](https://github.com/Whispergate/Starburst), the PIC agent framework this module plugs into (BSD-2-Clause).

## License

MIT, see [LICENSE](LICENSE). Derived from dagowda/notRDP (MIT). Integration glue for Starburst follows the upstream project's BSD-2-Clause.
