#!/usr/bin/env python3
"""Record and compare the private inline helpers called by AVOS."""
import argparse,json,pathlib,re
from audit_layouts import strip_comments
NAMES=['native_window_set_usage','native_window_set_buffers_format',
 'native_window_set_buffers_user_dimensions','native_window_set_scaling_mode',
 'native_window_set_buffer_count','native_window_api_connect','native_window_api_disconnect',
 'native_window_set_crop','native_window_set_buffers_transform']

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--snapshots',required=True,type=pathlib.Path)
    p.add_argument('--baseline',required=True,type=pathlib.Path)
    p.add_argument('--output',required=True,type=pathlib.Path)
    a=p.parse_args();results={};errors=[]
    for version,root in [('vendored-21',a.baseline)]+[(p.name,p) for p in sorted(a.snapshots.glob('api-*'))]:
        results[version]={}
        candidates=list(root.rglob('window.h'))+list(root.rglob('window-deprecated.h'))
        for name in NAMES:
            found=[]
            for path in candidates:
                text=strip_comments(path.read_text(errors='replace'))
                for m in re.finditer(r'static\s+inline\s+int\s+'+name+r'\s*\([^;{}]*\)\s*\{',text):
                    start=m.end();depth=1;end=start
                    while depth:
                        if text[end]=='{':depth+=1
                        elif text[end]=='}':depth-=1
                        end+=1
                    found.append((str(path.relative_to(root)),text[m.start():end]))
            definitions={re.sub(r'\s+','',body) for _,body in found}
            if len(definitions)!=1:errors.append([version,name,'missing or differing definitions'])
            else:results[version][name]={'origins':[path for path,_ in found],'definition':found[0][1]}
    differences=[]
    for version,items in results.items():
        for name,item in items.items():
            if re.sub(r'\s+','',item['definition'])!=re.sub(r'\s+','',results['vendored-21'][name]['definition']):
                differences.append({'version':version,'helper':name})
    a.output.write_text(json.dumps({'results':results,'differences':differences,'errors':errors},indent=2)+'\n')
    print('versions',len(results),'helper differences',len(differences),'errors',errors)
    return bool(errors)
if __name__=='__main__':raise SystemExit(main())
