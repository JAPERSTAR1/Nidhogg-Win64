"""Verify the observed full-white flash and compare room/stage transition captures."""
import json,sys
from pathlib import Path
from PIL import Image,ImageDraw

before,after=map(Path,sys.argv[1:3])
def white_frames(folder):
    result=[]
    for path in (folder/'shots').glob('shot_*.png'):
        with Image.open(path) as source:
            if all(low==255 for low,high in source.convert('RGB').getextrema()):
                result.append(int(path.stem.split('_')[1]))
    return sorted(result)
a=json.loads((before/'report.json').read_text());b=json.loads((after/'report.json').read_text())
original_white=white_frames(before);reduced_white=white_frames(after)
checks=dict(original_flash_reproduced=551 in original_white,no_full_white_frames=not reduced_white,
    clean_exit=a['exit']==b['exit']==0 and not a['fault'] and not b['fault'],
    same_room_progression=a['rooms']==b['rooms'],match_completed=a['winner']==b['winner'] and bool(b['winner']),
    compared_2001_frames=len(a['screenshots'])==len(b['screenshots'])==2001)
report=dict(checks=checks,beforeWhiteFrames=original_white,afterWhiteFrames=reduced_white)
(after/'portal-verification.json').write_text(json.dumps(report,indent=2))
frames=[551,1035,1518,2389]
sheet=Image.new('RGB',(768,236*len(frames)),(18,18,24));draw=ImageDraw.Draw(sheet)
for row,frame in enumerate(frames):
    for col,folder in enumerate([before,after]):
        with Image.open(folder/f'shots/shot_{frame}.png') as source:
            sheet.paste(source.convert('RGB').resize((384,216)),(col*384,row*236))
        draw.text((col*384+8,row*236+218),f'{"ORIGINAL FX" if col==0 else "FX OFF"} - frame {frame}',fill='white')
sheet.save(after/'before-after.png')
print(json.dumps(report,indent=2));assert all(checks.values())
