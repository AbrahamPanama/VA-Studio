#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Additional M2 preservation, guards, profile and publication boundary cases."""
import copy
import json
from pathlib import Path
import sys
sys.dont_write_bytecode=True
sys.path.insert(0,str(Path(__file__).parent))
import cases as c


def main():
    c.ARTIFACTS[:]=[p.name for p in c.ROOT.glob('*.json')]
    imp_params={k:v for k,v in c.load().items() if k!='discard'}
    imp_params['position']={'x':c.length(2),'y':c.length(3)}
    for cid,params in [('file.import',imp_params),('file.save',{'path':'{WORK}/saved.svg','embedding-policy':'embed'}),('file.export',c.export())]:
        c.write(cid.split('.')[1]+'-no-document',[c.step(cid,params,status='rejected',error='no-document',cells=['refusal'],document='absent')])
        c.write(cid.split('.')[1]+'-stale-document',[c.opened(),c.step(cid,params,status='rejected',error='stale-document',cells=['refusal'],document='old-incarnation')])
    for cid,params in [('file.new',{'width':c.length(16),'height':c.length(8)}),('file.open',c.load()),('file.close',{'discard':True})]:
        s=c.step(cid,params,'guard',status='rejected',error='stale-document',cells=['refusal'],document='old-incarnation',revision=1)
        c.write(cid.split('.')[1]+'-stale-document',[c.opened(),s])
    for name,fmt in [('sheet.svg','svg'),('sheet.svgz','svgz'),('sheet.cdr','cdr'),('two-pages.pdf','pdf'),('rgba.png','png'),('photo.jpg','jpeg'),('rgba.tiff','tiff')]:
        p={k:v for k,v in c.load(name,'auto',pages=[1] if fmt=='pdf' else None).items() if k!='discard'}
        p['position']={'x':c.length(0),'y':c.length(0)}
        c.write('import-auto-'+fmt,[c.opened('empty.svg'),c.step('file.import',p,'import',document=c.live(),cells=['success'])])
        # Full intake format/detection × page-one × resource × font policy product.
        # Raster sources have no fonts/external links, but both declared policies
        # must still be admitted rather than mistaken for successful format coverage.
        for choice in (fmt,'auto'):
            for resource in ('embed','reject-external'):
                for font in ('reject','substitute'):
                    for op in ('open','import'):
                        p=c.load(name,choice,resource=resource,font=font,pages=[1])
                        sequence=[]
                        if op=='open':
                            operation=c.opened(name,choice,resource=resource,font=font,pages=[1])
                            operation['expect']['data']={'dirty':fmt not in ('svg','svgz')}
                        else:
                            p.pop('discard'); p['position']={'x':c.length(0),'y':c.length(0)}
                            sequence=[c.opened('empty.svg')]
                            operation=c.step('file.import',p,'policy-import',document=c.live(),cells=['success'])
                            if fmt not in ('cdr','pdf'):
                                operation['expect_exact']={'data.root_affines.0.affine':[1,0,0,1,0,0]}
                        sequence.append(operation)
                        if fmt!='cdr':
                            q=c.query('dimensions'); q['expect_numeric']={'data.size.width.px':{'value':16,'abs':1e-6},'data.size.height.px':{'value':8,'abs':1e-6}}
                            sequence.append(q)
                        c.write('matrix-'+op+'-'+fmt+'-'+choice+'-page1-'+resource+'-'+font,sequence)
    for unit in ('mm','cm','in','pt','pc'):
        p=copy.deepcopy(imp_params); p['position']={'x':c.length(0,unit),'y':c.length(0,unit)}
        s=c.step('file.import',p,'import',document=c.live(),cells=['success'])
        s['expect_exact']={'data.root_affines.0.affine':[1,0,0,1,0,0]}
        c.write('import-position-unit-'+unit,[c.opened('empty.svg'),s])
        # A 3.75px square in each unit; ceil gives 4 pixels without an ambiguous floating endpoint.
        factor={'mm':96/25.4,'cm':96/2.54,'in':96,'pt':96/72,'pc':16}[unit]
        out=c.raster('png','area'); out['request']['params']['area']={key:c.length(value/factor,unit) for key,value in [('x',1),('y',1),('width',3.75),('height',3.75)]}
        c.write('export-area-unit-'+unit,[c.opened(),out])
    for op in ('open','import'):
        name=op+'-pdf-pages-1_2.json'; path=c.ROOT/name
        case=json.loads(path.read_text())
        if not any(row.get('request',{}).get('id')=='pages-query' for row in case['requests']):
            case['requests'].append({'request':{'schema':'va-studio.cli-request/1','id':'pages-query','command':'query.pages','params':{}},
            'expect':{'status':'ok','data':{'total':2}},'expect_numeric':{'data.items.0.rect.width.px':{'value':16,'abs':1e-6},'data.items.0.rect.height.px':{'value':8,'abs':1e-6}}})
        c.atomic_json(path,case)
    for fmt in ('svg','pdf'):
        for text in ('preserve','paths'):
            out=c.raster(fmt,'page'); out['request']['params']['text-policy']=text
            out['file_oracles'][0].update(samples=[],objects={},has_text=True,text='A',text_policy=text)
            c.write('export-'+fmt+'-real-text-'+text,[c.opened('text.svg'),out],
                fontconfig='{WORK}/fonts.conf',setup=[{'op':'write','path':'{WORK}/fonts.conf','text':'<?xml version="1.0"?><!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd"><fontconfig><dir>{WORK}/inputs/fonts</dir><cachedir>{WORK}/font-cache</cachedir></fontconfig>'}])
    imp=c.step('file.import',imp_params,'import',document=c.live(),cells=['success','group','save'])
    imp['expect_exact']={'data.root_affines.0.affine':[1,0,0,1,2,3]}
    imp['expect_nonempty']=['data.inserted_ids']
    save=c.save(revision=c.ref('import.revision_after'))
    save['file_oracles'][0]['objects']={'red':{'x':'0','y':'0','width':'8','height':'8'},'green':{'x':'8','width':'8'}}
    c.write('import-position-affine',[c.opened('empty.svg'),imp,save])
    groups=c.opened('groups.svg')
    group_save=c.save(); group_save['file_oracles'][0]['objects']={'parent':{'transform':'translate(2,3)'},'child':{'transform':'scale(2)'},'clone':{'{http://www.w3.org/1999/xlink}href':'#tile'},'hidden':{'style':'display:none'},'locked':{'{http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd}insensitive':'true'},'unrelated':{'x':'30','y':'20'}}
    c.write('groups-payload-save',[groups,group_save])
    for op in ('open','import'):
        p=c.load('linked.svg'); seq=[]; kwargs={}
        if op=='import': p.pop('discard'); p['position']={'x':c.length(0),'y':c.length(0)}; seq=[c.opened('empty.svg')]; kwargs['document']=c.live()
        success=c.step('file.'+op,p,'linked',cells=['success'],**kwargs)
        c.write(op+'-linked-embed-success',seq+[success])
        denied=copy.deepcopy(success); denied.update(expect={'status':'rejected','publication':c.N,'undo_effect':'none'},expect_error='resource-denied',cells=['refusal']); denied.pop('expect_absent')
        c.write(op+'-linked-missing',seq+[denied],setup=[{'op':'unlink','path':'{WORK}/inputs/rgba.png'}])
        rejected=copy.deepcopy(denied); rejected['request']['params']['resource-policy']='reject-external'
        c.write(op+'-reject-linked',seq+[rejected])
    for fmt in ('png','tiff'):
        alpha=c.raster(fmt,profile='custom-rgb.icc')
        alpha['file_oracles'][0]['samples'][1]['rgba']=[0,255,0,128]
        c.write('export-'+fmt+'-custom-alpha',[c.opened('rgba.png','png'),alpha])
        for fault in ('missing','ungranted','changed'):
            out=c.raster(fmt,profile='custom-rgb.icc')
            out.update(expect={'status':'rejected','publication':c.N,'undo_effect':'none'},expect_error={'missing':'resource-unavailable','ungranted':'read-grant-denied','changed':'stale-dependency'}[fault],cells=['refusal']); out.pop('expect_absent')
            out['file_oracles']=[{'path':out['request']['params']['path'],'absent':True}]
            startup=['--grant-read-file','{WORK}/inputs/sheet.svg','--grant-write','{WORK}'] if fault=='ungranted' else None
            if fault=='missing': out['request']['params']['profile']['path']='{WORK}/inputs/missing.icc'
            if fault=='changed': out['request']['params']['profile']['sha256']='0'*64
            c.write('export-'+fmt+'-profile-'+fault,[c.opened(),out],startup)
        for dpi in (1,192,9600):
            out=c.raster(fmt); out['request']['params']['dpi']=dpi
            out['file_oracles'][0]['size']=[max(1,round(16*dpi/96)),max(1,round(8*dpi/96))]
            # ceil is the frozen raster rule. Interior samples only; 1dpi is dimension-only.
            import math
            out['file_oracles'][0]['size']=[math.ceil(16*dpi/96),math.ceil(8*dpi/96)]
            out['file_oracles'][0]['samples']=[] if dpi==1 else [{'x':int(2*dpi/96),'y':int(4*dpi/96),'rgba':[255,0,0,255]},{'x':int(12*dpi/96),'y':int(4*dpi/96),'rgba':[0,255,0,255]}]
            out['cells']=['success','boundary']; c.write('export-'+fmt+'-dpi-'+str(dpi),[c.opened(),out])
        out=c.raster(fmt); out['request']['params']['background']=[1,1,1,1]
        out['file_oracles'][0]['samples'][1]['rgba']=[127,255,127,255]
        c.write('export-'+fmt+'-alpha-white',[c.opened('rgba.png','png'),out])
    for scope_index,scopes in enumerate(({},{'page':1,'drawing':True},{'drawing':False},{'ids':[]},{'ids':['red','red']},{'ids':['missing']},{'page':2})):
        out=c.raster(); p=out['request']['params']; p.pop('drawing'); p.update(scopes)
        out.update(expect={'status':'rejected','publication':c.N,'undo_effect':'none'},expect_error='invalid-target',cells=['refusal','boundary']); out.pop('expect_absent'); out['file_oracles']=[{'path':p['path'],'absent':True}]
        if scopes=={'ids':[]}:
            out['expect_error']='out-of-range'; out['expect_accepted']=False
        if scopes=={'ids':['red','red']}:
            # Frozen ids schema declares uniqueItems: true: reject at admission.
            out['expect_error']='invalid-argument'; out['expect_accepted']=False
        c.write('export-invalid-scope-'+str(scope_index),[c.opened(),out])
    for cid in ('file.save','file.export'):
        save=c.save() if cid=='file.save' else c.raster()
        save['request']['params']['path']='{WORK}/unicode space á.'+('svg' if cid=='file.save' else 'png')
        save['file_oracles'][0]['path']=save['request']['params']['path']
        c.write(cid.split('.')[1]+'-exact-file-unicode',[c.opened(),save],['--grant-read-file','{WORK}/inputs/sheet.svg','--grant-write-file',save['request']['params']['path']])
        probe=c.step(cid,dict(save['request']['params'],path='{WORK}/target.svg'),'probe',status='rejected',error='publication-conflict',cells=['refusal'],document=c.live())
        changed=copy.deepcopy(save); changed['request']['id']='replace'; changed['request']['params'].update(path='{WORK}/target.svg',overwrite=True)
        changed['request']['params']['expected-version']=c.ref('probe.data.destination_version'); changed['file_oracles'][0]['path']='{WORK}/target.svg'
        c.write(cid.split('.')[1]+'-guarded-overwrite',[c.opened(),probe,changed],setup=[{'op':'copy','source':'{WORK}/inputs/sheet.svg','path':'{WORK}/target.svg'}])
    new=c.step('file.new',{'width':c.length(16),'height':c.length(8)},'new',cells=['success'])
    imp=c.step('file.import',dict(imp_params,position={'x':c.length(0),'y':c.length(0)}),'import',document=c.ref('new.document_id'),cells=['success','group','save'])
    out=c.raster(); out['request'].update(document=c.ref('new.document_id'),if_revision=c.ref('import.revision_after'))
    save=c.save(); save['request'].update(document=c.ref('new.document_id'),if_revision=c.ref('import.revision_after'))
    before=c.query('before'); after=c.query('after-export'); after['expect_exact']={'data':c.ref('before.data')}
    close=c.step('file.close',{'discard':False},'close',document=c.ref('new.document_id'),revision=1,cells=['success'])
    reopen=c.step('file.open',dict(c.load(),path='{WORK}/saved.svg'),'reopen',revision=2,cells=['success'])
    fresh=c.query('fresh'); fresh['expect']['data']={'dirty':False}; fresh['expect_exact']={'data.size.width.px':16,'data.size.height.px':8}
    c.write('workflow-new-import-query-export-save-close-open',[new,imp,before,out,after,save,close,reopen,fresh])
    # Import empty source is a defined refusal if there is no supported no-op.
    empty=dict(imp_params,path='{WORK}/inputs/empty.svg')
    c.write('import-empty-refusal',[c.opened(),c.step('file.import',empty,status='rejected',error='import-failed',cells=['refusal'],document=c.live())])
    ledger=json.loads((c.ROOT/'support/inventory.json').read_text())
    ledger['required_cases']=sorted(set(c.ARTIFACTS))
    ledger['na']['file.import']['no-op']='Empty import has no guaranteed no-op; require explicit import-failed refusal with unchanged baseline'
    for domain in ('cdr-multipage-assembly','per-command-matrix-schema-and-persistence-snapshots'):
        if domain not in ledger['native_domains']: ledger['native_domains'].append(domain)
    c.atomic_json(c.ROOT/'support/inventory.json',ledger)
    print('CASES:',len(ledger['required_cases']))

if __name__=='__main__': main()
