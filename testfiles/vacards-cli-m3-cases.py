#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Independent M3 fixtures, wire outcomes and native oracle obligations.

Authoring never reads implementation output. --run uses the existing session
transport; unavailable commands and absent owner receipts are failures, not skips.
"""
import argparse
import base64
import copy
import hashlib
import gzip
import importlib.util
import json
import math
import os
from pathlib import Path
import shutil
import struct
import sys
import time
import xml.etree.ElementTree as ET
import zlib

ROOT = Path(__file__).resolve().parent
CASES = ROOT / 'cli_tests/vacards-agent-session'
FIXTURES = ROOT / 'cli_tests/vacards-agent/fixtures/m3'
MANIFEST = ROOT.parent / 'doc/vacards/cli/m3-outcome-manifest.json'
COMMANDS = ('selection.set selection.clear history.query history.undo history.redo '
            'geometry.move geometry.resize geometry.rotate geometry.skew geometry.flip geometry.matrix '
            'geometry.boolean geometry.offset geometry.corners bitmap.tone-query bitmap.histogram '
            'bitmap.tone-set bitmap.copy bitmap.explode.analyze bitmap.explode.contour '
            'bitmap.explode.explode bitmap.explode.create-contour-only bitmap.explode.apply-adjustment '
            'clip.set clip.destructive clip.release nest.contour-set nest.contour-release nest.analyze '
            'nest.solve nest.apply').split()
GROUPS = ('selection', 'history', 'geometry', 'bitmap', 'explode', 'clip', 'nest', 'workflow')
CELLS = ('success', 'invalid', 'boundary', 'refusal', 'noop', 'dry-run', 'cancel', 'preservation', 'enums')
CHANNELS = ('brightness', 'contrast', 'intensity', 'highlights', 'shadows', 'midtones')


def module(name):
    spec = importlib.util.spec_from_file_location(name.replace('-', '_'), ROOT / (name + '.py'))
    mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
    return mod


def atomic(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + '.p9-tmp')
    temporary.write_text(json.dumps(value, indent=2) + '\n' if not isinstance(value, str) else value)
    temporary.replace(path)


def sha(value): return hashlib.sha256(value).hexdigest()
def ref(path): return {'$ref': path}
def length(n, unit='px'): return {'value': n, 'unit': unit}
def group(cid): return 'explode' if cid.startswith('bitmap.explode.') else cid.split('.')[0]
def owner(cid): return 'P5' if group(cid) in ('selection','history','geometry') else 'P6' if group(cid) in ('bitmap','explode') else 'P7'

def native_target(cid):
    return 'test_vacards-cli-' + ('selection' if group(cid) == 'history' else 'bitmap' if group(cid) == 'explode' else group(cid))


def png(width, height, rgba):
    def chunk(kind, data): return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
    scan = b''.join(b'\0' + rgba[y*width*4:(y+1)*width*4] for y in range(height))
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width,height,8,6,0,0,0)) + chunk(b'IDAT',zlib.compress(scan)) + chunk(b'IEND',b'')


def fixtures():
    # Three separated 20x20 opaque white islands: analytic count, alpha and pixels.
    width,height = 120,80
    rgba = bytes(v for y in range(height) for x in range(width)
                 for v in ((255,255,255,255) if any(a <= x < a+20 and 20 <= y < 40 for a in (10,50,90)) else (0,0,0,0)))
    payload = png(width,height,rgba)
    FIXTURES.mkdir(parents=True, exist_ok=True)
    temp = FIXTURES/'rgba.png.p9-tmp'; temp.write_bytes(payload); temp.replace(FIXTURES/'rgba.png')
    image = '<image id="image1" x="0" y="0" width="120" height="80" xlink:href="data:image/png;base64,' + base64.b64encode(payload).decode() + '"/>'
    body = ('<path id="shape1" d="M 0,0 H 10 V 10 H 0 Z" fill="#ff0000"/>'
            '<path id="shape2" d="M 5,0 H 15 V 10 H 5 Z" fill="#00ff00"/>'
            '<g id="group1" transform="translate(30,0)"><rect id="child1" width="4" height="4"/>'
            '<rect id="child2" x="6" width="4" height="4"/></g>'
            '<rect id="untouched" x="150" y="120" width="7" height="9" fill="#123456"/>'
            '<rect id="cutter1" x="0" y="0" width="60" height="80"/>'
            '<path id="contour1" d="M 0,0 H 10 V 10 H 0 Z"/>'
            '<use id="broken-clone" xlink:href="#missing-source"/>'
            '<use id="cutter-reference" xlink:href="#cutter1"/>')
    header = '<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="200" height="200" viewBox="0 0 200 200">'
    for name in ('selection','geometry','tone','explode','clip','nest','sheet'):
        atomic(FIXTURES/(name+'.svg'), header + body + image + '</svg>\n')
    atomic(FIXTURES/'rotated-original.svg', header + body + '<g transform="rotate(30 60 40)">'+image+'</g></svg>\n')
    expected = {'provenance':'Analytic source primitives and literal row-major RGBA; no production result consumed',
                'shape1_bounds':[0,0,10,10], 'shape2_bounds':[5,0,15,10],
                'boolean_areas':{'union':150,'intersection':50,'difference':50,'xor':100,'division':100},
                'islands':3, 'opaque_pixels':1200, 'size':[width,height],
                'pixel_sha256':sha(struct.pack('<II',width,height)+rgba),
                'alpha_sha256':sha(struct.pack('<II',width,height)+rgba[3::4]), 'profile_sha256':sha(b''),
                'nest_single_copy_utilization_percent':100*100/(200*200)}
    atomic(FIXTURES/'expected.json', expected)
    files = [p for p in sorted(FIXTURES.iterdir()) if p.name != 'manifest.json' and p.suffix in ('.svg','.png','.json','.icc')]
    atomic(FIXTURES/'manifest.json', {'schema':'p9-m3-fixtures/1','fixtures':[{'path':p.name,'sha256':sha(p.read_bytes())} for p in files]})


def params(cid):
    # Explicit author decisions. Do not copy a catalog example or handler result.
    simple = {
        'selection.set':{'ids':['group1','child1','shape1']}, 'selection.clear':{},
        'history.query':{}, 'history.undo':{}, 'history.redo':{},
        'geometry.move':{'ids':['shape1'],'dx':length(2),'dy':length(3)},
        'geometry.resize':{'ids':['shape1'],'width':length(20),'height':length(30),'bbox':'geometric','anchor':'nw'},
        'geometry.rotate':{'ids':['shape1'],'angle':90,'pivot':{'x':length(0),'y':length(0)}},
        'geometry.skew':{'ids':['shape1'],'axis':'x','angle':45,'pivot':{'x':length(0),'y':length(0)}},
        'geometry.flip':{'ids':['shape1'],'axis':'horizontal','pivot':{'x':length(0),'y':length(0)}},
        'geometry.matrix':{'ids':['shape1'],'matrix':[1,0,0,1,2,3]},
        'geometry.boolean':{'ids':['shape1','shape2'],'op':'difference','empty-result':'reject'},
        'geometry.offset':{'ids':['shape1'],'distance':length(2),'corner':'miter'},
        'geometry.corners':{'ids':['child1'],'radius':length(1),'scope':'all','mode':'round'},
        'bitmap.tone-query':{'ids':['image1'],'mode':'compatible-members'},
        'bitmap.histogram':{'ids':['image1'],'channel':'luminance','bins':256,'remap':'none'},
        'bitmap.tone-set':{'ids':['image1'],'patch':{'brightness':10}},
        'bitmap.copy':{'ids':['shape1'],'size':{'width':10,'height':10},'bbox':'geometric','background':[0,0,0,0],'replace':False},
        'bitmap.explode.analyze':{'id':'image1','recipe':{'refine':False}},
        'bitmap.explode.contour':{'analysis-token':ref('prepare.data.analysis-token'),'contour':{'offset':length(0),'gap-tolerance':length(0)}},
        'bitmap.explode.explode':{'id':'image1','recipe':{'refine':False}},
        'bitmap.explode.create-contour-only':{'id':'image1','recipe':{'refine':False},'contour':{'offset':length(0),'gap-tolerance':length(0)}},
        'bitmap.explode.apply-adjustment':{'id':'image1','recipe':{'refine':False}},
        'clip.set':{'target-id':'shape1','cutter-id':'cutter1','keep-cutter':True},
        'clip.destructive':{'target-id':'image1','cutter-id':'cutter1','keep-cutter':True},
        'clip.release':{'ids':['shape1'],'keep-cutter':True},
        'nest.contour-set':{'payload-id':'shape1','contour-id':'contour1'},
        'nest.contour-release':{'ids':[ref('prepare.data.binding-ids.0')]},
        'nest.analyze':{'ids':['shape1'],'page':1,'gap':length(0),'margin':length(0),'rotations':{'mode':'none'},'fallback':'reject'},
        'nest.solve':{'analysis-token':ref('prepare.data.analysis-token'),'iterations':17,'seed':18446744073709551615,'engine':'native'},
        'nest.apply':{'analyze':{'ids':['shape1'],'page':1,'gap':length(0),'margin':length(0)},'solve':{'iterations':17,'seed':0},'partial':'reject'},
    }
    return copy.deepcopy(simple[cid])


def step(cid, p=None, ident='subject', dry=False, status=None):
    status = status or ('ok' if cid in ('history.query','bitmap.tone-query','bitmap.histogram','bitmap.explode.analyze','bitmap.explode.contour','nest.analyze','nest.solve') or dry else 'unchanged' if cid=='bitmap.explode.apply-adjustment' else 'changed')
    req={'schema':'va-studio.cli-request/1','id':ident,'command':cid,'params':params(cid) if p is None else p,
         'document':ref('open.document_id'),'if_revision':ref('before.data.session_revision' if cid.startswith('selection.') else 'before.revision_after')}
    if dry: req['dry_run']=True
    return {'request':req, 'm3':True, 'expect':{'status':status,'data':{'variant':'computed-dry-run' if dry else 'unchanged' if status=='unchanged' else 'success'}}, 'expect_absent':['error']}


def status_step(ident='before'):
    return {'request':{'schema':'va-studio.cli-request/1','id':ident,'command':'session.status','params':{}},'expect':{'status':'ok'}}


def prepare(cid):
    seq=[]
    if cid == 'selection.clear': seq=[step('selection.set',{'ids':['shape1']},'prepare')]
    elif cid in ('history.undo','history.redo'):
        seq=[step('geometry.move',ident='edit')]
        if cid == 'history.redo':
            undo=step('history.undo',ident='undo');undo['request']['if_revision']=ref('mid.revision_after')
            seq += [status_step('mid'),undo]
    elif cid == 'bitmap.explode.contour': seq=[step('bitmap.explode.analyze',ident='prepare')]
    elif cid == 'nest.solve': seq=[step('nest.analyze',ident='prepare')]
    elif cid == 'clip.release': seq=[step('clip.set',ident='prepare')]
    elif cid == 'nest.contour-release': seq=[step('nest.contour-set',ident='prepare')]
    return [status_step()] + seq + ([status_step('ready')] if seq else [])


def destructive_published_bounds():
    # Independent pixel-support oracle, before any implementation execution.
    # rgba.png is authored at 120x80 with opaque unit pixel cells in
    # [10,30)x[20,40), [50,70)x[20,40), [90,110)x[20,40).
    # clip.svg maps those pixels 1:1 to document CSS px. Its cutter is
    # [0,60)x[0,80). Intersection leaves [10,30)x[20,40) and
    # [50,60)x[20,40); trimming transparent borders gives endpoint bounds
    # [min x, min y, max(x+1), max(y+1)] = [10,20,60,40], NOT width/height.
    support = [(x, y) for y in range(80) for x in range(120)
               if any(a <= x < a + 20 and 20 <= y < 40 for a in (10, 50, 90))
               and 0 <= x < 60 and 0 <= y < 80]
    return [min(x for x, y in support), min(y for x, y in support),
            max(x + 1 for x, y in support), max(y + 1 for x, y in support)]


def assertions(cid):
    emptytone={key:0 for key in CHANNELS}
    return {
        'selection.set':{'normalized-ids':['group1','shape1'],'covered-ids':['child1'],'selection-after':['group1','shape1']},
        'selection.clear':{'selection-after':[],'cleared-count':1},
        'history.query':{'can-undo':False,'can-redo':False,'next-undo-label':None,'next-redo-label':None},
        'history.undo':{'can-undo':False,'can-redo':True},'history.redo':{'can-undo':True,'can-redo':False},
        'geometry.move':{'applied-affine':[1,0,0,1,2,3],'bounds-before':[0,0,10,10],'bounds-after':[2,3,12,13]},
        'geometry.matrix':{'applied-affine':[1,0,0,1,2,3],'bounds-after':[2,3,12,13]},
        'geometry.resize':{'bounds-before':[0,0,10,10],'bounds-after':[0,0,20,30]},
        'geometry.rotate':{'bounds-after':[-10,0,0,10]},'geometry.skew':{'bounds-after':[0,0,20,10]},
        'geometry.flip':{'bounds-after':[-10,0,0,10]},
        'geometry.boolean':{'paths':[{'bounds':[0,0,5,10]}],'consumed-ids':['shape1','shape2']},
        'geometry.offset':{'paths':[{'bounds':[-2,-2,12,12]}]},
        'geometry.corners':{'paths':[{'bounds':[30,0,34,4]}]},
        'bitmap.tone-query':{'total-count':1,'excluded-count':0,'next-cursor':None,'mixed':{k:False for k in CHANNELS},'members':[{'id':'image1','values':emptytone}]},
        'bitmap.histogram':{'width':120,'height':80,'remapped':False},
        'bitmap.tone-set':{'changed-count':1,'unchanged-count':0,'excluded-count':0,'total-count':1,'members':[{'id':'image1','before':emptytone,'after':dict(emptytone,brightness=10)}]},
        'bitmap.copy':{'width':10,'height':10,'dpi-x':96,'dpi-y':96,'source-retained':True,'pixel-sha256':sha(struct.pack('<II',10,10)+bytes([255,0,0,255])*100),'profile-sha256':sha(b'')},
        'bitmap.explode.analyze':{'piece-count':3,'pieces':[{'index':i,'pixel-count':400,'alpha-sum':102000} for i in range(3)]},
        'bitmap.explode.contour':{'no-contour-count':0,'pieces':[{'index':i,'noContour':False,'topology-count':1} for i in range(3)]},
        'bitmap.explode.create-contour-only':{'source-retained':True},
        'bitmap.explode.apply-adjustment':{'source-retained':True},'bitmap.explode.explode':{'piece-count':3},
        'clip.set':{'relations':[{'target-id':'shape1','cutter-id':'cutter1','target-retained':True,'cutter-retained':True}]},
        'clip.release':{'relations':[{'target-id':'shape1','target-retained':True,'cutter-retained':True}]},
        'clip.destructive':{'cutter-retained':True,'straightened':False,'bounds':destructive_published_bounds()},
        'nest.contour-set':{'payload-ids':['shape1'],'contour-ids':['contour1']},
        'nest.contour-release':{'payload-ids':['shape1'],'contour-ids':['contour1']},
        'nest.analyze':{'sheet-bounds':[0,0,200,200],'requested-copies':[{'id':'shape1','count':1}]},
        'nest.solve':{'engine':'native','iterations':17,'stop-reason':'work-limit','deterministic':True,'unplaced':[],'utilization':0.25,'placements':[{'id':'shape1','copy':0}]},
        'nest.apply':{'unplaced':[],'source-copy':[{'source':'shape1','copy':0,'output-id':'shape1'}]},
    }.get(cid,{})


def case(cid, cell, suffix='', p=None):
    seq=prepare(cid)
    subject=step(cid,p,dry=cell in ('dry-run','preservation'))
    if len(seq)>1:
        subject['request']['if_revision']=ref('ready.data.session_revision' if cid.startswith('selection.') else 'ready.revision_after')
    if cell == 'success': subject['expect']['data'].update(assertions(cid))
    if cell in ('invalid','boundary'):
        subject['request']['params']['p9-unknown-property']=True
        if cell=='boundary': subject['request']['params']=[]
        subject.update(expect={'status':'rejected','undo_effect':'none'}, expect_error='invalid-argument',expect_accepted=False)
        subject.pop('expect_absent',None)
    if cell=='refusal':
        subject['request']['document']='p9-stale-document'
        subject.update(expect={'status':'rejected','undo_effect':'none'},expect_error='stale-document')
        subject.pop('expect_absent',None)
    if cell == 'preservation':
        baseline='ready' if len(seq)>1 else 'before'
        history=step('history.query',ident='history_before')
        history['request']['if_revision']=ref(baseline+'.revision_after')
        selection={'request':{'schema':'va-studio.cli-request/1','id':'selection_before','command':'query.selection','params':{}},'expect':{'status':'ok'}}
        seq += [history,selection]
    seq.append(subject)
    if cell == 'preservation':
        after=status_step('after')
        after['expect_exact']={key:ref(baseline+'.'+key) for key in ('revision_after','data.session_revision','data.retention')}
        history=step('history.query',ident='history_after');history['request']['if_revision']=ref('after.revision_after')
        history['expect_exact']={'data':ref('history_before.data')}
        selection={'request':{'schema':'va-studio.cli-request/1','id':'selection_after','command':'query.selection','params':{}},'expect':{'status':'ok'},'expect_exact':{'data':ref('selection_before.data')}}
        seq += [after,history,selection]
    fixture=('tone' if group(cid)=='bitmap' else group(cid) if group(cid) in ('selection','geometry','explode','clip','nest') else 'selection')
    opened={'request':{'schema':'va-studio.cli-request/1','id':'open','command':'file.open','if_revision':0,
        'params':{'path':'{FIXTURES}/m3/'+fixture+'.svg','format':'svg','resource-policy':'embed','font-policy':'reject','discard':False}},
        'expect':{'status':'changed'}}
    seq.insert(0,opened)
    ident=cid+':'+cell+(':'+suffix if suffix else '')
    return {'id':ident,'command':cid,'cell':cell,'description':'Independent P9 '+ident,
            'startup_args':['--grant-read','{FIXTURES}/m3'],
            'timeout':30,'requests':seq,'expect_exit':0}


def native_oracle(cid):
    # Independently authored obligation; the owner must supply a genuine executable case.
    return {'id':cid+':native-preservation','command':cid,'owner':owner(cid),'target':native_target(cid),
            'case':'P9Independent.'+cid.replace('.','_').replace('-','_'),
            'fixture':'fixtures/m3/'+('tone' if group(cid)=='bitmap' else 'selection' if group(cid)=='history' else group(cid))+'.svg',
            'operation_mode':'see frozen per-command target metadata',
            'assertions':assertions(cid),
            'required_observations':['canonical XML before/after','ordered live selection','document revision','session revision',
                'Undo and Redo snapshots including labels','retained token identities and unique held bytes','settled native status',
                'unrelated untouched object exact attributes'],
            'preview_cancel':'Compare all observations exactly; no live edit followed by Undo for preview; tokens retained.',
            'commit':'Document edit settles once; one Undo restores XML/selection and one Redo restores committed output; failed write rolls back before return.',
            'status':'required-unimplemented','receipt':None}


def author():
    fixtures()
    bundles={(g,c):[] for g in GROUPS for c in CELLS}
    for cid in COMMANDS:
        for cell in ('success','invalid','boundary','refusal','dry-run','preservation'):
            bundles[group(cid),cell].append(case(cid,cell))
    # Boundary probes with independently specified numeric limits, not schema-derived expected data.
    probes=[('geometry.move',{'dx':length(1000001)}),('geometry.boolean',{'ids':['shape1','shape2','contour1'],'op':'division'}),
            ('bitmap.copy',{'size':{'width':0,'height':10}}),('bitmap.explode.analyze',{'recipe':{'faint-floor':26}}),
            ('nest.solve',{'seed':18446744073709551616}),('nest.solve',{'iterations':0})]
    for index,(cid,patch) in enumerate(probes):
        obj=case(cid,'boundary',str(index)); obj['requests'][-1]['request']['params']=dict(params(cid),**patch)
        bundles[group(cid),'boundary'].append(obj)
    # Explicit no-ops (history empty refuses and is not a successful no-op).
    for cid,p in [('selection.set',{'ids':[]}),('selection.clear',{}),('geometry.move',{'ids':['shape1'],'dx':length(0),'dy':length(0)}),
                  ('geometry.matrix',{'ids':['shape1'],'matrix':[1,0,0,1,0,0]}),('bitmap.tone-set',{'ids':['image1'],'patch':{'brightness':0}}),
                  ('bitmap.explode.apply-adjustment',params('bitmap.explode.apply-adjustment'))]:
        obj=case(cid,'noop',p=p);obj['requests']=[obj['requests'][0],status_step(),step(cid,p,status='unchanged')]
        obj['requests'][-1]['expect']['undo_effect']='none';bundles[group(cid),'noop'].append(obj)
    for cid in ('history.undo','history.redo'):
        obj=case(cid,'noop');obj['requests']=[obj['requests'][0],status_step(),step(cid)]
        obj['requests'][-1].update(expect={'status':'rejected','undo_effect':'none'},expect_error='history-empty')
        obj['requests'][-1].pop('expect_absent',None);bundles['history','noop'].append(obj)
    # Parameter policies are explicit. Each enum case executes, schema membership alone is never coverage.
    enums={
      'geometry.boolean':{'op':['union','intersection','difference','xor','division'],'empty-result':['reject','allow']},
      'geometry.resize':{'anchor':['nw','n','ne','w','center','e','sw','s','se'],'bbox':['visual','geometric']},
      'geometry.rotate':{'anchor':['nw','n','ne','w','center','e','sw','s','se'],'bbox':['visual','geometric']},
      'geometry.skew':{'axis':['x','y'],'anchor':['nw','n','ne','w','center','e','sw','s','se'],'bbox':['visual','geometric']},
      'geometry.flip':{'axis':['horizontal','vertical'],'anchor':['nw','n','ne','w','center','e','sw','s','se'],'bbox':['visual','geometric']},
      'geometry.offset':{'direction':['outward','inward','both'],'corner':['round','bevel','miter']},
      'geometry.corners':{'mode':['round','inverse-round'],'scope':['all','nodes']},
      'bitmap.tone-query':{'mode':['compatible-members','legacy-roots']},
      'bitmap.histogram':{'channel':['luminance'],'bins':[256],'remap':['none','current']},
      'bitmap.copy':{'bbox':['visual','geometric']},'nest.analyze':{'fallback':['reject','conservative-hull','conservative-bounds']},
      'nest.solve':{'engine':['native']},'nest.apply':{'partial':['reject','allow']}}
    for cid,fields in enums.items():
        for field,values in fields.items():
            for value in values:
                p=params(cid);p[field]=value
                if field=='anchor':p.pop('pivot',None)
                if field=='scope' and value=='nodes':p['nodes']=['0:0']
                obj=case(cid,'enums',field+'='+str(value),p);obj['enum']={'path':'params.'+field,'value':value}
                bundles[group(cid),'enums'].append(obj)
    # All six length units at every nested length route, with independently
    # chosen physical sizes. Change inputs only, never derive expected output.
    factors={'px':1,'mm':96/25.4,'cm':96/2.54,'in':96,'pt':96/72,'pc':16}
    unit_commands=['geometry.move','geometry.resize','geometry.rotate','geometry.skew','geometry.flip','geometry.offset','geometry.corners',
                   'bitmap.explode.contour','bitmap.explode.explode','bitmap.explode.create-contour-only','nest.analyze','nest.apply']
    for cid in unit_commands:
        for unit,factor in factors.items():
            p=params(cid)
            if cid=='geometry.offset':p['simplify-tolerance']=length(0.05)
            if cid in ('bitmap.explode.explode','bitmap.explode.create-contour-only'):
                p['contour']={'offset':length(0),'gap-tolerance':length(0)}
                p['contour-style']={'stroke-width':length(1)}
                if cid=='bitmap.explode.explode':p['contours']=True
            if cid in ('nest.analyze','nest.apply'):
                a=p if cid=='nest.analyze' else p['analyze'];a.pop('page',None)
                a['sheet']={'x':length(0),'y':length(0),'width':length(200),'height':length(200)}
                a['gap']=length(0);a['margin']=length(0)
            def convert(value):
                if isinstance(value,dict) and set(value)=={'value','unit'}:
                    return length(value['value']/factor,unit)
                if isinstance(value,dict):return {k:convert(v) for k,v in value.items()}
                if isinstance(value,list):return [convert(v) for v in value]
                return value
            obj=case(cid,'enums','all-lengths-'+unit,convert(p))
            bundles[group(cid),'enums'].append(obj)
    for cid in ('nest.analyze','nest.apply'):
        for mode in ('none','right-angles','discrete','free'):
            p=params(cid);a=p if cid=='nest.analyze' else p['analyze']
            a['rotations']={'mode':mode}
            if mode=='discrete':a['rotations']['step-degrees']=90
            bundles['nest','enums'].append(case(cid,'enums','rotation-'+mode,p))
        for fallback in ('reject','conservative-hull','conservative-bounds'):
            p=params(cid);a=p if cid=='nest.analyze' else p['analyze'];a['fallback']=fallback
            bundles['nest','enums'].append(case(cid,'enums','nested-fallback-'+fallback,p))
    # Paths are outside the document-only corner seam; preserve the real refusal.
    obj=case('geometry.corners','refusal','path-not-supported',dict(params('geometry.corners'),ids=['shape1']))
    st=obj['requests'][-1];st['request']['document']=ref('open.document_id');st['expect_error']='path-not-supported'
    bundles['geometry','refusal'].append(obj)
    # F2 reachable refusals, authored from the amendment; error inventory remains
    # provisional until integration publishes its regenerated contract hash.
    for name,target,keep,reason in [('broken-clone','broken-clone',True,'MissingSource'),('referenced-cutter','shape1',False,'ReferencedCutter')]:
        obj=case('clip.set','refusal',name,{'target-id':target,'cutter-id':'cutter1','keep-cutter':keep})
        st=obj['requests'][-1];st['request']['document']=ref('open.document_id')
        st['expect_error']='unsupported-target'
        st['expect']['error']={'retryable':False,'details':{'reason':'ClipDocumentService::Reason::'+reason}}
        bundles['clip','refusal'].append(obj)
    # Boolean policies and exclusive routes are distinct from string enums.
    policies={
      'geometry.resize':['keep-ratio'],
      'geometry.offset':['outer-only','delete-originals','simplify','select-results'],
      'bitmap.copy':['replace'], 'bitmap.explode.analyze':['retain'],
      'bitmap.explode.contour':['retain'], 'clip.set':['inverse','keep-cutter'],
      'clip.release':['keep-cutter'], 'nest.analyze':['retain'],'nest.solve':['retain']}
    for cid,fields in policies.items():
        for field in fields:
            for value in (False,True):
                p=params(cid);p[field]=value
                if cid=='clip.set' and field=='keep-cutter' and not value:p['cutter-id']='contour1'
                obj=case(cid,'enums',field+'='+str(value).lower(),p)
                obj['requests'][-1]['expect_exact']={'normalized_params.'+field:value}
                bundles[group(cid),'enums'].append(obj)
    for cid in ('bitmap.tone-set','bitmap.histogram'):
        for channel in CHANNELS:
            p=params(cid)
            if cid=='bitmap.tone-set':p['patch']={channel:10}
            else:p[channel]=10
            obj=case(cid,'enums','channel-'+channel,p)
            if cid=='bitmap.histogram':obj['requests'][-1]['expect']['data']['remapped']=True
            bundles['bitmap','enums'].append(obj)
    for value in (72,96,192):
        p=params('bitmap.copy');p.pop('size');p['dpi']=value
        bundles['bitmap','enums'].append(case('bitmap.copy','enums','dpi-'+str(value),p))
    for cid in ('bitmap.explode.explode','bitmap.explode.apply-adjustment','bitmap.explode.create-contour-only','nest.apply'):
        if cid=='nest.apply':
            p={'solution-token':ref('solution.data.solution-token'),'partial':'reject'}
            prep=[step('nest.analyze',ident='prepare'),step('nest.solve',ident='solution')]
        else:
            prep=[step('bitmap.explode.analyze',ident='prepare')]
            if cid=='bitmap.explode.create-contour-only':
                prep.append(step('bitmap.explode.contour',ident='contours'))
                p={'contour-token':ref('contours.data.contour-token')}
            else:p={'analysis-token':ref('prepare.data.analysis-token')}
        obj=case(cid,'enums','retained-token-route',p)
        obj['requests'][-1:-1]=prep+[status_step('retained_guard')]
        obj['requests'][-1]['request']['if_revision']=ref('retained_guard.revision_after')
        obj['requests'][-1]['expect_absent'] += ['normalized_params.'+k for k in ('recipe','analyze','solve')]
        after=status_step('token_after')
        after['expect_exact']={'data.retention.tokens':[ref('prepare.data.analysis-token')] if cid=='bitmap.explode.apply-adjustment' else []}
        obj['requests'].append(after)
        obj['route']='retained-token'
        bundles[group(cid),'enums'].append(obj)
    flow=case('geometry.move','success','undo-redo')
    flow['id']='workflow:move-undo-redo';flow['cell']='success'
    for ident,command in [('undo','history.undo'),('redo','history.redo')]:
        flow['requests'].append(status_step(ident+'_guard'))
        operation=step(command,ident=ident);operation['request']['if_revision']=ref(ident+'_guard.revision_after')
        flow['requests'].append(operation)
    bundles['workflow','success'].append(flow)
    for (g,cell),rows in bundles.items():
        # Empty cells are explicit obligations in the matrix; never empty passing cases.
        if rows: atomic(CASES/g/('m3-'+cell+'.json'),{'schema':'p9-m3-cases/1','cases':rows})
    return [native_oracle(cid) for cid in COMMANDS]


def validate_result(record, request, manifest=None):
    schema=module('vacards-cli-schema')
    manifest=manifest or json.loads(MANIFEST.read_text())
    entry=next(c for c in manifest['commands'] if c['id']==request['command'])
    status=record.get('status')
    if status not in ('ok','changed','unchanged'): return
    data=record.get('data');schema.validate(data,entry['data_variants'])
    variant='computed-dry-run' if request.get('dry_run') else 'unchanged' if status=='unchanged' else 'success'
    if data['variant']!=variant: raise ValueError('M3 status/data variant mismatch')
    if request.get('dry_run'):
        if record.get('undo_effect')!='none' or record.get('revision_before')!=record.get('revision_after'):
            raise ValueError('M3 preview changed history/revision')
    if request['command']=='bitmap.histogram':
        if len(data['counts'])!=256 or sum(data['counts'])!=data['total-samples']: raise ValueError('histogram conservation')
    if request['command']=='nest.solve':
        if not 0<=data['utilization']<=100: raise ValueError('utilization percent')
        keys=[(p['id'],p['copy']) for p in data['placements']]+[(p['id'],p['copy']) for p in data['unplaced']]
        if len(keys)!=len(set(keys)):raise ValueError('duplicate nesting source/copy')


def load_cases():
    cases=[]
    for g in GROUPS:
        for path in sorted((CASES/g).glob('m3-*.json')):
            bundle=module('vacards-cli-schema').loads(path.read_text())
            if bundle.get('schema')!='p9-m3-cases/1' or not bundle.get('cases'):raise ValueError('empty/invalid M3 bundle '+str(path))
            cases += [(path,c) for c in bundle['cases']]
    ids=[c['id'] for _,c in cases]
    if not ids or len(ids)!=len(set(ids)):raise ValueError('zero or duplicate M3 cases')
    if {c['command'] for _,c in cases}!=set(COMMANDS):raise ValueError('M3 command inventory differs from 31')
    return cases


def run(binary,evidence):
    runner=module('vacards-agent-cli-test');evidence.mkdir(parents=True,exist_ok=False)
    rows=[];start=time.monotonic()
    for path,c in load_cases():
        result=runner.typed_case(path,str(binary),'session',case_override=c)
        if result.get('skip'):result['failures'].append('required M3 case skipped')
        row={'id':c['id'],'command':c['command'],'cell':c['cell'],'passed':not result['failures'],
             'failures':result['failures'],'exit':result['returncode'],'records':result['records'],'argv':result['cmd'],'stdout':result['stdout'],'stderr':result['stderr']}
        rows.append(row)
        with gzip.open(evidence/(c['id'].replace(':','_')+'.json.gz'),'wt',encoding='utf-8') as stream:json.dump(row,stream)
        print(('PASS ' if row['passed'] else 'FAIL ')+c['id']+(': '+'; '.join(row['failures']) if row['failures'] else ''),flush=True)
        # Only our own temporary fixture/profile directory; records retained above.
        shutil.rmtree(result['work_dir'])
    summary={'schema':'p9-m3-run/1','cases':[{k:v for k,v in r.items() if k not in ('stdout','stderr','records')} for r in rows],'passed':sum(r['passed'] for r in rows),'failed':sum(not r['passed'] for r in rows),
             'skipped':0,'elapsed':time.monotonic()-start,'binary':str(binary),'binary_sha256':sha(binary.read_bytes())}
    atomic(evidence/'summary.json',summary)
    suite=ET.Element('testsuite',name='P9M3',tests=str(len(rows)),failures=str(summary['failed']),skipped='0')
    for row in rows:
        node=ET.SubElement(suite,'testcase',classname='P9M3',name=row['id'])
        if not row['passed']:ET.SubElement(node,'failure',message='; '.join(row['failures']))
    ET.ElementTree(suite).write(evidence/'junit.xml',encoding='utf-8',xml_declaration=True)
    print('M3: {passed} passed, {failed} failed, {skipped} skipped'.format(**summary))
    return int(bool(summary['failed']))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--author',action='store_true');parser.add_argument('--run',type=Path);parser.add_argument('--evidence',type=Path,required=True)
    args=parser.parse_args()
    if args.author:
        native=author();atomic(args.evidence/'native-oracles.json',{'schema':'p9-m3-native-obligations/1','cases':native})
        print('Authored',len(load_cases()),'wire cases and',len(native),'native oracle obligations');return 0
    if args.run:return run(args.run.resolve(),args.evidence)
    parser.error('choose --author or --run')

if __name__=='__main__':sys.dont_write_bytecode=True;sys.exit(main())
