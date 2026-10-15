# NanoSync Mac

**NanoSync Mac** sends music from your Mac to a real iPod nano 7G. The
**NanoSync** homebrew app on the iPod then adds it to the iPod's music
library, with tags and cover art.

## Using it

1. Open **NanoSync Mac** (it comes in the NanoEmu `.dmg`).
2. Drag songs or whole folders onto the window, or click the drop area to
   pick them.
3. Plug the iPod into the Mac with its USB cable. It has to show up in
   Finder as a drive (the drive has an `iPod_Control` folder). The window
   turns to "iPod connected" with the free space.
4. Click **Sync**.
5. Click **Eject**, unplug the iPod, open **NanoSync** on it and tap
   **Importar**.

Next time you plug the iPod in, the window shows NanoSync's last report.

## What it sends

| File | What happens |
|---|---|
| MP3 | copied as it is |
| M4A, AAC, WAV, AIFF, FLAC, OGG, Opus, WMA, ALAC, CAF | converted to MP3 (VBR ~190 kbps), tags and cover art kept. Needs [ffmpeg](https://ffmpeg.org): `brew install ffmpeg` |
| Videos | listed, not sent: NanoSync imports music only |

Cover art: NanoSync reads JPEG covers from the MP3's ID3 tag (PNG covers are
skipped, the song still goes in).

## How it works

NanoSync, on the iPod, imports every `.mp3` it finds in
`/Apps/Data/Importar/` on the iPod's drive into the music library (it
writes the iTunesCDB, the SQLite library and the ArtworkDB itself), removes
each file once imported, and writes its report to `log.txt` and `done.txt`
in that folder. With a `dryrun.txt` file there it only rehearses.

NanoSync Mac only puts files in that folder, so it needs no special USB
protocol: the iPod's normal USB disk mode is enough. It also:

- gives every file a short FAT-safe name (NanoSync skips names that are too
  long) and never overwrites a file already waiting there,
- copies into a hidden `.part` file first, so NanoSync never sees half a
  song,
- removes the hidden `._` files macOS writes next to files on FAT drives
  (NanoSync would see them as more `.mp3` files),
- leaves the checks to NanoSync: it looks at the battery and free memory
  before importing, and its report says why when it refuses.

## Command line

```sh
"/Applications/NanoSync Mac.app/Contents/MacOS/NanoSyncMac" --cli ~/Music/Album
"/Applications/NanoSync Mac.app/Contents/MacOS/NanoSyncMac" --cli --ipod "/Volumes/IPOD NANO" song.mp3
```

## Building

```sh
sync/macos/build.sh        # -> emulator/dist/NanoSync Mac.app
```
