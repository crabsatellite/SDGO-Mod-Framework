"""Read a native importer capability without loading or executing its DLL."""
from __future__ import annotations
import struct

EXPORT=b'SdgoUnitDirectedFormsCapabilityV1'


def directed_forms(raw):
    """Return the checked family capacity; old or malformed binaries return zero."""
    try:
        if not isinstance(raw,bytes) or not 512<=len(raw)<=32*1024*1024 or raw[:2]!=b'MZ':return 0
        pe=struct.unpack_from('<I',raw,60)[0]
        if raw[pe:pe+4]!=b'PE\0\0':return 0
        machine,count=struct.unpack_from('<HH',raw,pe+4)
        optional=pe+24;size=struct.unpack_from('<H',raw,pe+20)[0]
        if machine!=0x14c or not 1<=count<=96 or size<104 or optional+size+count*40>len(raw):return 0
        if struct.unpack_from('<H',raw,optional)[0]!=0x10b or struct.unpack_from('<I',raw,optional+92)[0]<1:return 0
        sections=[]
        for i in range(count):
            offset=optional+size+i*40
            virtual_size,rva,extent,start=struct.unpack_from('<4I',raw,offset+8)
            flags=struct.unpack_from('<I',raw,offset+36)[0]
            if start+extent>len(raw):return 0
            sections.append((rva,extent,start,flags))
        def read(rva,size,readonly=False):
            matches=[s for s in sections if s[0]<=rva and rva+size<=s[0]+s[1]]
            if len(matches)!=1:raise ValueError('unmapped or ambiguous export')
            base,extent,start,flags=matches[0]
            if readonly and flags&0x80000000:raise ValueError('mutable capability')
            return raw[start+rva-base:start+rva-base+size]
        export,export_size=struct.unpack_from('<II',raw,optional+96)
        if not export or export_size<40:return 0
        functions,names,addresses,strings,ordinals=struct.unpack_from('<5I',read(export,40),20)
        if not 1<=names<=functions<=4096:return 0
        function_table=read(addresses,functions*4);name_table=read(strings,names*4);ordinal_table=read(ordinals,names*2)
        found=[]
        for i in range(names):
            name=struct.unpack_from('<I',name_table,i*4)[0]
            try:match=read(name,len(EXPORT)+1)==EXPORT+b'\0'
            except ValueError:continue
            if not match:continue
            ordinal=struct.unpack_from('<H',ordinal_table,i*2)[0]
            if ordinal>=functions:return 0
            target=struct.unpack_from('<I',function_table,ordinal*4)[0]
            if export<=target<export+export_size:return 0 # A forwarder is not capability data.
            found.append(read(target,32,readonly=True))
        if len(found)!=1:return 0
        magic,size,version,capacity,cached,reuse,cleanup=struct.unpack('<8s6I',found[0])
        return capacity if (magic==b'SDGODIR1' and size==32 and version==1 and
            2<=capacity<=64 and capacity<=cached<=512 and reuse==cleanup==1) else 0
    except (ValueError,struct.error,OverflowError):return 0
