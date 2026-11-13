# dr_libs audio decoders

**Upstream:** https://github.com/mackron/dr_libs

**Vendored headers:** `dr_flac.h` **0.13.4**, `dr_mp3.h` **0.7.4**, `dr_wav.h` **0.14.6**.

**Status:** Required for native Winamp playback and audio metadata handling.

## What they do

The included single-header libraries decode **FLAC**, **MP3** and **WAV**. They are compiled by the backend and Winamp playback source with the libraries' own implementation macros. They are **audio decoders**, not archive engines.

## Local modifications

No edits to the imported headers are recorded since the initial Git import `027a8aa`. The project's wrapper, playback state, seek logic and metadata operations are implemented outside this folder. The three versions are noted individually; do not assume a common dr_libs release tag.

## Licence

Each header offers a choice of **public domain/Unlicense-style** or **MIT No Attribution (MIT-0)**. The full, authoritative licence alternatives are at the **end of each respective source header**. `dr_mp3.h` also acknowledges its minimp3 origins. Preserve notices in the source and keep a credit/licence inventory for binary release packaging.

Do not confuse the project-specific archive playback code with original dr_libs source.
