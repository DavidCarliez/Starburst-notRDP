# notRDP for Starburst (Mythic)

A hidden-desktop (HVNC-style) module for the [Whispergate/Starburst](https://github.com/Whispergate/Starburst) PIC shellcode agent.

`notrdp` creates an invisible alternate Windows desktop inside the agent's session, launches `explorer.exe` and a persistent shell on it, and lets the operator pull screenshots and inject mouse/keyboard input through normal Mythic tasking — no RDP, no VNC, no extra listening service on the target.

> Ported from [dagowda/notRDP](https://github.com/dagowda/notRDP) — a Havoc C2 plugin by **Dhanush Arvind** (MIT). The hidden-desktop technique, the input model, and the overall workflow originate there. This version re-implements the idea as a Starburst PIC C++ module with Mythic tasking instead of BOFs + a browser streaming viewer.

```text
Payload Type : starburst (PIC shellcode, x64)
Commands     : notrdp [start|shot|input|stop]
Windows      : 10/11 (tested on Win11 25H2)
```

## How it works

1. **`start`** — `CreateDesktopW(L"StarburstHidden")` with an explicit `SECURITY_ATTRIBUTES` (NULL DACL, so child processes and later agent instances can always open it), then launches `explorer.exe` and `cmd.exe /k` on that desktop via `STARTUPINFOW.lpDesktop`. Reports both PIDs in the task response.
2. **`shot`** — opens the hidden desktop with `DESKTOP_SWITCHDESKTOP`, makes it *active* for ~0.7 s (DWM only composites the active desktop — without this the capture is black on Win10/11), captures via `GetDC(NULL)` + `BitBlt`, and streams the 24-bit BMP back through Mythic's chunked file-transfer flow.
3. **`input`** — `WindowFromPoint` at the given screen coordinates on the hidden desktop, `ScreenToClient`, then `PostMessage` mouse-button/key sequences. No cursor movement, no focus stealing.
4. **`stop`** — terminates the shell/explorer children, closes the desktop, frees agent state. Re-running `start` after `stop` (or after an agent restart) re-adopts an existing desktop instead of failing.

## Install

Apply the integration patch against Starburst `b0365af`, then reload the agent container:

```bash
git clone https://github.com/Whispergate/Starburst
cd Starburst
git apply /path/to/patches/starburst-notrdp.patch   # wires config/commands/dispatch/table/instance-init/translator
# cmd_notrdp.cc and notrdp.py are already included in the patch
./mythic-cli install folder . --force
```

Build requirements are those of Starburst itself (the container compiles the module with `x86_64-w64-mingw32-gcc` automatically; no CRT, no .bss, FNV-1a hash-based API resolution — see Starburst's README).

**Payload note:** include the `download` and `download_resp` commands in the payload. Mythic 3.4 has no automatic download-response tasking — the frame BMP arrives as a Mythic file transfer keyed by the `download_resp` translation injection, and builds without those commands stall at chunk 0.

## Usage

| Task | Effect |
|---|---|
| `notrdp start` | create hidden desktop + explorer + persistent shell |
| `notrdp shot` | capture a BMP of the hidden desktop (streams as file response) |
| `notrdp input <x> <y> <action> [key]` | inject mouse/keyboard on the hidden desktop |
| `notrdp stop` | tear down and free state |

`input` action codes: `0` move, `1` left-click, `2` right-click, `3` double-click, `4` left-down, `5` left-up, `6` right-down, `7` right-up, `10` key-press (`key` = virtual-key code, e.g. `13` for Enter, `0x74` for F5).

Typical loop — look, click, look again:

```text
notrdp start
notrdp shot                     # frame 1: find what to click
notrdp input 400 300 1          # left-click at (400,400)
notrdp shot                     # see the result
notrdp input 100 100 10 13      # press Enter
notrdp stop
```

Frame latency is one tasking round-trip — set the agent sleep lower (e.g. `sleep 5`) for faster iteration. Each frame is a ~7 MB BMP at 1852x1316 (adjust to taste in `cmd_notrdp.cc` if you want smaller).

## Differences from the original

| | [dagowda/notRDP](https://github.com/dagowda/notRDP) (Havoc) | this module (Starburst) |
|---|---|---|
| Delivery | BOFs inside the Havoc Demon | PIC C++ module compiled into the Starburst shellcode |
| View | browser viewer, **streaming JPEG** (interactive) | on-demand **BMP snapshots** via tasking (snapshot-driven) |
| Capture | `PrintWindow` window compositing | desktop-surface `BitBlt` + short `SwitchDesktop` render blip |
| Shell | kills the running explorer first, restarts one on the hidden desktop | **non-destructive**: leaves the user's shell alone, adds a second one on the hidden desktop |
| Access | grants ACLs after creation | NULL DACL applied at creation (agent restarts and children can always re-open) |

Both inject input the same way (`WindowFromPoint` + `PostMessage`), so focus/UAC/modality quirks apply identically: synthetic input doesn't move the real cursor or raise windows.

## Notes and gotchas

- **Session matters.** Run inside a logged-on user session. An agent running as SYSTEM in session 0 gets black frames (session-0 desktops don't render) and explorer self-exits on secondary desktops there.
- **The shot blip is visible.** For ~0.7 s per `shot` the hidden desktop becomes the active desktop. If a user is staring at the console they will see it flicker.
- **`start` after `stop` works**, and a desktop leaked by a crashed agent is adopted on the next `start`.
- Module config lives in the top of `cmd_notrdp.cc` (desktop name, blip duration, console creation flags).

## Attribution

- [dagowda/notRDP](https://github.com/dagowda/notRDP) by Dhanush Arvind — the original Havoc C2 hidden-desktop plugin (MIT). This module is a port of its concept: hidden desktop, explorer on it, screenshot + PostMessage input.
- [Whispergate/Starburst](https://github.com/Whispergate/Starburst) — the PIC agent framework this module plugs into (BSD-2-Clause).

## License

MIT — see [LICENSE](LICENSE). Derived from dagowda/notRDP (MIT). Integration glue for Starburst follows the upstream project's BSD-2-Clause.
