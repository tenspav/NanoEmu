1. Database Format & Files (iPod Nano 7th Gen, FAT32 partition @ offset 258048):
- Format: Dual database - compressed iTunesCDB (zlib) + SQLite3 bundle in iTunes Library.itlp/.
- iTunes files: iTunesCDB (1,110 B active compressed DB; iTunesDB is an empty 0-byte stub).
- iTunes Library.itlp/: Library.itdb, Locations.itdb, Dynamic.itdb, Locations.itdb.cbk, Extras.itdb, Genius.itdb.
- Audio: iPod_Control/Music/Fxx/YYYY.mp3 across 50 hashed directories (F00..F49).

2. Prior Work (music-db-proto / gen_out / calcHashAB):
- Scheme: HashAB (scheme 3, 57-byte signature) implemented in calcHashAB.c / calcHashAB.wasm.
- HashAB uses 8-byte GUID (000A27002423B001) + normalized SHA-1 + 23 salt bytes via permutation ciphers & Edon-R.
- Signed targets: (a) iTunesCDB header @ 0xAB; (b) Locations.itdb.cbk (per-1KB SHA-1s -> master SHA-1 -> HashAB).
- Status: mdb_gen (C) & iOpenPod (Python) already produce validated, bit-exact databases and cbk checksums.

3. Minimal Steps to Add One MP3:
- Copy audio: Write MP3 to iPod_Control/Music/Fxx/YYYY.mp3 (F00..F49, unique 4-char filename).
- Update iTunesCDB: Insert mhit track entry and mhod descriptors (title, artist, album, location).
- Update SQLite: Insert rows in Library.itdb (item/album/artist), Locations.itdb, and Dynamic.itdb.
- Recompute Hashes: Generate Locations.itdb.cbk with HashAB; deflate iTunesCDB and sign header @ 0xAB.
- Atomic write: Write new databases, update cbk, and write iTunesCDB last; flush FAT32.

4. FUSE Mount vs Offline Image Editor:
- Offline image editor (user-space FAT32 via mtools `mcopy -i img@@offset` or pyfatfs) is strictly better.
- FUSE risks fatal dual-writer disk cache desynchronization / corruption while emulator runs, requires root/daemons, and leaves dirty mount states if crashed. Offline edits are unprivileged, fast, atomic, and safe.
