import fnmatch
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import zipfile


config = json.loads(Path('.github/rc/candidate.json').read_text())
incoming = Path('incoming')
output = Path('rc-assets')
output.mkdir(exist_ok=False)

# Preserve package bytes/signatures and first verify checksums supplied by
# the producing workflow (Android and the driver already provide these).
for checksums in incoming.rglob('SHA256SUMS*'):
    if checksums.is_file():
        subprocess.run(['sha256sum', '--check', checksums.name],
                       cwd=checksums.parent, check=True)

selected = {}
for source in incoming.rglob('*'):
    if not source.is_file() or source.is_symlink() or '-debug-' in source.name:
        continue
    if not any(fnmatch.fnmatchcase(source.name, pattern) for pattern in config['groups']):
        continue
    if source.name in selected:
        raise RuntimeError(f'Duplicate release asset: {source.name}')
    selected[source.name] = source

for pattern, expected in config['groups'].items():
    found = sum(fnmatch.fnmatchcase(name, pattern) for name in selected)
    if found != expected:
        raise RuntimeError(f'Expected {expected} assets matching {pattern}, found {found}')

if config['component'] == 'Hermes-KMS':
    if selected['SOURCE_COMMIT'].read_text().strip() != config['source_sha']:
        raise RuntimeError('Driver source commit does not match its candidate')
if config['component'] == 'Hestia-Android':
    for name, source in selected.items():
        if name.endswith('-arm64-v8a.apk'):
            with zipfile.ZipFile(source) as apk:
                if 'lib/arm64-v8a/libpyrowave-shared.so' not in apk.namelist():
                    raise RuntimeError(f'{name} does not ship the PyroWave runtime')

for name, source in selected.items():
    shutil.copyfile(source, output / name)

manifest = dict(config)
manifest['promotion'] = 'Exact CI outputs; embedded build versions and signatures are unchanged.'
(output / 'RC_MANIFEST.json').write_text(json.dumps(manifest, indent=2) + '\n')
(output / 'SHA256SUMS').write_text(''.join(
    f'{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}\n'
    for path in sorted(output.iterdir()) if path.is_file()
))
subprocess.run(['sha256sum', '--check', 'SHA256SUMS'], cwd=output, check=True)
print(f"Staged {len(selected)} original {config['component']} assets plus provenance/checksums")
