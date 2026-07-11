
probe_caller.o:     file format pe-x86-64


Disassembly of section .text:

0000000000000000 <mix_int_II>:
   0:	add    ecx,edx
   2:	sar    rdx,0x20
   6:	add    ecx,edx
   8:	lea    eax,[rcx+r8*1]
   c:	ret

000000000000000d <mix_ff_II>:
   d:	movaps xmm1,xmm0
  10:	pxor   xmm0,xmm0
  14:	cvtsi2ss xmm0,edx
  18:	addss  xmm0,xmm1
  1c:	sar    rdx,0x20
  20:	pxor   xmm1,xmm1
  24:	cvtsi2ss xmm1,edx
  28:	addss  xmm0,xmm1
  2c:	addss  xmm0,xmm2
  30:	ret

0000000000000031 <mix_d_ID>:
  31:	movapd xmm1,xmm0
  35:	pxor   xmm0,xmm0
  39:	cvtsi2sd xmm0,DWORD PTR [rdx]
  3d:	addsd  xmm0,xmm1
  41:	addsd  xmm0,QWORD PTR [rdx+0x8]
  46:	addsd  xmm0,xmm2
  4a:	ret

000000000000004b <mix_vec_scalar>:
  4b:	movss  xmm0,DWORD PTR [rcx]
  4f:	addss  xmm0,DWORD PTR [rcx+0x4]
  54:	addss  xmm0,DWORD PTR [rcx+0x8]
  59:	addss  xmm0,xmm1
  5d:	ret

000000000000005e <mix_scalar_vec>:
  5e:	addss  xmm0,DWORD PTR [rdx]
  62:	addss  xmm0,DWORD PTR [rdx+0x4]
  67:	addss  xmm0,DWORD PTR [rdx+0x8]
  6c:	ret

000000000000006d <make_big>:
  6d:	mov    rax,rcx
  70:	mov    DWORD PTR [rcx],edx
  72:	cvttsd2si edx,xmm2
  76:	mov    DWORD PTR [rcx+0x4],edx
  79:	cvttss2si edx,DWORD PTR [rsp+0x28]
  7f:	add    edx,r9d
  82:	mov    DWORD PTR [rcx+0x8],edx
  85:	ret

0000000000000086 <do_calls>:
  86:	push   rsi
  87:	push   rbx
  88:	sub    rsp,0x98
  8f:	movabs rbx,0x200000000
  99:	mov    rdx,rbx
  9c:	or     rdx,0x1
  a0:	mov    r8d,0x14
  a6:	mov    ecx,0xa
  ab:	call   0 <mix_int_II>
  b0:	mov    DWORD PTR [rsp+0x74],eax
  b4:	or     rbx,0x1
  b8:	mov    rdx,rbx
  bb:	movss  xmm2,DWORD PTR [rip+0xc]        # cf <do_calls+0x49>
  c3:	mov    ebx,DWORD PTR [rip+0x0]        # c9 <do_calls+0x43>
  c9:	movd   xmm0,ebx
  cd:	call   d <mix_ff_II>
  d2:	movss  DWORD PTR [rsp+0x70],xmm0
  d8:	mov    eax,0x0
  dd:	mov    ecx,eax
  df:	movabs r8,0xffffffff00000000
  e9:	and    rcx,r8
  ec:	or     rcx,0x1
  f0:	mov    rdx,QWORD PTR [rip+0x8]        # ff <do_calls+0x79>
  f7:	mov    QWORD PTR [rsp+0x40],rcx
  fc:	mov    QWORD PTR [rsp+0x48],rdx
 101:	lea    rdx,[rsp+0x40]
 106:	movsd  xmm2,QWORD PTR [rip+0x10]        # 11e <do_calls+0x98>
 10e:	movsd  xmm0,QWORD PTR [rip+0x18]        # 12e <do_calls+0xa8>
 116:	call   31 <mix_d_ID>
 11b:	movsd  QWORD PTR [rsp+0x68],xmm0
 121:	mov    DWORD PTR [rsp+0x78],ebx
 125:	mov    DWORD PTR [rsp+0x7c],0x40000000
 12d:	mov    DWORD PTR [rsp+0x80],0x40400000
 138:	mov    rax,QWORD PTR [rsp+0x78]
 13d:	mov    QWORD PTR [rsp+0x30],rax
 142:	mov    DWORD PTR [rsp+0x38],0x40400000
 14a:	lea    rsi,[rsp+0x30]
 14f:	movss  xmm1,DWORD PTR [rip+0x20]        # 177 <do_calls+0xf1>
 157:	mov    rcx,rsi
 15a:	call   4b <mix_vec_scalar>
 15f:	movss  DWORD PTR [rsp+0x64],xmm0
 165:	mov    DWORD PTR [rsp+0x84],ebx
 16c:	mov    DWORD PTR [rsp+0x88],0x40000000
 177:	mov    DWORD PTR [rsp+0x8c],0x40400000
 182:	mov    rax,QWORD PTR [rsp+0x84]
 18a:	mov    QWORD PTR [rsp+0x30],rax
 18f:	mov    DWORD PTR [rsp+0x38],0x40400000
 197:	mov    rdx,rsi
 19a:	movss  xmm0,DWORD PTR [rip+0x20]        # 1c2 <do_calls+0x13c>
 1a2:	call   5e <mix_scalar_vec>
 1a7:	movss  DWORD PTR [rsp+0x60],xmm0
 1ad:	lea    rcx,[rsp+0x54]
 1b2:	mov    DWORD PTR [rsp+0x20],0x40800000
 1ba:	mov    r9d,0x3
 1c0:	movsd  xmm2,QWORD PTR [rip+0x8]        # 1d0 <do_calls+0x14a>
 1c8:	mov    edx,0x1
 1cd:	call   6d <make_big>
 1d2:	mov    eax,DWORD PTR [rsp+0x74]
 1d6:	movss  xmm0,DWORD PTR [rsp+0x70]
 1dc:	movsd  xmm0,QWORD PTR [rsp+0x68]
 1e2:	movss  xmm0,DWORD PTR [rsp+0x64]
 1e8:	movss  xmm0,DWORD PTR [rsp+0x60]
 1ee:	add    rsp,0x98
 1f5:	pop    rbx
 1f6:	pop    rsi
 1f7:	ret
 1f8:	nop
 1f9:	nop
 1fa:	nop
 1fb:	nop
 1fc:	nop
 1fd:	nop
 1fe:	nop
 1ff:	nop
