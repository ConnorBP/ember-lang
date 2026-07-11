#include <cstdint>
#include <cstdio>
#include <cstddef>

// Standard-layout (POD) — should match C layout
struct Vec3 { float x,y,z; };
struct Mat4 { float m[16]; };
struct MixID { int32_t a; double b; };        // 16B
struct Nest { Vec3 v; float w; };             // nested struct
struct ArrIn { float arr[4]; int32_t tag; };  // array member
struct Empty {};                              // C++ empty -> size 1

// Non-standard-layout: has a base class + virtual function
struct Base { int32_t bi; };
struct Derived : Base { virtual void f(); int32_t di; };
struct WithVtable { virtual void g(); int32_t a; float b; };

// alignas experiment
struct alignas(16) Mat4A { float m[16]; };
struct Mat4Inner16 { alignas(16) float m[16]; }; // member aligned to 16

int main(){
  #define LINE(t) printf("%-14s sizeof=%2zu align=%2zu", #t, sizeof(t), alignof(t))
  printf("POD / standard-layout:\n");
  printf("  Vec3        sizeof=%2zu align=%2zu  off(x,y,z)=(%zu,%zu,%zu)\n", sizeof(Vec3), alignof(Vec3), offsetof(Vec3,x), offsetof(Vec3,y), offsetof(Vec3,z));
  printf("  Mat4        sizeof=%2zu align=%2zu\n", sizeof(Mat4), alignof(Mat4));
  printf("  MixID       sizeof=%2zu align=%2zu  off(a,b)=(%zu,%zu)\n", sizeof(MixID), alignof(MixID), offsetof(MixID,a), offsetof(MixID,b));
  printf("  Nest{Vec3;float w} sizeof=%2zu align=%2zu off(v,w)=(%zu,%zu)\n", sizeof(Nest), alignof(Nest), offsetof(Nest,v), offsetof(Nest,w));
  printf("  ArrIn{float[4];int} sizeof=%2zu align=%2zu off(arr,tag)=(%zu,%zu)\n", sizeof(ArrIn), alignof(ArrIn), offsetof(ArrIn,arr), offsetof(ArrIn,tag));
  printf("  Empty       sizeof=%2zu align=%2zu\n", sizeof(Empty), alignof(Empty));
  printf("Non-standard-layout (Itanium vs MSVC differ):\n");
  printf("  Base        sizeof=%2zu align=%2zu off(bi)=%zu\n", sizeof(Base), alignof(Base), offsetof(Base,bi));
  printf("  Derived:Base sizeof=%2zu align=%2zu (has vtable)\n", sizeof(Derived), alignof(Derived));
  printf("  WithVtable  sizeof=%2zu align=%2zu off(a,b)=(%zu,%zu)\n", sizeof(WithVtable), alignof(WithVtable), offsetof(WithVtable,a), offsetof(WithVtable,b));
  printf("alignas experiments:\n");
  printf("  alignas(16) Mat4A  sizeof=%2zu align=%2zu\n", sizeof(Mat4A), alignof(Mat4A));
  printf("  Mat4Inner16        sizeof=%2zu align=%2zu off(m)=%zu\n", sizeof(Mat4Inner16), alignof(Mat4Inner16), offsetof(Mat4Inner16,m));
  return 0;
}
