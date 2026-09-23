# dlssg-open

NVIDIA DLSS Frame Generation on RTX 30-series GPUs, on Linux, under Proton. Open source. Makes NVIDIA's DLSS-G runtime interoperate with the open Linux graphics stack on hardware the runtime already carries kernels for.

Status: working. Tested 2026-09-22 with Ghost of Tsushima Director's Cut, RTX 3080, driver 615.71, Proton Hotfix (wine 11), NVIDIA DLSS-G runtime 310.6. Every frame-gen kernel loads and runs.

## How it works

On Windows, NVIDIA's DLSS-G runtime talks to NVIDIA's own driver, NVAPI and CUDA. On Linux under Proton, every one of those layers is an open reimplementation: Vulkan through vkd3d-proton, NVAPI through dxvk-nvapi, CUDA through Wine's `nvcuda.dll`. This project fills the gaps in those open layers so the runtime works on Ampere.

The DLSS-G 310.x runtime does not need Ada hardware. It stops on Ampere for two reasons: it checks the GPU architecture through NVAPI, and it hands the driver kernels compiled for `sm_89`. The same runtime DLL also carries an `sm_86` build of every kernel that has no PTX fallback. Nothing needs to be added to the runtime. The open layers only need to present it with the answers and the kernels it already has.

Three pieces:

1. **nvcuda-lite** (`nvcuda-lite/`). A 22 KB `nvcuda.dll` written in C. NGX and the runtime only ask CUDA for device enumeration, the compute capability and the adapter LUID. This answers those calls, reads the LUID from Vulkan, and reports the compute capability you tell it. Proton ships an empty `nvcuda.dll` stub. This file replaces it. It carries the Wine builtin signature so Proton's `nvcuda=b` override accepts it. It is written against the public CUDA driver API and contains no NVIDIA code.

2. **dxvk-nvapi patch** (`patches/`). Two additions to the D3D12 cubin path, both applied in memory at upload time:
   - Kernels uploaded as CUDA fatbins carry LZ4-compressed PTX declared `.target sm_89`. The patch rewrites that literal to your SM in flight. The driver skips the incompatible ELF and JIT-compiles the PTX. All 70 fatbins in the 310.6 runtime pass this way.
   - The neural-network kernels are uploaded as bare `sm_89` ELF cubins with no PTX. The same DLL embeds an `sm_86` build of every one of them. The patch reads the loaded runtime DLL from disk, indexes its `sm_86` cubins, and substitutes the twin: same kernel name, same code size, same constant banks, highest per-instruction equality. The runtime's shared-memory size is copied onto the twin. All 39 network kernels match one to one. The `sm_86` kernels are NVIDIA's own, taken from the user's own copy of the runtime, and are used only in memory.

3. **Environment**. `DXVK_NVAPI_GPU_ARCH=AD100` so Streamline and the runtime accept the adapter. `NVCUDA_LITE_CC=8.9` for the same reason on the CUDA side. `DXVK_NVAPI_CUBIN_RETARGET_SM=86` to enable the patch.

Nothing of NVIDIA's is copied into this repository, modified on disk, or redistributed. The runtime comes with the game. The `sm_86` kernels are already inside it. The installer copies only files built from this repository.

## Quick start (prebuilt)

1. Download the latest release tarball and extract it.
2. Find your game's Steam app ID and check its DLSS-G runtime:
   ```
   ./install.py --list
   ```
   Only games that show a 310.x runtime will work.
3. Install:
   ```
   ./install.py --appid <appid>
   ```
   The installer locates your Steam library, the game's Proton build and its prefix on its own. The game must have been launched with Proton at least once.
4. Paste the launch options it prints into Steam (right-click the game, Properties, Launch Options):
   ```
   DXVK_NVAPI_GPU_ARCH=AD100 NVCUDA_LITE_CC=8.9 DXVK_NVAPI_CUBIN_RETARGET_SM=86 %command%
   ```
5. Start the game and enable DLSS and Frame Generation in its graphics settings.

`./install.py --appid <appid> --undo` puts Proton's original files back. Proton updates overwrite them too, so rerun the installer if frame generation disappears after an update.

Optional launch options: `DXVK_NVAPI_SET_NGX_DEBUG_OPTIONS=DLSSGIndicator=2` shows NVIDIA's on-screen frame-gen overlay. `DXVK_NVAPI_LOG_LEVEL=info DXVK_NVAPI_LOG_PATH=/some/dir` logs every retarget and substitution.

## Requirements

- Ampere GPU (tested: RTX 3080). Turing should work with `RETARGET_SM=75` and `NVCUDA_LITE_CC=7.5` but is untested.
- NVIDIA open kernel modules (`nvidia-open`) or the proprietary kernel module. Tested with nvidia-open 615.71.
- Proton 10 or newer (tested: Proton Hotfix and Proton Experimental, both wine 11).
- A game whose DLSS-G runtime is 310.x. Older 3.x runtimes carry encrypted PTX and cannot be retargeted. If a game ships an older `nvngx_dlssg.dll`, replace it with a 310.x one from another game you own. `./install.py --list` shows what each game has.
- Python 3 for the installer.

## Build from source

Packages: Arch `mingw-w64-gcc meson ninja python`; Debian and Ubuntu `gcc-mingw-w64-x86-64 meson ninja-build python3`; Fedora `mingw64-gcc meson ninja-build python3`.

```
make -C nvcuda-lite          # nvcuda-lite/nvcuda.dll
./build-dxvk-nvapi.sh        # build/nvapi64.dll (clones upstream dxvk-nvapi, applies patches/, builds)
./install.py --appid <appid> # picks up the freshly built files
```

## Tools

`tools/fatbin.py <dll> [outdir]` lists and extracts the CUDA fatbins in any file, with a lenient LZ4 decoder for NVIDIA's framing. Use it to check which PTX targets and instruction sets a runtime carries.

## Known issues

- Menu depth-of-field flicker in Ghost of Tsushima. The same artefact is reported on Ada under Windows.
- The runtime calls `NvAPI_D3D12_GetCudaIndependentDescriptorObject` with null descriptors for surfaces the game does not provide. dxvk-nvapi rejects those. No visible effect observed.
- Multi-frame generation above 2X is untested.

## Scope

What the project touches, and what it does not:

- No NVIDIA code is in this repository or its releases. nvcuda-lite is written from the public CUDA driver API.
- The DLSS-G module, `nvngx_dlssg.dll`, is never shipped or downloaded by this project. It arrives inside the game through NVIDIA's Streamline SDK, under a licence between NVIDIA and the game developer. The project reads the user's own copy.
- The driver is not modified. The project was developed and tested on the open kernel modules (`nvidia-open`, MIT/GPL). The only proprietary parts involved are the userspace driver libraries the distro already installs.
- The runtime is never modified on disk. The dxvk-nvapi patch works in memory at kernel upload time. It retargets PTX to the user's own SM and swaps in the `sm_86` cubins the runtime already carries. It does not strip a licence check and does not defeat encryption. Runtimes with encrypted PTX are unsupported.
- The runtime was analysed only to learn which CUDA calls it makes and how its kernels are named and laid out. The results are the call list in nvcuda-lite and the matching rules in the patch.

The work falls under the interoperability exemptions: Copyright Act s.41.12 and s.30.61 in Canada, 17 U.S.C. 1201(f) in the United States.

## Credits

- [dxvk-nvapi](https://github.com/jp7677/dxvk-nvapi) and [vkd3d-proton](https://github.com/HansKristian-Work/vkd3d-proton), which already carried DLSS-G on Ada through `VK_NVX_binary_import`.
- tB0nE and alperenalbay, for the analysis of Wine's `nvcuda.dll` exports in [sdli1995/dlssg_for_sm86#10](https://github.com/sdli1995/dlssg_for_sm86/issues/10).
- [SveSop/nvcuda](https://github.com/SveSop/nvcuda), a complete CUDA driver relay for Wine. It established that NGX can be served this way. nvcuda-lite was written independently against the same API and covers only the calls NGX makes.

## License

MIT. The dxvk-nvapi patch is under dxvk-nvapi's MIT license.
