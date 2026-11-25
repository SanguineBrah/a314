# Picture datatype (a314picture.datatype)

*a314picture.datatype* lets the Amiga show pictures it has no datatype
for, by having the Raspberry Pi decode them: **WebP, AVIF, HEIC/HEIF,
JPEG XL and TIFF**. As it is a datatype, every program that uses datatypes
can show them: MultiView, web browsers, picture viewers, and so on.
Formats the Amiga already has datatypes for, such as JPEG and PNG, are
left to those.

Datatypes need Workbench 3.0 or later. With picture.datatype V43 or later
(Workbench 3.5, 3.9 and 3.2) pictures are shown in full colour, fitted to
the screen by picture.datatype; with Workbench 3.0 and 3.1 the Pi reduces
them to 256 colours first.

## Installing

On the Pi, `install-pi.sh` installs the `picture` service. The decoding is
done by the usual libraries, each of which is optional; a format whose
library is missing is reported as not available:

```
sudo apt install libwebp7 libwebpdemux2 libheif1 libheif-plugin-dav1d libheif-plugin-libde265 libjxl0.11 libtiff6
```

(`libwebpdemux2` is for animated WebP, `libheif-plugin-dav1d` for AVIF and
`libheif-plugin-libde265` for HEIC.)

On the Amiga, `make -f Makefile-amiga` puts the files where they go on the
system disk: copy `bin_amiga/Classes/DataTypes/a314picture.datatype` to
SYS:Classes/DataTypes, and the descriptors in `bin_amiga/Devs/DataTypes` to
DEVS:DataTypes, and the `Prefs/A314` drawer to SYS:Prefs. Then restart the
Amiga (or run `AddDataTypes REFRESH`).

## Notes

- Pictures larger than 1280 x 1024 (640 x 512 on Workbench 3.0 and 3.1)
  are made smaller on the Pi, so that a photo from a phone doesn't need tens
  of megabytes on the Amiga. The size can be changed with Pictures in the
  A314 drawer in Prefs (it is the environment variable
  `A314/PictureSize`). HEIC and AVIF are scaled smoothly by libheif; the
  other formats take the nearest pixel.
- Each picture is decoded in a process of its own on the Pi, with limits
  on its memory and time, so that a damaged or hostile file can't harm the
  Pi or other pictures. Pictures with more pixels than the Pi has memory
  for (up to 100 million) are refused.
- Animated WebP and JPEG XL files show their first picture.
- Only files can be shown, not pictures in the clipboard.
- The descriptors are made by [make_descriptors.py](make_descriptors.py).
  Their names end in "(A314)", so that they don't replace a descriptor
  of another datatype for the same format.

## How it works

When a program opens one of these pictures, the datatype sends the file to
the `picture` service on the Pi ([picture.py](picture.py), which describes
the messages) through a buffer in A314 memory. The Pi decodes it, scales
it if need be, and sends the pixels back the same way, a block of rows at
a time, and the datatype passes them to picture.datatype.
