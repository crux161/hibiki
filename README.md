# Hibiki (響)

Hibiki is a modern RE project dedicated to making the MyGiga USB-C ATSC tuner usable 
outside of the NewPadTVHD app for Android.

## Extracted Firmware Candidates

Source tree: `MyGica-atsc/NewPad`

## Primary candidates

### Silicon Labs patch streams

Extracted from `lib/armeabi-v7a/libPadTV_jni.so`, section `.data`.

File offset = symbol value - `0x1000`.

| Symbol | VA | File offset | Size | Notes |
| --- | ---: | ---: | ---: | --- |
| `Si2144_FW_2_1b2` | `0x000d61f8` | `0x000d51f8` | 510 | `30 * 17` framed rows |
| `Si2141_FW_1_1b12` | `0x000d63f6` | `0x000d53f6` | 2159 | `127 * 17` framed rows |
| `Si2168B_PATCH16_4_0b11` | `0x000d6c65` | `0x000d5c65` | 6919 | `407 * 17` framed rows |
| `Si2183_PATCH16_6_0b13` | `0x000d876c` | `0x000d776c` | 10829 | `637 * 17` framed rows |

For each symbol:

- `*.table.bin` is the exact 17-byte-row table from the shared object.
- `*.commands.bin` is the concatenated command stream with each row's leading length byte removed and zero padding discarded.

### MaxLinear Eagle blob

Extracted from `lib/armeabi-v7a/libPadTV_jni.so`.

| Symbol | VA | File offset | Size | Notes |
| --- | ---: | ---: | ---: | --- |
| `mbin_mxl_eagle_fw` | `0x000ecd48` | `0x000ebd48` | 43997 | Starts with `M1`; contains ThreadX Xtensa strings |

This is probably firmware for the MxL692/Eagle ATSC frontend, not 8051.

### PT930 pre-symbol 8051-like region

Extracted from `lib/armeabi-v7a/libPT930Drv.so`.

| Region | File offset | Size | Notes |
| --- | ---: | ---: | --- |
| `.data` pre-symbol region | `0x00016000` | 4212 | Contains 8051-like opcodes and loader-style records before exported `MN88553` config symbols |

The first exported PT930 symbols at `.data` are text/config resources such as `MN88553`, `MN88553_chfile_20160317X26_3`, `PSEQ1_MN88553_160304_bprty`, and `TMM3_TNCTL_*`. Those are extracted under `PT930_MN88553/` as exact bytes.

## SHA-256

```text
32403bbb46b673dd4e24ddcf458f79dc16e8663f06c404cc2d01981d14edf13f  Si2141_FW_1_1b12.commands.bin
5df0cb9fbc18f12a420c6741afe5e8f5322d48e620b5f16858f5cdb9e4a40a53  Si2141_FW_1_1b12.table.bin
2642da7d8b1e64a7a5778871aec89bd6840687fd046ba59e643c515b0a443437  Si2144_FW_2_1b2.commands.bin
ec993282e01f7173c60362e60a41711d813da1d06005d7659abc59541c296a3e  Si2144_FW_2_1b2.table.bin
72c91339f5246b4bbe468c4774c59b57a74b09a48e787adaaee98eca8ebc427f  Si2168B_PATCH16_4_0b11.commands.bin
8507536630d75a316d0719d6b95c04b90c36baa5b457ad457c9bacadafcef134  Si2168B_PATCH16_4_0b11.table.bin
82bae47f82e6c3b2829d487ca701684e60ef3a705ee162dcf22827f384ccddbd  Si2183_PATCH16_6_0b13.commands.bin
5e1c860e95a8d92b2716b0075dd00e55f99c2c368e35340f6b527c366ff27ee7  Si2183_PATCH16_6_0b13.table.bin
8f77d5cb0de9111ed63c113cb2fd54a63e07a30040ae0b562cb025ab9c4e600d  mbin_mxl_eagle_fw.bin
0c3348a7f30fba3c9d3a1463f2583ce64f6af7922da4e60531f05473ce711d2a  PT930_MN88553/PT930_pre_symbol_8051_region.bin
```
