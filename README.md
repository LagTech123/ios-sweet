# ios-sweet-kagura

**Apple's iOS 16 WIP on a Xperia XZ F8331.**

This project ports Apple's open-source XNU kernel (xnu-8796, the iOS 16 generation) to the Sony Xperia XZ F8331 (`kagura`, Qualcomm MSM8996 / Snapdragon 820).

As of **21-11-2016** the phone boots to shim and the XNU kernel panics

> Experimental bring-up, not a usable phone OS. Many things are missing or stubbed (see below), and Apple's security model is mostly switched off. It installs into the `userdata` partition and so **erases Android's user data**.

## What works

| Area | State |
| --- | --- |
| Boot | Stock Sony bootloader → Boot partition Flashed with shim → XNU. |

## What does not work (yet)

- **Everything**
