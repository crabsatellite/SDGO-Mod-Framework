import copy
import hashlib
import io
import struct
import zipfile

from owner.units import model


def wchar(raw,offset,text):
    value=text.encode('utf-16-le');raw[offset:offset+len(value)]=value


def unit(identity=90001,target=0):
    raw=bytearray(4976);struct.pack_into('<I',raw,0,identity);wchar(raw,5,'测试机体')
    raw[308]=1;raw[309]=2;raw[313]=2;struct.pack_into('<I',raw,316,target);struct.pack_into('<I',raw,321,10000)
    struct.pack_into('<IIII',raw,585,90010,0,0,90011);wchar(raw,612,'mdrs/test.mod')
    return bytes(raw)


def weapon(identity):
    raw=bytearray(1871);struct.pack_into('<I',raw,0,identity);wchar(raw,5,'测试武器');return bytes(raw)


def archive(identity=90001):
    out=io.BytesIO()
    with zipfile.ZipFile(out,'w',zipfile.ZIP_DEFLATED) as z:
        def write(name,raw):
            info=zipfile.ZipInfo(name,date_time=(2000,1,1,0,0,0));info.compress_type=zipfile.ZIP_DEFLATED
            z.writestr(info,raw)
        write('gdrs/unit.bin',unit(identity));write('gdrs/first.bin',weapon(90010));write('gdrs/special.bin',weapon(90011))
        write('mdrs/test.mod',b'\xe9\xc6\0\0ZOA\0\x05'+bytes(55))
    return out.getvalue()


def png(color='orange'):
    from PIL import Image
    out=io.BytesIO();Image.new('RGBA',(32,24),color).save(out,format='PNG');return out.getvalue()


class Baseline:
    def unit_index(self):
        raw=bytearray(614);struct.pack_into('<I',raw,0,0x375BE1);raw[5]=1
        struct.pack_into('<I',raw,6,10001)
        return {0x375BE1:bytes(raw)}
    def exists(self,path):return False
    def read(self,path):raise KeyError(path)
    def any_id(self,identity):return identity in {10001,41001,1110001}
    def portrait(self,path):raise KeyError(path)
    def record(self,kind,identity):
        if kind=='item' and identity==1110001:
            raw=bytearray(614);struct.pack_into('<I',raw,0,identity);wchar(raw,5,'原生模板');struct.pack_into('<I',raw,105,10001);return bytes(raw)
        return None
    def public_info(self):return {'client_configured':True,'fixture':True}


class Repository:
    def __init__(self,configured=False,fail=False):
        self.configured=configured;self.fail=fail;self.applied=[]
        u={name:(0.0 if name in model.FLOAT_FIELDS else '' if name in model.TEXT_FIELDS else 0) for name in model.UNIT_COLUMNS}
        u.update(UnitNumber=10001,UnitModelName='Native Unit',UnitGradeNumber=1,UnitGradeNumber2=2,HP=10000,UnitGrowIndex=1)
        w={name:('Native Weapon' if name=='WeaponName' else 1.0 if name=='AttackSpeed' else 0) for name in model.WEAPON_COLUMNS};w['WeaponNo']=41001
        self.catalog={'units':{10001:u},'weapons':{41001:w},'items':{1110001:{'CatalogueID':1110001,'Name':'原生机体','RefIndex':10001,'TimeLimit':0,'Option1':1,'Option2':2}},'names':{},'grades':{(1,2)},'level_indices':{1},'skill_ids':set()}
    def public_info(self):return {'server_configured':self.configured,'snapshot_configured':True}
    def check_plan(self,plan):
        for row in plan['units']:assert set(row)==set(model.UNIT_COLUMNS)
        for row in plan['weapons']:assert set(row)==set(model.WEAPON_COLUMNS)
        assert plan['item']['RefIndex']==plan['root_unit_id']
    def apply(self,plan):
        if self.fail:raise ValueError('模拟数据库冲突，事务已拒绝')
        self.applied.append(copy.deepcopy(plan));return {'applied':True,'restart_required':True}


def publisher(workspace,state,selected,transaction):
    path=transaction/'client.zip'
    with zipfile.ZipFile(path,'w') as z:z.writestr('confirmed.txt',state['drafts'][selected]['name'])
    return {'client_zip':str(path),'client_zip_sha256':hashlib.sha256(path.read_bytes()).hexdigest(),'revision':'fixture'}


def complete(workspace,state):
    d=state['draft'];identity=d['id']
    patch={'item_template_id':1110001,'forms':[{'id':r['id'],'template_id':10001} for r in d['forms']],
           'weapons':[{'id':r['id'],'template_id':41001} for r in d['weapons']]}
    state=workspace.update(identity,patch,state['revision'])
    state=workspace.upload_asset(identity,'shop.png',png(),'shop',state['revision'])
    state=workspace.upload_asset(identity,'capsule.png',png('blue'),'capsule',state['revision'])
    return state
