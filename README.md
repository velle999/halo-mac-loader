# maloader-carbon

A fork of [maloader](https://github.com/shinh/maloader) that runs Carbon
applications built for Mac OS X on Intel, starting with one: the i386 build
of *Halo: Combat Evolved* (Halo Universal 2.0, Westlake Interactive and
MacSoft, 2006), running natively on 32-bit x86 Linux with its own OpenGL
renderer.

The loader maps the Mac executable into memory, binds its imports and starts
it. Where Mac OS X would supply CoreFoundation, Carbon, AGL or IOKit, this
project supplies the calls the game makes, over glibc, SDL 2 and the
system's OpenGL. No part of the game is included; you need your own copy.

## Status

Work in progress. Every one of the game's 724 imports binds. It runs its
startup checks (CPU, memory, QuickTime and OpenGL versions, an OpenGL context
probe, video memory, disk space), finds its disc, shows its EULA and asks for
its product key, which are answered without being shown (see
Configuration), loads its maps and shaders, and draws its main menu: 30
frames a second in an 800x600 window on a Pentium 4 with a GeForce 7600 GS
and NVIDIA's 304 driver. Playing has not been tried yet. Sound, the intro
movies and pbuffers are not implemented.

An import with no implementation is bound to a guard page, so its first use
stops the program with its name, its caller and the registers.

## Requirements

- 32-bit x86 Linux with glibc. The game's i386 code needs SSE2.
- SDL 2 (`libSDL2-2.0.so.0`) and an OpenGL driver (`libGL.so.1`). SDL's
  headers are in `third_party/`, so only the libraries are needed.
- `vm.mmap_min_addr` of 4096 or lower. Mac OS X i386 executables are not
  position-independent, and their `__TEXT` segment starts at 0x1000:

      sudo sysctl -w vm.mmap_min_addr=4096

## Build and run

    make ld-mac libmac.so
    cd /path/to/Halo.app/Contents/MacOS
    HLE_CD_PATH="/path/to/Halo Universal" /path/to/maloader-carbon/ld-mac ./Halo

`ld-mac` is linked `-no-pie`. A 32-bit kernel loads position-independent
executables at 0x400000, which is inside the game's image.

## Configuration

- `HLE_CD_PATH`: the game checks that its disc is in the drive. Point this
  at a directory with the disc's contents, such as the disc image extracted.
  It appears as a mounted CD named after the directory (the Mac release's is
  `Halo Universal`), or after `HLE_CD_NAME` when that is set.
- `HLE_WINDOWED=1` plays in a window rather than changing the screen's mode,
  and ticks the game's own "Play in a window" setting.
- `HLE_VRAM_MB` is the video memory the renderer reports, 256 by default.
- Dialogs are answered without being shown. When the game runs a dialog
  from its NIB, `HLE_CONTROL_<code>=<value>` first sets the control with that
  four-letter signature (`HLE_CONTROL_FSAA=2` picks the second FSAA setting),
  then the default button's command is sent, or the one `HLE_DIALOG_<name>`
  gives (`HLE_DIALOG_EULA=not!` declines the licence). stderr says what was
  chosen. Alerts print their text and take their default button.
- Classic dialogs from `DLOG` resources are answered the same way:
  `HLE_DIALOG_<id>` fills their text fields, split at dashes, and their first
  button is hit; without it, or when the game refuses the text, their second
  button (Cancel or Quit) is. The game asks for its product key, printed on
  the back of the Halo manual, in `DLOG` 10001 (10002 in German, 10003 in
  French): `HLE_DIALOG_10001=XXXX-XXXX-XXXX-XXXX`. The game saves a key it
  accepts with its preferences.
- `HLE_TRACE=1` logs lookups and decisions, and `LD_MAC_LIST_UNDEFINED=1`
  lists the imports with no implementation.
- `LD_MAC_TRACE_IMPORTS=1` logs each implemented import the first time the
  game calls it, with the address of the call, and `LD_MAC_TRACE_IMPORTS=all`
  logs every call.
- `HLE_FRAME_DUMP=<directory>` saves the first frame the game draws, and
  every 600th after it, to `frame-<n>.ppm` in that directory. With
  `HLE_TRACE=1`, each of those frames also logs the frame rate.

## CoreFoundation

`hle/` implements the 77 CoreFoundation calls the game imports, with the
Darwin i386 calling convention: strings (including the compiler's constant
strings), arrays, dictionaries, numbers, data, XML property lists, URLs,
bundles and localized strings, preferences, UUIDs and character sets.

- Preferences are XML property lists in
  `~/.local/share/halo-mac-loader/home/Library/Preferences/`, or in
  `$HALO_MAC_HOME/Library/Preferences/` when that is set.
- `HLE_TRACE=1` logs bundle, resource and preference lookups.
- `make tests/cf_test && tests/cf_test /path/to/Halo.app` checks it,
  including against the game's own bundle.

## Carbon

- The File Manager puts the Mac's startup volume at / and, when
  `HLE_CD_PATH` is set, the disc as a second volume. FSRefs, FSSpecs with
  classic partial pathnames, catalog information, directory iteration, forks
  and the parameter-block calls the game uses are implemented. Carbon keeps
  68K structure alignment on Intel; `hle/carbon.h` asserts it against offsets
  read from the game's code.
- Paths the game opens through the C library are read the way a Mac reads
  them: without regard to case, since the game opens `shaders/vsh/...` where
  the folder is `Shaders`, and with `/Volumes/<disc name>` as the disc's
  directory and `/Volumes/Macintosh HD` as /.
- Folders a Mac keeps in the home, such as Preferences and Application
  Support, are under `~/.local/share/halo-mac-loader/home`. System folders are
  under `~/.local/share/halo-mac-loader/root`.
- Resource forks come from AppleDouble companion files (`._name`). The game's
  `EULA.rsrc` keeps its resources that way, so extract the `._` files along
  with the rest of the application.
- The Resource Manager reads those forks, and the Memory Manager provides
  pointers and handles. Dates, clocks and the Gestalt selectors the game asks
  for are answered.
- `make tests/files_test && tests/files_test /path/to/Halo.app` checks it.

## Windows, graphics, input and sound

- CGL and AGL contexts are SDL OpenGL contexts. Code built with
  `aglMacro.h` calls through a context's dispatch table, which holds thunks
  generated from the 10.4 SDK's `gliDispatch.h` by `tools/gen_gl_dispatch.py`;
  the game's direct `gl` imports bind to the system's libGL. The renderer
  described is one accelerated NVIDIA renderer. Pbuffers are not supported
  yet.
- The game copies `GL_EXTENSIONS` into a 4096-byte buffer, which a newer
  driver's list overruns, so it sees only the extensions whose names its
  executable contains, plus `GL_EXT_texture_rectangle` where the driver has
  the ARB extension of the same enumerants.
- Apple's ARB program assembler accepts an `ALIAS` of a binding, as in
  `ALIAS oPos = result.position;`, and all 90 of the game's vertex programs
  use one. The ARB grammar aliases only declared variables, so NVIDIA's
  driver refuses those programs. As each program is loaded, such an `ALIAS`
  becomes the declaration it stands for, `OUTPUT oPos = result.position;`
  (`ATTRIB` for a vertex binding). A program the driver still refuses is
  reported on stderr with the driver's message.
  `make tests/arb_test && tests/arb_test GameData/Shaders/vsh/*.vsh` checks
  the rewrite, with the game's own programs when they are named.
- Displays, their modes and the main GDevice describe SDL's display 0. A mode
  switch resizes the game's window, and a window covering a captured display
  goes full screen unless `HLE_WINDOWED` is set. Gamma tables are recorded,
  not applied.
- Windows, controls and menus come from the application's NIB
  (`objects.xib`) and are kept as records the game queries; only a window the
  game draws in with OpenGL is real. The Carbon Event Manager, window groups,
  the Process Manager and Multiprocessing Services are implemented. SDL input
  arrives as Carbon keyboard and mouse events with Mac key codes, and while
  the game hides the cursor the pointer is held in relative mode.
- QuickDraw keeps ports, GWorlds with real pixels, colors and rectangles.
- Sound Manager channels keep time but are silent. QuickTime reports no
  movies, so the intro is skipped. IOKit shows the disc and no HID devices.

## Tools

- `tools/asm_annotate.py BINARY DISASSEMBLY LO HI` prints part of an
  `llvm-objdump --macho -d` listing with the C strings, CFStrings and
  four-character codes its constants name.
- `tools/import_walk.py DISASSEMBLY ADDR [DEPTH] [IMPLEMENTED]` lists the
  imports a function reaches, in the order a walk meets them, marking the
  ones with no implementation.

## Changes from maloader

- Pre-10.5 i386 images: `__IMPORT,__jump_table` stubs, external relocations,
  LOCAL and ABSOLUTE indirect symbols, and a Darwin initial stack with `envp`
  and `apple[]`.
- An undefined symbol marked `N_REF_TO_WEAK` binds to the library that defines
  it. The bit is `N_WEAK_DEF` on a defined symbol; taking it for that bound
  `operator new` and `delete` to address 0.
- `libmac` additions for 10.4-era executables: `__sF`, keymgr, the `errno`
  variable, `bootstrap_port`, a monotonic `mach_absolute_time`.
- An unimplemented import stops the program with its name, its caller, the
  registers and the frame-pointer chain.
- `sysctl` and `sysctlbyname` answer as a 10.4.9 Intel Mac with this
  machine's CPU and memory; maloader's `sysctl` aborted on most queries.
- Lookups the program makes at run time, through
  `CFBundleGetFunctionPointerForName` or `dlsym`, resolve the way its imports
  do. `dlopen` of a Mac library that is not present as a Mach-O file returns
  a handle for exactly that, instead of exiting.

---

## maloader (upstream README)

This is a userland Mach-O loader for linux.

## Installation

```bash
$ make release
```
## Usage

```bash
$ ./ld-mac mac_binary [options...]
```
You need OpenCFLite (http://sourceforge.net/projects/opencflite/)
installed if you want to run some programs such as dsymutil.
opencflite-476.17.2 is recommended.

## How to use compiler toolchains of Xcode

Recent Xcode toolchains are built by clang/libc++. This means some C++
programs do not work with ld-mac linked against libstdc++. To build
ld-mac with clang/libc++, run

```bash
$ make clean
$ make USE_LIBCXX=1
```
You need compiler toolchain binaries on your Linux. unpack_xcode.sh in
this repository helps you to set up them if you have a dmg package of
Xcode. This script was checked with Xcode 4.3.3, 4.4.1, 4.5.2, 4.6.2,
and 5.0.1. It would work even on other Xcode releases, but you may
need to modify the script by yourself. How things are stored in dmg
packages heavily depend on the version of Xcode. You can use this like

```bash
$ ./unpack_xcode.sh ~/Downloads/xcode_5.0.1_command_line_tools*.dmg
```

This will create xcode_5.0.1_command_line_tools*/root. We will call
this directory as $ROOT.

```bash
$ ./ld-mac $ROOT/usr/bin/clang --sysroot=$ROOT -c mach/hello.c
$ file hello.o
hello.o: Mach-O 64-bit x86_64 object
```
To link binaries on Linux, you also need necessary dylibs in
root/usr/lib. For example, simple hello world program requires
/usr/lib/libSystem.dylib and /usr/lib/system. Do something like

```bash
(mac)$ tar -cvzf sys.tgz /usr/lib/libSystem* /usr/lib/system
(mac)$ scp sys.tgz $USER@linux:/tmp
(linux)$ cd $ROOT && tar -xvzf /tmp/sys.tgz
```
Also note that it seems ld does not like the version number of
Xcode. So you need to move xcode_5.0.1_command_line_tools*/root to
somewhere else. For example:

```bash
$ ln -sf xcode_5.0.1_command_line_tools_10.9_20131022/root
$ ./ld-mac $ROOT/usr/bin/clang --sysroot=$ROOT -g mach/hello.c
$ ./ld-mac ./a.out
Hello, 64bit world!
```
## How to use compiler toolchains of Xcode 3.2.6

Get xcode_3.2.6_and_ios_sdk_4.3__final.dmg (or another xcode package).

```bash
$ git clone git@github.com:shinh/maloader.git
$ ./maloader/unpack_xcode.sh xcode_3.2.6_and_ios_sdk_4.3__final.dmg
$ sudo cp -a xcode_3.2.6_and_ios_sdk_4.3__final/root /usr/i686-apple-darwin10
$ cd maloader
$ make release
$ ./ld-mac /usr/i686-apple-darwin10/usr/bin/gcc mach/hello.c
$ ./ld-mac a.out
```

## How to run Mach-O binaries using binfmt_misc
```bash
$ ./binfmt_misc.sh
$ /usr/i686-apple-darwin10/usr/bin/gcc mach/hello.c
$ ./a.out
```
To remove the entries, run the following command:

```bash
$ ./binfmt_misc.sh stop
```

## How to try 32bit support

```bash
$ make clean
$ make all BITS=32
```
If you see permission errors like:

```bash
ld-mac: ./mach/hello.c.bin mmap(file) failed: Operation not permitted
```
you should run the following command to allow users to mmap files to
addresses less than 0x10000.

```bash
$ sudo sh -c 'echo 4096 > /proc/sys/vm/mmap_min_addr'
```
Or, running ld-mac as a super user would also work.

## How to run both 64bit Mach-O and 32bit Mach-O binaries
```bash
$ make both
$ ./binfmt_misc.sh start `pwd`/ld-mac.sh
$ /usr/i686-apple-darwin10/usr/bin/gcc -arch i386 mach/hello.c -o hello32
$ /usr/i686-apple-darwin10/usr/bin/gcc -arch x86_64 mach/hello.c -o hello64
$ /usr/i686-apple-darwin10/usr/bin/gcc -arch i386 -arch x86_64 mach/hello.c -o hello
$ ./hello32
Hello, 32bit world!
$ ./hello64
Hello, 64bit world!
$ ./hello
Hello, 64bit world!
$ LD_MAC_BITS=32 ./hello
Hello, 32bit world!
```

## Which programs should work

OK

- gcc-4.2 (link with -g requires OpenCFLite)
- otool
- nm
- dyldinfo
- dwarfdump
- strip
- size
- dsymutil (need OpenCFLite)
- cpp-4.2
- clang

not OK

- llvm-gcc
- gnumake and bsdmake
- lex and flex
- ar
- m4
- gdb
- libtool
- nasm and ndisasm (i386)
- mpicc, mpicxx, and mpic++

## Notice

- Running all Mac binaries isn't my goal. Only command line tools such
  as compiler tool chain can be executed by this loader.
- A slide about this: http://shinh.skr.jp/slide/ldmac/000.html

## TODO

- read dwarf for better backtracing
- make llvm-gcc work
- improve 32bit support
- handle dwarf and C++ exception

## License

Simplified BSD License or GPLv3.

Note that all files in "include" directory and some files in "libmac"
were copied from Apple's Libc-594.9.1.
http://www.opensource.apple.com/release/mac-os-x-1064/

See http://www.gnu.org/licenses/gpl-3.0.txt for GPLv3.
