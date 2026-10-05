#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Independent disk/pixel/profile oracles. Standard library only.

Inputs and expectations come from fixture geometry, never CLI output. Native
libtiff/Poppler are independent readers; missing readers are failures, not skips.
"""
import binascii
import ctypes
import ctypes.util
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import xml.etree.ElementTree as ET
import zlib

MAX_BYTES = 32 * 1024 * 1024
_DLL_DIRECTORIES = []


def native_library(name):
    """Use existing platform readers, including the pinned UCRT64 DLL names."""
    located=ctypes.util.find_library(name)
    if not located and os.name=='nt':
        names={'tiff':('libtiff-6.dll','libtiff-5.dll','libtiff.dll','tiff.dll'),
               'lcms2':('liblcms2-2.dll','lcms2.dll')}
        # DLLs are data files, not PATHEXT executables. Search the supplied
        # toolchain PATH, never a product API or a product-provided oracle.
        located=next((str(candidate.resolve())
                      for directory in os.environ.get('PATH', '').split(os.pathsep) if directory
                      for item in names.get(name, ())
                      if (candidate := Path(directory.strip('"')) / item).is_file()), None)
    if not located: raise ValueError(name+' independent native reader missing')
    if os.name=='nt' and Path(located).is_absolute():
        _DLL_DIRECTORIES.append(os.add_dll_directory(str(Path(located).parent)))
    return ctypes.CDLL(located)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def lookup(record, path):
    for key in path.split('.'):
        record = record[int(key)] if isinstance(record, list) else record[key]
    return record


def png(path):
    data = Path(path).read_bytes()
    if len(data) > MAX_BYTES or data[:8] != b'\x89PNG\r\n\x1a\n': raise ValueError('invalid/big PNG')
    pos, chunks = 8, {}
    while pos < len(data):
        n = struct.unpack_from('>I', data, pos)[0]; kind = data[pos+4:pos+8]
        payload = data[pos+8:pos+8+n]
        if len(payload) != n or pos+12+n > len(data): raise ValueError('truncated PNG')
        if binascii.crc32(kind+payload) & 0xffffffff != struct.unpack_from('>I', data, pos+8+n)[0]: raise ValueError('PNG CRC')
        chunks.setdefault(kind, []).append(payload); pos += 12+n
    if len(chunks.get(b'IHDR', [])) != 1 or chunks.get(b'IEND') != [b'']: raise ValueError('PNG structure')
    w,h,depth,color,comp,filt,interlace = struct.unpack('>IIBBBBB', chunks[b'IHDR'][0])
    channels = {0:1,2:3,3:1,4:2,6:4}.get(color)
    if depth != 8 or interlace or comp or filt or not channels or w*h > 4000000: raise ValueError('unsupported PNG encoding')
    stride = w*channels
    decoder = zlib.decompressobj(); raw = decoder.decompress(b''.join(chunks.get(b'IDAT', [])), (stride+1)*h+1)
    if len(raw) != (stride+1)*h or not decoder.eof: raise ValueError('PNG inflated size')
    pixels = []; prev = bytearray(stride)
    for y in range(h):
        method = raw[y*(stride+1)]; row = bytearray(raw[y*(stride+1)+1:(y+1)*(stride+1)])
        for i in range(stride):
            a = row[i-channels] if i >= channels else 0; b = prev[i]; c = prev[i-channels] if i >= channels else 0
            p = a+b-c; pa,pb,pc = abs(p-a),abs(p-b),abs(p-c)
            predictor = (0,a,b,(a+b)//2,a if pa<=pb and pa<=pc else b if pb<=pc else c)
            if method > 4: raise ValueError('PNG filter')
            row[i] = (row[i]+predictor[method]) & 255
        for x in range(w):
            v = tuple(row[x*channels:(x+1)*channels])
            if color == 6: rgba = v
            elif color == 2: rgba = v+(255,)
            elif color == 4: rgba = (v[0],)*3+(v[1],)
            elif color == 0: rgba = (v[0],)*3+(255,)
            else:
                palette = chunks[b'PLTE'][0]; index = v[0]
                alpha = chunks.get(b'tRNS', [b''])[0]
                rgba = tuple(palette[index*3:index*3+3])+(alpha[index] if index<len(alpha) else 255,)
            pixels.append(rgba)
        prev = row
    profile = None
    if b'iCCP' in chunks:
        if len(chunks[b'iCCP']) != 1: raise ValueError('duplicate PNG profile')
        name, packed = chunks[b'iCCP'][0].split(b'\0', 1)
        if not name or packed[:1] != b'\0': raise ValueError('invalid iCCP')
        dec = zlib.decompressobj(); profile = dec.decompress(packed[1:], 15*1024*1024+1)
        if not dec.eof or len(profile)>15*1024*1024: raise ValueError('unbounded ICC')
    return w,h,pixels,profile


def tiff(path):
    data = Path(path).read_bytes()
    if len(data)>MAX_BYTES or data[:4] not in (b'II*\0',b'MM\0*'): raise ValueError('invalid TIFF')
    endian = '<' if data[:2]==b'II' else '>'
    offset = struct.unpack_from(endian+'I',data,4)[0]; count=struct.unpack_from(endian+'H',data,offset)[0]
    tags={}
    for i in range(count):
        tag,kind,n,val = struct.unpack_from(endian+'HHII',data,offset+2+12*i)
        size={1:1,2:1,3:2,4:4,5:8,7:1}.get(kind)
        if not size: continue
        start=offset+2+12*i+8 if n*size<=4 else val
        if n*size>MAX_BYTES or start+n*size>len(data): raise ValueError('TIFF tag bounds')
        tags[tag]=(kind,n,data[start:start+n*size])
    def integer(tag):
        kind,n,raw=tags[tag]
        if n!=1 or kind not in (3,4): raise ValueError('TIFF scalar')
        return struct.unpack(endian+('H' if kind==3 else 'I'),raw)[0]
    w,h=integer(256),integer(257)
    if w*h>4000000: raise ValueError('TIFF pixel bounds')
    lib=native_library('tiff')
    lib.TIFFOpen.argtypes=[ctypes.c_char_p,ctypes.c_char_p]; lib.TIFFOpen.restype=ctypes.c_void_p
    lib.TIFFClose.argtypes=[ctypes.c_void_p]
    lib.TIFFReadRGBAImageOriented.argtypes=[ctypes.c_void_p,ctypes.c_uint32,ctypes.c_uint32,ctypes.POINTER(ctypes.c_uint32),ctypes.c_int,ctypes.c_int]
    if os.name=='nt' and hasattr(lib,'TIFFOpenW'):
        lib.TIFFOpenW.argtypes=[ctypes.c_wchar_p,ctypes.c_char_p]; lib.TIFFOpenW.restype=ctypes.c_void_p
        handle=lib.TIFFOpenW(str(path),b'r')
    else: handle=lib.TIFFOpen(os.fsencode(path),b'r')
    if not handle: raise ValueError('TIFF reopen')
    raster=(ctypes.c_uint32*(w*h))()
    try:
        if not lib.TIFFReadRGBAImageOriented(handle,w,h,raster,1,1): raise ValueError('TIFF decode')
    finally: lib.TIFFClose(handle)
    pixels=[]
    for v in raster:
        r,g,b,a=v&255,(v>>8)&255,(v>>16)&255,(v>>24)&255
        # libtiff returns associated alpha. Restore straight samples for comparison.
        if a and a!=255: r,g,b=(min(255,round(c*255/a)) for c in (r,g,b))
        pixels.append((r,g,b,a))
    return w,h,pixels,tags.get(34675,(None,None,None))[2]


def converted(samples, profile):
    lib=native_library('lcms2')
    lib.cmsCreate_sRGBProfile.restype=ctypes.c_void_p
    lib.cmsOpenProfileFromMem.argtypes=[ctypes.c_void_p,ctypes.c_uint32]; lib.cmsOpenProfileFromMem.restype=ctypes.c_void_p
    lib.cmsCreateTransform.argtypes=[ctypes.c_void_p,ctypes.c_uint32,ctypes.c_void_p,ctypes.c_uint32,ctypes.c_uint32,ctypes.c_uint32]; lib.cmsCreateTransform.restype=ctypes.c_void_p
    lib.cmsDoTransform.argtypes=[ctypes.c_void_p,ctypes.c_void_p,ctypes.c_void_p,ctypes.c_uint32]
    lib.cmsDeleteTransform.argtypes=[ctypes.c_void_p]; lib.cmsCloseProfile.argtypes=[ctypes.c_void_p]
    raw=ctypes.create_string_buffer(profile); src=lib.cmsCreate_sRGBProfile(); dst=lib.cmsOpenProfileFromMem(raw,len(profile))
    fmt=(4<<16)|(1<<7)|(3<<3)|1 # TYPE_RGBA_8, straight alpha
    transform=lib.cmsCreateTransform(src,fmt,dst,fmt,1,0x2000|0x04000000) if src and dst else None
    try:
        if not transform: raise ValueError('ICC oracle cannot create transform')
        inp=(ctypes.c_ubyte*(len(samples)*4))(*[c for s in samples for c in s]); out=(ctypes.c_ubyte*len(inp))()
        lib.cmsDoTransform(transform,inp,out,len(samples))
        return [tuple(out[i:i+4]) for i in range(0,len(out),4)]
    finally:
        if transform: lib.cmsDeleteTransform(transform)
        if src: lib.cmsCloseProfile(src)
        if dst: lib.cmsCloseProfile(dst)


def verify_file(spec, record=None):
    path=Path(spec['path'])
    if spec.get('absent'):
        if path.exists() or path.is_symlink(): raise ValueError('unexpected persisted file '+str(path))
        return
    if not path.is_file() or path.is_symlink(): raise ValueError('missing/unsafe output '+str(path))
    if path.stat().st_size>MAX_BYTES: raise ValueError('output exceeds oracle bound')
    if 'sha256' in spec and sha(path)!=spec['sha256']: raise ValueError('file hash mismatch')
    if spec.get('reported'):
        if lookup(record,spec.get('hash_field','data.sha256'))!=sha(path): raise ValueError('reported hash mismatch')
        if lookup(record,spec.get('bytes_field','data.bytes'))!=path.stat().st_size: raise ValueError('reported byte count mismatch')
    kind=spec.get('format')
    if kind=='svg':
        tree=ET.parse(path); root=tree.getroot()
        if root.tag!='{http://www.w3.org/2000/svg}svg': raise ValueError('not SVG')
        ids={n.get('id'):n for n in root.iter() if n.get('id')}
        for ident,attrs in spec.get('objects',{}).items():
            if ident not in ids: raise ValueError('lost SVG object '+ident)
            for key,val in attrs.items():
                if ids[ident].get(key)!=val: raise ValueError('SVG payload changed '+ident+'/'+key)
        for ident in spec.get('absent_ids',[]):
            if ident in ids: raise ValueError('SVG scope includes excluded '+ident)
        texts=[n for n in root.iter() if n.tag=='{http://www.w3.org/2000/svg}text']
        if spec.get('text_policy')=='paths' and texts: raise ValueError('paths SVG retains text')
        if spec.get('text_policy')=='preserve' and spec.get('has_text'):
            if not texts or ''.join(''.join(n.itertext()) for n in texts)!=spec.get('text','A'): raise ValueError('preserved SVG text payload lost')
        if spec.get('has_text') and spec.get('text_policy')=='paths' and not any(n.tag=='{http://www.w3.org/2000/svg}path' for n in root.iter()): raise ValueError('text paths missing')
        for node in root.iter():
            href=node.get('{http://www.w3.org/1999/xlink}href',node.get('href',''))
            if spec.get('embedded') and href and not href.startswith(('#','data:')): raise ValueError('nonembedded SVG resource')
        return
    if kind=='pdf':
        data=path.read_bytes()
        if not data.startswith(b'%PDF-') or b'%%EOF' not in data[-2048:]: raise ValueError('invalid PDF')
        tool=shutil.which('pdftoppm')
        info=shutil.which('pdfinfo')
        if not tool or not info: raise ValueError('Poppler independent PDF readers missing')
        result=subprocess.run([info,str(path)],capture_output=True,timeout=15,check=True)
        if not re.search(rb'^Pages:\s+'+str(spec.get('pages',1)).encode()+rb'\s*$',result.stdout,re.M): raise ValueError('PDF page count')
        if spec.get('text_policy') in ('paths','preserve'):
            fonts=shutil.which('pdffonts')
            if not fonts: raise ValueError('pdffonts reader missing')
            fontlist=subprocess.run([fonts,str(path)],capture_output=True,timeout=15,check=True).stdout.splitlines()
            if spec['text_policy']=='paths' and len(fontlist)>2: raise ValueError('paths PDF retains fonts')
            if spec['text_policy']=='preserve' and spec.get('has_text') and len(fontlist)<=2: raise ValueError('preserve PDF lost font/text')
        with tempfile.TemporaryDirectory(prefix='p9-pdf-') as temp:
            subprocess.run([tool,'-f','1','-singlefile','-r',str(spec.get('dpi',96)),'-png',str(path),temp+'/page'],capture_output=True,timeout=20,check=True)
            w,h,pixels,profile=png(temp+'/page.png')
    elif kind in ('png','tiff'): w,h,pixels,profile=(png if kind=='png' else tiff)(path)
    else: return
    if [w,h]!=spec['size']: raise ValueError('independent dimensions mismatch: '+repr([w,h]))
    if 'reference' in spec:
        rw,rh,rp,_=png(spec['reference'])
        if (w,h)!=(rw,rh): raise ValueError('rendered reference dimensions')
        mean=sum(abs(a-b) for p,q in zip(pixels,rp) for a,b in zip(p,q))/(w*h*4)
        if mean>spec.get('mean_tolerance',2): raise ValueError('independent rendered reference mismatch')
    samples=spec.get('samples',[]); expected=[tuple(s['rgba']) for s in samples]
    if 'profile' in spec:
        wanted=Path(spec['profile']).read_bytes()
        if profile!=wanted: raise ValueError('embedded ICC exact bytes mismatch')
        expected=converted(expected,wanted)
    for s,want in zip(samples,expected):
        actual=pixels[s['y']*w+s['x']]
        if any(abs(a-b)>spec.get('tolerance',2) for a,b in zip(actual,want)): raise ValueError('independent pixel mismatch: '+repr((s,actual,want)))


def publication(record, request):
    """Truth table independent of advertised schemas and service serialization."""
    command=request['command']; pub=record.get('publication',{})
    state,persisted=pub.get('state'),pub.get('persisted')
    if type(persisted) is not bool: raise ValueError('publication persisted must be boolean')
    publishing=command in ('file.save','file.export') and not request.get('dry_run',False)
    if not publishing:
        if state!='not-published' or persisted: raise ValueError('nonpublication command persisted')
        return
    status=record.get('status'); code=record.get('error',{}).get('code')
    expected={'publication-conflict':('not-published',False,'rejected'),
              'publication-unsupported':('not-published',False,'rejected'),
              'publication-uncertain':('uncertain',False,'uncertain')}
    if code in expected and (state,persisted,status)!=expected[code]: raise ValueError('incorrect publication outcome/status')
    if status=='changed' and (state,persisted)!=('published',True): raise ValueError('Published must persist')
    if status in ('ok','unchanged'): raise ValueError('save/export cannot claim success without publication')
    if persisted and (status!='changed' or state!='published'): raise ValueError('false persisted publication')
    if status in ('rejected','failed','cancelled') and (state,persisted)!=('not-published',False): raise ValueError('prepublication failure persisted')
    outcome=record.get('data',{}).get('publication',{}).get('outcome')
    if outcome is not None:
        truth={'Published':('published',True,'changed'),'Conflict':('not-published',False,'rejected'),
               'Unsupported':('not-published',False,'rejected'),'Uncertain':('uncertain',False,'uncertain'),
               'Failed':('not-published',False,'failed')}
        if outcome not in truth or truth[outcome]!=(state,persisted,status): raise ValueError('incorrect detailed publication outcome')
    if status=='changed':
        if outcome!='Published': raise ValueError('Published lacks detailed outcome')
        detail=record['data']['publication']
        for key in ('destination','durability','cleanup_ok','recovery_path','recovery_availability'):
            if key not in detail: raise ValueError('publication lacks '+key)
        if Path(detail['destination']).resolve(strict=True)!=Path(request['params']['path']).resolve(strict=True): raise ValueError('publication destination mismatch')
        verify_file({'path':request['params']['path'],'reported':True},record)
        disk=Path(request['params']['path'])
        observed=record['data']['destination_version']
        if not isinstance(observed.get('identity'),str) or not observed['identity']: raise ValueError('destination identity missing')
        if observed['sha256']!=sha(disk) or observed['bytes']!=disk.stat().st_size: raise ValueError('destination version not observed from disk')
        if command=='file.save' and (record['data']['dirty'] is not False or record['data']['saved_revision']!=record['revision_before']): raise ValueError('save did not confirm current clean revision')


def command_result(record, request):
    """Required successful payload independently frozen from the plan."""
    contract=json.loads(Path(__file__).with_name('contract.json').read_text())
    command=request['command']; publication(record,request)
    observed=record.get('data',{}).get('destination_version')
    if observed is not None:
        disk=Path(request['params']['path'])
        if not disk.is_file() or observed.get('sha256')!=sha(disk) or observed.get('bytes')!=disk.stat().st_size or not observed.get('identity'):
            raise ValueError('reported observed destination version disagrees with independent disk oracle')
    if record.get('status') not in ('changed','unchanged','ok'): return
    if request.get('dry_run'): return # computed/preflight intentionally omit live identities
    for key in contract['commands'][command]['success_data']:
        if key not in record.get('data',{}): raise ValueError('required successful payload missing '+key)
    if command in ('file.new','file.open'):
        if not record.get('document_id') or record['data']['document_id']!=record['document_id'] or record['data']['revision']!=0: raise ValueError('new/open incarnation baseline')
    if command=='file.new' and record['data']['dirty'] is not True: raise ValueError('new document must be dirty')
    if command=='file.close' and record.get('document_id'): raise ValueError('close retained active document identity')
    if command=='file.import':
        if not record['data']['inserted_ids'] or not record['data']['root_affines']: raise ValueError('import lacks insertion/affine evidence')
        if record.get('undo_effect')!='one-step': raise ValueError('import not one Undo step')


def fixture_manifest(root):
    manifest=json.loads((Path(root)/'manifest.json').read_text())
    for item in manifest['fixtures']:
        path=Path(root)/item['path']
        if not item.get('provenance') or not item.get('oracle'): raise ValueError('fixture lacks provenance/oracle')
        if not path.is_file() or path.stat().st_size!=item['bytes'] or sha(path)!=item['sha256']: raise ValueError('fixture provenance/hash drift '+item['path'])
    actual={str(p.relative_to(root)) for p in Path(root).rglob('*') if p.is_file() and p.name!='manifest.json'}
    if actual!={item['path'] for item in manifest['fixtures']}: raise ValueError('fixture manifest incomplete')
    return manifest
