import json
import pathlib
import subprocess
import sys
import tempfile

binary = str(pathlib.Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory() as directory:
    root = pathlib.Path(directory)
    (root / 'audio.flac').touch()
    cue = root / 'test.cue'

    def check(content, valid, tracks=None):
        cue.write_bytes(content)
        result = subprocess.run([binary, '--json', str(cue)], capture_output=True, text=True)
        assert result.returncode == (0 if valid else 1), result
        parsed = json.loads(result.stdout)
        assert parsed['valid'] == valid, parsed
        if tracks is not None:
            assert len(parsed['tracks']) == tracks, parsed
        return parsed

    basic = b'FILE "audio.flac" WAVE\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n'
    check(basic, True, 1)
    check(b'\xef\xbb\xbf' + basic.replace(b'\n', b'\r\n'), True, 1)
    check(basic.rstrip(b'\n'), True, 1)
    check(b'', False, 0)
    check(b'TRACK 01 AUDIO\n', False, 1)
    check(b'FILE audio.flac WAVE\nTRACK 01 MODE1/2352\n', False, 0)
    check(b'FILE missing.flac WAVE\nTRACK 01 AUDIO\n', False, 1)
    check(b'FILE missing.flac WAVE\n' + basic, False, 1)
    check(b'FILE "" WAVE\nTRACK 01 AUDIO\n', False, 1)
    check(b'FILE https://example.org/audio WAVE\nTRACK 01 AUDIO\n', False, 1)
    check(b'FILE . WAVE\nTRACK 01 AUDIO\n', False, 1)
    for invalid in [b'\xff', b'\xc0\x80', b'\xed\xa0\x80', b'\xf4\x90\x80\x80', b'\xe2\x82']:
        result = check(basic + b'REM ignored ' + invalid, False, 0)
        assert 'Invalid UTF-8' in result['errors'][0]
    check(b'REM ignored \xf4\x8f\xbf\xbf\n' + basic, True, 1)
    check(basic + b'UNKNOWN ignored\nTITLE "unclosed\n', True, 1)
    check(b'FILE audio.flac NONSTANDARD\nTRACK not-a-number AUDIO\n', True, 1)
    check(b'FILE audio.flac WAVE\nTRACK 01 AUDIO\nINDEX 01 invalid\n', True, 1)
    result = check(b'TITLE "Album"\nPERFORMER "Artist"\n' + basic +
                   b'TRACK 02 AUDIO\nINDEX 00 01:00:00\nINDEX 01 01:02:01\n'
                   b'TRACK 03 AUDIO\nINDEX 01 02:00:00\n', True, 3)
    assert result['tracks'][0]['end_ms'] == 62013
    assert result['tracks'][1]['start_ms'] == 62013
    assert result['tracks'][1]['end_ms'] == 120000
    assert result['tracks'][2]['end_ms'] is None
    assert {'name': 'Album', 'value': 'Album'} in result['tracks'][0]['tags']
    check(basic + b'TITLE "\xe4\xb8\xad\xe6\x96\x87"\n', True, 1)
    (root / 'link.flac').symlink_to(root / 'audio.flac')
    check(basic.replace(b'audio.flac', b'link.flac'), True, 1)
    check(basic.replace(b'audio.flac', str(root / 'audio.flac').encode()), True, 1)
    check(b'REM ' + b'x' * 10000 + b'\n' + basic, True, 1)
    check(basic.replace(b' WAVE', b'\x00 WAVE'), False, 1)
    cue.write_bytes(basic)
    result = subprocess.run([binary, str(cue)], capture_output=True, text=True)
    assert result.returncode == 0 and 'Track 1' in result.stdout and 'unspecified' in result.stdout
    for args in [[], [str(cue), str(cue)], ['--unknown']]:
        assert subprocess.run([binary, *args], capture_output=True).returncode == 2
    result = subprocess.run([binary, '--json', str(root / 'absent.cue')], capture_output=True, text=True)
    assert result.returncode == 1 and not json.loads(result.stdout)['valid']
    result = subprocess.run([binary, '--json', str(root)], capture_output=True, text=True)
    assert result.returncode == 1 and not json.loads(result.stdout)['valid']
