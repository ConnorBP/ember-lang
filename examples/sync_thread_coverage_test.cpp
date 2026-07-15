// Focused invalid-handle/capacity and thread lifecycle coverage.
#include "ext_sync.hpp"
#include "ext_thread.hpp"
#include "sema.hpp"
#include "context.hpp"

#include <atomic>
#include <climits>
#include <cstdio>
#include <string>
#include <unordered_map>

using namespace ember;

template<class T> static T native(const std::unordered_map<std::string,NativeSig>& n,const char* s){return reinterpret_cast<T>(n.at(s).fn_ptr);}
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);return 1;}}while(0)

extern "C" int64_t worker_plus_one(int64_t x){return x+1;}

int main(){
    std::unordered_map<std::string,NativeSig> n; ext_sync::register_natives(n); ext_thread::register_natives(n);
    CHECK(n.size()>=33);

    auto anew=native<int64_t(*)(int64_t,int64_t)>(n,"atomic_new");
    auto aload=native<int64_t(*)(int64_t)>(n,"atomic_load");
    auto astore=native<void(*)(int64_t,int64_t)>(n,"atomic_store");
    auto afree=native<void(*)(int64_t)>(n,"atomic_free");
    int64_t a=anew(123,9); CHECK(a>0 && aload(a)==9); // invalid width falls back to 64
    int64_t out=0;bool swapped=false;
    CHECK(!ext_sync::atomic_load_host(0,&out)); CHECK(!ext_sync::atomic_store_host(-1,1));
    CHECK(!ext_sync::atomic_cas_host(INT64_MAX,0,1,&swapped));
    astore(9999,1);afree(9999);afree(a);afree(a); // invalid and double-free are safe

    auto snew=native<int64_t(*)(int64_t)>(n,"swapbuf_new");
    auto sfree=native<void(*)(int64_t)>(n,"swapbuf_free");
    CHECK(snew(-1)==0);int64_t sb=snew(2);CHECK(sb>0);
    int64_t* data=nullptr;int64_t len=0;
    CHECK(!ext_sync::swapbuf_back_ptr(0,&data,&len));CHECK(ext_sync::swapbuf_back_ptr(sb,&data,&len)&&len==2);
    CHECK(ext_sync::swapbuf_front_index_host(sb)==0);CHECK(ext_sync::swapbuf_front_write_host(sb,0,77));
    CHECK(!ext_sync::swapbuf_front_write_host(sb,-1,1));CHECK(!ext_sync::swapbuf_front_write_host(sb,2,1));
    CHECK(ext_sync::swapbuf_swap_host(sb));CHECK(ext_sync::swapbuf_back_ptr(sb,&data,&len)&&data[0]==77);sfree(sb);

    auto qnew=native<int64_t(*)(int64_t)>(n,"spsc_new");auto qfree=native<void(*)(int64_t)>(n,"spsc_free");
    CHECK(qnew(-1)==0);int64_t q=qnew(3);CHECK(q>0); // rounds to four
    bool flag=true;CHECK(ext_sync::spsc_try_pop_host(q,&out,&flag)&&!flag);
    for(int i=0;i<4;++i){CHECK(ext_sync::spsc_push_host(q,i,&flag)&&flag);}CHECK(ext_sync::spsc_push_host(q,9,&flag)&&!flag);
    for(int i=0;i<4;++i){CHECK(ext_sync::spsc_try_pop_host(q,&out,&flag)&&flag&&out==i);}qfree(q);
    CHECK(!ext_sync::spsc_push_host(q,1,&flag));CHECK(!ext_sync::spsc_try_pop_host(q,&out,&flag));

    auto mnew=native<int64_t(*)(int64_t,int64_t)>(n,"mpsc_new");
    auto mph=native<int64_t(*)(int64_t,int64_t)>(n,"mpsc_producer_handle");
    auto mfree=native<void(*)(int64_t)>(n,"mpsc_free");
    CHECK(mnew(4,-1)==0);int64_t m=mnew(2,0);CHECK(m>0); // zero producers means one
    int64_t ph=mph(m,0);CHECK(ph>0&&mph(m,-1)==0&&mph(m,1)==0);
    CHECK(ext_sync::mpsc_try_pop_host(m,&out,&flag)&&!flag);CHECK(ext_sync::mpsc_push_host(ph,55,&flag)&&flag);
    CHECK(ext_sync::mpsc_try_pop_host(m,&out,&flag)&&flag&&out==55);mfree(m);
    CHECK(!ext_sync::mpsc_try_pop_host(m,&out,&flag));CHECK(!ext_sync::mpsc_push_host(ph,1,&flag));

    auto cnew=native<int64_t(*)(int64_t)>(n,"mpmc_new");auto cfree=native<void(*)(int64_t)>(n,"mpmc_free");
    CHECK(cnew(-1)==0);int64_t c=cnew(0);CHECK(c>0); // minimum capacity one
    CHECK(ext_sync::mpmc_try_pop_host(c,&out,&flag)&&!flag);CHECK(ext_sync::mpmc_push_host(c,8,&flag)&&flag);
    CHECK(ext_sync::mpmc_push_host(c,9,&flag)&&!flag);CHECK(ext_sync::mpmc_try_pop_host(c,&out,&flag)&&flag&&out==8);
    cfree(c);CHECK(!ext_sync::mpmc_push_host(c,1,&flag));CHECK(!ext_sync::mpmc_try_pop_host(c,&out,&flag));

    // Thread initialization rejects malformed state and native spawn/join reject
    // invalid handles before touching worker synchronization.
    auto spawn=native<int64_t(*)(int64_t,int64_t)>(n,"thread_spawn");
    auto join=native<int64_t(*)(int64_t)>(n,"thread_join");
    auto reason=native<int64_t(*)(int64_t)>(n,"thread_trap_reason");
    context_t ctx;std::atomic<void*> slots[1];slots[0].store(reinterpret_cast<void*>(&worker_plus_one));
    CHECK(!ext_thread::thread_init(nullptr,slots,1));CHECK(!ext_thread::thread_init(&ctx,nullptr,1));CHECK(!ext_thread::thread_init(&ctx,slots,0));
    CHECK(spawn(0,1)==0&&join(1)==0&&reason(1)==0); // uninitialized
    CHECK(ext_thread::thread_init(&ctx,slots,1));
    CHECK(spawn(-1,1)==0&&spawn(1,1)==0);slots[0].store(nullptr);CHECK(spawn(0,1)==0);
    slots[0].store(reinterpret_cast<void*>(&worker_plus_one));int64_t tid=spawn(0,41);CHECK(tid>0);
    CHECK(join(tid)==42);CHECK(join(tid)==0);CHECK(reason(tid)==0);CHECK(reason(999)==0);
    ext_thread::thread_reset();ext_thread::thread_reset();CHECK(spawn(0,1)==0);

    ext_sync::reset();ext_sync::reset();
    std::puts("sync/thread coverage: PASS");return 0;
}
