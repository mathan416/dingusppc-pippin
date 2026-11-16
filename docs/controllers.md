# Controllers and input

How to play with a gamepad, a mouse or trackball, and a keyboard, using only what's built
into the emulator. No extra software is needed.

## Contents

- [How the Pippin's input works](#how-the-pippins-input-works)
- [Gamepads](#gamepads)
- [The pointer: mouse, trackball or analog stick](#the-pointer-mouse-trackball-or-analog-stick)
- [Keyboard](#keyboard)
- [Checking what the emulator receives](#checking-what-the-emulator-receives)
- [Gamepads SDL doesn't recognise](#gamepads-sdl-doesnt-recognise)
- [Setting it up in RetroPie / EmulationStation](#setting-it-up-in-retropie--emulationstation)
- [Quitting a game](#quitting-a-game)
- [Controller Router (optional)](#controller-router-optional)

## How the Pippin's input works

The real Pippin's controller, the **AppleJack**, is an ADB device: a gamepad with a
trackball in the middle. To the Mac OS inside the Pippin it looks like two things at once:

- **A mouse**, driven by the trackball. Its two triggers also work as the mouse buttons.
- **A set of game buttons**: four coloured buttons, a directional pad, two triggers and three
  buttons along the front.

The emulator attaches an AppleJack and a keyboard (`--adb_devices=AppleJack,Keyboard`,
which the launcher sets). It then feeds them from your host devices:

```
  Host device                         Emulated Pippin device
  -----------                         ----------------------
  Gamepad buttons and D-pad  ------>  AppleJack buttons
  Mouse / trackball          ------>  AppleJack trackball (the guest's mouse)
  Gamepad shoulder buttons   ------>  AppleJack triggers (also mouse buttons)
  Keyboard                   ------>  ADB keyboard
```

## Gamepads

Any gamepad SDL 2 recognises works: Xbox, PlayStation, Switch Pro, 8BitDo and most USB or
Bluetooth pads. SDL names buttons by position, like an Xbox pad, so on a PlayStation pad
"A" is Cross, and on a Nintendo pad "A" is the right-hand button, labelled B.

| SDL / Xbox button | PlayStation | AppleJack |
| --- | --- | --- |
| A (bottom) | Cross | Red |
| B (right) | Circle | Green |
| X (left) | Square | Yellow |
| Y (top) | Triangle | Blue |
| D-pad | D-pad | Directional pad |
| LB (left shoulder) | L1 | Left trigger, and the primary mouse button |
| RB (right shoulder) | R1 | Right trigger, and the secondary mouse button |
| Back / View | Share / Create | Front button, left |
| Guide / Xbox | PS | Front button, middle |
| Start / Menu | Options | Front button, right |

Other controls (sticks, stick clicks, the analog triggers LT/RT or L2/R2, paddles,
touchpads) aren't used.

A few details:

- **Several gamepads at once** all drive the same AppleJack. A button counts as held while
  it's held on any pad, so two players can't fight over a button.
- **Plugging in while running** works. A pad connected after start-up is picked up
  straight away. Unplugging one releases any buttons it was holding.
- **Losing focus** (switching windows on a desktop) releases every button, so nothing gets
  stuck down.

## The pointer: mouse, trackball or analog stick

Pippin software is driven by a pointer, so you need something that moves it.

- **A USB trackball** is the closest thing to the real AppleJack, and the most comfortable
  for long play.
- **A mouse** works the same way. Left button = primary click, right button = secondary.
- **The gamepad's analog stick isn't read by the emulator.** To steer the pointer with a
  stick you need something that turns stick movement into mouse movement:
  - On a desktop (X11 or Wayland), a tool such as
    [AntiMicroX](https://github.com/AntiMicroX/antimicrox) can map the left stick to the
    mouse while the emulator handles the buttons.
  - On a cabinet that runs without a desktop, a trackball is the simplest answer.
    Controller Router (below) is how our own cabinet does it.

The triggers (LB/RB) click even when the pointer comes from a mouse or trackball, so with a
trackball and a gamepad you can play one-handed on the ball and click with the shoulder
buttons, much like the original.

## Keyboard

A USB keyboard types into the Pippin as an ADB keyboard: useful for name entry and the few
titles that ask for text. With a keyboard you also get the emulator's shortcuts (full
screen, window size and so on), listed in [configuration.md](configuration.md#keyboard-shortcuts).
Those shortcuts use the left Ctrl key and aren't passed to the game.

## Checking what the emulator receives

Start the emulator with `PIPPIN_INPUT_TRACE=1` and `--log-to-stderr`:

```
PIPPIN_INPUT_TRACE=1 ./run.sh kinka13.rom disc.bin --log-to-stderr
```

Each press and release prints a line:

```
PIPPIN_INPUT id=0 button=4 down=1
PIPPIN_POINTER buttons=1
```

If nothing appears when you press a button, SDL doesn't know the pad. See the next section.

## Gamepads SDL doesn't recognise

SDL ships with mappings for hundreds of pads. For one it doesn't know, give it a mapping
string through `SDL_GAMECONTROLLERCONFIG`:

1. Get a mapping. Either find your pad in the community
   [SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB) list (the
   `gamecontrollerdb.txt` file, under "Linux"), or create one with a mapping tool such as
   [SDL2 Gamepad Tool](https://generalarcade.com/gamepadtool/) or SDL's own
   `controllermap` test program.
2. Pass it to the emulator:

   ```
   export SDL_GAMECONTROLLERCONFIG="03000000...,My Pad,a:b0,b:b1,x:b2,y:b3,..."
   ./run.sh kinka13.rom disc.bin
   ```

   Or keep a whole file of mappings and point SDL at it:

   ```
   export SDL_GAMECONTROLLERCONFIG_FILE=$HOME/gamecontrollerdb.txt
   ```

3. Check with `PIPPIN_INPUT_TRACE=1` that the buttons now arrive.

The mapping only needs the buttons in the table above. To change which pad button becomes
which AppleJack button, change the mapping string: for example, assign `a:` to a different
physical button.

## Setting it up in RetroPie / EmulationStation

These steps add a Pippin system that launches the release build directly. They assume the
release is unpacked in `/home/pi/dingusppc-pippin` and the ROM is in RetroPie's BIOS folder.

1. **Files**

   ```
   /home/pi/dingusppc-pippin/            run.sh, dingusppc, ...
   /home/pi/RetroPie/BIOS/kinka13.rom    the Pippin ROM
   /home/pi/RetroPie/roms/pippin/        disc images (.bin or .iso, unzipped, writable)
   /home/pi/RetroPie/saves/pippin/       the Pippin's flash and NVRAM (create it)
   ```

2. **Emulator entry.** Create `/opt/retropie/configs/pippin/emulators.cfg`:

   ```
   dingusppc-pippin = "/home/pi/dingusppc-pippin/run.sh /home/pi/RetroPie/BIOS/kinka13.rom %ROM% -w /home/pi/RetroPie/saves/pippin"
   dingusppc-pippin-authentic = "env PIPPIN_AUTHENTIC=on /home/pi/dingusppc-pippin/run.sh /home/pi/RetroPie/BIOS/kinka13.rom %ROM% -w /home/pi/RetroPie/saves/pippin"
   default = "dingusppc-pippin"
   ```

   The second line is optional: it adds a real-Pippin-speed choice to the runcommand menu.
   All games share one save folder, as they shared one flash chip on the real console.

3. **System entry.** If EmulationStation doesn't list a Pippin system yet, add this inside
   `<systemList>` in `/etc/emulationstation/es_systems.cfg` (or a copy in
   `/opt/retropie/configs/all/emulationstation/es_systems.cfg`, which survives updates):

   ```
   <system>
     <name>pippin</name>
     <fullname>Bandai Pippin</fullname>
     <path>/home/pi/RetroPie/roms/pippin</path>
     <extension>.bin .BIN .iso .ISO</extension>
     <command>/opt/retropie/supplementary/runcommand/runcommand.sh 0 _SYS_ pippin %ROM%</command>
     <platform>pippin</platform>
     <theme>pippin</theme>
   </system>
   ```

4. **Restart EmulationStation**, then launch a game from the new Pippin system.

The gamepad works as soon as the game starts; EmulationStation's own controller setup
doesn't affect the emulator. Use a trackball or mouse for the pointer (see above).

## Quitting a game

The emulator has no quit button or gamepad combination of its own, and RetroPie's hotkey
exit only works for RetroArch cores, not standalone emulators like this one. The ways out
today:

- **On a desktop**, close the window.
- **From the terminal** you started it in, press Ctrl+C.
- **From another computer**, over SSH:

  ```
  pkill -INT -f dingusppc
  ```

  `-INT` asks the emulator to stop cleanly, the same as Ctrl+C, so the Pippin's flash and
  NVRAM are saved.

Avoid `kill -9`: it skips saving.

## Controller Router (optional)

Our own cabinet uses Controller Router, a separate tool that sits between the gamepads and
the emulator. It adds the things above that the emulator doesn't do itself: steering the
pointer with the analog stick, and a gamepad combination to quit back to EmulationStation.
Controller Router is not part of this project, and nothing here requires it. The cabinet's
launcher sets `DINGUS_CABINET_ROUTER=1` and related variables only so Controller Router can
recognise the running emulator.
