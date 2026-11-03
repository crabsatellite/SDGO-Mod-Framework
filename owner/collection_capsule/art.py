"""Prepare collection activity-capsule cover art for the native poster slot."""
from pathlib import Path

from .client import COVER_SIZE

def render_cover(source:Path,output:Path,rules:dict,*,size=COVER_SIZE):
    """Add styled rule lettering only; retain the supplied illustration."""
    from PIL import Image,ImageDraw,ImageFont,ImageFilter,ImageChops
    from .rules import validate
    rules=validate(rules)
    canvas_size=(1444,1288)
    with Image.open(source) as im:
        if im.width<COVER_SIZE[0] or im.height<COVER_SIZE[1]:raise ValueError('Collection cover is too small')
        image=im.convert('RGBA').resize(canvas_size,Image.Resampling.LANCZOS)
    candidates=[Path('C:/Windows/Fonts')/n for n in ('NotoSansSC-VF.ttf','msyhbd.ttc','msjh.ttc','simhei.ttf','meiryo.ttc')]
    font_path=next((p for p in candidates if p.is_file()),None);chinese=font_path is not None
    def font_at(height):
        try:font=ImageFont.truetype(str(font_path or 'arialbd.ttf'),height)
        except OSError:font=ImageFont.load_default(size=height)
        try:font.set_variation_by_axes([900])
        except (OSError,ValueError,AttributeError):pass
        return font
    def lettering(value,position,height,width):
        font=font_at(height);box=font.getbbox(value)
        while box[2]-box[0]>width-50 and height>30:
            height-=1;font=font_at(height);box=font.getbbox(value)
        mask=Image.new('L',(box[2]-box[0]+48,box[3]-box[1]+48));ImageDraw.Draw(mask).text((24-box[0],24-box[1]),value,font=font,fill=255)
        h=mask.height;w=mask.width+int(.18*h)
        mask=mask.transform((w,h),Image.Transform.AFFINE,(1,.18,-.18*h,0,1,0),resample=Image.Resampling.BICUBIC)
        layer=Image.new('RGBA',mask.size)
        glow=mask.filter(ImageFilter.GaussianBlur(8)).point(lambda v:min(180,v))
        layer.paste((0,134,255,255),(0,0),glow)
        extrusion=ImageChops.offset(mask,5,7).filter(ImageFilter.MaxFilter(13));layer.paste((2,22,68,255),(0,0),extrusion)
        layer.paste((0,100,255,255),(0,0),mask.filter(ImageFilter.MaxFilter(11)))
        layer.paste((179,244,255,255),(0,0),mask.filter(ImageFilter.MaxFilter(3)))
        gradient=Image.new('RGBA',mask.size);draw=ImageDraw.Draw(gradient)
        for y in range(h):
            fraction=max(0,min(1,(y-24)/max(1,h-48)))
            color=(int(255-125*fraction),int(255-35*fraction),255,255);draw.line((0,y,w,y),fill=color)
        layer.paste(gradient,(0,0),mask);image.alpha_composite(layer,position)
    lettering(f"{rules['price_gp']:,} GB / "+('次' if chinese else 'DRAW'),(16,1110),84,720)
    limited=rules['limit_period']!='none' and rules['draw_limit']>0
    if limited:
        label=(('每周' if rules['limit_period']=='week' else '每日')+f" {rules['draw_limit']} 次 · 可集中使用") if chinese else f"{rules['draw_limit']} / "+('WEEK' if rules['limit_period']=='week' else 'DAY')
    else:label='不限次数' if chinese else 'UNLIMITED'
    lettering(label,(28,1214),39,700)
    output=Path(output);output.parent.mkdir(parents=True,exist_ok=True)
    image.convert('RGB').resize(size,Image.Resampling.LANCZOS).save(output,'PNG',compress_level=9)
    return output


def prepare_cover(source: Path, output: Path) -> None:
    """Scale a complete supplied poster to the native 361x322 sprite frame."""
    from PIL import Image

    source = Path(source)
    output = Path(output)
    with Image.open(source) as opened:
        opened.load()
        if opened.width < COVER_SIZE[0] or opened.height < COVER_SIZE[1]:
            raise ValueError(
                f"Cover source must be at least {COVER_SIZE[0]}x{COVER_SIZE[1]}"
            )
        image = opened.convert("RGB").resize(
            COVER_SIZE, Image.Resampling.LANCZOS
        )
    output.parent.mkdir(parents=True, exist_ok=True)
    image.save(output, "PNG", optimize=False, compress_level=9)
