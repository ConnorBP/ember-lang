
probe_c.o:     file format pe-x86-64


Disassembly of section .text:

0000000000000000 <take_II>:
   0:	mov    rax,rcx
   3:	shr    rax,0x20
   7:	lea    eax,[rax+rcx*1]
   a:	ret

000000000000000b <take_FF>:
   b:	movd   xmm0,ecx
   f:	movq   xmm2,rcx
  14:	shufps xmm2,xmm2,0x55
  18:	addss  xmm0,xmm2
  1c:	ret

000000000000001d <take_IF>:
  1d:	pxor   xmm0,xmm0
  21:	cvtsi2ss xmm0,ecx
  25:	movq   xmm2,rcx
  2a:	shufps xmm2,xmm2,0x55
  2e:	addss  xmm0,xmm2
  32:	ret

0000000000000033 <take_ID>:
  33:	pxor   xmm0,xmm0
  37:	cvtsi2sd xmm0,DWORD PTR [rcx]
  3b:	addsd  xmm0,QWORD PTR [rcx+0x8]
  40:	ret

0000000000000041 <take_DD>:
  41:	movsd  xmm0,QWORD PTR [rcx]
  45:	addsd  xmm0,QWORD PTR [rcx+0x8]
  4a:	ret

000000000000004b <take_Vec3>:
  4b:	movss  xmm0,DWORD PTR [rcx]
  4f:	addss  xmm0,DWORD PTR [rcx+0x4]
  54:	addss  xmm0,DWORD PTR [rcx+0x8]
  59:	ret

000000000000005a <take_Mat4>:
  5a:	movss  xmm0,DWORD PTR [rcx]
  5e:	addss  xmm0,DWORD PTR [rcx+0x3c]
  63:	ret

0000000000000064 <ret_small>:
  64:	movabs rax,0x200000001
  6e:	ret

000000000000006f <ret_big>:
  6f:	mov    rax,rcx
  72:	mov    DWORD PTR [rcx],0x1
  78:	mov    DWORD PTR [rcx+0x4],0x2
  7f:	mov    DWORD PTR [rcx+0x8],0x3
  86:	ret

0000000000000087 <ret_II>:
  87:	movabs rax,0x200000001
  91:	ret

0000000000000092 <ret_FF>:
  92:	movabs rax,0x400000003f800000
  9c:	ret

000000000000009d <ret_Vec3>:
  9d:	mov    rax,rcx
  a0:	mov    DWORD PTR [rcx],0x3f800000
  a6:	mov    DWORD PTR [rcx+0x4],0x40000000
  ad:	mov    DWORD PTR [rcx+0x8],0x40400000
  b4:	ret

00000000000000b5 <ret_Mat4>:
  b5:	mov    rax,rcx
  b8:	pxor   xmm0,xmm0
  bc:	movups XMMWORD PTR [rcx],xmm0
  bf:	movups XMMWORD PTR [rcx+0x10],xmm0
  c3:	movups XMMWORD PTR [rcx+0x20],xmm0
  c7:	movups XMMWORD PTR [rcx+0x30],xmm0
  cb:	ret

00000000000000cc <caller_pass>:
  cc:	mov    DWORD PTR [rcx],0x1
  d2:	mov    DWORD PTR [rcx+0x4],0x2
  d9:	ret

00000000000000da <caller_ret>:
  da:	sub    rsp,0x98
  e1:	call   64 <ret_small>
  e6:	mov    QWORD PTR [rsp+0x88],rax
  ee:	lea    rcx,[rsp+0x7c]
  f3:	call   6f <ret_big>
  f8:	call   87 <ret_II>
  fd:	mov    QWORD PTR [rsp+0x74],rax
 102:	call   92 <ret_FF>
 107:	mov    QWORD PTR [rsp+0x6c],rax
 10c:	lea    rcx,[rsp+0x60]
 111:	call   9d <ret_Vec3>
 116:	lea    rcx,[rsp+0x20]
 11b:	call   b5 <ret_Mat4>
 120:	mov    rax,QWORD PTR [rsp+0x88]
 128:	mov    rax,QWORD PTR [rsp+0x74]
 12d:	mov    rax,QWORD PTR [rsp+0x6c]
 132:	add    rsp,0x98
 139:	ret

000000000000013a <main>:
 13a:	sub    rsp,0x28
 13e:	call   143 <main+0x9>
 143:	call   da <caller_ret>
 148:	mov    eax,0x0
 14d:	add    rsp,0x28
 151:	ret
 152:	nop
 153:	nop
 154:	nop
 155:	nop
 156:	nop
 157:	nop
 158:	nop
 159:	nop
 15a:	nop
 15b:	nop
 15c:	nop
 15d:	nop
 15e:	nop
 15f:	nop
