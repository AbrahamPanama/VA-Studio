#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Author contract cases from the plan and fixture geometry, not service output."""
import copy
import hashlib
import json
import os
from pathlib import Path
import sys

ROOT=Path(__file__).resolve().parents[1]
FIXTURES=ROOT.parents[1]/'vacards-agent/fixtures/m2'
sys.path.insert(0,str(Path(__file__).parent))
import oracles
N={'state':'not-published','persisted':False}
P={'state':'published','persisted':True}
FILES=('file.new','file.open','file.close','file.import','file.save','file.export')
LIFECYCLE=FILES[:3]
manifest=json.loads((FIXTURES/'manifest.json').read_text())
HASHES={e['path']:e['sha256'] for e in manifest['fixtures']}
ARTIFACTS=[]


def atomic_json(path, value):
    temporary=path.with_name(path.name+'.p9-tmp')
    temporary.write_text(json.dumps(value,indent=2)+'\n')
    os.replace(temporary,path)


def ref(path): return {'$ref':path}
def length(value,unit='px'): return {'value':value,'unit':unit}
def load(name='sheet.svg',fmt='svg',resource='embed',font='reject',pages=None):
    params={'path':'{WORK}/inputs/'+name,'format':fmt,'resource-policy':resource,'font-policy':font,'discard':False}
    if pages is not None: params['pages']=pages
    return params
def export(fmt='png',scope='drawing'):
    out={'path':'{WORK}/output.'+fmt,'format':fmt,'drawing':True,'dpi':96,'background':[0,0,0,0],'text-policy':'preserve','overwrite':False}
    if fmt in ('png','tiff'): out['profile']={'id':'srgb'}
    return out
def step(cid,params,ident=None,status='changed',error=None,cells=(),document=None,revision=0,dry=False):
    ident=ident or cid.split('.')[1]
    req={'schema':'va-studio.cli-request/1','id':ident,'command':cid,'params':copy.deepcopy(params),'if_revision':revision}
    if document is not None: req['document']=document
    if dry: req['dry_run']=True
    pub=P if cid in FILES[4:] and status=='changed' and not dry else N
    expected={'status':status,'publication':pub}
    if cid!='file.import' or dry or error: expected['undo_effect']='none'
    else: expected['undo_effect']='one-step'
    out={'request':req,'command':cid,'cells':list(cells),'expect':expected}
    if error:
        out['expect_error']=error
        if error in ('unknown-key','wrong-type','out-of-range','missing-required','not-a-choice','invalid-argument'): out['expect_accepted']=False
    else: out['expect_absent']=['error']
    return out
def opened(name='sheet.svg',fmt='svg',**kw):
    out=step('file.open',load(name,fmt,**kw),'open',cells=['success'])
    out['file_oracles']=[{'path':'{WORK}/inputs/'+name,'sha256':HASHES[name],'reported':True,'hash_field':'data.source_version.sha256','bytes_field':'data.source_version.bytes'}]
    out['expect_nonempty']=['document_id','data.source_version.identity','data.intake']
    return out
def live(): return ref('open.document_id')
def query(ident='query'):
    return {'request':{'schema':'va-studio.cli-request/1','id':ident,'command':'query.document','params':{}},'expect':{'status':'ok','undo_effect':'none','publication':N}}
def save(ident='save',revision=0):
    s=step('file.save',{'path':'{WORK}/saved.svg','embedding-policy':'embed','overwrite':False},ident,cells=['success','save'],document=live(),revision=revision)
    s['file_oracles']=[{'path':'{WORK}/saved.svg','format':'svg','reported':True,'embedded':True}]; return s
def raster(fmt='png',scope='drawing',ident='export',revision=0,profile=None):
    params=export(fmt); params.pop('drawing')
    if scope=='drawing': params['drawing']=True
    elif scope=='page': params['page']=1
    elif scope=='ids': params['ids']=['red']
    else: params['area']={'x':length(1),'y':length(1),'width':length(4),'height':length(4)}
    if profile: params['profile']={'id':'file','path':'{WORK}/inputs/'+profile,'sha256':HASHES[profile]}
    s=step('file.export',params,ident,cells=['success','save','group'] if scope=='ids' else ['success','save'],document=live(),revision=revision)
    size=[8,8] if scope=='ids' else [4,4] if scope=='area' else [16,8]
    samples=[{'x':2,'y':2,'rgba':[255,0,0,255]}]
    if scope in ('drawing','page'): samples.append({'x':12,'y':4,'rgba':[0,255,0,255]})
    spec={'path':params['path'],'format':fmt,'reported':True,'size':size,'samples':samples,'dpi':96}
    if fmt=='svg': spec.update(objects={'red':{'fill':'#ff0000'}},absent_ids=['green'] if scope=='ids' else [])
    if profile: spec['profile']='{WORK}/inputs/'+profile
    s['file_oracles']=[spec]; return s
def windows_symlink_policy(case):
    case['windows_symlink_skip'] = 'windows-symlink-creation-not-permitted'
    step = case['requests'][-1]
    path = step['request']['params']['path']
    step['platform_expect'] = {'win32': {
        'expect': {'status':'rejected', 'publication':N, 'undo_effect':'none'},
        'expect_error':'unsafe-destination', 'cells':['refusal'],
        'file_oracles':[{'path':path,'absent':True},
                        {'path':path.replace('/link/','/real/'),'absent':True}]}}
    return case


def write(name,steps,startup=None,**extra):
    case=dict(description='P9 independent M2 '+name,timeout=30,requests=steps,expect_exit=0,startup_args=startup if startup is not None else ['--grant-read','{WORK}','--grant-write','{WORK}'],**extra)
    if name in ('save-symlink-parent','export-symlink-parent','save-symlink-target-ungranted','export-symlink-target-ungranted'):
        windows_symlink_policy(case)
    path=ROOT/(name+'.json'); atomic_json(path,case); ARTIFACTS.append(path.name)


def main():
    # Per-command contract cells; invalid/refused/dry-run are never success enums.
    params={'file.new':{'width':length(16),'height':length(8),'discard':False},'file.open':load(),'file.close':{'discard':False},'file.import':dict(load(),position={'x':length(2),'y':length(3)}),'file.save':{'path':'{WORK}/saved.svg','overwrite':False,'embedding-policy':'embed'},'file.export':export()}
    params['file.import'].pop('discard')
    for cid in FILES:
        tail=cid.split('.')[1]; active=cid in FILES[3:]
        base=[opened()] if active else []
        args=dict(document=live() if active else None)
        invalid=copy.deepcopy(params[cid]); invalid['unknown-key']=True
        write(tail+'-invalid',base+[step(cid,invalid,'invalid',status='rejected',error='unknown-key',cells=['invalid'],**args)])
        write(tail+'-stale-revision',base+[step(cid,params[cid],'stale',status='rejected',error='stale-revision',cells=['refusal'],revision=9007199254740991,**args)])
        dry=step(cid,params[cid],'dry',status='ok',cells=['dry-run'],dry=True,**args)
        dry['expect_exact']={'dry_run':True}
        dry['file_oracles']=[{'path':'{WORK}/'+f,'absent':True} for f in ('saved.svg','output.png')]
        write(tail+'-dry-run',base+[dry])
        if cid != 'file.new':
            boundary = copy.deepcopy(params[cid])
            if cid in ('file.open','file.import'): boundary['pages']=[0]
            elif cid=='file.close': boundary['discard']='yes'
            elif cid=='file.save': boundary['overwrite']='yes'
            else: boundary['dpi']=0
            write(tail+'-boundary',base+[step(cid,boundary,'boundary',status='rejected',error='out-of-range' if cid in ('file.open','file.import','file.export') else 'wrong-type',cells=['boundary','invalid'],**args)])
        # one-shot each eligible command; startup editable document is explicit.
        shot=step(cid,params[cid],'shot',status='unchanged' if cid=='file.close' else 'changed',cells=['success'],document='d1' if active else None)
        startup=['--grant-read','{WORK}/inputs','--grant-write','{WORK}']
        if active: startup+=['--document','{WORK}/inputs/sheet.svg']
        if cid in FILES[4:]: shot['file_oracles']=[{'path':params[cid]['path'],'format':'svg' if cid=='file.save' else 'png','size':[16,8],'samples':[{'x':2,'y':4,'rgba':[255,0,0,255]}],'reported':True}]
        write('one-shot-'+tail+'-success',[shot],startup,transport='one-shot')
        shotbad=step(cid,invalid,'shot-invalid',status='rejected',error='unknown-key',cells=['invalid'],document='d1' if active else None)
        write('one-shot-'+tail+'-invalid',[shotbad],startup,transport='one-shot')
        # one-shot refusal exit remains 3, unlike a continuing session.
        data=json.loads((ROOT/('one-shot-'+tail+'-invalid.json')).read_text()); data['expect_exit']=3
        atomic_json(ROOT/('one-shot-'+tail+'-invalid.json'),data)
    for unit,factor in [('px',1),('mm',96/25.4),('in',96),('cm',96/2.54),('pt',96/72),('pc',16)]:
        new=step('file.new',{'width':length(16,unit),'height':length(8,unit),'discard':False},'new',cells=['success','boundary'])
        q=query(); q['expect_numeric']={'data.size.width.px':{'value':16*factor,'abs':1e-7},'data.size.height.px':{'value':8*factor,'abs':1e-7}}
        q['expect']['data']={'dirty':True}
        write('new-unit-'+unit,[new,q])
    for value in (0,-1,1e20):
        write('new-length-'+str(value).replace('.','_'),[step('file.new',{'width':length(value),'height':length(8)},status='rejected',error='out-of-range',cells=['boundary'])])
    for name,fmt in [('sheet.svg','svg'),('sheet.svgz','svgz'),('sheet.cdr','cdr'),('two-pages.pdf','pdf'),('rgba.png','png'),('photo.jpg','jpeg'),('rgba.tiff','tiff')]:
        for op in ('open','import'):
            pages=[1] if fmt=='pdf' else None
            s=opened(name,fmt,pages=pages) if op=='open' else step('file.import',dict({k:v for k,v in load(name,fmt,pages=pages).items() if k!='discard'},position={'x':length(0),'y':length(0)}),'import',cells=['success'],document=live())
            revision=0 if op=='open' else ref('import.revision_after')
            seq=[] if op=='open' else [opened('empty.svg')]
            seq+=[s,save(revision=revision)]
            # Reopen persisted SVG and independently query physical dimensions.
            close=step('file.close',{'discard':True},'close',revision=1,document=live(),cells=['success'])
            reopen=step('file.open',dict(load('sheet.svg'),path='{WORK}/saved.svg'),'reopen',revision=2,cells=['success'])
            seq+=[close,reopen]
            render=raster(revision=0); render['request']['document']=ref('reopen.document_id')
            spec=render['file_oracles'][0]
            if fmt=='cdr':
                w,h,_,_=oracles.png(FIXTURES/'sheet-cdr-reference.png'); spec.update(size=[w,h],reference='{WORK}/inputs/sheet-cdr-reference.png',samples=[],mean_tolerance=3)
                # Upstream CDR golden is a full A4 page, not drawing bounds.
                # Imported content lives in the 16x8 target page; explicitly
                # render the source's physical page region after insertion.
                render['request']['params'].pop('drawing')
                render['request']['params']['area']={'x':length(0),'y':length(0),'width':length(210,'mm'),'height':length(297,'mm')}
            elif fmt in ('png','tiff'): spec['samples'][1]['rgba']=[0,255,0,128]
            elif fmt=='jpeg': spec['tolerance']=4
            seq+=[render]; write(op+'-format-'+fmt,seq)
        write('open-auto-'+fmt,[opened(name,'auto',pages=[1] if fmt=='pdf' else None)])
    for fmt in ('svg','png','pdf','tiff'):
        for scope in ('page','ids','area','drawing'):
            before=query('before'); after=query('after')
            after['expect_exact']={'data':ref('before.data'),'selection_after':ref('before.selection_after'),'revision_after':ref('before.revision_after')}
            write('export-'+fmt+'-'+scope,[opened(),before,raster(fmt,scope),after])
    for fmt in ('png','tiff'):
        for profile in ('custom-rgb.icc','srgb.icc'):
            write('export-'+fmt+'-'+profile.replace('.','-'),[opened(),raster(fmt,profile=profile)])
        for name in ('bad-header.icc','cmyk.icc','oversize.icc'):
            s=raster(fmt,profile=name); s.update(expect={'status':'rejected','publication':N,'undo_effect':'none'},expect_error='engine-limit' if name=='oversize.icc' else 'profile-invalid',cells=['refusal']); s.pop('expect_absent'); s['file_oracles']=[{'path':s['request']['params']['path'],'absent':True}]
            write('export-'+fmt+'-refuse-'+name.replace('.','-'),[opened(),s])
        for policy in ('default','srgb'):
            s=raster(fmt); s['request']['params']['profile']={'id':policy}
            extra={}
            if policy=='default':
                extra['profile_override']='{WORK}/inputs/custom-rgb.icc'
                s['file_oracles'][0]['profile']='{WORK}/inputs/custom-rgb.icc'
            write('export-'+fmt+'-profile-'+policy,[opened(),s],**extra)
        for text in ('preserve','paths'):
            s=raster(fmt); s['request']['params']['text-policy']=text; write('export-'+fmt+'-text-'+text,[opened(),s])
    for fmt in ('svg','pdf'):
        s=raster(fmt); s['request']['params']['profile']={'id':'srgb'}; s.update(expect={'status':'rejected','publication':N},expect_error='profile-unsupported',cells=['refusal']); s.pop('expect_absent'); s['file_oracles']=[{'path':s['request']['params']['path'],'absent':True}]
        write('export-'+fmt+'-profile-refusal',[opened(),s])
        for text in ('preserve','paths'):
            s=raster(fmt); s['request']['params']['text-policy']=text; s['file_oracles'][0]['text_policy']=text; write('export-'+fmt+'-text-'+text,[opened(),s])
    for op in ('open','import'):
        for resource in ('embed','reject-external'):
            for font in ('reject','substitute'):
                for name in ('sheet.svg','embedded.svg'):
                    p=load(name,resource=resource,font=font)
                    s=step('file.'+op,p,'load',cells=['success'])
                    seq=[]
                    if op=='import': p.pop('discard'); s['request']['params'].pop('discard'); s['request']['params']['position']={'x':length(0),'y':length(0)}; s['request']['document']=live(); seq=[opened()]
                    write(op+'-'+name.replace('.','-')+'-'+resource+'-'+font,seq+[s])
        for name,error in [('remote.svg','remote-resource'),('entity.svg','unsafe-xml'),('script.svg','resource-denied'),('malformed.svgz','invalid-svgz'),('oversized-header.png','engine-limit'),('deep.svg','engine-limit'),('objects-limit.svg','engine-limit')]:
            p=load(name,'auto'); seq=[]; kwargs={}
            if op=='import': p.pop('discard'); p['position']={'x':length(0),'y':length(0)}; seq=[opened()]; kwargs['document']=live()
            s=step('file.'+op,p,'hostile',status='rejected',error=error,cells=['refusal','boundary'],**kwargs)
            q=query('before'); after=query('after'); after['expect_exact']={'data':ref('before.data')}
            write(op+'-hostile-'+name.replace('.','-'),seq+([q] if seq else [])+[s]+([after] if seq else []))
        for pages in ([],[0],[3],[1,1],[1,2]):
            p=load('two-pages.pdf','pdf',pages=pages); seq=[]; kwargs={}
            if op=='import': p.pop('discard'); p['position']={'x':length(0),'y':length(0)}; seq=[opened()]; kwargs['document']=live()
            # Both requested pages must succeed; no implicit first-page truncation.
            good=pages==[1,2]
            s=step('file.'+op,p,'pages',status='changed' if good else 'rejected',error=None if good else 'invalid-argument' if pages==[1,1] else 'out-of-range' if pages in ([],[0]) else 'pages-invalid',cells=['success'] if good else ['boundary','refusal'],**kwargs)
            write(op+'-pdf-pages-'+('_'.join(map(str,pages)) or 'empty'),seq+[s])
        for font in ('reject','substitute'):
            p=load('missing-font.pdf','pdf',font=font,pages=[1]); seq=[]; kwargs={}
            if op=='import': p.pop('discard'); p['position']={'x':length(0),'y':length(0)}; seq=[opened()]; kwargs['document']=live()
            s=step('file.'+op,p,'font',status='rejected' if font=='reject' else 'changed',error='font-policy-required' if font=='reject' else None,cells=['refusal'] if font=='reject' else ['success'],**kwargs)
            if font=='substitute': s['expect_nonempty']=['warnings']
            write(op+'-missing-font-'+font,seq+[s])
    # Lifecycle: refusal preserves old incarnation; dirty discard and repeated close.
    new=step('file.new',params['file.new'],'new',cells=['success'])
    write('close-empty-repeat',[step('file.close',{'discard':False},'c1',status='unchanged',cells=['no-op','success']),step('file.close',{'discard':False},'c2',status='unchanged',cells=['no-op'])])
    for cid in LIFECYCLE:
        s=step(cid,params[cid],'refuse',status='rejected',error='dirty-document',cells=['refusal'],document=ref('new.document_id'),revision=1)
        write(cid.split('.')[1]+'-dirty-refusal',[new,query('before'),s,dict(query('after'),expect_exact={'data':ref('before.data')})])
        p=copy.deepcopy(params[cid]); p['discard']=True
        write(cid.split('.')[1]+'-dirty-discard',[new,step(cid,p,'discard',document=ref('new.document_id'),revision=1,cells=['success'])])
    write('eof-never-saves',[new],file_oracles=[{'path':'{WORK}/saved.svg','absent':True}])
    write('import-groups-save-reopen',[opened('empty.svg'),step('file.import',dict({k:v for k,v in load('groups.svg').items() if k!='discard'},position={'x':length(0),'y':length(0)}),'import',document=live(),cells=['success','group','save']),save(revision=ref('import.revision_after'))])
    # Save policy and resource grants; root and exact-file admission are distinct.
    for policy in ('embed','reject-external'):
        s=save(); s['request']['params']['embedding-policy']=policy; s['file_oracles'][0]['objects']={'red':{'width':'8','height':'8'},'green':{'x':'8'}}
        write('save-'+policy,[opened(),s])
    for op in ('open','import'):
        for grant_index,(grants,error) in enumerate([([], 'read-grant-denied'),(['--grant-write','{WORK}'],'read-grant-denied'),(['--grant-read-file','{WORK}/inputs/linked.svg'],'resource-denied')]):
            p=load('linked.svg'); seq=[]; kwargs={}
            if op=='import': p.pop('discard'); p['position']={'x':length(0),'y':length(0)}; kwargs['document']=live(); seq=[opened()]; grants+=['--grant-read-file','{WORK}/inputs/sheet.svg']
            write(op+'-grant-'+str(grant_index),seq+[step('file.'+op,p,status='rejected',error=error,cells=['refusal'],**kwargs)],grants)
    write('open-exact-file-grant',[opened()],['--grant-read-file','{WORK}/inputs/sheet.svg'])
    for cid in FILES[4:]:
        p=params[cid]
        write(cid.split('.')[1]+'-read-only-grant',[opened(),step(cid,p,status='rejected',error='write-grant-denied',cells=['refusal'],document=live())],['--grant-read','{WORK}/inputs'])
        p=copy.deepcopy(p); p['overwrite']=True
        write(cid.split('.')[1]+'-expected-required',[opened(),step(cid,p,status='rejected',error='expected-version-required',cells=['refusal'],document=live())])
        for destination in ('existing.svg','unicode space á.svg'):
            s=step(cid,dict(params[cid],path='{WORK}/'+destination),'conflict',status='rejected',error='publication-conflict',cells=['refusal'],document=live())
            s['file_oracles']=[{'path':'{WORK}/'+destination,'sha256':HASHES['sheet.svg']}]
            write(cid.split('.')[1]+'-conflict-'+destination.replace('.','-').replace(' ','_'),[opened(),s],setup=[{'op':'copy','source':'{WORK}/inputs/sheet.svg','path':'{WORK}/'+destination}])
        for change in ('replace-same-bytes','change-same-size-mtime'):
            probe=step(cid,dict(p,path='{WORK}/target.svg',**{'expected-version':{'identity':'wrong','sha256':HASHES['sheet.svg'],'bytes':1}}),'probe',status='rejected',error='publication-conflict',cells=['refusal'],document=live())
            retry=step(cid,dict(p,path='{WORK}/target.svg',**{'expected-version':ref('probe.data.destination_version')}),'retry',status='rejected',error='publication-conflict',cells=['refusal'],document=live())
            retry['before']=[{'op':change,'path':'{WORK}/target.svg'}]
            if change=='replace-same-bytes': retry['file_oracles']=[{'path':'{WORK}/target.svg','sha256':HASHES['sheet.svg']}]
            else:
                changed=bytearray((FIXTURES/'sheet.svg').read_bytes()); changed[-1]^=1
                retry['file_oracles']=[{'path':'{WORK}/target.svg','sha256':hashlib.sha256(changed).hexdigest()}]
            write(cid.split('.')[1]+'-'+change,[opened(),probe,retry],setup=[{'op':'copy','source':'{WORK}/inputs/sheet.svg','path':'{WORK}/target.svg'}])
        for path_index,path in enumerate(('{WORK}/out/../forbidden.svg','{WORK}/inputs-sibling/output.svg','file://server/share/p9.svg','//server/share/p9.svg','C:\\p9.svg:ads')):
            s=step(cid,dict(params[cid],path=path),'unsafe',status='rejected',error='write-grant-denied' if path_index==1 else 'unsafe-destination',cells=['refusal'],document=live())
            write(cid.split('.')[1]+'-unsafe-'+str(path_index),[opened(),s],['--grant-read','{WORK}/inputs','--grant-write','{WORK}/out'],setup=[{'op':'mkdir','path':'{WORK}/out'},{'op':'mkdir','path':'{WORK}/inputs-sibling'}])
        s=step(cid,dict(params[cid],path='{WORK}/link/output.svg'),cells=['success'],document=live())
        s['file_oracles']=[{'path':'{WORK}/link/output.svg','reported':True}]
        write(cid.split('.')[1]+'-symlink-parent',[opened(),s],setup=[{'op':'mkdir','path':'{WORK}/real'},{'op':'symlink','source':'{WORK}/real','path':'{WORK}/link','directory':True}])
    # Independent one-shot guards for M1 inspection remains non-editable.
    for cid in ('file.import','file.save'):
        write(cid.split('.')[1]+'-inspection-read-only',[step(cid,params[cid],status='rejected',error='document-read-only',cells=['refusal'],document='d1')],['--inspect-file','{WORK}/inputs/sheet.svg','--grant-read','{WORK}/inputs','--grant-write','{WORK}'])
    # Required native-only cells cannot be exercised by a production wire backdoor.
    native_domains=['selection-contract','no-ungranted-parser-opens','native-uninterruptible-phase-truth','import-unrelated-group-clone-lock-hidden-preservation','save-reopen-anchor-and-export-hints','destination-parent-stability-trusted-boundary','import-exact-undo-redo-xml-selection','prepare-cancel','before-publish-cancel','after-boundary-too-late','eof-preparation-cancel',
       'publication-create-failure','publication-write-failure','publication-flush-failure','publication-sync-failure','publication-close-failure','publication-seal-failure','publication-full-disk',
       'expected-version-before-replace','raced-new-file-conflict','publication-unsupported','publication-uncertain','uncertain-dirty-anchor-writes-latch','reconciliation-close-open',
       'published-cleanup-warning','source-resource-changed','profile-changed','profile-missing','profile-ungranted','default-fallback-notice','executable-extension-refusal',
       'network-mounted-mac','network-mapped-windows','windows-sharing-acl-read-only-hardlinks','windows-reparse-alias-device-ads','installed-printer-cli-smoke']
    na={cid:{'no-op':'new/open create a fresh incarnation; publication has no no-op guarantee',
             'undo':'lifecycle/save/export do not enter document history',
             'group':'lifecycle/save have no selection target'} for cid in FILES}
    na['file.close']['no-op']='covered by repeated close'
    na['file.import'].pop('undo'); na['file.import'].pop('group'); na['file.export'].pop('group')
    atomic_json(ROOT/'support/inventory.json',{'schema':'p9-m2-inventory/1','required_cases':sorted(ARTIFACTS),'na':na,'native_domains':native_domains,
        'native_cells':{'file.import':['undo','cancel'],'file.new':['cancel'],'file.open':['cancel'],'file.close':['cancel'],'file.save':['cancel'],'file.export':['cancel']},
        'frozen_descriptor_status':'Plan contract plus P4 static descriptors; success/error semantics independently pinned; unimplemented formats remain mandatory'})
    print('CASES:',len(ARTIFACTS))

if __name__=='__main__': main()
