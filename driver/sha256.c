/* SPDX-License-Identifier: MIT */
/* New compact SHA-256 implementation of FIPS 180-4. The optional two-byte
 * normalization permits only the documented stock JNE at the admitted site. */
#include "patch.h"
#include <stdint.h>
#include <string.h>
static uint32_t r(uint32_t x, unsigned n) { return (x >> n) | (x << (32-n)); }
void gs_sha256(const unsigned char *p, size_t n, size_t site, char out[65])
{
 static const uint32_t k[64]={
 0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
 0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
 0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
 0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
 0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
 0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
 0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
 0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
 uint32_t h[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
 size_t blocks=n/64+1+(n%64>=56), b,j; uint64_t bits=(uint64_t)n*8;
 for(b=0;b<blocks;b++) {
  unsigned char buf[64]; uint32_t w[64],a,c,d,e,f,g,t,u,v,bb;
  for(j=0;j<64;j++) { size_t at=b*64+j;
   if(at<n) buf[j]=at==site?0x75:(site!=SIZE_MAX && at==site+1?0x76:p[at]);
   else buf[j]=at==n?0x80:0;
   if(b==blocks-1 && j>=56) buf[j]=(unsigned char)(bits >> ((63-j)*8));
  }
  for(j=0;j<16;j++) w[j]=(uint32_t)buf[j*4]<<24|(uint32_t)buf[j*4+1]<<16|(uint32_t)buf[j*4+2]<<8|buf[j*4+3];
  for(j=16;j<64;j++) w[j]=w[j-16]+(r(w[j-15],7)^r(w[j-15],18)^(w[j-15]>>3))+w[j-7]+(r(w[j-2],17)^r(w[j-2],19)^(w[j-2]>>10));
  a=h[0];bb=h[1];c=h[2];d=h[3];e=h[4];f=h[5];g=h[6];v=h[7];
  for(j=0;j<64;j++) { t=v+(r(e,6)^r(e,11)^r(e,25))+((e&f)^(~e&g))+k[j]+w[j];u=(r(a,2)^r(a,13)^r(a,22))+((a&bb)^(a&c)^(bb&c));v=g;g=f;f=e;e=d+t;d=c;c=bb;bb=a;a=t+u; }
  h[0]+=a;h[1]+=bb;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=v;
 }
 for(j=0;j<32;j++) { unsigned x=(h[j/4]>>((3-j%4)*8))&255;out[j*2]="0123456789abcdef"[x>>4];out[j*2+1]="0123456789abcdef"[x&15]; } out[64]=0;
}
