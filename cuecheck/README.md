# cuecheck

Check a single CUE file using the current checkout's MPD parser and display its parsed audio tracks. The checker retains MPD's permissive syntax handling, but rejects invalid UTF-8, missing local file references, and files that produce no audio tracks.

## Build

From the MPD repository root, with a C++23 compiler, Meson, Ninja, and Python 3 installed:

```sh
meson setup cuecheck/build cuecheck
meson compile -C cuecheck/build
```

No separate MPD build is required.

## Usage

```sh
cuecheck/build/cuecheck /path/to/album.cue
cuecheck/build/cuecheck --json /path/to/album.cue
cuecheck/build/cuecheck -- ./-album.cue
```

Text output is the default. Each track includes its file reference, resolved local path, all parser-produced tags, start time, and end time in milliseconds. An unspecified end time is shown as `unspecified` in text or `null` in JSON. JSON contains `valid`, `errors`, and `tracks`; tags are an array to preserve repeated values.

The entire input must be strict UTF-8, including ignored commands and comments. A leading UTF-8 BOM is allowed. Invalid encoding is reported with a zero-based byte offset and a one-based line number; no tracks are parsed in that case. Encoding is never guessed or converted. Text that is garbled but still valid UTF-8 cannot be detected by encoding validation alone.

Every `FILE` declaration from which MPD recognizes both a filename and a type is checked, even if it produces no audio tracks. Relative references are resolved against the CUE file's directory. References must point to regular files; symbolic links to regular files are allowed. Non-local references fail verification. Audio decoding, readability, and duration are not checked.

Missing references do not suppress parsed tracks. Unknown commands, malformed indices, and other syntax tolerated by MPD remain tolerated; this is not a strict CUE specification validator.

Exit codes:

- `0`: all checks passed.
- `1`: the input failed validation or could not be read.
- `2`: invalid command-line usage.

Run the checks with:

```sh
meson test -C cuecheck/build --print-errorlogs
```
