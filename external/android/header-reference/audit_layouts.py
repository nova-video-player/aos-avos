#!/usr/bin/env python3
"""Measure extracted C ABI declarations; this does not compile complete AOSP headers."""
import argparse, json, pathlib, re, subprocess

RECORDS=[('native_handle','native_handle_t'),('android_native_base_t','android_native_base_t'),
 ('android_native_rect_t','android_native_rect_t'),('ANativeWindowBuffer','ANativeWindowBuffer_t'),
 ('ANativeWindow','ANativeWindow'),('hw_module_t','hw_module_t'),('gralloc_module_t','gralloc_module_t')]
FIELDS={
 'native_handle_t':['version','numFds','numInts','data'],
 'android_native_base_t':['magic','version','reserved','incRef','decRef'],
 'android_native_rect_t':['left','top','right','bottom'],
 'ANativeWindowBuffer_t':['common','width','height','stride','format','handle'],
 'ANativeWindow':['common','query','perform','dequeueBuffer_DEPRECATED','lockBuffer_DEPRECATED',
                  'queueBuffer_DEPRECATED','cancelBuffer_DEPRECATED','dequeueBuffer','queueBuffer','cancelBuffer'],
 'hw_module_t':['tag','module_api_version','hal_api_version','id','methods','dso'],
 'gralloc_module_t':['common','registerBuffer','unregisterBuffer','lock','unlock'],
}
TARGETS={'armeabi-v7a':'armv7a-linux-androideabi23','arm64-v8a':'aarch64-linux-android23',
         'x86':'i686-linux-android23','x86_64':'x86_64-linux-android23'}

def strip_comments(text):
    return re.sub(r'/\*.*?\*/|//[^\n]*','',text,flags=re.S)

def record(text,name,alias):
    match=re.search(r'(?m)^(typedef\s+)?struct\s+'+name+r'\s*\{',text)
    if not match: return None
    start=match.end()-1;depth=1;end=start+1
    while depth:
        if text[end]=='{':depth+=1
        elif text[end]=='}':depth-=1
        end+=1
    semi=text.index(';',end)+1
    result=text[match.start():semi]
    if not match.group(1): result+='\ntypedef struct '+name+' '+alias+';'
    return result

def collect(root):
    # Only the relevant families, excluding unrelated structs with similar names.
    candidates=[]
    for name in ['window.h','window-deprecated.h','nativebase.h','native_handle.h','hardware.h','gralloc.h']:
        candidates.extend(p for p in root.rglob(name) if
                          ('/system/' in str(p) or '/nativebase/' in str(p) or
                           '/hardware/libhardware/' in str(p) or '/cutils/' in str(p)))
    definitions={}; origins={}
    for name,alias in RECORDS:
        choices=[]
        for p in sorted(candidates):
            code=record(strip_comments(p.read_text(errors='replace')),name,alias)
            if code: choices.append((p,code))
        if not choices: raise RuntimeError('missing record '+name+' under '+str(root))
        # Forwarding aliases are recorded separately in SNAPSHOT.json, not copied.
        distinct={code for _,code in choices}
        if len(distinct)!=1: raise RuntimeError('ambiguous record '+name+': '+str([str(p) for p,_ in choices]))
        definitions[alias]=choices[0][1];origins[alias]=str(choices[0][0].relative_to(root))
    return definitions,origins

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--snapshots',type=pathlib.Path,required=True)
    ap.add_argument('--baseline',type=pathlib.Path,required=True)
    ap.add_argument('--ndk',type=pathlib.Path,required=True)
    ap.add_argument('--output',type=pathlib.Path,required=True)
    args=ap.parse_args();args.output.mkdir(parents=True,exist_ok=True)
    clang=args.ndk/'toolchains/llvm/prebuilt/darwin-x86_64/bin/clang'
    sources=[('vendored-21',args.baseline)]+[(p.name,p) for p in sorted(args.snapshots.glob('api-*'))]
    results={};errors=[]
    for version,root in sources:
        try: defs,origins=collect(root)
        except RuntimeError as e: errors.append(str(e));continue
        prelude='''#include <stddef.h>
#include <stdint.h>
struct android_ycbcr;
struct android_flex_layout;
struct hw_module_methods_t;
'''
        body=prelude
        for name,alias in RECORDS:
            body+=defs[alias]+'\n'
            if alias=='native_handle_t':body+='typedef const native_handle_t *buffer_handle_t;\n'
        labels=[];exprs=[]
        for alias,fields in FIELDS.items():
            labels.append(alias+'.sizeof');exprs.append('sizeof('+alias+')')
            for field in fields:
                labels.append(alias+'.'+field);exprs.append('offsetof('+alias+','+field+')')
        # Assert the callback signatures used by AVOS, not only their offsets.
        body+='''typedef int (*expected_query)(const ANativeWindow*, int, int*);
typedef int (*expected_perform)(ANativeWindow*, int, ...);
typedef int (*expected_dequeue)(ANativeWindow*, struct ANativeWindowBuffer**);
typedef int (*expected_buffer_op)(ANativeWindow*, struct ANativeWindowBuffer*);
typedef int (*expected_gralloc_lock)(const struct gralloc_module_t*, buffer_handle_t, int, int, int, int, int, void**);
typedef int (*expected_gralloc_unlock)(const struct gralloc_module_t*, buffer_handle_t);
#define CHECK_TYPE(T,F,E) _Static_assert(__builtin_types_compatible_p(__typeof__(((T*)0)->F),E), #T "." #F)
CHECK_TYPE(ANativeWindow,query,expected_query);
CHECK_TYPE(ANativeWindow,perform,expected_perform);
CHECK_TYPE(ANativeWindow,dequeueBuffer_DEPRECATED,expected_dequeue);
CHECK_TYPE(ANativeWindow,lockBuffer_DEPRECATED,expected_buffer_op);
CHECK_TYPE(ANativeWindow,queueBuffer_DEPRECATED,expected_buffer_op);
CHECK_TYPE(ANativeWindow,cancelBuffer_DEPRECATED,expected_buffer_op);
CHECK_TYPE(gralloc_module_t,lock,expected_gralloc_lock);
CHECK_TYPE(gralloc_module_t,unlock,expected_gralloc_unlock);
'''
        body+='const unsigned long long audit_values[] = {\n'+',\n'.join(exprs)+'\n};\n'
        cfile=args.output/(version+'.c');cfile.write_text(body)
        results[version]={'origins':origins,'abis':{}}
        for abi,target in TARGETS.items():
            command=[str(clang),'--target='+target,'-std=gnu11','-Werror','-S','-emit-llvm',str(cfile),'-o','-']
            result=subprocess.run(command,text=True,capture_output=True)
            if result.returncode:
                (args.output/(version+'-'+abi+'.error.txt')).write_text(result.stderr)
                errors.append(version+' '+abi+': '+result.stderr)
                continue
            m=re.search(r'@audit_values = .*?\[(?:\d+) x i64\] \[(.*?)\]',result.stdout)
            if not m:raise RuntimeError('LLVM array missing')
            vals=[int(x) for x in re.findall(r'i64 (\d+)',m.group(1))]
            assert len(vals)==len(labels)
            results[version]['abis'][abi]=dict(zip(labels,vals))
    baseline=results.get('vendored-21',{}).get('abis',{})
    differences=[]
    for version,data in results.items():
        for abi,values in data['abis'].items():
            for field,value in values.items():
                if value!=baseline[abi][field]: differences.append({'version':version,'abi':abi,'field':field,'baseline':baseline[abi][field],'value':value})
    report={'method':'C declarations extracted from snapshots; NDK clang target layouts and signature assertions. Not a full AOSP header build or device test.',
            'results':results,'differences':differences,'errors':errors}
    (args.output/'layouts.json').write_text(json.dumps(report,indent=2)+'\n')
    print('versions',len(results),'ABI measurements',sum(len(r['abis']) for r in results.values()),'differences',len(differences),'errors',len(errors))
    for error in errors[:4]:print(error[:1000])
    return bool(errors)
if __name__=='__main__':raise SystemExit(main())
