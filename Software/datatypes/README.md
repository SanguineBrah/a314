# A314 datatypes (a314picture.datatype, a314sound.datatype)

The A314 datatypes let the Amiga show pictures and play sounds that it has
no datatype for, by having the Raspberry Pi decode them:

- **Pictures:** WebP, AVIF, HEIC/HEIF, JPEG XL and TIFF
  (*a314picture.datatype*)
- **Sounds:** MP3, FLAC and Ogg Vorbis/Opus (*a314sound.datatype*)

As they are datatypes, every program that uses datatypes can show and
play them: MultiView, web browsers, picture viewers, and so on. Formats
the Amiga already has datatypes for, such as JPEG, PNG, WAV and AIFF, are
left to those. Either datatype can be installed without the other.

Datatypes need Workbench 3.0 or later.

- **Pictures:** with picture.datatype V43 or later (Workbench 3.5, 3.9 and
  3.2) they are shown in full colour, fitted to the screen by
  picture.datatype. With Workbench 3.0 and 3.1 the Pi reduces them to 256
  colours first.
- **Sounds:** they are played by sound.datatype through Paula, as 8-bit
  samples at up to 22050 a second. They are stereo with sound.datatype V44
  or later (Workbench 3.5, 3.9 and 3.2), and mono with 3.0 and 3.1.

## Installing

On the Pi, `install-pi.sh` installs the `datatypes` service. The decoding
is done by the usual libraries, each of which is optional; a format whose
library is missing is reported as not available:

```
sudo apt install libwebp7 libwebpdemux2 libheif1 libheif-plugin-dav1d libheif-plugin-libde265 libjxl0.11 libtiff6
sudo apt install libsndfile1 libsamplerate0 libmpg123-0
```

The first line is for pictures:
- `libwebpdemux2` is for animated WebP.
- `libheif-plugin-dav1d` is for AVIF.
- `libheif-plugin-libde265` is for HEIC.

The second line is for sounds:
- `libsndfile1` decodes them.
- `libsamplerate0` resamples them for Paula.
- `libmpg123-0` decodes MP3 if libsndfile can't (libsndfile before 1.1).

On the Amiga, `make -f Makefile-amiga` puts the files where they go on the
system disk:

- `bin_amiga/Classes/DataTypes/a314picture.datatype` and
  `a314sound.datatype` go to SYS:Classes/DataTypes.
- The descriptors in `bin_amiga/Devs/DataTypes` go to DEVS:DataTypes. Those
  whose names start with MP3, FLAC or Ogg are for sounds; the others are
  for pictures.
- The `Prefs/A314` drawer goes to SYS:Prefs (for the Pictures setting).

Then restart the Amiga (or run `AddDataTypes REFRESH`).

## Notes

- **Picture size limit:** pictures larger than 1280 x 1024 (640 x 512 on
  Workbench 3.0 and 3.1) are made smaller on the Pi, so that a photo from
  a phone doesn't need tens of megabytes on the Amiga.
  - The size can be changed with Pictures in the A314 drawer in Prefs (it
    is the environment variable `A314/PictureSize`).
  - HEIC and AVIF are scaled smoothly by libheif; the other formats take
    the nearest pixel.
- **Long sounds:** a sound is kept in the Amiga's memory, so a long one is
  cut short to what half of the largest free block holds (at most 16 MB
  for each channel). For music, an MHI player such as AmigaAMP with
  [mhia314.library](../mhi) is better.
- **Animations:** animated WebP and JPEG XL files show their first picture.
- **Files only:** only files can be shown or played, not data from the
  clipboard.
- **Safety:** each file is decoded in a process of its own on the Pi, with
  limits on its memory and time, so that a damaged or hostile file can't
  harm the Pi or other files. Pictures with more pixels than the Pi has
  memory for (up to 100 million) are refused.
- **Descriptors:** they are made by
  [make_descriptors.py](make_descriptors.py). Their names end in "(A314)",
  so that they don't replace a descriptor of another datatype for the same
  format.

## How it works

When a program opens one of these files, the datatype sends the file to
the `datatypes` service on the Pi ([datatypes.py](datatypes.py), which
describes the messages) through a buffer in A314 memory. The Pi decodes it,
scales or resamples it, and sends the pixels or samples back the same way,
a block at a time. The datatype passes them to picture.datatype or
sound.datatype. The two datatypes share the code for this
([a314dt.c](a314dt.c)).
