#include "local_recordings.h"
#include "device_diagnostics.h"
#include <string.h>
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

enum slot_state { SLOT_BLANK, SLOT_DIRTY, SLOT_SAVED, SLOT_WRITING, SLOT_ERASING };
typedef struct { enum slot_state state; recording_info_t info; unsigned readers; } slot_t;
static const uint8_t owner[16] = "EDGEAI_REC_V1";
static const esp_partition_t *partition;
static slot_t slots[REC_SLOT_COUNT];
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t worker;
static recording_copy_fn copy_frame;
static local_recordings_stats_t stats = {.status="not_initialized"};
static int ready_slot=-1, active_slot=-1;
static uint32_t next_id=1, active_stream;
static uint64_t first_frame;
static bool maintenance_allowed=true;
// Sole owner: initialization before task creation, then recording worker.
static int16_t scratch[3*320];
_Static_assert(REC_DATA_OFFSET+REC_MAX_PCM_BYTES <= REC_SLOT_BYTES, "Recording must fit in its slot");

static uint32_t base(int slot) { return (uint32_t)slot*REC_SLOT_BYTES; }
static void failure(const char *why) {
    portENTER_CRITICAL(&lock);
    stats.enabled=false; stats.active=false; stats.preparing=false; stats.ready=false;
    stats.status=why; stats.errors++; ready_slot=-1; active_slot=-1;
    portEXIT_CRITICAL(&lock);
    device_diagnostics_event(ESP_LOG_ERROR,"Local recordings disabled: %s; existing flash not formatted",why);
}
static bool erased(const void *data,size_t size) {
    const uint8_t *p=data; for(size_t i=0;i<size;++i) if(p[i]!=255) return false;
    return true;
}
static bool blank_slot(int slot) {
    for(uint32_t pos=0;pos<REC_SLOT_BYTES;pos+=sizeof(scratch)) {
        size_t n=REC_SLOT_BYTES-pos; if(n>sizeof(scratch)) n=sizeof(scratch);
        if(esp_partition_read(partition,base(slot)+pos,scratch,n)!=ESP_OK || !erased(scratch,n)) return false;
        if((pos/sizeof(scratch))%32==31) vTaskDelay(1);
    }
    return true;
}
static bool valid_pcm(int slot,const recording_info_t *info) {
    uint32_t crc=0;
    for(uint32_t pos=0;pos<info->pcm_bytes;pos+=sizeof(scratch)) {
        size_t n=info->pcm_bytes-pos; if(n>sizeof(scratch)) n=sizeof(scratch);
        if(esp_partition_read(partition,base(slot)+REC_DATA_OFFSET+pos,scratch,n)!=ESP_OK) return false;
        crc=recording_crc32(crc,scratch,n);
        if((pos/sizeof(scratch))%32==31) vTaskDelay(1);
    }
    return crc==info->pcm_crc;
}
// lock held; only the latest three committed IDs are exposed to new readers.
static unsigned list_locked(int selected[REC_KEEP_COUNT]) {
    unsigned count=0;
    for(int s=0;s<REC_SLOT_COUNT;++s) if(slots[s].state==SLOT_SAVED) {
        unsigned at=0;
        while(at<count && slots[selected[at]].info.id>slots[s].info.id) ++at;
        if(at>=REC_KEEP_COUNT) continue;
        if(count<REC_KEEP_COUNT) ++count;
        for(unsigned i=count-1;i>at;--i) selected[i]=selected[i-1];
        selected[at]=s;
    }
    return count;
}
// No producer is allowed to reserve a slot until erasing has finished.
static void prepare_slot(void) {
    int chosen=-1; bool dirty=false; uint32_t oldest=UINT32_MAX;
    portENTER_CRITICAL(&lock);
    if(!stats.enabled || stats.active || ready_slot>=0 || !maintenance_allowed) { portEXIT_CRITICAL(&lock); return; }
    for(int s=0;s<REC_SLOT_COUNT;++s) if(slots[s].state==SLOT_BLANK || slots[s].state==SLOT_DIRTY) {
        chosen=s; dirty=slots[s].state==SLOT_DIRTY; break;
    }
    if(chosen<0) {
        for(int s=0;s<REC_SLOT_COUNT;++s) if(slots[s].state==SLOT_SAVED && slots[s].info.id<oldest) {
            oldest=slots[s].info.id; chosen=s;
        }
        dirty=true;
    }
    if(chosen<0 || slots[chosen].readers) { stats.status="waiting_for_download"; portEXIT_CRITICAL(&lock); return; }
    if(dirty) { slots[chosen].state=SLOT_ERASING; stats.preparing=true; stats.status="preparing"; }
    portEXIT_CRITICAL(&lock);
    if(dirty) {
        // Invalidate commit BEFORE erasing data. Preserve owner marker until all
        // body sectors have been erased; interrupted cleanup is safely recognized.
        const uint8_t invalid[8]={0};
        if(esp_partition_write(partition,base(chosen)+64,invalid,sizeof(invalid))!=ESP_OK) { failure("invalidate_failed"); return; }
        for(uint32_t pos=REC_DATA_OFFSET;pos<REC_SLOT_BYTES;pos+=4096) {
            if(esp_partition_erase_range(partition,base(chosen)+pos,4096)!=ESP_OK) { failure("erase_failed"); return; }
            vTaskDelay(1); // Give capture/other work time between individual sectors.
        }
        if(esp_partition_erase_range(partition,base(chosen),4096)!=ESP_OK) { failure("erase_header_failed"); return; }
    }
    portENTER_CRITICAL(&lock);
    slots[chosen].state=SLOT_BLANK; ready_slot=chosen;
    stats.preparing=false; stats.ready=true; stats.status="ready";
    portEXIT_CRITICAL(&lock);
}

static void record_slot(int slot,uint32_t stream,uint64_t cursor) {
    portENTER_CRITICAL(&lock); uint32_t id=next_id++; portEXIT_CRITICAL(&lock);
    recording_info_t info={.id=id,.stream_id=stream,.reason=REC_SILENCE};
    if(esp_partition_write(partition,base(slot),owner,sizeof(owner))!=ESP_OK) { failure("owner_write_failed"); return; }
    device_diagnostics_event(ESP_LOG_INFO,"Local recording %lu started (stream %lu, max 30s)",(unsigned long)info.id,(unsigned long)stream);
    bool done=false;
    unsigned waiting=0;
    while(!done) {
        unsigned frames=0;
        while(frames<3 && info.pcm_bytes+frames*640<REC_MAX_PCM_BYTES) {
            recording_frame_result_t result=copy_frame(stream,cursor+frames,scratch+frames*320);
            if(result==REC_FRAME_DATA) { ++frames; waiting=0; continue; }
            if(result==REC_FRAME_END) done=true;
            else if(result==REC_FRAME_LOST) { info.reason=REC_GAP; done=true; }
            break;
        }
        if(frames) {
            const size_t bytes=frames*640;
            if(esp_partition_write(partition,base(slot)+REC_DATA_OFFSET+info.pcm_bytes,scratch,bytes)!=ESP_OK) {
                failure("pcm_write_failed"); return;
            }
            info.pcm_crc=recording_crc32(info.pcm_crc,scratch,bytes);
            info.pcm_bytes+=bytes; cursor+=frames;
            portENTER_CRITICAL(&lock); stats.pcm_bytes=info.pcm_bytes; portEXIT_CRITICAL(&lock);
        }
        if(info.pcm_bytes==REC_MAX_PCM_BYTES) { info.reason=REC_LIMIT; done=true; }
        if(!frames && !done) {
            if(++waiting>=250) { info.reason=REC_GAP; done=true; } // 5s without capture progress.
            vTaskDelay(pdMS_TO_TICKS(20));
        } else if(!done) vTaskDelay(1);
    }
    if(info.pcm_bytes) {
        uint8_t metadata[REC_META_BYTES]; recording_encode(metadata,&info);
        if(esp_partition_write(partition,base(slot)+64,metadata,sizeof(metadata))!=ESP_OK) { failure("commit_failed"); return; }
    }
    portENTER_CRITICAL(&lock);
    slots[slot].info=info; slots[slot].state=info.pcm_bytes?SLOT_SAVED:SLOT_DIRTY;
    stats.active=false; active_slot=-1;
    stats.status=info.pcm_bytes?"saved":"empty_clip";
    if(info.pcm_bytes) ++stats.saved;
    if(info.reason==REC_GAP) ++stats.errors;
    portEXIT_CRITICAL(&lock);
    if(info.pcm_bytes) device_diagnostics_event(info.reason==REC_GAP?ESP_LOG_WARN:ESP_LOG_INFO,
        "Local recording %lu: %lu bytes, %s; /recordings/%lu.wav",(unsigned long)info.id,
        (unsigned long)info.pcm_bytes,recording_reason_name(info.reason),(unsigned long)info.id);
    else device_diagnostics_event(ESP_LOG_WARN,"Local recording %lu discarded: no contiguous PCM",(unsigned long)info.id);
}
static void task(void *unused) {
    (void)unused;
    for(;;) {
        portENTER_CRITICAL(&lock);
        bool enabled=stats.enabled; int slot=active_slot;
        uint32_t stream=active_stream; uint64_t first=first_frame;
        portEXIT_CRITICAL(&lock);
        if(enabled) {
            if(slot>=0) record_slot(slot,stream,first);
            else prepare_slot();
        }
        ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(50));
    }
}
esp_err_t local_recordings_init(recording_copy_fn copy) {
    copy_frame=copy;
    partition=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_ANY,"storage");
    if(!copy || !partition || partition->size<REC_SLOT_COUNT*REC_SLOT_BYTES || partition->encrypted) {
        failure("partition_unavailable"); return ESP_FAIL;
    }
    for(int s=0;s<REC_SLOT_COUNT;++s) {
        uint8_t header[16],metadata[REC_META_BYTES];
        if(esp_partition_read(partition,base(s),header,sizeof(header))!=ESP_OK) { failure("read_failed"); return ESP_FAIL; }
        if(!memcmp(header,owner,sizeof(owner))) {
            slots[s].state=SLOT_DIRTY;
            if(esp_partition_read(partition,base(s)+64,metadata,sizeof(metadata))!=ESP_OK) { failure("read_failed"); return ESP_FAIL; }
            if(recording_decode(metadata,&slots[s].info)) {
                // Preserve ID monotonicity even if a clip has a damaged payload.
                if(slots[s].info.id>=next_id) next_id=slots[s].info.id==UINT32_MAX?UINT32_MAX:slots[s].info.id+1;
                if(valid_pcm(s,&slots[s].info)) { slots[s].state=SLOT_SAVED; ++stats.saved; }
                else ++stats.errors;
            }
        } else if(erased(header,sizeof(header)) && blank_slot(s)) slots[s].state=SLOT_BLANK;
        else { failure("unrecognized_storage_preserved"); return ESP_FAIL; }
    }
    if(next_id==UINT32_MAX) { failure("recording_id_exhausted"); return ESP_FAIL; }
    stats.enabled=true; stats.status="preparing"; stats.preparing=true;
    if(xTaskCreate(task,"recording_store",4096,NULL,1,&worker)!=pdPASS) {
        failure("no_task_memory"); return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
bool local_recordings_begin(uint32_t stream,uint64_t first) {
    portENTER_CRITICAL(&lock);
    bool ok=stats.enabled && !stats.active && ready_slot>=0 && next_id<UINT32_MAX;
    if(ok) {
        active_slot=ready_slot; ready_slot=-1; active_stream=stream; first_frame=first;
        slots[active_slot].state=SLOT_WRITING;
        stats.active=true; stats.ready=false; stats.preparing=false; stats.pcm_bytes=0; stats.status="recording";
    }
    portEXIT_CRITICAL(&lock);
    if(ok) xTaskNotifyGive(worker);
    return ok;
}
bool local_recordings_busy(void) {
    portENTER_CRITICAL(&lock); bool busy=stats.active||stats.preparing; portEXIT_CRITICAL(&lock); return busy;
}
void local_recordings_allow_maintenance(bool allowed) {
    portENTER_CRITICAL(&lock); maintenance_allowed=allowed; portEXIT_CRITICAL(&lock);
}
void local_recordings_get_stats(local_recordings_stats_t *out) {
    portENTER_CRITICAL(&lock); *out=stats; portEXIT_CRITICAL(&lock);
}
uint32_t local_recordings_stack_free(void) {
    return worker?(uint32_t)uxTaskGetStackHighWaterMark(worker):0;
}
unsigned local_recordings_list(recording_info_t out[REC_KEEP_COUNT]) {
    int selected[REC_KEEP_COUNT]; portENTER_CRITICAL(&lock);
    unsigned count=list_locked(selected);
    for(unsigned i=0;i<count;++i) out[i]=slots[selected[i]].info;
    portEXIT_CRITICAL(&lock); return count;
}
int local_recordings_open(uint32_t id,recording_info_t *info) {
    int found=-1,selected[REC_KEEP_COUNT]; portENTER_CRITICAL(&lock);
    unsigned count=list_locked(selected);
    for(unsigned i=0;i<count;++i) if(slots[selected[i]].info.id==id) {
        found=selected[i]; ++slots[found].readers; *info=slots[found].info; break;
    }
    portEXIT_CRITICAL(&lock); return found;
}
esp_err_t local_recordings_read(int slot,uint32_t offset,void *data,size_t bytes) {
    if(slot<0 || slot>=REC_SLOT_COUNT) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&lock);
    bool valid=slots[slot].readers && slots[slot].state==SLOT_SAVED &&
        offset<=slots[slot].info.pcm_bytes && bytes<=slots[slot].info.pcm_bytes-offset;
    portEXIT_CRITICAL(&lock);
    return valid?esp_partition_read(partition,base(slot)+REC_DATA_OFFSET+offset,data,bytes):ESP_ERR_INVALID_ARG;
}
void local_recordings_close(int slot) {
    if(slot<0 || slot>=REC_SLOT_COUNT) return;
    portENTER_CRITICAL(&lock); if(slots[slot].readers) --slots[slot].readers; portEXIT_CRITICAL(&lock);
}
