#include "hook/process_runtime.h"

#include <algorithm>
#include <limits>

namespace winexinfo::hook {
namespace {
std::atomic<ProcessRuntime*> g_runtime{nullptr};
Status Ok() noexcept { return {ErrorCode::OK, S_OK, 0}; }
Status Fail(DWORD error) noexcept { return {ErrorCode::WINDOW_ATTACH_FAILED, HRESULT_FROM_WIN32(error), error}; }
bool Valid(const ipc::TabSetUpdate& u) noexcept {
    if (!u.top_level_hwnd || !u.top_level_generation || u.tabs.empty() ||
        u.tabs.size() > ipc::kMaximumTabDescriptors || !u.tabs.front().ui_thread_id) return false;
    const DWORD tid = u.tabs.front().ui_thread_id;
    return std::all_of(u.tabs.begin(), u.tabs.end(), [tid](const auto& t) {
        return t.tab_hwnd && t.tab_generation && t.ui_thread_id == tid;
    });
}
}

ProcessRuntime::ProcessRuntime() noexcept {
    for (std::size_t i = 0; i < kMaximumRuntimeWindows; ++i) {
        slots_[i] = std::make_unique<WindowRuntime>();
        admission_epoch_[i].store(0, std::memory_order_relaxed);
        storage_epoch_[i].store(0, std::memory_order_relaxed);
    }
}
WindowRuntimeCallbackLease::~WindowRuntimeCallbackLease(){ if(window_) window_->callback_gate.Leave(); }
WindowRuntimeCallbackLease::WindowRuntimeCallbackLease(WindowRuntimeCallbackLease&& o) noexcept : window_(o.window_){o.window_=nullptr;}
WindowRuntimeCallbackLease& WindowRuntimeCallbackLease::operator=(WindowRuntimeCallbackLease&& o) noexcept {
    if(this!=&o){if(window_)window_->callback_gate.Leave();window_=o.window_;o.window_=nullptr;}return *this;
}
WindowRuntimeStorageLease::~WindowRuntimeStorageLease(){if(window_)window_->storage_gate.Leave();}
WindowRuntimeStorageLease::WindowRuntimeStorageLease(WindowRuntimeStorageLease&& o) noexcept:window_(o.window_){o.window_=nullptr;}
WindowRuntimeStorageLease& WindowRuntimeStorageLease::operator=(WindowRuntimeStorageLease&& o) noexcept{
    if(this!=&o){if(window_)window_->storage_gate.Leave();window_=o.window_;o.window_=nullptr;}return *this;
}
std::size_t ProcessRuntime::active_window_count() const noexcept{return active_count_.load(std::memory_order_acquire);}
std::size_t ProcessRuntime::retained_window_count() const noexcept{
    std::size_t n=0;for(const auto& e:storage_epoch_)if(e.load(std::memory_order_acquire))++n;return n;
}
bool ProcessRuntime::retention_required()const noexcept{return retention_required_.load(std::memory_order_acquire);}

Status RegisterInitialProvisionalProcessWindow(ProcessRuntime&r,std::unique_ptr<WindowRuntime>v){
    if(!v||v->key.generation||r.retained_window_count()||!v->key.top_level||!v->key.process_id||!v->key.ui_thread_id)return Fail(ERROR_INVALID_STATE);
    if(!r.process_id)r.process_id=v->key.process_id;if(r.process_id!=v->key.process_id)return Fail(ERROR_INVALID_WINDOW_HANDLE);
    auto&slot=*r.slots_[0];slot.key=v->key;slot.resources=std::move(v->resources);slot.creation_sequence=++r.next_creation_sequence_;
    r.occupied_[0]=true;return Ok();
}
Status ActivateAndPublishInitialProcessWindow(ProcessRuntime&r,HWND top){
    if(!r.occupied_[0]||r.slots_[0]->key.top_level!=top||!r.operations.activate_window)return Fail(ERROR_INVALID_STATE);
    Status s=r.operations.activate_window(*r.slots_[0]);if(!s.ok())return s;
    const auto epoch=++r.next_admission_epoch_;r.storage_epoch_[0].store(epoch,std::memory_order_release);
    r.admission_epoch_[0].store(epoch,std::memory_order_release);r.active_count_.store(1,std::memory_order_release);return Ok();
}
WindowRuntimeCallbackLease AcquireProcessWindowCallbackAt(ProcessRuntime&r,std::size_t i)noexcept{
    if(i>=kMaximumRuntimeWindows)return{};const auto epoch=r.admission_epoch_[i].load(std::memory_order_acquire);if(!epoch)return{};
    auto*w=r.slots_[i].get();if(!w->callback_gate.Enter())return{};
    if(r.admission_epoch_[i].load(std::memory_order_acquire)!=epoch){w->callback_gate.Leave();return{};}return WindowRuntimeCallbackLease{w};
}
WindowRuntimeCallbackLease AcquireProcessWindowCallback(ProcessRuntime&r,HWND top)noexcept{
    for(std::size_t i=0;i<kMaximumRuntimeWindows;++i){auto l=AcquireProcessWindowCallbackAt(r,i);if(l&&l.get()->key.top_level==top)return l;}return{};
}
WindowRuntimeStorageLease AcquireProcessWindowStorageAt(ProcessRuntime&r,std::size_t i)noexcept{
    if(i>=kMaximumRuntimeWindows)return{};const auto epoch=r.storage_epoch_[i].load(std::memory_order_acquire);if(!epoch)return{};
    auto*w=r.slots_[i].get();if(!w->storage_gate.Enter())return{};
    if(r.storage_epoch_[i].load(std::memory_order_acquire)!=epoch){w->storage_gate.Leave();return{};}return WindowRuntimeStorageLease{w};
}
Status PrepareProcessRuntimeControl(ProcessRuntime&r,const ipc::TabSetUpdate&u,std::uint64_t token){
    if(!Valid(u)||!token||r.retention_required()||r.pending_.token.load(std::memory_order_acquire))return Fail(ERROR_INVALID_STATE);
    r.pending_.kind=ProcessRuntime::ControlKind::Update;r.pending_.update=u;r.pending_.result={};
    r.pending_.key={reinterpret_cast<HWND>(u.top_level_hwnd),r.process_id,u.tabs.front().ui_thread_id,u.top_level_generation};
    r.pending_.status=Ok();r.pending_.completed.store(false,std::memory_order_relaxed);r.pending_.token.store(token,std::memory_order_release);return Ok();
}

bool HandleProcessRuntimeControlMessageFor(ProcessRuntime&r,HWND top,UINT msg,WPARAM magic,LPARAM raw)noexcept{
 try{
    const auto token=static_cast<std::uint64_t>(raw);
    if(!top||msg!=r.control_message||magic!=kProcessRuntimeControlMagic||!token||r.retention_required()||
       r.pending_.token.load(std::memory_order_acquire)!=token||r.pending_.key.top_level!=top||
       !r.operations.get_current_process_id||!r.operations.get_current_thread_id||!r.operations.get_window_thread_process_id)return false;
    DWORD pid=0;const DWORD tid=r.operations.get_window_thread_process_id(top,&pid);
    if(pid!=r.pending_.key.process_id||tid!=r.pending_.key.ui_thread_id||r.operations.get_current_process_id()!=pid||r.operations.get_current_thread_id()!=tid)return false;
    std::uint64_t expected=token;if(!r.pending_.token.compare_exchange_strong(expected,0,std::memory_order_acq_rel))return false;
    std::size_t existing=kMaximumRuntimeWindows;
    for(std::size_t i=0;i<kMaximumRuntimeWindows;++i)if(r.occupied_[i]&&r.slots_[i]->key.top_level==top){existing=i;break;}
    auto retire=[&](std::size_t i,bool worker)->Status{
        auto&w=*r.slots_[i];r.admission_epoch_[i].store(0,std::memory_order_release);w.callback_gate.RejectNewWork();unsigned a=0;
        if(!w.lifecycle.compare_exchange_strong(a,1,std::memory_order_acq_rel))return Fail(ERROR_BUSY);
        if(!w.callback_gate.WaitForZero(5000)){w.lifecycle.store(3,std::memory_order_release);r.retention_required_.store(true,std::memory_order_release);return Fail(ERROR_TIMEOUT);}
        Status s=worker?r.operations.cleanup_window_from_worker(w):r.operations.cleanup_window_on_ui(w);w.lifecycle.store(s.ok()?2u:3u,std::memory_order_release);
        r.occupied_[i]=false;r.removed_[i]=true;r.active_count_.fetch_sub(1,std::memory_order_acq_rel);if(!s.ok())r.retention_required_.store(true,std::memory_order_release);return s;
    };
    Status status=Ok();
    if(r.pending_.kind==ProcessRuntime::ControlKind::Remove){
        if(existing==kMaximumRuntimeWindows||r.slots_[existing]->key!=r.pending_.key)status=Fail(ERROR_INVALID_STATE);else status=retire(existing,false);
    }else if(existing!=kMaximumRuntimeWindows&&r.slots_[existing]->key.generation&&r.pending_.key.generation<r.slots_[existing]->key.generation){status=Fail(ERROR_INVALID_STATE);
    }else if(existing!=kMaximumRuntimeWindows&&(r.slots_[existing]->key.generation==0||r.slots_[existing]->key.generation==r.pending_.key.generation)){
        auto&w=*r.slots_[existing];status=r.operations.apply_update?r.operations.apply_update(w,r.pending_.update,&r.pending_.result):Fail(ERROR_INVALID_STATE);
        if(status.ok())w.key.generation=r.pending_.key.generation;else if(!w.tab_subclasses->cleanup_safe())r.retention_required_.store(true,std::memory_order_release);
    }else{
        if(existing!=kMaximumRuntimeWindows){status=retire(existing,false);if(status.ok())status=ReapRemovedProcessWindows(r,0);}
        std::size_t free=kMaximumRuntimeWindows;for(std::size_t i=0;i<kMaximumRuntimeWindows;++i)if(!r.occupied_[i]&&!r.removed_[i]){free=i;break;}
        if(status.ok()&&free==kMaximumRuntimeWindows)status=Fail(ERROR_TOO_MANY_OPEN_FILES);
        std::unique_ptr<WindowRuntime> created;
        if(status.ok())status=r.operations.create_window?r.operations.create_window(r.pending_.key,&created):Fail(ERROR_INVALID_STATE);
        if(status.ok()&&created){
            auto&w=*r.slots_[free];w.key=created->key;w.resources=std::move(created->resources);
            w.creation_sequence=++r.next_creation_sequence_;w.lifecycle.store(0,std::memory_order_release);
            r.occupied_[free]=true;const auto epoch=++r.next_admission_epoch_;
            status=r.operations.apply_update?r.operations.apply_update(w,r.pending_.update,&r.pending_.result):Fail(ERROR_INVALID_STATE);
            if(status.ok())status=r.operations.activate_window?r.operations.activate_window(w):Fail(ERROR_INVALID_STATE);
            if(status.ok()){
                r.storage_epoch_[free].store(epoch,std::memory_order_release);
                r.admission_epoch_[free].store(epoch,std::memory_order_release);
                r.active_count_.fetch_add(1,std::memory_order_acq_rel);
            }else{
                Status c=r.operations.cleanup_window_on_ui?r.operations.cleanup_window_on_ui(w):Fail(ERROR_INVALID_STATE);
                w.lifecycle.store(c.ok()?2u:3u,std::memory_order_release);r.occupied_[free]=false;r.removed_[free]=true;
                r.storage_epoch_[free].store(epoch,std::memory_order_release);
                if(!c.ok())r.retention_required_.store(true,std::memory_order_release);
            }
        }else if(!status.ok()&&created){
            Status c=r.operations.cleanup_window_on_ui?r.operations.cleanup_window_on_ui(*created):Fail(ERROR_INVALID_STATE);
            if(!c.ok()){
                r.retention_required_.store(true,std::memory_order_release);
                auto&w=*r.slots_[free];w.key=created->key;w.resources=std::move(created->resources);
                w.creation_sequence=++r.next_creation_sequence_;w.lifecycle.store(3,std::memory_order_release);
                r.removed_[free]=true;r.storage_epoch_[free].store(++r.next_admission_epoch_,std::memory_order_release);
            }
        }
    }
    if(!status.ok()&&r.pending_.kind==ProcessRuntime::ControlKind::Update&&r.pending_.result.result==0)r.pending_.result={r.pending_.key.generation,status.win32?status.win32:ERROR_INVALID_STATE,"window runtime update failed"};
    r.pending_.status=status;r.pending_.completed.store(true,std::memory_order_release);return true;
 }catch(...){r.retention_required_.store(true,std::memory_order_release);return false;}
}

Status ReapRemovedProcessWindows(ProcessRuntime&r,DWORD timeout)noexcept{
    for(std::size_t i=0;i<kMaximumRuntimeWindows;++i)if(r.removed_[i]){
        auto&w=*r.slots_[i];if(w.lifecycle.load(std::memory_order_acquire)!=2){r.retention_required_.store(true,std::memory_order_release);return Fail(ERROR_INVALID_STATE);}
        if(!w.callback_gate.WaitForZero(timeout))return Fail(ERROR_TIMEOUT);w.storage_gate.RejectNewWork();if(!w.storage_gate.WaitForZero(timeout))return Fail(ERROR_TIMEOUT);
        r.storage_epoch_[i].store(0,std::memory_order_release);w.resources.reset();w.tab_subclasses=std::make_unique<TabSubclassSet>();w.key={};w.creation_sequence=0;
        w.callback_gate.ResetForReuse();w.storage_gate.ResetForReuse();w.lifecycle.store(0,std::memory_order_release);r.removed_[i]=false;
    }return Ok();
}
Status ProcessRequestedWindowRemovals(ProcessRuntime&r){
    Status first=Ok();for(;;){std::size_t i=kMaximumRuntimeWindows;std::uint64_t seq=0;
        for(std::size_t candidate=0;candidate<kMaximumRuntimeWindows;++candidate)if(r.occupied_[candidate]&&
            r.slots_[candidate]->lifecycle.load(std::memory_order_acquire)==4&&r.slots_[candidate]->creation_sequence>seq){seq=r.slots_[candidate]->creation_sequence;i=candidate;}
        if(i==kMaximumRuntimeWindows)break;
        auto&w=*r.slots_[i];unsigned q=4;if(!w.lifecycle.compare_exchange_strong(q,1,std::memory_order_acq_rel))continue;
        Status s=w.callback_gate.WaitForZero(5000)&&r.operations.cleanup_window_from_worker?r.operations.cleanup_window_from_worker(w):Fail(ERROR_TIMEOUT);
        w.lifecycle.store(s.ok()?2u:3u,std::memory_order_release);r.occupied_[i]=false;r.removed_[i]=true;
        if(!s.ok()){r.retention_required_.store(true,std::memory_order_release);if(first.ok())first=s;}
    }return first;
}
Status RetireProcessWindowOnCurrentUi(ProcessRuntime&r,HWND top){
    for(std::size_t i=0;i<kMaximumRuntimeWindows;++i)if(r.occupied_[i]&&r.slots_[i]->key.top_level==top){
        auto&w=*r.slots_[i];const bool admitted=r.admission_epoch_[i].load(std::memory_order_acquire)!=0;
        r.admission_epoch_[i].store(0,std::memory_order_release);w.callback_gate.RejectNewWork();unsigned a=0;
        if(!w.lifecycle.compare_exchange_strong(a,1,std::memory_order_acq_rel))return Fail(ERROR_INVALID_STATE);
        if(!w.callback_gate.WaitForZero(5000)){w.lifecycle.store(3,std::memory_order_release);r.retention_required_.store(true,std::memory_order_release);return Fail(ERROR_TIMEOUT);}
        Status s=r.operations.cleanup_window_on_ui?r.operations.cleanup_window_on_ui(w):Fail(ERROR_INVALID_STATE);
        w.lifecycle.store(s.ok()?2u:3u,std::memory_order_release);r.occupied_[i]=false;r.removed_[i]=true;
        if(!r.storage_epoch_[i].load(std::memory_order_acquire))r.storage_epoch_[i].store(++r.next_admission_epoch_,std::memory_order_release);
        if(admitted)r.active_count_.fetch_sub(1,std::memory_order_acq_rel);
        if(!s.ok())r.retention_required_.store(true,std::memory_order_release);return s;
    }return Fail(ERROR_INVALID_WINDOW_HANDLE);
}
bool HandleProcessRuntimeTopLevelDestroy(ProcessRuntime&r,HWND top)noexcept{
    for(std::size_t i=0;i<kMaximumRuntimeWindows;++i){auto lease=AcquireProcessWindowCallbackAt(r,i);if(!lease)continue;
        auto&w=*lease.get();if(w.key.top_level!=top)continue;unsigned a=0;if(!w.lifecycle.compare_exchange_strong(a,1,std::memory_order_acq_rel))return true;
        r.admission_epoch_[i].store(0,std::memory_order_release);w.callback_gate.RejectNewWork();
        Status shelter=r.operations.shelter_window_on_destroy?r.operations.shelter_window_on_destroy(w):Fail(ERROR_INVALID_STATE);
        w.lifecycle.store(shelter.ok()?4u:3u,std::memory_order_release);r.active_count_.fetch_sub(1,std::memory_order_acq_rel);
        if(!shelter.ok())r.retention_required_.store(true,std::memory_order_release);return true;}return false;
}

Status DispatchProcessTabSetUpdate(ProcessRuntime&r,const ipc::TabSetUpdate&u,ipc::TabSetResult*out){
    if(!out||!Valid(u)||!r.process_id||!r.control_message||!r.operations.get_window_thread_process_id||!r.operations.send_control||r.retention_required())return Fail(ERROR_INVALID_PARAMETER);
    std::scoped_lock lock{r.dispatch_mutex_};Status s=ProcessRequestedWindowRemovals(r);if(!s.ok())return s;static_cast<void>(ReapRemovedProcessWindows(r,0));
    if(r.next_token_==std::numeric_limits<std::uint64_t>::max()){r.retention_required_.store(true,std::memory_order_release);return Fail(ERROR_INVALID_STATE);}
    DWORD pid=0;HWND top=reinterpret_cast<HWND>(u.top_level_hwnd);DWORD tid=r.operations.get_window_thread_process_id(top,&pid);if(pid!=r.process_id||!tid||u.tabs.front().ui_thread_id!=tid)return Fail(ERROR_INVALID_WINDOW_HANDLE);
    const auto token=++r.next_token_;s=PrepareProcessRuntimeControl(r,u,token);DWORD_PTR ignored=0;if(s.ok())s=r.operations.send_control(top,r.control_message,kProcessRuntimeControlMagic,static_cast<LPARAM>(token),SMTO_ABORTIFHUNG|SMTO_BLOCK,5000,&ignored);
    if(!s.ok()||!r.pending_.completed.load(std::memory_order_acquire)||r.pending_.token.load(std::memory_order_acquire)){r.retention_required_.store(true,std::memory_order_release);return s.ok()?Fail(ERROR_INVALID_STATE):s;}
    *out=r.pending_.result;return Ok();
}
Status DispatchProcessWindowRemoval(ProcessRuntime&r,const WindowRuntimeKey&key){
    if(!key.top_level||!key.process_id||!key.ui_thread_id||!key.generation||key.process_id!=r.process_id||r.retention_required()||!r.control_message||!r.operations.send_control)return Fail(ERROR_INVALID_PARAMETER);
    std::scoped_lock lock{r.dispatch_mutex_};Status s=ProcessRequestedWindowRemovals(r);if(!s.ok())return s;static_cast<void>(ReapRemovedProcessWindows(r,0));
    if(r.next_token_==std::numeric_limits<std::uint64_t>::max()){r.retention_required_.store(true,std::memory_order_release);return Fail(ERROR_INVALID_STATE);}
    const auto token=++r.next_token_;r.pending_.kind=ProcessRuntime::ControlKind::Remove;r.pending_.key=key;r.pending_.status=Ok();r.pending_.completed.store(false,std::memory_order_relaxed);r.pending_.token.store(token,std::memory_order_release);
    DWORD_PTR ignored=0;s=r.operations.send_control(key.top_level,r.control_message,kProcessRuntimeControlMagic,static_cast<LPARAM>(token),SMTO_ABORTIFHUNG|SMTO_BLOCK,5000,&ignored);
    if(!s.ok()||!r.pending_.completed.load(std::memory_order_acquire)){r.retention_required_.store(true,std::memory_order_release);return s.ok()?Fail(ERROR_INVALID_STATE):s;}return r.pending_.status;
}
Status DispatchProcessWindowRemovalByIdentity(ProcessRuntime&r,const ipc::WindowRemoveRequest&request,ipc::WindowRemoveResult*out){
    if(!out||!request.top_level_hwnd||!request.top_level_generation)return Fail(ERROR_INVALID_PARAMETER);
    *out={request.top_level_generation,ERROR_INVALID_WINDOW_HANDLE,"window runtime removal failed"};
    const HWND top=reinterpret_cast<HWND>(request.top_level_hwnd);
    WindowRuntimeKey key{};
    for(std::size_t i=0;i<kMaximumRuntimeWindows;++i){
        auto lease=AcquireProcessWindowStorageAt(r,i);
        if(!lease)continue;
        const auto&candidate=lease.get()->key;
        if(candidate.top_level==top&&candidate.generation==request.top_level_generation){key=candidate;break;}
    }
    if(!key.top_level)return Ok();
    const Status removed=DispatchProcessWindowRemoval(r,key);
    if(removed.ok())*out={request.top_level_generation,0,{}};
    else *out={request.top_level_generation,removed.win32?removed.win32:ERROR_INVALID_STATE,"window runtime removal failed"};
    return Ok();
}
Status RemoveAllProcessWindows(ProcessRuntime&r){
    Status first=Ok();for(;;){std::size_t chosen=kMaximumRuntimeWindows;std::uint64_t seq=0;
        for(std::size_t i=0;i<kMaximumRuntimeWindows;++i)if(r.occupied_[i]&&r.slots_[i]->creation_sequence>seq){seq=r.slots_[i]->creation_sequence;chosen=i;}if(chosen==kMaximumRuntimeWindows)break;
        auto&w=*r.slots_[chosen];r.admission_epoch_[chosen].store(0,std::memory_order_release);w.callback_gate.RejectNewWork();unsigned a=0;Status s=Ok();
        bool decrement=true;
        if(w.lifecycle.compare_exchange_strong(a,1,std::memory_order_acq_rel)){}
        else if(a==4){unsigned requested=4;if(!w.lifecycle.compare_exchange_strong(requested,1,std::memory_order_acq_rel))s=Fail(ERROR_INVALID_STATE);decrement=false;}
        else s=Fail(ERROR_INVALID_STATE);
        if(s.ok()){if(!w.callback_gate.WaitForZero(5000))s=Fail(ERROR_TIMEOUT);else s=r.operations.cleanup_window_from_worker?r.operations.cleanup_window_from_worker(w):Fail(ERROR_INVALID_STATE);}
        w.lifecycle.store(s.ok()?2u:3u,std::memory_order_release);r.occupied_[chosen]=false;r.removed_[chosen]=true;if(decrement)r.active_count_.fetch_sub(1,std::memory_order_acq_rel);if(!s.ok()&&first.ok())first=s;
    }if(!first.ok())r.retention_required_.store(true,std::memory_order_release);return first;
}
Status CaptureProcessDetachCleanupProof(
    ProcessRuntime&r,ipc::DetachCleanupProof*out)noexcept{
    if(!out||!r.operations.capture_cleanup_proof)return Fail(ERROR_INVALID_PARAMETER);
    ipc::DetachCleanupProof total{};
    const auto add=[](std::uint32_t*target,std::uint32_t value){
        if(value>(std::numeric_limits<std::uint32_t>::max)()-*target)return false;
        *target+=value;return true;
    };
    for(std::size_t i=0;i<kMaximumRuntimeWindows;++i){
        if(!r.slots_[i]||(!r.occupied_[i]&&!r.removed_[i]))continue;
        ipc::DetachCleanupProof window{};
        const Status status=r.operations.capture_cleanup_proof(*r.slots_[i],&window);
        if(!status.ok()||
            !add(&total.pane_count,window.pane_count)||
            !add(&total.tab_subclass_count,window.tab_subclass_count)||
            !add(&total.parent_subclass_count,window.parent_subclass_count)||
            !add(&total.refresh_worker_count,window.refresh_worker_count)||
            !add(&total.callback_count,window.callback_count)||
            !add(&total.callback_count,r.slots_[i]->callback_gate.in_flight()))
            return status.ok()?Fail(ERROR_ARITHMETIC_OVERFLOW):status;
    }
    *out=total;return Ok();
}
Status FinalizeProcessWindowsAfterDrain(ProcessRuntime&r,DWORD timeout)noexcept{
    for(auto&e:r.admission_epoch_)e.store(0,std::memory_order_release);for(auto&e:r.storage_epoch_)e.store(0,std::memory_order_release);
    for(auto&s:r.slots_)s->storage_gate.RejectNewWork();for(auto&s:r.slots_)if(!s->storage_gate.WaitForZero(timeout)){r.retention_required_.store(true,std::memory_order_release);return Fail(ERROR_TIMEOUT);}return Ok();
}
bool HandleProcessRuntimeControlMessage(HWND h,UINT m,WPARAM w,LPARAM l)noexcept{auto*r=g_runtime.load(std::memory_order_acquire);return r&&HandleProcessRuntimeControlMessageFor(*r,h,m,w,l);}
void SetProcessRuntimeForCallbacks(ProcessRuntime*r)noexcept{g_runtime.store(r,std::memory_order_release);}
} // namespace winexinfo::hook
