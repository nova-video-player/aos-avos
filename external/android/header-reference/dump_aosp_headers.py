#!/usr/bin/env python3
"""Archive AOSP header families for review, never for the AVOS include path."""
import argparse
import base64
import concurrent.futures
import datetime
import hashlib
import io
import json
import pathlib
import tarfile
import time
import threading
import urllib.error
import urllib.request

BASE = 'https://android.googlesource.com/'
RELEASES = [(23,'6.0.0'),(24,'7.0.0'),(25,'7.1.0'),(26,'8.0.0'),
            (27,'8.1.0'),(28,'9.0.0'),(29,'10.0.0'),(30,'11.0.0'),
            (31,'12.0.0'),(32,'12.1.0'),(33,'13.0.0'),(34,'14.0.0'),
            (35,'15.0.0'),(36,'16.0.0'),(37,'17.0.0')]
ROOTS = {
 'platform/frameworks/native': ['include','headers','libs/nativewindow/include',
     'libs/nativebase/include','libs/ui/include','libs/gui/include','libs/binder/include'],
 'platform/frameworks/av': ['include','media/libstagefright/include',
     'media/libstagefright/foundation/include','media/libmedia/include',
     'media/libaudioclient/include','media/libaudiofoundation/include','media/libnbaio/include'],
 'platform/system/core': ['include','libcutils/include','libutils/include',
     'liblog/include','libsync','libsystem/include','libutils/binder/include'],
 'platform/hardware/libhardware': ['include','include_all'],
 'platform/system/media': ['graphics/include','audio/include'],
 'platform/system/logging': ['liblog/include'],
 'platform/system/libbase': ['include'],
 'platform/system/libutils': ['include'],
 'platform/hardware/interfaces': ['graphics'],
}
SUFFIXES = {'.h','.hpp','.hxx','.inc','.hal','.aidl'}
LICENSES = {'NOTICE','LICENSE','COPYING','NOTICE.txt','LICENSE.txt'}

CACHE = None
REQUEST_LOCK = threading.Lock()
LAST_REQUEST = 0.0

def get(url):
    global LAST_REQUEST
    cache = CACHE / hashlib.sha256(url.encode()).hexdigest()
    missing = cache.with_suffix('.404')
    if cache.exists(): return cache.read_bytes()
    if missing.exists(): return None
    for attempt in range(5):
        with REQUEST_LOCK:
            time.sleep(max(0, 0.4 - (time.monotonic() - LAST_REQUEST)))
            LAST_REQUEST = time.monotonic()
        try:
            with urllib.request.urlopen(url, timeout=45) as response:
                data = response.read()
                cache.write_bytes(data)
                return data
        except urllib.error.HTTPError as error:
            if error.code == 404:
                missing.touch()
                return None
            if error.code not in (429,500,502,503,504) or attempt == 4:
                raise
            delay = max(int(error.headers.get('Retry-After', '0')), 15 * (attempt + 1))
        except (TimeoutError, OSError):
            if attempt == 4: raise
            delay = 2 ** attempt
        time.sleep(delay)

def digest(data):
    return hashlib.sha256(data).hexdigest()

def save(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        if path.read_bytes() != data:
            raise RuntimeError('refusing to overwrite differing snapshot: ' + str(path))
    else:
        path.write_bytes(data)

def safe_path(value):
    path = pathlib.PurePosixPath(value)
    if path.is_absolute() or '..' in path.parts:
        raise RuntimeError('unsafe archive path: ' + value)
    return path

def verify_file(data, item):
    if len(data) != item['bytes'] or digest(data) != item['sha256']:
        raise RuntimeError('snapshot checksum/size mismatch: ' + item['path'])

def restore_snapshot(dest, manifest):
    """Restore missing bytes from the recorded commit without rewriting provenance."""
    missing = {}
    for item in manifest['files']:
        original = str(safe_path(item['path']))
        path = dest / safe_path(item.get('stored_path', item['path']))
        if path.exists():
            verify_file(path.read_bytes(), item)
        else:
            missing[original] = item
    count = len(missing)
    for entry in manifest['archives']:
        if entry['status'] != 'downloaded':
            continue
        root = safe_path(entry['path'])
        if not any(root in pathlib.PurePosixPath(name).parents for name in missing):
            continue
        # Construct the URL from the pinned revision, never re-resolve the tag.
        url = (BASE + manifest['repository'] + '/+archive/' +
               manifest['commit'] + '/' + str(root) + '.tar.gz')
        payload = get(url)
        if payload is None or digest(payload) != entry['archive_sha256']:
            raise RuntimeError('archive checksum mismatch or unavailable: ' + url)
        with tarfile.open(fileobj=io.BytesIO(payload), mode='r:gz') as archive:
            for member in archive.getmembers():
                relative = str(root / safe_path(member.name))
                item = missing.get(relative)
                if item is None:
                    continue
                if not member.isfile():
                    raise RuntimeError('expected regular snapshot file: ' + relative)
                data = archive.extractfile(member).read()
                verify_file(data, item)
                save(dest / safe_path(item.get('stored_path', item['path'])), data)
                del missing[relative]
    if missing:
        raise RuntimeError('files absent from pinned archives: ' + ', '.join(sorted(missing)))
    return count

def fetch_repo(output, api, version, repo, roots):
    tag = 'android-' + version + '_r1'
    dest = output / ('api-%d_%s' % (api,tag)) / repo.removeprefix('platform/')
    manifest_path = dest / 'SNAPSHOT.json'
    if manifest_path.exists():
        previous = json.loads(manifest_path.read_text())
        if (previous.get('status') != 'complete' or
                previous.get('api') != api or previous.get('tag') != tag or
                previous.get('repository') != repo or
                [item['path'] for item in previous.get('archives', [])] != roots):
            raise RuntimeError('snapshot policy differs; use a new output directory: ' + str(dest))
        restored = restore_snapshot(dest, previous)
        return api, repo, len(previous['files']), 'restored' if restored else 'cached'
    raw = get(BASE + repo + '/+/refs/tags/' + tag + '?format=JSON')
    if raw is None:
        return api, repo, 0, 'tag_absent'
    info = json.loads(raw.split(b'\n',1)[1])
    commit = info['commit']
    files = {}; archives = []; links = []
    for root in roots:
        url = BASE + repo + '/+archive/' + commit + '/' + root + '.tar.gz'
        # Gitiles returns HTTP 400 for some absent archive paths. Confirm
        # existence through its tree endpoint; never hide other fetch errors.
        try:
            tree_info = get(BASE + repo + '/+/' + commit + '/' + root + '/?format=JSON')
        except urllib.error.HTTPError as error:
            if error.code != 400: raise
            # Some historical include roots are forwarding symlinks, not trees.
            parent, name = root.rsplit('/', 1)
            parent_data = get(BASE + repo + '/+/' + commit + '/' + parent + '/?format=JSON')
            if parent_data is None: raise
            entries = json.loads(parent_data.split(b'\n', 1)[1])['entries']
            entry = next((e for e in entries if e['name'] == name), None)
            if entry and entry['type'] == 'blob' and entry['mode'] == 40960:
                target = base64.b64decode(get(BASE + repo + '/+/' + commit + '/' + root + '?format=TEXT'), validate=True).decode()
                links.append({'path': root, 'target': target, 'kind': 'symlink'})
                archives.append({'path': root, 'status': 'forwarding_symlink', 'target': target, 'blob': entry['id']})
                continue
            raise
        payload = get(url) if tree_info is not None else None
        if payload is None:
            archives.append({'path': root, 'url': url, 'status': 'path_absent'})
            continue
        archives.append({'path': root, 'url': url, 'status': 'downloaded',
                         'archive_sha256': digest(payload)})
        with tarfile.open(fileobj=io.BytesIO(payload), mode='r:gz') as archive:
            for member in archive.getmembers():
                relative = safe_path(root) / safe_path(member.name)
                if relative.suffix not in SUFFIXES and relative.name not in LICENSES:
                    continue
                if member.issym() or member.islnk():
                    links.append({'path':str(relative),'target':member.linkname,
                                  'kind':'symlink' if member.issym() else 'hardlink'})
                    continue
                if not member.isfile():
                    continue
                data = archive.extractfile(member).read()
                stored = relative
                parent = (dest / relative).parent
                if parent.exists():
                    aliases = [p.name for p in parent.iterdir()
                               if p.name.casefold() == relative.name.casefold() and p.name != relative.name]
                    if aliases:
                        # Case-insensitive hosts cannot represent DataURISource.h
                        # and DataUriSource.h at the same location. Preserve both
                        # byte streams and record the reversible storage mapping.
                        stored = relative.with_name(relative.stem + '.aosp-case-' +
                            digest(str(relative).encode())[:12] + relative.suffix)
                save(dest / stored, data)
                files[str(relative)] = {'path':str(relative), 'stored_path':str(stored),
                                       'sha256':digest(data), 'bytes':len(data)}
    # Preserve link targets as metadata rather than creating potentially dangling
    # or escaping filesystem links. Actual definitions are archived separately.
    manifest = {'status':'complete','api':api,'tag':tag,'repository':repo,
                'commit':commit,'tree':info['tree'],
                'source_url':BASE+repo+'/+/'+commit,
                'captured_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),
                'archives':archives,'files':sorted(files.values(),key=lambda x:x['path']),
                'links':links}
    manifest_path.parent.mkdir(parents=True,exist_ok=True)
    manifest_path.write_text(json.dumps(manifest,indent=2)+'\n')
    return api,repo,len(files),'complete'

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=pathlib.Path,required=True)
    parser.add_argument('--api',type=int,action='append',help='default: all API 23 through 37')
    parser.add_argument('--jobs',type=int,default=2)
    args=parser.parse_args()
    if args.jobs < 1 or args.jobs > 12: parser.error('--jobs must be between 1 and 12')
    if args.api and set(args.api)-{api for api,_ in RELEASES}: parser.error('unsupported API')
    global CACHE
    args.output.mkdir(parents=True,exist_ok=True)
    CACHE = args.output.parent / 'http-cache'
    CACHE.mkdir(exist_ok=True)
    tasks=[(args.output,api,version,repo,roots) for api,version in RELEASES
           if not args.api or api in args.api for repo,roots in ROOTS.items()]
    results=[]; errors=[]
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures={pool.submit(fetch_repo,*task): task for task in tasks}
        for future in concurrent.futures.as_completed(futures):
            task=futures[future]
            try:
                api,repo,count,status=future.result()
                results.append({'api':api,'repository':repo,'files':count,'status':status})
                print(api,repo,count,status,flush=True)
            except Exception as error:
                errors.append({'api':task[1],'repository':task[3],'error':repr(error), 'url':getattr(error,'url',None)})
                print('ERROR',task[1],task[3],repr(error),flush=True)
    index={'scope':ROOTS,'suffixes':sorted(SUFFIXES),'licenses':sorted(LICENSES),
           'releases':[{'api':api,'tag':'android-'+version+'_r1'} for api,version in RELEASES
                       if not args.api or api in args.api],
           'results':sorted(results,key=lambda x:(x['api'],x['repository'])), 'errors':errors}
    (args.output/'INDEX.json').write_text(json.dumps(index,indent=2)+'\n')
    return int(bool(errors))
if __name__ == '__main__':
    raise SystemExit(main())
