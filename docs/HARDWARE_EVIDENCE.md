# P210 hardware-evidence gate

The physical-board build must not combine contacts taken from different FPGA
generations. The versioned manifest at
`src/neptunesdr_firmwave/data/p210-hardware-candidate-v1.json` records the
currently available observations and deliberately remains `flashable: false`.
Its format is defined by `specs/p210-hardware-manifest-v1.schema.json`.

Run the gate with:

```sh
neptune-firmwave validate-hardware-manifest --json
```

Exit status `0` means every required contact meets its configured evidence
count, all observations agree, artifact hashes match `firmware-lock.json`, and
every required GPIO offset fits each handoff's PS GPIO capacity. Exit
status `1` means the manifest is valid but incompatible. Exit status `2` means
the manifest, lock, or invocation is malformed.

The checked-in candidate is expected to return `1`. It proves that the public
inputs are not one board definition:

| Contact | Locked SD HDF | Locked XSA | Public DTB |
|---|---:|---:|---:|
| Vivado | 2019.1 | 2023.2 | unknown |
| DDR bus | 32 bit | 16 bit | unknown |
| DDR size | 1 GiB | 512 MiB | 512 MiB |
| AD9361 interface | LVDS | CMOS | LVDS |
| RX/TX channels | 1/1 | 2/2 | 2/2 |
| PS EMIO width | 64 | 18 | unknown |

The DTB maps the AD9361 controls to absolute PS GPIO offsets 98 through 102
and USB reset to 85. The XSA exposes 54 MIO plus 18 EMIO GPIOs, so those
offsets exceed its capacity of 72. This is a hard compatibility failure, not a
warning to waive.

## Replacing the candidate

Create a new manifest only from one revision-matched board evidence set. Keep
the locked source artifact name, SHA-256 digest, member path, and short
evidence note with every observation. At minimum, establish agreement across
separately inspected artifacts for:

- Zynq part and Vivado/source identity;
- DDR bus width and addressable size;
- CMOS or LVDS interface and 1R1T or 2R2T topology;
- PS MIO/EMIO capacity and every required control mapping.

Do not turn unknown values into guesses. `null` is allowed for an individual
observation, but the configured minimum evidence count must still be met. A
candidate passing this gate is evidence-consistent; it is not by itself
authorization to flash hardware. Recovery media, a revision-matched vendor
procedure, and bench validation remain separate prerequisites.
