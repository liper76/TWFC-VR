# TWFC-VR

**English** | [Русский](README.ru.md)

A VR mod for **Transformers: War for Cybertron** (2010, PC, Unreal Engine 3, 32-bit, DirectX 9).
It runs through OpenXR (tested with Virtual Desktop and Quest controllers).

- first-person view, stereo (both eyes from the same game tick), head tracking;
- the robot's arms follow your controllers (IK), aiming with the controller, two-handed grip;
- weapon wheel, grenades from the belt, pulling items to your hand, melee by swinging;
- HUD on a floating panel, VR button hints, a settings menu inside the headset;
- movies and menus on a virtual screen;
- the mod's own interface in English and Russian (the switch is the first item of the VR settings menu).

## Language

The VR settings menu and the weapon wheel come in English and Russian. By default the language follows Windows
(Russian, Ukrainian or Belarusian Windows → Russian, anything else → English). To switch it, open the settings
menu in the headset (hold the menu button + A) and press the first item, **Language / Язык**, or set it in
`BinariesVR\twfcvr.ini`:

```
[General]
Language=en        ; auto | en | ru
```

The player guide is available in both languages: `VR mod - instructions.txt` and `VR-мод — инструкция.txt`.

## How it works

The mod is a proxy `xinput1_3.dll` that the game loads. It patches the engine (MinHook), separates the HUD,
takes the frames from D3D11 (the game renders through dgVoodoo: D3D9 → D3D11) and hands them to OpenXR.
Details: [_vrmod/HANDOFF.md](_vrmod/HANDOFF.md); work log: [_vrmod/PROGRESS.md](_vrmod/PROGRESS.md)
(both in Russian).

The repository lives right in the game folder, but holds only the mod's files (see `.gitignore`):

```
README.md / README.ru.md     this page (English / Russian)
VR mod - instructions.txt    player guide (controls, settings)
VR-мод — инструкция.txt      the same in Russian
Launch VR.bat                starts the game with the mod (same as Запуск VR.bat)
Launch VR (classic).bat      starts the old third-person version (same as Запуск VR (классика).bat)
BinariesVR/dgVoodoo.conf     dgVoodoo settings used by the mod
_vrmod/src/                  mod sources (C++)
_vrmod/build.bat             builds and installs into BinariesVR
_vrmod/tools/                analysis tools (Python), MockXR (a fake OpenXR runtime), test scripts
_vrmod/snapshots/            a snapshot of the classic version's sources
```

## Building

Requirements: VS 2022 Build Tools (x86); in `_vrmod_tools\` next to `_vrmod\`: MinHook 1.3.4
(`minhook\minhook-1.3.4`) and the OpenXR SDK (`openxr\include`, `openxr\Win32\lib|bin`).

```
cmd /c "_vrmod\build.bat"
```

This builds `xinput1_3.dll` and copies it, together with `openxr_loader.dll`, into `BinariesVR\`.

## Installation

`BinariesVR\` is a copy of the game's `Binaries\` with `xinput1_3.dll` (the mod), `openxr_loader.dll`
and dgVoodoo 2 (`D3D9.dll` etc. + `dgVoodoo.conf` from this repository) added. The original `Binaries\` folder
is left untouched. Mod settings: `BinariesVR\twfcvr.ini` (created on first launch); log: `BinariesVR\twfcvr.log`.

The repository does not include game files, dgVoodoo, MinHook, the OpenXR SDK or built binaries.
