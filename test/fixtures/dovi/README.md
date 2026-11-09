# Dolby Vision RPU fixtures

Copied from [dovi_tool](https://github.com/quietvoid/dovi_tool) `assets/tests`
at tag `libdovi-3.4.0`, under the MIT license in `LICENSE`:

- `fel_orig.bin`: Profile 7 FEL RPU.
- `mel_orig.bin`: Profile 7 MEL RPU.
- `mel_to_81.bin`: `mel_orig.bin` after libdovi mode 2 conversion to Profile 8.1.

Each file holds a 4-byte start code followed by the RPU payload as stored in
the NAL (with emulation prevention bytes), without the two-byte NAL header.
For FEL, mode 2 also removes the luma/chroma mapping, so there is no direct
FEL-to-8.1 reference file.
