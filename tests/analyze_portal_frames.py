"""Summarize captured transition frames without changing game assets."""
import csv,json,sys
from pathlib import Path
from PIL import Image,ImageChops,ImageDraw,ImageFilter,ImageStat

folder=Path(sys.argv[1])
rooms={int(x['frame']):x['room'] for x in csv.DictReader((folder/'logs/render.csv').open())}
rows=[];previous=None
for path in sorted((folder/'shots').glob('shot_*.png'),key=lambda p:int(p.stem.split('_')[1])):
    frame=int(path.stem.split('_')[1])
    with Image.open(path) as source:thumb=source.convert('RGB').resize((128,72))
    gray=thumb.convert('L');mean=ImageStat.Stat(gray).mean[0]
    high=sum(gray.histogram()[220:])/(gray.width*gray.height)
    delta=ImageStat.Stat(ImageChops.difference(thumb,previous)).mean if previous else [0,0,0]
    rows.append(dict(frame=frame,room=rooms.get(frame,''),mean=round(mean,2),bright=round(high,4),delta=round(sum(delta)/3,2)))
    previous=thumb
with (folder/'frame-brightness.csv').open('w',newline='') as f:
    writer=csv.DictWriter(f,fieldnames=list(rows[0]));writer.writeheader();writer.writerows(rows)
top=sorted(rows,key=lambda x:x['mean'],reverse=True)[:12]
jumps=sorted(rows,key=lambda x:x['delta'],reverse=True)[:12]
transitions=[row for i,row in enumerate(rows) if i and row['room']!=rows[i-1]['room']]
frames=sorted(set(x['frame'] for x in top+jumps+transitions))
sheet=Image.new('RGB',(256*4,164*((len(frames)+3)//4)),(20,20,24));draw=ImageDraw.Draw(sheet)
for i,frame in enumerate(frames):
    with Image.open(folder/f'shots/shot_{frame}.png') as source:thumb=source.convert('RGB').resize((256,144))
    x=i%4*256;y=i//4*164;sheet.paste(thumb,(x,y));draw.text((x+4,y+145),f'{frame}: {rooms.get(frame,"")}',fill='white')
sheet.save(folder/'transition-contact.png')
print(json.dumps(dict(brightest=top[:6],changes=jumps[:6],transitions=transitions),indent=2))
