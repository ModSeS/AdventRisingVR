#pragma once
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include "../shared/HudState.h"
namespace arvr {
class HudCanvas {
 HudData cached{};bool haveCached=false;uint64_t revision=0;
 HDC dc=nullptr;HBITMAP bitmap=nullptr;HGDIOBJ oldBitmap=nullptr;HFONT label=nullptr,value=nullptr;void* pixels=nullptr;
 void Text(int x,int y,int width,const wchar_t* text,HFONT font,COLORREF color){
  auto old=SelectObject(dc,font);SetTextColor(dc,color);RECT r{x,y,x+width,Height};DrawTextW(dc,text,-1,&r,DT_LEFT|DT_TOP|DT_SINGLELINE|DT_NOPREFIX);SelectObject(dc,old);
 }
 void Fill(RECT r,COLORREF color){auto brush=CreateSolidBrush(color);FillRect(dc,&r,brush);DeleteObject(brush);}
public:
 uint64_t Revision()const{return revision;}
 static constexpr int Width=768,Height=72;
 ~HudCanvas(){if(dc&&oldBitmap)SelectObject(dc,oldBitmap);if(bitmap)DeleteObject(bitmap);if(label)DeleteObject(label);if(value)DeleteObject(value);if(dc)DeleteDC(dc);}
 const uint8_t* Draw(const HudData& d){
  if(!dc){
   dc=CreateCompatibleDC(nullptr);if(!dc)return nullptr;
   BITMAPINFO bi{};bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=Width;bi.bmiHeader.biHeight=-Height;bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;
   bitmap=CreateDIBSection(dc,&bi,DIB_RGB_COLORS,&pixels,nullptr,0);if(!bitmap)return nullptr;oldBitmap=SelectObject(dc,bitmap);
   label=CreateFontW(-27,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Segoe UI");
   value=CreateFontW(-36,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Segoe UI");SetBkMode(dc,TRANSPARENT);
  }
  if(!pixels||!label||!value)return nullptr;
  if(haveCached && !memcmp(&cached.health,&d.health,20))return (const uint8_t*)pixels;
  cached=d;haveCached=true;

  Fill({0,0,Width,Height},RGB(0,0,0));
  const bool healthOk=std::isfinite(d.health)&&d.health>=0&&d.health<=1;
  const bool critical=healthOk&&d.health<.25f;
  wchar_t hp[32]=L"--";if(healthOk)swprintf_s(hp,L"%d%%",(int)std::lround(d.health*100));
  Text(18,20,35,L"+",label,RGB(220,220,220));Text(54,13,150,hp,value,critical?RGB(255,100,85):RGB(220,220,220));
  for(int side=0;side<2;++side){
   int clip=side?d.rightClip:d.leftClip,reserve=side?d.rightReserve:d.leftReserve;wchar_t ammo[48],c[16]=L"--",r[16]=L"--";
   if(clip>=0)swprintf_s(c,L"%d",clip);if(reserve>=0)swprintf_s(r,L"%d",reserve);swprintf_s(ammo,L"%s/%s",c,r);
   const int x=238+side*260;
   Text(x,20,36,side?L"\u041f":L"\u041b",label,RGB(190,190,190));
   Text(x+38,13,215,ammo,value,RGB(220,220,220));
  }
  GdiFlush();
  // GDI coverage on black becomes straight-alpha text. No rectangle is
  // submitted behind the glyphs. Preserve red only for critical health.
  auto b=(uint8_t*)pixels;
  for(int i=0;i<Width*Height;++i){
   auto p=b+i*4;unsigned coverage=std::max({p[0],p[1],p[2]});
   p[3]=(uint8_t)(coverage*(critical&&i%Width<220?.90f:.64f));
   if(coverage)for(int channel=0;channel<3;++channel)p[channel]=(uint8_t)(unsigned(p[channel])*255/coverage);
  }
  ++revision;
  GdiFlush();return (const uint8_t*)pixels;
 }
};
}
