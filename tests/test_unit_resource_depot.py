from pathlib import Path
import hashlib,json,sys,tempfile,unittest,zipfile
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT))
from owner.units.resource_depot import ResourceDepot,DepotError,INDEX_SCHEMA,PAGE_SCHEMA,CHUNK_LIMIT,RAW_LIMIT,FILE_LIMIT,PAGE_LIMIT

def raw(value):return json.dumps(value,sort_keys=True,separators=(',',':')).encode()+b'\n'
def sha(value):return hashlib.sha256(value).hexdigest()

class ResourceDepotTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup);self.root=Path(self.temp.name)
        self.revision='a'*64;self.payloads={sha(b'first'):b'first',sha(b'second'):b'second'}
        chunk=self.root/'chunks/0001.zip';chunk.parent.mkdir()
        with zipfile.ZipFile(chunk,'w',zipfile.ZIP_DEFLATED) as archive:
            for digest,payload in self.payloads.items():archive.writestr('blobs/'+digest+'.bin',payload)
        sections={'blobs':[{'sha256':digest,'bytes':len(payload),'chunk':1,'entry':'blobs/'+digest+'.bin'} for digest,payload in sorted(self.payloads.items())],
            'logicalBindings':[{'logical_path':'mdrs/model.mod','variants':[{'sha256':digest,'bytes':len(payload),'transport':'depot-blob'} for digest,payload in sorted(self.payloads.items())],
                'ambiguousLogicalPath':True}],
            'families':[{'sourceRootUnitId':90001,'exactSemanticClosureProven':False}],
            'logicalPathConflicts':[],'opaqueSourceGamePrograms':[]}
        fields={'blobs':'sha256','logicalBindings':'logical_path','families':'sourceRootUnitId'}
        self.index={'schema':INDEX_SCHEMA,'revision':self.revision,'authoringOnly':True,
            'idsReserved':False,'nativeActivationAuthorityIssued':False,'nativeGameplayAccepted':False,
            'nativeInstallerOrBundleSetConsumerAccepted':False,'sqlAuthorityIssued':False,'containsSqlOrOperationalScripts':False,
            'limits':{'compressedBytesPerChunk':CHUNK_LIMIT,'rawBytesPerChunk':RAW_LIMIT,'filesPerChunk':FILE_LIMIT},
            'metadataPageByteLimit':PAGE_LIMIT,'sectionCounts':{key:len(value) for key,value in sections.items()},
            'programActivationPolicy':{'activationAllowed':False,'registrationRequired':True,'originalLuaJitDirectExecutionAllowed':False},
            'chunks':[{'ordinal':1,'path':'chunks/0001.zip','sha256':sha(chunk.read_bytes()),'bytes':chunk.stat().st_size,'files':2,'rawBytes':sum(map(len,self.payloads.values()))}],
            'metadataPages':[]}
        (self.root/'metadata').mkdir()
        for section,rows in sections.items():
            if not rows:continue
            name='metadata/'+section+'-0000.jsonl';field=fields[section]
            lines=[raw({'schema':PAGE_SCHEMA,'revision':self.revision,'section':section,'ordinal':0}),*[raw(row) for row in rows]]
            content=b''.join(lines);(self.root/name).write_bytes(content)
            self.index['metadataPages'].append({'section':section,'ordinal':0,'path':name,'format':'jsonl-header-and-rows/1','rows':len(rows),'bytes':len(content),'sha256':sha(content),
                'maxLineBytes':max(map(len,lines)),'selector':{'keyField':field,'first':rows[0][field],'last':rows[-1][field]}})
        self.pin=self.save_index()

    def save_index(self):
        content=raw(self.index);(self.root/'index.json').write_bytes(content);return sha(content)

    def open(self):return ResourceDepot(self.root,index_sha256=self.pin)

    def rewrite_page(self,section,mutator):
        page=next(p for p in self.index['metadataPages'] if p['section']==section)
        path=self.root/page['path'];lines=path.read_bytes().splitlines();rows=[json.loads(line) for line in lines]
        mutator(rows);encoded=[raw(row) for row in rows];data=b''.join(encoded);path.write_bytes(data)
        page.update(bytes=len(data),sha256=sha(data),maxLineBytes=max(map(len,encoded)))
        self.pin=self.save_index()

    def test_streams_only_authenticated_blobs_and_retains_ambiguous_variants(self):
        with self.open() as depot:
            self.assertFalse(depot.lookup('families',90001)['exactSemanticClosureProven'])
            self.assertEqual(len(depot.lookup('logicalBindings','mdrs/model.mod')['variants']),2)
            for digest,payload in self.payloads.items():
                with depot.open_blob(digest) as stream:self.assertEqual(stream.read(),payload)
            self.assertEqual(list(depot.records('logicalPathConflicts')),[])
            copied=depot.index;copied['nativeGameplayAccepted']=True
            self.assertIs(depot.index['nativeGameplayAccepted'],False)
        with self.assertRaises(DepotError):depot.lookup('families',90001)

    def test_index_pin_and_authority_claims_are_not_trusted(self):
        with self.assertRaises(DepotError):ResourceDepot(self.root,index_sha256='b'*64)
        self.index['idsReserved']=True;self.pin=self.save_index()
        with self.assertRaisesRegex(DepotError,'authority'):self.open()

    def test_corrupt_page_is_rejected_before_rows_are_returned(self):
        page=self.index['metadataPages'][0];(self.root/page['path']).write_bytes(b'changed')
        with self.open() as depot:
            with self.assertRaises(DepotError):list(depot.records(page['section']))

    def test_metadata_header_and_real_key_order_must_match(self):
        self.rewrite_page('blobs',lambda rows:rows.reverse())
        with self.open() as depot:
            with self.assertRaises(DepotError):list(depot.records('blobs'))

    def test_forged_page_range_cannot_hide_rows(self):
        page=next(p for p in self.index['metadataPages'] if p['section']=='families');page['selector']['last']=90002;self.pin=self.save_index()
        with self.open() as depot:
            with self.assertRaises(DepotError):depot.lookup('families',90001)

    def test_chunk_and_blob_hashes_are_checked_independently(self):
        path=self.root/'chunks/0001.zip';path.write_bytes(path.read_bytes()+b'changed')
        with self.open() as depot:
            with self.assertRaises(DepotError):
                with depot.open_blob(next(iter(self.payloads))):self.fail('unverified stream exposed')

    def test_blob_size_and_entry_name_are_exact(self):
        self.rewrite_page('blobs',lambda rows:rows[1].update(entry='../escaped.bin'))
        with self.open() as depot:
            with self.assertRaises(DepotError):
                with depot.open_blob(sorted(self.payloads)[0]):self.fail('unsafe entry exposed')

    def test_metadata_page_counts_and_chunk_budgets_fail_closed(self):
        self.index['chunks'][0]['rawBytes']=RAW_LIMIT+1;self.pin=self.save_index()
        with self.assertRaises(DepotError):self.open()

    def test_path_traversal_is_rejected(self):
        self.index['chunks'][0]['path']='../outside.zip';self.pin=self.save_index()
        with self.assertRaises(DepotError):self.open()

    def test_linked_metadata_member_is_rejected(self):
        page=self.index['metadataPages'][0];path=self.root/page['path'];original=path.with_suffix('.original')
        path.rename(original)
        try:path.symlink_to(original)
        except OSError:
            original.rename(path);self.skipTest('symlink creation unavailable in this environment')
        with self.open() as depot:
            with self.assertRaises(DepotError):list(depot.records(page['section']))

    def test_zip_member_rejected_even_with_an_updated_chunk_digest(self):
        path=self.root/'chunks/0001.zip'
        with zipfile.ZipFile(path,'a') as archive:archive.writestr('../outside.bin',b'')
        chunk=self.index['chunks'][0];chunk.update(files=3,bytes=path.stat().st_size,sha256=sha(path.read_bytes()))
        self.pin=self.save_index()
        with self.open() as depot:
            with self.assertRaises(DepotError):
                with depot.open_blob(next(iter(self.payloads))):self.fail('unsafe ZIP accepted')

    def test_later_index_changes_cannot_rebind_an_existing_reader(self):
        with self.open() as depot:
            self.index['revision']='b'*64;self.save_index()
            with self.assertRaises(DepotError):depot.lookup('families',90001)

if __name__=='__main__':unittest.main()
