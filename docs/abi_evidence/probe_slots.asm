
probe_slots.o:     file format pe-x86-64


Disassembly of section .text:

0000000000000000 <alt>:
   0:	add    ecx,r8d
   3:	pxor   xmm0,xmm0
   7:	cvtsi2ss xmm0,ecx
   b:	addss  xmm0,xmm1
   f:	addss  xmm0,xmm3
  13:	ret

0000000000000014 <altf>:
  14:	add    edx,r9d
  17:	pxor   xmm1,xmm1
  1b:	cvtsi2ss xmm1,edx
  1f:	addss  xmm0,xmm2
  23:	addss  xmm0,xmm1
  27:	ret

0000000000000028 <five>:
  28:	add    ecx,r8d
  2b:	add    ecx,DWORD PTR [rsp+0x28]
  2f:	pxor   xmm0,xmm0
  33:	cvtsi2ss xmm0,ecx
  37:	addss  xmm0,xmm1
  3b:	addss  xmm0,xmm3
  3f:	ret

0000000000000040 <two_II>:
  40:	mov    rax,rcx
  43:	shr    rax,0x20
  47:	lea    eax,[rax+rcx*1]
  4a:	add    eax,edx
  4c:	sar    rdx,0x20
  50:	add    eax,edx
  52:	ret

0000000000000053 <drive>:
  53:	sub    rsp,0x48
  57:	movss  xmm3,DWORD PTR [rip+0x0]        # 5f <drive+0xc>
  5f:	mov    r8d,0x3
  65:	movss  xmm1,DWORD PTR [rip+0x4]        # 71 <drive+0x1e>
  6d:	mov    ecx,0x1
  72:	call   0 <alt>
  77:	movss  DWORD PTR [rsp+0x3c],xmm0
  7d:	mov    r9d,0x4
  83:	movss  xmm2,DWORD PTR [rip+0x8]        # 93 <drive+0x40>
  8b:	mov    edx,0x2
  90:	movss  xmm0,DWORD PTR [rip+0xc]        # a4 <drive+0x51>
  98:	call   14 <altf>
  9d:	movss  DWORD PTR [rsp+0x38],xmm0
  a3:	mov    DWORD PTR [rsp+0x20],0x5
  ab:	movss  xmm3,DWORD PTR [rip+0x0]        # b3 <drive+0x60>
  b3:	mov    r8d,0x3
  b9:	movss  xmm1,DWORD PTR [rip+0x4]        # c5 <drive+0x72>
  c1:	mov    ecx,0x1
  c6:	call   28 <five>
  cb:	movss  DWORD PTR [rsp+0x34],xmm0
  d1:	movabs rdx,0x400000003
  db:	movabs rcx,0x200000001
  e5:	call   40 <two_II>
  ea:	mov    DWORD PTR [rsp+0x30],eax
  ee:	movss  xmm0,DWORD PTR [rsp+0x3c]
  f4:	movss  xmm0,DWORD PTR [rsp+0x38]
  fa:	movss  xmm0,DWORD PTR [rsp+0x34]
 100:	mov    eax,DWORD PTR [rsp+0x30]
 104:	add    rsp,0x48
 108:	ret
 109:	nop
 10a:	nop
 10b:	nop
 10c:	nop
 10d:	nop
 10e:	nop
 10f:	nop
