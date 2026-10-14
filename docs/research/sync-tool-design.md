��s packaged WASM signer and `verify_hashab`/`verify_locations_cbk`; retain study `calcHashAB.wasm` and `diffdb.py` as reference fixtures and diagnostics.
7. **New code:** CLI/planning, offset-aware FAT adapter, image transactions, app-pack merging, emulator generation tracking, and cache-refresh integration.
8. **Write sequence, 1:** Stop the emulator; lock the image; validate partition/signatures/free space; clone `dumps/ipod-disk.dd` to a same-filesystem transaction image.
9. **2:** Read CDB, SQLite bundle, artwork, and playback sidecars; preserve existing identities/playlists/unknown fields and consume or remap positional sidecars when track order changes.
10. **3:** Allocate collision-checked `iPod_Control/Music/F00..F49/ABCD.mp3` paths, balancing directory occupancy; copy audio into the clone and verify SHA-256 by rereading.
11. **4:** Prepare one coherent library: CDB `mhit`/`mhod`, master-playlist membership/counts/indexes, and matching Library/Locations/Dynamic rows and identifiers.
12. **5:** Include generated Extras/Genius and optional ArtworkDB/`.ithmb` artifacts as required; preserve the empty `iTunesDB` stub.
13. **6:** Finalize Locations bytes; compute SHA-1 per 1 KiB block, SHA-1 of concatenated digests, then GUID-bound HashAB for `Locations.itdb.cbk`.
14. **7:** Deflate CDB first, then sign its physical bytes: normalized digest uses discriminator 4; published scheme is 3; write the 57-byte signature at `0xAB`.
15. **8:** Stage/readback all outputs; publish artwork payloads → ArtworkDB → Library → Locations → Dynamic → Extras/Genius → Locations.cbk → **iTunesCDB last**.
16. **9:** Validate SQLite integrity, signatures, references/counts, media hashes, and FAT consistency; fsync clone, atomically replace the image, then fsync its parent directory.
17. **Atomicity:** FAT `mdel+mren` and “CDB last” do not make a multi-file transaction atomic; whole-image replacement supplies the commit boundary and backup supplies rollback.
18. **Apps:** Merge the active NanoApps pack, preserving IDs/layout; write `/Apps/Executables/<name>.hbapp` and `/Apps/Icons/<name>.bin`, then publish `AllApps.pack` or `AllApps-B.pack` last.
19. Home visibility requires the resident loader’s registration path and a reload; copying an executable alone does not register an icon.
20. **Recommended backend:** Offline mtools using `-i transaction.dd@@258048`; unprivileged and compatible with the existing tooling.
21. Launch with `--nand-miss oracle --nand-disk dumps/ipod-disk.dd --nand-disk-offset 49216`; 49216 is the FTL offset in **4 KiB blocks**, distinct from the FAT byte offset.
22. **Sparse NAND alternative:** Its records contain physical CE/page data, validity masks, and FTL metadata; FAT writes require translation and sequence handling. Store hits override image fallback.
23. Therefore avoid direct sparse-store injection; preserve the old session, use a fresh store for the new image, and refuse existing overlays until an explicit export/merge path exists.
24. **CLI:** `nanoemu-sync [--image PATH] [--offset 258048] [--guid HEX] add song.mp3`; mutations support `--dry-run` and produce a transaction manifest.
25. Subcommands: `list`, `add`, `remove`, `playlist create|add|remove`, `apps list|install|remove`, `verify`, `backup`, `restore`, `diff`, and `launch`.
26. **Risks/mitigations:** Corruption or interrupted writes → clone/validate/atomic commit; identity or sidecar drift → shared preparation engine and cross-database checks.
27. Stale RAM/FAT/Music/Home caches → fresh process plus verified filesystem/library/app reload; restarting from snap6 still restores cached RAM, so reload support remains required.
28. Bind save states to image-generation hashes and reject mismatches: F1/F4 restore RAM and NAND state, not a newly synced library.
29. Existing code validates bytes/signatures; injected-library and app registration acceptance still need emulator smoke tests: list/play the added song and open the added Home icon.
