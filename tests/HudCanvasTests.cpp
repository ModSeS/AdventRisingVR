#include "../src/bridge/HudCanvas.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
int main(int argc,char**argv){
 arvr::HudCanvas c;arvr::HudData d{};d.health=.73f;d.leftClip=12;d.leftReserve=90;d.rightClip=7;d.rightReserve=40;
 const auto* pixels=c.Draw(d);if(!pixels)return 1;
 unsigned transparent=0;for(int i=0;i<c.Width*c.Height;i++)if(pixels[i*4+3]==0)++transparent;
 if(transparent<c.Width*c.Height*3/4)return 5;
 const auto revision=c.Revision();c.Draw(d);if(c.Revision()!=revision)return 6;
 unsigned bright=0;for(int i=0;i<c.Width*c.Height;i++)if(pixels[i*4]>180)++bright;if(bright<300)return 2;
 if(argc>1){FILE* f=nullptr;fopen_s(&f,argv[1],"wb");if(!f)return 3;BITMAPFILEHEADER h{};h.bfType=0x4d42;h.bfOffBits=sizeof(h)+sizeof(BITMAPINFOHEADER);h.bfSize=h.bfOffBits+c.Width*c.Height*4;BITMAPINFOHEADER bi{};bi.biSize=sizeof(bi);bi.biWidth=c.Width;bi.biHeight=-c.Height;bi.biPlanes=1;bi.biBitCount=32;fwrite(&h,sizeof(h),1,f);fwrite(&bi,sizeof(bi),1,f);fwrite(pixels,c.Width*c.Height*4,1,f);fclose(f);}
 d.health=0;d.leftClip=-1;d.rightClip=0;if(!c.Draw(d))return 4;
 puts("PASS: HUD GDI raster, visible text and empty/depleted states");
}
