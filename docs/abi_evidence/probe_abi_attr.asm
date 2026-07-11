
probe_abi_attr.o:     file format pe-x86-64


Disassembly of section .text:

0000000000000000 <take_ID_ms>:
   0:	pxor   xmm0,xmm0
   4:	cvtsi2sd xmm0,DWORD PTR [rcx]
   8:	addsd  xmm0,QWORD PTR [rcx+0x8]
   d:	ret

000000000000000e <take_FF_ms>:
   e:	movd   xmm0,ecx
  12:	movq   xmm2,rcx
  17:	shufps xmm2,xmm2,0x55
  1b:	addss  xmm0,xmm2
  1f:	ret

0000000000000020 <take_ID_sysv>:
  20:	movq   rax,xmm0
  25:	pxor   xmm0,xmm0
  29:	cvtsi2sd xmm0,edi
  2d:	movq   xmm1,rax
  32:	addsd  xmm0,xmm1
  36:	ret

0000000000000037 <take_FF_sysv>:
  37:	movdqa xmm2,xmm0
  3b:	shufps xmm2,xmm2,0x55
  3f:	addss  xmm0,xmm2
  43:	ret

0000000000000044 <drive>:
  44:	push   rdi
  45:	push   rsi
  46:	push   rbx
  47:	sub    rsp,0xf0
  4e:	movups XMMWORD PTR [rsp+0x50],xmm6
  53:	movups XMMWORD PTR [rsp+0x60],xmm7
  58:	movups XMMWORD PTR [rsp+0x70],xmm8
  5e:	movups XMMWORD PTR [rsp+0x80],xmm9
  67:	movups XMMWORD PTR [rsp+0x90],xmm10
  70:	movups XMMWORD PTR [rsp+0xa0],xmm11
  79:	movups XMMWORD PTR [rsp+0xb0],xmm12
  82:	movups XMMWORD PTR [rsp+0xc0],xmm13
  8b:	movups XMMWORD PTR [rsp+0xd0],xmm14
  94:	movups XMMWORD PTR [rsp+0xe0],xmm15
  9d:	mov    eax,0x0
  a2:	mov    ecx,eax
  a4:	movabs rsi,0xffffffff00000000
  ae:	and    rcx,rsi
  b1:	or     rcx,0x1
  b5:	mov    rdx,QWORD PTR [rip+0x0]        # bc <drive+0x78>
  bc:	mov    QWORD PTR [rsp+0x20],rcx
  c1:	mov    QWORD PTR [rsp+0x28],rdx
  c6:	lea    rcx,[rsp+0x20]
  cb:	call   0 <take_ID_ms>
  d0:	movsd  QWORD PTR [rsp+0x48],xmm0
  d6:	movabs rbx,0x4000000000000000
  e0:	mov    rcx,rbx
  e3:	or     rcx,0x3f800000
  ea:	call   e <take_FF_ms>
  ef:	movss  DWORD PTR [rsp+0x44],xmm0
  f5:	mov    eax,0x0
  fa:	mov    ecx,eax
  fc:	and    rcx,rsi
  ff:	or     rcx,0x1
 103:	mov    rdx,QWORD PTR [rip+0x0]        # 10a <drive+0xc6>
 10a:	mov    edi,ecx
 10c:	movq   xmm0,rdx
 111:	call   20 <take_ID_sysv>
 116:	movsd  QWORD PTR [rsp+0x38],xmm0
 11c:	or     rbx,0x3f800000
 123:	movq   xmm0,rbx
 128:	call   37 <take_FF_sysv>
 12d:	movss  DWORD PTR [rsp+0x34],xmm0
 133:	movsd  xmm0,QWORD PTR [rsp+0x48]
 139:	movss  xmm0,DWORD PTR [rsp+0x44]
 13f:	movsd  xmm0,QWORD PTR [rsp+0x38]
 145:	movss  xmm0,DWORD PTR [rsp+0x34]
 14b:	movups xmm6,XMMWORD PTR [rsp+0x50]
 150:	movups xmm7,XMMWORD PTR [rsp+0x60]
 155:	movups xmm8,XMMWORD PTR [rsp+0x70]
 15b:	movups xmm9,XMMWORD PTR [rsp+0x80]
 164:	movups xmm10,XMMWORD PTR [rsp+0x90]
 16d:	movups xmm11,XMMWORD PTR [rsp+0xa0]
 176:	movups xmm12,XMMWORD PTR [rsp+0xb0]
 17f:	movups xmm13,XMMWORD PTR [rsp+0xc0]
 188:	movups xmm14,XMMWORD PTR [rsp+0xd0]
 191:	movups xmm15,XMMWORD PTR [rsp+0xe0]
 19a:	add    rsp,0xf0
 1a1:	pop    rbx
 1a2:	pop    rsi
 1a3:	pop    rdi
 1a4:	ret
 1a5:	nop
 1a6:	nop
 1a7:	nop
 1a8:	nop
 1a9:	nop
 1aa:	nop
 1ab:	nop
 1ac:	nop
 1ad:	nop
 1ae:	nop
 1af:	nop
