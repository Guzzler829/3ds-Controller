# 3DS Controller

### A 3DS homebrew application that lets you use your (New or Old) Nintendo 3DS or 2DS as a wireless controller for Windows and Linux.


## Features
- Use your 3DS as a wireless gamepad for PC games and emulators
- Cross-platform support for Windows and Linux
- Support for all buttons, Circle Pad, and C-Stick
- Configurable server IP and port
- Low latency wireless connection
- Battery level indicator
- LCD toggle option to save battery

## Requirements

### 3DS
- Nintendo 3DS with custom firmware (CFW)
- Homebrew Launcher access or ability to install CIA files
- Wi-Fi connection (same network as PC)

### PC
- Windows or Linux
- Python 3.6+
- Windows: ViGEmBus driver
- Linux: uinput kernel module

## Installation

### 3DS Setup
#### Method 1: Homebrew menu
1. Copy `3ds_controller_vX.X.X.3dsx` to your 3DS' SD card in `/3ds/` folder
2. Launch Homebrew Launcher on your 3DS
3. Run the 3DS Controller application

#### Method 2: Home screen
1. Copy `3ds_controller_vX.X.X.cia" onto your 3DS' SD card
2. Open FBI or another tool that can install CIA files, I **highly** recommend [3dsDL](https://github.com/ankushKun/3dsDL), it's a really great tool.
3. Select the option to install a CIA from the SD card
4. Either install and keep the file, or install and delete; you don't need to keep the file if you don't want to.
5. Launch from the home screen, or, if using 3dsDL, select Launch after it installs.

### PC Setup

#### Windows
1. Install [ViGEmBus driver](https://github.com/ViGEm/ViGEmBus/releases)
2. Install Python 3.6+
3. Install required packages:
   ```
   pip install vgamepad
   ```
4. Run the PC receiver:
   ```
   python pc.py
   ```

#### Linux
1. Load uinput module:
   ```
   sudo modprobe uinput
   ```
2. Install dependencies:
   ```
   sudo apt-get install libudev-dev
   pip install python-uinput
   ```
3. Run PC receiver with root privileges:
   ```
   su -
   modprobe python-uinput
   python3 path/to/pc.py
   ```

## Usage
1. Start PC receiver script
2. Launch 3DS Controller on your 3DS
3. Set the IP address in the 3DS app to match your PC's IP
4. Connect and start using your 3DS as a controller

### Control Mapping
#### All modes
| Hold SELECT button | Toggle LCD on/off |

#### Controller mode
| 3DS Input | PC Controller Output |
|-----------|----------------------|
| A/B/X/Y Buttons | A/B/X/Y Buttons |
| Circle Pad | Left Analog Stick |
| C-Stick | Right Analog Stick |
| D-Pad | D-Pad |
| L/R Buttons | LB/RB (Left/Right Bumpers) |
| ZL/ZR Buttons | LT/RT (Left/Right Triggers) |
| Start/Select | Start/Back |

#### Mouse mode
| 3DS Input | PC Mouse Output |
|-----------|----------------------|
| Touchscreen | Move mouse |
| Circle Pad | Toggle between move mouse or scroll up & down |
| C-Stick | Toggle between move mouse or scroll up & down |
| D-Pad | Arrow Keys |
| L button/A button | Left click |
| R button/B button | Right click |
| ZL button/Y button | Back |
| ZR button/X button | Forward |
| START button | Enter |

#### Keyboard mode
| 3DS Input | PC Keyboard Output |
|-----------|--------------------|
| Touch | Press keys |
| Tap Keyboard tab again | Drop-down: "Change IP?" |
| A button/START button | Enter|
| B button | Backspace |
| L button/R button | Shift |
| Y button | Copy |
| X button | Paste |
| D-pad | Arrow keys |
| ZL button/ZR button | Back/Forward |
| Circle Pad | Move Cursor |
| C-stick | Scroll |

## Configuration

### 3DS
- Config file at `/config.ini` on SD card
- Edit to change server IP and port
- Default port: 8888

### PC
- Listens on all interfaces on port 8888
- Debug mode: `python pc.py --debug`

## Building from Source

### Requirements

#### 3DS Application

You need one of following group of dependencies:

##### Native build
- DevkitPro and DevkitARM
- 3DS development libraries (libctru)
- Make utility

##### Container build
- Docker or Podman

#### PC Application
- Python 3.6+
- Windows: ViGEmBus driver
- Linux: uinput module and headers

# Building 3DS Application (not that you care lol)

## Using native tools (Windows/Linux)

1. Install DevkitPro with 3DS support
   ```
   # Windows: Download from https://devkitpro.org/wiki/Getting_Started

   # Linux/macOS
   wget https://apt.devkitpro.org/install-devkitpro-pacman
   chmod +x ./install-devkitpro-pacman
   sudo ./install-devkitpro-pacman
   sudo dkp-pacman -S 3ds-dev
   ```

2. Install CIA building tools (makerom and bannertool)
   ```
   # Windows
   # Download makerom and bannertool from:
   # - makerom: https://github.com/3DSGuy/Project_CTR/releases
   # - bannertool: https://github.com/Steveice10/bannertool/releases
   # Add both to your PATH

   # Linux/macOS
   git clone https://github.com/3DSGuy/Project_CTR.git
   cd Project_CTR/makerom
   make
   sudo cp makerom /usr/local/bin

   git clone https://github.com/Steveice10/bannertool.git
   cd bannertool
   make
   sudo cp output/*/bannertool /usr/local/bin
   ```

3. Set environment variables (if needed)
   ```
   export DEVKITPRO=/opt/devkitpro
   export DEVKITARM=${DEVKITPRO}/devkitARM
   ```

4. Clone repository
   ```
   git clone https://github.com/icicle1133/3ds-controller.git
   cd 3ds-controller
   ```

5. Build application
   ```
   # For 3DSX file
   make clean
   make

   # For CIA file
   make cia
   ```

6. Output files:
   - `3ds_controller.3dsx` (Homebrew format)
   - `3ds_controller.cia` (CIA format for installation via FBI)

## Container build (Linux or WSL)

1. Clone repository
   ```
   git clone https://github.com/icicle1133/3ds-controller.git
   cd 3ds-controller
   ```

2. Build application (Replace podman by docker if using Docker)
   ```
   # For 3DSX file
   podman run -v $(pwd):/home-ext --workdir /home-ext -it --rm --name 3ds docker.io/devkitpro/devkitarm:latest make clean
   podman run -v $(pwd):/home-ext --workdir /home-ext -it --rm --name 3ds docker.io/devkitpro/devkitarm:latest make

   # For CIA file
   podman run -v $(pwd):/home-ext --workdir /home-ext -it --rm --name 3ds docker.io/devkitpro/devkitarm:latest make cia
   ```

3. Output files:
   - `3ds_controller.3dsx` (Homebrew format)
   - `3ds_controller.cia` (CIA format for installation via FBI)

# Development Setup

#### Windows
1. Install Python 3.6+
2. Install packages: `pip install vgamepad`
3. Install ViGEmBus driver
4. Run: `python pc.py`

#### Linux
1. Install dependencies: `sudo apt-get install python3-dev libudev-dev`
2. Install packages: `pip install python-uinput`
3. Load uinput: `sudo modprobe uinput`
4. Run: `sudo python pc.py`

# Project Status

### TO-DO
- [ ] Add ability to save multiple server configurations
- [ ] Add multiple device connections for local multiplayer
- [ ] Re-add keyboard/touchscreen support like CTurt's 3ds Controller had.

## License
This project is protected by the PolyForm Noncommercial License
