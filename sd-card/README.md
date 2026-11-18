# SD card layout

The DFPlayer Mini reads a **FAT32** micro-SD card (32 GB or less). Every audio file must be an MP3 whose name
**ends in `.mp3`**. A file called `song.mp3.mpeg` (common after sending audio through WhatsApp) is ignored.
In Windows File Explorer, turn on *View → Show → File name extensions* to see the real name.

```
/1-Alert.mp3          dose alarm sound
/2-PillsReady.mp3     "pills are ready" chime
/01/001.mp3           voice prompt 1   (Schedule → Alarm sound → Voice 1)
/01/002.mp3           voice prompt 2   ...
/02/001.mp3           Party mode song 1
/02/002.mp3           Party mode song 2 ...  (played in turn, on repeat)
/03/001.mp3           Shabad 1
/03/002.mp3           Shabad 2 ...           (played in turn, on repeat)
```

## Rules
1. **Root files: copy one at a time, alarm first.** The alarm and chime are played by number, and the DFPlayer
   numbers root files in the order they were copied, not by name.
2. **Folders `01`, `02`, `03`** use 3-digit file names: `001.mp3`, `002.mp3`, …
3. Any normal MP3 works (8–48 kHz, up to 320 kbps). Long files are fine.
4. Use any short alarm and chime sounds you like. A recorded voice ("Dadi ji, dawai ka time ho gaya") works well as `/01/001.mp3`.

## Test
Web app → **Settings → Sound & screen → Alarm / Pills ready**, or on the touch screen **⚙ → ▶** next to Volume.
The small LED on the DFPlayer lights up while a file is playing.
