#include "MathVR.h"
#include <cmath>
namespace arvr {
static constexpr float PI=3.14159265358979323846f; static int U(float r){return (int)std::lround(r*(65536.0f/(2.0f*PI)));}
Pose YawOnlyOrigin(const Pose& p){
 Pose o=p;float yaw=std::atan2(2*(p.qw*p.qy+p.qx*p.qz),1-2*(p.qy*p.qy+p.qx*p.qx));
 o.qx=0;o.qz=0;o.qy=std::sin(yaw*.5f);o.qw=std::cos(yaw*.5f);return o;
}
Quat Conjugate(Quat q){return {-q.x,-q.y,-q.z,q.w};}
Quat Mul(Quat a,Quat b){return {a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};}
FVector Rotate(Quat q,FVector v){Quat p{v.X,v.Y,v.Z,0},r=Mul(Mul(q,p),Conjugate(q));return {r.x,r.y,r.z};}
static FVector Map(FVector x){return {-x.Z,x.X,x.Y};}
static Quat Q(const Pose&p){return {p.qx,p.qy,p.qz,p.qw};}
FRotator RelativePoseToUnrealRotation(const Pose&o,const Pose&c,float ps,float ys,float rs){Quat rel=Mul(Conjugate(Q(o)),Q(c));
 FVector f=Map(Rotate(rel,{0,0,-1})), r=Map(Rotate(rel,{1,0,0})), u=Map(Rotate(rel,{0,1,0}));
 float yaw=std::atan2(f.Y,f.X), pitch=std::asin(fmaxf(-1.0f,fminf(1.0f,f.Z))), roll=std::atan2(-r.Z,u.Z);
 return {U(pitch*ps),U(yaw*ys),U(roll*rs)}; }
FVector RelativePoseToUnrealPosition(const Pose&o,const Pose&c,float upm,int baseYaw){FVector d{c.px-o.px,c.py-o.py,c.pz-o.pz}; d=Rotate(Conjugate(Q(o)),d); d=Map(d); float y=baseYaw*(2*PI/65536.0f),cs=cosf(y),sn=sinf(y); return {(d.X*cs-d.Y*sn)*upm,(d.X*sn+d.Y*cs)*upm,d.Z*upm};}
}
