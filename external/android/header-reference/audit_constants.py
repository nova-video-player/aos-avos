#!/usr/bin/env python3
"""Resolve used enum/macro constants from archived AOSP source, without importing it."""
import argparse, ast, json, pathlib, re
from audit_layouts import strip_comments
NAMES='''NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS NATIVE_WINDOW_CONCRETE_TYPE NATIVE_WINDOW_API_MEDIA
NATIVE_WINDOW_SCALING_MODE_SCALE_TO_WINDOW NATIVE_WINDOW_SET_USAGE NATIVE_WINDOW_SET_CROP
NATIVE_WINDOW_SET_BUFFER_COUNT NATIVE_WINDOW_SET_BUFFERS_TRANSFORM NATIVE_WINDOW_SET_BUFFERS_FORMAT
NATIVE_WINDOW_SET_SCALING_MODE NATIVE_WINDOW_API_CONNECT NATIVE_WINDOW_API_DISCONNECT
NATIVE_WINDOW_SET_BUFFERS_USER_DIMENSIONS HAL_PIXEL_FORMAT_RGBA_8888 HAL_PIXEL_FORMAT_RGBX_8888
HAL_PIXEL_FORMAT_BGRA_8888 HAL_PIXEL_FORMAT_RGB_888 HAL_PIXEL_FORMAT_RGB_565 HAL_PIXEL_FORMAT_YV12
HAL_PIXEL_FORMAT_YCrCb_420_SP HAL_TRANSFORM_ROT_90 HAL_TRANSFORM_ROT_180 HAL_TRANSFORM_ROT_270
GRALLOC_USAGE_SW_READ_OFTEN GRALLOC_USAGE_SW_WRITE_OFTEN GRALLOC_USAGE_HW_RENDER
GRALLOC_USAGE_HW_TEXTURE GRALLOC_USAGE_EXTERNAL_DISP'''.split()

def resolve(root):
    candidates=[p for p in root.rglob('*.h') if p.name in
                ['window.h','window-deprecated.h','native_window.h','gralloc.h','graphics.h',
                 'graphics-base.h','graphics-base-v1.0.h','graphics-base-v1.1.h','graphics-base-v1.2.h']]
    values={}; origins={}
    for p in sorted(candidates):
        text=strip_comments(p.read_text(errors='replace')).replace('\\\n',' ')
        assignments=re.findall(r'\b([A-Z][A-Za-z0-9_]+)\s*=\s*([^,;{}\n]+)',text)
        assignments+=re.findall(r'(?m)^\s*#define\s+([A-Z][A-Za-z0-9_]+)[ \t]+([^\n]+)',text)
        for name,expression in assignments:
            values.setdefault(name,[]).append(expression.strip())
            origins.setdefault(name,[]).append(str(p.relative_to(root)))
    def number(name,seen=()):
        if name in seen:raise ValueError('cyclic '+name)
        possibilities=[]
        for expr in values.get(name,[]):
            expr=re.sub(r'\b(0[xX][0-9a-fA-F]+|\d+)[uUlL]+\b',r'\1',expr)
            tree=ast.parse(expr,mode='eval')
            def evaluate(n):
                if isinstance(n,ast.Expression):return evaluate(n.body)
                if isinstance(n,ast.Constant) and type(n.value)==int:return n.value
                if isinstance(n,ast.Name):return number(n.id,seen+(name,))
                if isinstance(n,ast.UnaryOp):
                    x=evaluate(n.operand)
                    if isinstance(n.op,ast.USub):return -x
                    if isinstance(n.op,ast.UAdd):return x
                    if isinstance(n.op,ast.Invert):return ~x
                if isinstance(n,ast.BinOp):
                    a,b=evaluate(n.left),evaluate(n.right)
                    ops={ast.BitOr:lambda:a|b,ast.BitAnd:lambda:a&b,ast.LShift:lambda:a<<b,
                         ast.RShift:lambda:a>>b,ast.Add:lambda:a+b,ast.Sub:lambda:a-b}
                    if type(n.op) in ops:return ops[type(n.op)]()
                raise ValueError('unsupported expression '+expr)
            possibilities.append(evaluate(tree))
        if not possibilities:raise ValueError('missing constant '+name)
        if len(set(possibilities))!=1:raise ValueError('conflicting constant '+name)
        return possibilities[0]
    result={};errors=[]
    for name in NAMES:
        try:result[name]={'value':number(name),'origins':origins[name]}
        except (ValueError,SyntaxError) as error:errors.append(str(error))
    return result,errors

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--snapshots',required=True,type=pathlib.Path)
    p.add_argument('--baseline',required=True,type=pathlib.Path)
    p.add_argument('--output',required=True,type=pathlib.Path)
    a=p.parse_args();report={};errors={};differences=[]
    for version,root in [('vendored-21',a.baseline)]+[(p.name,p) for p in sorted(a.snapshots.glob('api-*'))]:
        report[version],errs=resolve(root)
        if errs:errors[version]=errs
    for version,data in report.items():
        for name,item in data.items():
            if item['value']!=report['vendored-21'][name]['value']:differences.append([version,name,item['value']])
    a.output.write_text(json.dumps({'results':report,'errors':errors,'differences':differences},indent=2)+'\n')
    print('versions',len(report),'constants',len(NAMES),'differences',len(differences),'errors',errors)
    return bool(errors)
if __name__=='__main__':raise SystemExit(main())
