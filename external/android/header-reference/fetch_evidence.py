#!/usr/bin/env python3
import base64,hashlib,json,pathlib
import dump_aosp_headers as dump
root=pathlib.Path(__file__).resolve().parent
dump.CACHE=root/'http-cache';dump.CACHE.mkdir(exist_ok=True)
files=[]
for api,tag,path in [
 (23,'android-6.0.0_r1','libs/gui/Surface.cpp'),
 (26,'android-8.0.0_r1','libs/ui/GraphicBufferMapper.cpp'),
 (37,'android-17.0.0_r1','libs/gui/Surface.cpp')]:
 repo='platform/frameworks/native'
 manifest=json.loads((root/'snapshots'/('api-%d_%s'%(api,tag))/'frameworks/native/SNAPSHOT.json').read_text())
 commit=manifest['commit'];url=dump.BASE+repo+'/+/'+commit+'/'+path+'?format=TEXT'
 data=base64.b64decode(dump.get(url),validate=True)
 dest=root/'evidence'/('api-%d'%api)/path
 dump.save(dest,data)
 files.append({'api':api,'repository':repo,'commit':commit,'path':path,'url':url,'sha256':hashlib.sha256(data).hexdigest()})
(root/'evidence/INDEX.json').write_text(json.dumps(files,indent=2)+'\n')
print('Archived',len(files),'implementation files')
