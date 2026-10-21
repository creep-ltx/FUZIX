FUZIX for the Amiga 0.1b1 (beta): a ready-made hard disk image
=============================================================

fuzix-amiga-0.1b1.hdf   40 MB hard disk with FUZIX fully installed
                        (all Extras included); boots on its own
fuzix-amiga.fs-uae      a ready configuration for FS-UAE

FUZIX drives the Amiga's own hardware, so the image must sit on the
emulated A1200 IDE port, not on UAE's own hard disk controller.

FS-UAE:  open fuzix-amiga.fs-uae (keep it next to the .hdf). It needs an
         A1200 Kickstart 3.1 ROM: add a kickstart_file line, or keep the
         ROM in FS-UAE's Kickstarts folder.

WinUAE:  (not tried yet) A1200 with Kickstart 3.1. Hard drives: Add
         Hardfile, choose the .hdf, HD Controller "A600/A1200/A4000 IDE".
         Optional: the battery-backed clock (Chipset settings) MSM6242B,
         so FUZIX gets the date without asking.

Log in as root. Four screens, each with its own login: Amiga+F1 to F4
(the left Windows key in FS-UAE, which keeps left Alt for itself).
PAL and NTSC Amigas both work: NTSC gives 80x25 instead of 80x32.
Commands: keymap (keyboard layout), fontprefs (FUZIX's
font or the ROM's Topaz), setup, and the usual Unix tools.

Source and licence (GPL v2): https://github.com/creep-ltx/FUZIX (branch amiga)
