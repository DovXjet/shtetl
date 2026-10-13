# shtetl

A simple 3D model viewer with a thumbnail browser, built on the same stack as
vgiv, XjetStudio and RobotMotion: C++20, Qt 6 and the VulkanSceneGraph (VSG)
through vsgQt.

- **Open a directory** and see thumbnails of the 3D files in it, in a dock next
  to the 3D view, or pick a model in an Open dialog with the same thumbnail grid
  (like Part Add in XjetStudio). Sub-directories can be entered, and the size of
  the thumbnails is adjustable.
- **3D view** with the orientation cube of RobotMotion: click a face, edge or
  corner to look from there; right-click it to switch to orthographic projection.
- **Choose the 3D interaction flavor** under View > Navigation Style: Netfabb,
  FreeCAD, Blender, Maya, Inventor, SolidWorks, OpenSCAD or touchpad gestures
  (the same presets as XjetStudio and RobotMotion, saved under the same setting).
- A **menu** in the style of vgiv, with a Menu Bar toggle, recent files, and
  Next/Previous Model to step through the directory.

## Formats

STL and 3MF are read by shtetl itself into a compact mesh (ported from
XjetStudio): 12 bytes per vertex, with 24-bit positions and an octahedrally
encoded face normal that a shader decodes, drawn without an index buffer. Binary
STL is read straight from the memory-mapped file. 3MF components, build items,
transforms and units are honored; colors and materials are not.

Everything else that vsgXchange can load (OBJ, PLY, glTF/GLB, FBX, ... - what
assimp supports) goes through vsgXchange. The list of extensions is built at
run time from the readers that are registered.

## Thumbnails

Thumbnails are rendered off-screen on a worker thread, on the viewer's Vulkan
device, and cached as 256x256 PNGs in a `.thumbnails/` directory next to the
models (or in the user's cache directory if that is read-only). A thumbnail is
remade when its model is newer. The cache file of `part.stl` is
`.thumbnails/part.stl.png`; a `.thumbnails/part.png` made by XjetStudio is used
if there is no thumbnail of ours.

## Keys

| Key | |
| --- | --- |
| Ctrl+O | Open a model (thumbnail dialog) |
| Ctrl+Shift+O | Open a directory |
| F5 | Reload |
| F, Ctrl+0 | Fit the model to the window |
| Numpad 5 | Perspective / orthographic |
| 7 / 1 / 3 (or Home / End / PgDn) | Top / front / right view |
| Space, Shift+Down / Backspace, Shift+Up | Next / previous model |
| Ctrl+M | Show or hide the menu bar |
| F9 | Show or hide the thumbnail dock |
| Q, Ctrl+Q | Quit |

The mouse buttons depend on the navigation style (View > Navigation Style).

## Building

Linux (Fedora; needs Vulkan, Qt 6 with its private headers, vsg and vsgXchange
with assimp installed, e.g. in `/usr/local`):

    cmake --preset linux-release
    cmake --build build/linux-release
    build/linux-release/shtetl [model-file | directory]

vsgQt is fetched and built from source against Qt 6 on the first configure.
The commit shown in Help > About is taken when CMake configures, so configure
again after committing.

### Windows

- **Native**, with Visual Studio and vcpkg (`vcpkg.json`): the `windows-release`
  and `windows-RelWithDebInfo` presets. `Installers\BuildInstaller.bat` builds
  the NSIS installer from the latter.
- **Cross compiled from Fedora** with MinGW: run `sudo ./install-mingw-deps.sh`
  once, then `./windows-cross-compile.sh`. It builds vsg, vsgXchange and assimp
  into `~/.cache/shtetl-mingw` the first time, and writes the installer to
  `Installers/`. Pass `--no-installer` to stop at the staged tree.

Windows file paths may be non-ASCII: the exe carries a UTF-8 code page manifest
and paths go through `PathUtils.h` / `QtPathUtils.h`.

## Origins

Copied and adapted, and to be kept in sync by hand: `NavigationStyles.h`,
`MyTrackball.h`, `CubeGizmo` (RobotMotion, originally XjetStudio), the
thumbnail renderer, worker and cache convention (XjetStudio), the compact mesh
encoding and shaders (XjetStudio), the path helpers, About dialog, MinGW
toolchain and installer scripts (vgiv).
