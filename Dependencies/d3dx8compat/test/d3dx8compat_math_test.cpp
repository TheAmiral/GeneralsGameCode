#include <windows.h>
#include <d3d8.h>
#include <d3dx8.h>
#include <stdio.h>
#include <math.h>
int fails=0;
#define CHECK(c) do{ if(!(c)){printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c);fails++;} }while(0)
static bool approx(float a,float b){return fabsf(a-b)<1e-4f;}
int main(){
  D3DXMATRIX a,b,c,inv,id;
  float vals[16]={2,0,0,0, 0,3,0,0, 0,0,4,0, 5,6,7,1};
  memcpy(a.m,vals,sizeof vals);
  float det; CHECK(D3DXMatrixInverse(&inv,&det,&a)); CHECK(approx(det,24));
  D3DXMatrixMultiply(&c,&a,&inv);
  for(int r=0;r<4;r++)for(int k=0;k<4;k++) CHECK(approx(c.m[r][k], r==k?1.f:0.f));
  // general matrix
  float g[16]={1,2,3,4, 0,1,5,6, 7,0,1,2, 3,4,0,1};
  memcpy(b.m,g,sizeof g); CHECK(D3DXMatrixInverse(&inv,&det,&b)); D3DXMatrixMultiply(&c,&inv,&b);
  for(int r=0;r<4;r++)for(int k=0;k<4;k++) CHECK(approx(c.m[r][k], r==k?1.f:0.f));
  CHECK(approx(det, D3DXMatrixfDeterminant(&b)));
  D3DXMatrixTranslation(&c,1,2,3); D3DXVECTOR4 v(1,1,1,1),o; D3DXVec4Transform(&o,&v,&c);
  CHECK(approx(o.x,2)&&approx(o.y,3)&&approx(o.z,4)&&approx(o.w,1));
  D3DXMatrixRotationZ(&c,(float)M_PI/2); D3DXVECTOR4 x(1,0,0,1); D3DXVec4Transform(&o,&x,&c);
  CHECK(approx(o.x,0)&&approx(o.y,1));
  D3DXMatrixScaling(&c,2,3,4); D3DXVECTOR3 p(1,1,1); D3DXVec3Transform(&o,&p,&c); CHECK(approx(o.x,2)&&approx(o.y,3)&&approx(o.z,4)&&approx(o.w,1));
  D3DXMatrixTranspose(&c,&b); CHECK(c.m[0][1]==b.m[1][0]);
  float s[16]={0}; D3DXMATRIX z; memcpy(z.m,s,sizeof s); CHECK(D3DXMatrixInverse(&inv,nullptr,&z)==nullptr);
  CHECK(D3DXGetFVFVertexSize(D3DFVF_XYZ|D3DFVF_DIFFUSE|D3DFVF_TEX1)==24);
  CHECK(D3DXGetFVFVertexSize(D3DFVF_XYZRHW|D3DFVF_DIFFUSE|D3DFVF_SPECULAR|D3DFVF_TEX2)==40);
  CHECK(D3DXGetFVFVertexSize(D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_TEX1|D3DFVF_TEXCOORDSIZE3(0))==36);
  printf("sizeof(void*)=%d fails=%d\n",(int)sizeof(void*),fails); return fails;
}
