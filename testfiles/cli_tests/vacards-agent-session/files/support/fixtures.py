#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Reproducible M2 fixture authoring; never calls the implementation under test."""
import binascii
import ctypes
import ctypes.util
import gzip
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import zlib

TESTFILES=Path(__file__).resolve().parents[4]
ROOT=TESTFILES/'cli_tests/vacards-agent/fixtures/m2'


def chunk(kind,payload):
    return struct.pack('>I',len(payload))+kind+payload+struct.pack('>I',binascii.crc32(kind+payload)&0xffffffff)


def png_bytes(w,h,pixels):
    raw=b''.join(b'\0'+bytes(c for p in pixels[y*w:(y+1)*w] for c in p) for y in range(h))
    return b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',w,h,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(raw,9))+chunk(b'IEND',b'')


def pdf_bytes(missing_font=False):
    objects=[b'<< /Type /Catalog /Pages 2 0 R >>',b'<< /Type /Pages /Kids [3 0 R 5 0 R] /Count 2 >>']
    for content_id in (4,6):
        objects.append(b'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 12 6] /Resources << '+(b'/Font << /F1 7 0 R >>' if missing_font else b'')+b' >> /Contents '+str(content_id).encode()+b' 0 R >>')
        stream=b'1 0 0 rg 0 0 6 6 re f 0 1 0 rg 6 0 6 6 re f\n'
        if missing_font: stream+=b'BT /F1 3 Tf 1 1 Td (Missing font) Tj ET\n'
        objects.append(b'<< /Length '+str(len(stream)).encode()+b' >>\nstream\n'+stream+b'endstream')
    if missing_font: objects.append(b'<< /Type /Font /Subtype /Type1 /BaseFont /P9DefinitelyMissingFont >>')
    result=b'%PDF-1.4\n'; offsets=[0]
    for i,obj in enumerate(objects,1):
        offsets.append(len(result)); result+=str(i).encode()+b' 0 obj\n'+obj+b'\nendobj\n'
    xref=len(result); result+=b'xref\n0 '+str(len(offsets)).encode()+b'\n0000000000 65535 f \n'
    result+=b''.join(('%010d 00000 n \n'%o).encode() for o in offsets[1:])
    return result+b'trailer\n<< /Size '+str(len(offsets)).encode()+b' /Root 1 0 R >>\nstartxref\n'+str(xref).encode()+b'\n%%EOF\n'


def main():
    ROOT.mkdir(parents=True,exist_ok=True); entries=[]
    def write(name,data,provenance,oracle):
        (ROOT/name).parent.mkdir(parents=True,exist_ok=True)
        (ROOT/name).write_bytes(data)
        entries.append(dict(path=name,bytes=len(data),sha256=hashlib.sha256(data).hexdigest(),provenance=provenance,oracle=oracle))
    sheet=b'<svg xmlns="http://www.w3.org/2000/svg" width="16px" height="8px" viewBox="0 0 16 8"><title>P9 M2 sheet</title><rect id="red" x="0" y="0" width="8" height="8" fill="#ff0000"/><rect id="green" x="8" y="0" width="8" height="8" fill="#00ff00"/></svg>\n'
    known={'size_px':[16,8],'samples':[{'x':2,'y':4,'rgba':[255,0,0,255]},{'x':12,'y':4,'rgba':[0,255,0,255]}],'ids':['red','green']}
    write('sheet.svg',sheet,'P9 literal SVG, CSS 96 dpi, no fonts/resources',known)
    write('fonts/FreeSans.ttf',(TESTFILES/'rendering_tests/fonts/FreeSans.ttf').read_bytes(),'Copied unchanged from existing source/testfiles/rendering_tests/fonts/FreeSans.ttf',{'use':'isolated deterministic font, no user font discovery'})
    write('text.svg',b'<svg xmlns="http://www.w3.org/2000/svg" width="16" height="8"><text id="letter" x="2" y="6" font-family="FreeSans" font-size="6">A</text></svg>','P9 literal text using pinned FreeSans fixture',{'size_px':[16,8],'text':'A','font':'FreeSans'})
    write('sheet.svgz',gzip.compress(sheet,mtime=0),'P9 gzip(sheet.svg), zero timestamp',known)
    write('two-pages.pdf',pdf_bytes(),'P9 literal PDF objects/xref; two 12x6 pt pages',dict(known,pages=2))
    write('missing-font.pdf',pdf_bytes(True),'P9 PDF with deliberately nonexistent unembedded Type1 font',{'pages':2,'font':'P9DefinitelyMissingFont','policy':'reject or explicitly report substitution'})
    pixels=[(255,0,0,255) if x<8 else (0,255,0,128) for y in range(8) for x in range(16)]
    write('rgba.png',png_bytes(16,8,pixels),'P9 zlib/PNG RGBA8 literal samples',{'size_px':[16,8],'samples':[{'x':2,'y':4,'rgba':[255,0,0,255]},{'x':12,'y':4,'rgba':[0,255,0,128]}]})
    # Baseline TIFF: contiguous uncompressed RGBA8, unassociated alpha.
    tags=[(256,4,1,16),(257,4,1,8),(258,3,4,0),(259,3,1,1),(262,3,1,2),(273,4,1,0),(277,3,1,4),(278,4,1,8),(279,4,1,512),(284,3,1,1),(338,3,1,2)]
    extra=8+2+12*len(tags)+4; tags[2]=(258,3,4,extra); tags[5]=(273,4,1,extra+8)
    tiff=b'II*\0'+struct.pack('<I',8)+struct.pack('<H',len(tags))+b''.join(struct.pack('<HHII',*t) for t in tags)+struct.pack('<I',0)+struct.pack('<HHHH',8,8,8,8)+bytes(c for p in pixels for c in p)
    write('rgba.tiff',tiff,'P9 literal baseline TIFF, straight alpha',{'size_px':[16,8],'samples':[{'x':2,'y':4,'rgba':[255,0,0,255]},{'x':12,'y':4,'rgba':[0,255,0,128]}]})
    ppm=b'P6\n16 8\n255\n'+bytes(c for y in range(8) for x in range(16) for c in ((255,0,0) if x<8 else (0,255,0)))
    tool=shutil.which('cjpeg')
    if not tool: raise RuntimeError('fixture authoring requires existing native cjpeg')
    shot=subprocess.run([tool,'-quality','100','-sample','1x1'],input=ppm,capture_output=True,check=True)
    write('photo.jpg',shot.stdout,'P9 PPM stripes -> native cjpeg -quality 100 -sample 1x1; output pinned by hash',dict(known,tolerance=3))
    for src,dst,oracle in [('cli_tests/testcases/librevenge_formats/corel_draw2.cdr','sheet.cdr',{'reference':'sheet-cdr-reference.png','page_mm':[210,297],'qualification':'active upstream import_cdr2 CLI reference, full page at 96dpi; legacy corel_draw test is disabled for a libcdr regression'}),('cli_tests/testcases/librevenge_formats/corel_draw2_expected.png','sheet-cdr-reference.png',{'use':'independent active upstream full-page CDR reference at 96dpi','size_px':[794,1123]}),('data/colors/SwappedRedAndGreen.icc','custom-rgb.icc',{'use':'RGB profile; expected pixel transform from independent Little CMS'}),('data/colors/default_cmyk.icc','cmyk.icc',{'use':'must refuse CMYK output profile'})]:
        write(dst,(TESTFILES/src).read_bytes(),'Copied unchanged from source/testfiles/'+src,oracle)
    lib=ctypes.CDLL(ctypes.util.find_library('lcms2'))
    lib.cmsCreate_sRGBProfile.restype=ctypes.c_void_p; lib.cmsSaveProfileToMem.argtypes=[ctypes.c_void_p,ctypes.c_void_p,ctypes.POINTER(ctypes.c_uint32)]; lib.cmsCloseProfile.argtypes=[ctypes.c_void_p]
    profile=lib.cmsCreate_sRGBProfile(); n=ctypes.c_uint32()
    try:
        if not lib.cmsSaveProfileToMem(profile,None,ctypes.byref(n)): raise ValueError('sRGB profile sizing')
        data=ctypes.create_string_buffer(n.value)
        if not lib.cmsSaveProfileToMem(profile,data,ctypes.byref(n)): raise ValueError('sRGB profile bytes')
        raw=bytearray(data.raw); raw[24:36]=struct.pack('>6H',2020,1,1,0,0,0); raw[84:100]=b'\0'*16
        write('srgb.icc',bytes(raw),'Native cmsCreate_sRGBProfile; normalized date and zero optional profile ID',{'use':'exact immutable custom-file sRGB profile'})
    finally: lib.cmsCloseProfile(profile)
    write('bad-header.icc',b'bad ICC header','P9 literal invalid ICC',{'error':'profile-invalid'})
    # Deliberate over-limit compressed-on-disk fixture remains only 15 MiB+1.
    write('oversize.icc',b'\0'*(15*1024*1024+1),'P9 zeros, ICC bound plus one',{'error':'profile-invalid'})
    groups=b'<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd" width="40" height="30"><g id="parent" transform="translate(2,3)"><g id="child" transform="scale(2)"><rect id="tile" width="4" height="5" fill="red"/></g></g><use id="clone" xlink:href="#tile" x="20"/><g id="hidden" style="display:none"><circle id="hidden-circle" r="2"/></g><g id="locked" sodipodi:insensitive="true"><path id="locked-path" d="M 0,0 L 2,2"/></g><rect id="unrelated" x="30" y="20" width="2" height="2"/></svg>'
    write('groups.svg',groups,'P9 literal nested affine/clone/hidden/locked/unrelated SVG',{'ids':['parent','child','tile','clone','hidden','hidden-circle','locked','locked-path','unrelated'],'affines':{'parent':[1,0,0,1,2,3],'child':[2,0,0,2,0,0]}})
    linked=b'<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="16" height="8"><image id="raster" width="16" height="8" xlink:href="rgba.png"/></svg>'
    write('linked.svg',linked,'P9 local linked-image SVG',{'resource':'rgba.png','size_px':[16,8]})
    import base64
    write('embedded.svg',linked.replace(b'rgba.png',b'data:image/png;base64,'+base64.b64encode((ROOT/'rgba.png').read_bytes())),'P9 embedded variant of linked.svg',{'resource':'embedded PNG','size_px':[16,8]})
    cases={'remote.svg':sheet.replace(b'<title>',b'<image href="https://example.invalid/p9.png"/><title>'),
           'script.svg':sheet.replace(b'<title>',b'<script>alert(1)</script><title>'),
           'entity.svg':b'<!DOCTYPE svg [<!ENTITY x SYSTEM "file:///P9-ungranted-sentinel">]>'+sheet.replace(b'P9 M2 sheet',b'&x;'),
           'malformed.svgz':b'\x1f\x8bgarbage',
           'oversized-header.png':b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',2147483647,2147483647,8,6,0,0,0))+chunk(b'IEND',b''),
           'deep.svg':b'<svg xmlns="http://www.w3.org/2000/svg">'+b'<g>'*1025+b'</g>'*1025+b'</svg>',
           'objects-limit.svg':b'<svg xmlns="http://www.w3.org/2000/svg">'+b'<rect width="1" height="1"/>'*100001+b'</svg>',
           'empty.svg':b'<svg xmlns="http://www.w3.org/2000/svg" width="16" height="8"/>'}
    for name,data in cases.items(): write(name,data,'P9 literal hostile/boundary input, never fetched',{'use':'bounded refusal or explicitly supported empty no-op'})
    (ROOT/'manifest.json').write_text(json.dumps({'schema':'p9-m2-fixtures/1','producer':'files/support/fixtures.py','fixtures':entries},indent=2)+'\n')
    print('FIXTURES:',len(entries))

if __name__=='__main__': main()
