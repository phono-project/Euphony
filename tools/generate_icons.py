"""Export original SVG rectangles/polygons as PNG and multi-size ICO."""
from pathlib import Path
import math
import struct
import xml.etree.ElementTree as ET
import zlib
ROOT = Path(__file__).resolve().parents[1]

def png(shapes, size):
    scale = size / 128
    pixels = bytearray(size * size * 4)
    for x,y,width,height,radius,color,points in shapes:
        x,y,width,height,radius = (v*scale for v in (x,y,width,height,radius))
        cx,cy = x+width/2,y+height/2
        for row in range(max(0,math.floor(y)),min(size,math.ceil(y+height))):
            for col in range(max(0,math.floor(x)),min(size,math.ceil(x+width))):
                px,py = col+.5,row+.5
                if points:
                    inside,distance = False,float('inf')
                    for index,(ax,ay) in enumerate(points):
                        bx,by = points[(index+1)%len(points)]
                        ax,ay,bx,by = ax*scale,ay*scale,bx*scale,by*scale
                        dx,dy = bx-ax,by-ay
                        fraction = max(0,min(1,((px-ax)*dx+(py-ay)*dy)/(dx*dx+dy*dy)))
                        distance = min(distance,math.hypot(px-ax-fraction*dx,py-ay-fraction*dy))
                        if (ay>py)!=(by>py) and px<(bx-ax)*(py-ay)/(by-ay)+ax:
                            inside = not inside
                    if inside: distance = -distance
                else:
                    qx,qy = abs(px-cx)-(width/2-radius),abs(py-cy)-(height/2-radius)
                    distance = math.hypot(max(qx,0),max(qy,0))+min(max(qx,qy),0)-radius
                alpha = max(0,min(1,.5-distance))
                if not alpha: continue
                offset = (row*size+col)*4
                old_alpha = pixels[offset+3]/255
                combined = alpha+old_alpha*(1-alpha)
                for channel in range(3):
                    pixels[offset+channel] = round((color[channel]*alpha+pixels[offset+channel]*old_alpha*(1-alpha))/combined)
                pixels[offset+3] = round(combined*255)
    raw = b''.join(b'\0'+pixels[r*size*4:(r+1)*size*4] for r in range(size))
    def chunk(kind,data):
        # PNG requires a CRC; this is not artifact verification.
        return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data))
    return b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',size,size,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(raw))+chunk(b'IEND',b'')

for name in ('euphony','mode-chinese','mode-english'):
    shapes = []
    for node in ET.parse(ROOT/f'img/{name}.svg').getroot():
        if not node.tag.endswith(('rect','polygon')): continue
        color = bytes.fromhex(node.attrib['fill'][1:])
        if node.tag.endswith('rect'):
            shapes.append((*(float(node.attrib[k]) for k in ('x','y','width','height','rx')),color,None))
        elif node.tag.endswith('polygon'):
            points = [tuple(map(float,p.split(','))) for p in node.attrib['points'].split()]
            xs,ys = zip(*points)
            shapes.append((min(xs),min(ys),max(xs)-min(xs),max(ys)-min(ys),0,color,points))
    sizes = (16,20,24,32,40,48,64,128,256)
    frames = [png(shapes,size) for size in sizes]
    offset = 6+len(frames)*16
    entries = []
    for size,frame in zip(sizes,frames):
        entries.append(struct.pack('<BBBBHHII',size%256,size%256,0,0,1,32,len(frame),offset))
        offset += len(frame)
    (ROOT/f'img/{name}.ico').write_bytes(struct.pack('<HHH',0,1,len(frames))+b''.join(entries+frames))
    (ROOT/f'img/{name}.png').write_bytes(png(shapes,512))
    print(f'Exported {name} PNG / ICO from original SVG')
