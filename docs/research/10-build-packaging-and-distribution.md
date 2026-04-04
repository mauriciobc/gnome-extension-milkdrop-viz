# Build, Packaging, and Distribution

## Packaging assumptions

The project is intended to run as a distro-installed system component using dynamic linking against distribution-provided libraries. This is the default Linux/GNOME-friendly model and matches the PRD’s expectation that users already have a package manager and platform libraries available.

## Runtime dependencies

The core runtime stack consists of:

- `libprojectM` 4.x.
- PipeWire 0.3 runtime.
- GTK4 runtime.
- GNOME Shell for the extension host.

The dependency goal is to stay close to standard distro packaging, minimizing bundled third-party code and avoiding private copies of system graphics/audio libraries.

## Build dependencies

Meson and Ninja are the primary build tools, with development headers for projectM, PipeWire, and GTK4.

## Version pinning

The PRD requires `projectM >= 4.0.0` and explicitly excludes 3.x compatibility. This must remain encoded in build expectations and in the documentation presented to developers and packagers.

## Meson philosophy

The build should remain simple:

- One native executable target.
- One extension install subtree.
- Optional static-link flag only for edge cases.
- No vendor subtree for FFT or expression libraries removed by the v2 rewrite.

## Install layout

The extension install layout should match GNOME’s extension directory conventions, and the binary should install into the normal executable path for the distro or package.

When packaging, ensure:

- The executable is discoverable by the extension.
- The extension UUID directory is correct.
- Metadata references remain accurate.
- Optional tools/scripts do not assume a development checkout path in production.

## Distribution considerations

Different distributions name the same libraries differently. Maintain a packager-facing mapping of package names for Debian/Ubuntu, Arch, and Fedora based on the PRD, and keep it updated if upstream package names change.

## Static linking policy

Static linking is not the default. It increases binary size and risks shipping stale copies of libraries that distributions already manage. Use the optional static mode only if there is a clearly documented deployment reason.

## Agent rules

Agents should not add vendored copies of removed components such as kissfft or exprtk back into the tree. Any packaging change that alters runtime discovery of the binary or extension must update documentation and installation scripts together.
