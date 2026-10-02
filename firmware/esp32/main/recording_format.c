#include "recording_format.h"
#include <string.h>
static void put32(uint8_t *p, uint32_t n) { for (int i=0;i<4;++i) p[i]=(uint8_t)(n>>(8*i)); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
uint32_t recording_crc32(uint32_t crc, const void *data, size_t size) {
    const uint8_t *p=data; crc=~crc;
    while (size--) { crc ^= *p++; for (int b=0;b<8;++b) crc=(crc>>1)^((0u-(crc&1u))&0xedb88320u); }
    return ~crc;
}
void recording_wav_header(uint8_t out[44], uint32_t pcm_bytes) {
    memset(out,0,44); memcpy(out,"RIFF",4); put32(out+4,pcm_bytes+36);
    memcpy(out+8,"WAVEfmt ",8); put32(out+16,16); out[20]=1; out[22]=1;
    put32(out+24,16000); put32(out+28,32000); out[32]=2; out[34]=16;
    memcpy(out+36,"data",4); put32(out+40,pcm_bytes);
}
void recording_encode(uint8_t out[REC_META_BYTES], const recording_info_t *info) {
    memset(out,0,REC_META_BYTES); memcpy(out,"REC1DONE",8);
    put32(out+8,info->id); put32(out+12,info->stream_id); put32(out+16,info->pcm_bytes);
    put32(out+20,info->pcm_crc); put32(out+24,info->reason); put32(out+28,16000);
    put32(out+36,recording_crc32(0,out,36));
}
bool recording_decode(const uint8_t in[REC_META_BYTES], recording_info_t *info) {
    if (memcmp(in,"REC1DONE",8) || get32(in+36)!=recording_crc32(0,in,36) || get32(in+28)!=16000) return false;
    recording_info_t v={get32(in+8),get32(in+12),get32(in+16),get32(in+20),(recording_reason_t)get32(in+24)};
    if (!v.id || !v.pcm_bytes || v.pcm_bytes>REC_MAX_PCM_BYTES || v.pcm_bytes%640 || (uint32_t)v.reason>REC_IO_ERROR) return false;
    *info=v; return true;
}
static bool number(const char **p, uint32_t *n) {
    if (**p<'0'||**p>'9') return false;
    *n=0;
    do { unsigned d=(unsigned)(**p-'0'); if (*n>(UINT32_MAX-d)/10) return false; *n=*n*10+d; ++*p; } while (**p>='0'&&**p<='9');
    return true;
}
bool recording_parse_path(const char *path, uint32_t *id) {
    const char prefix[]="/recordings/";
    if (strncmp(path,prefix,sizeof(prefix)-1)) return false;
    const char *p=path+sizeof(prefix)-1;
    return number(&p,id) && *id!=0 && !strcmp(p,".wav");
}
bool recording_parse_range(const char *s, uint32_t size, uint32_t *first, uint32_t *last) {
    if (!size) return false;
    *first=0; *last=size-1;
    if (!s) return true;
    if (strncmp(s,"bytes=",6)) return false;
    s+=6;
    if (*s=='-') {
        uint32_t suffix; ++s;
        if (!number(&s,&suffix)||*s||!suffix) return false;
        *first=suffix>=size?0:size-suffix; return true;
    }
    if (!number(&s,first)||*s++!='-'||*first>=size) return false;
    if (!*s) return true;
    uint32_t end;
    if (!number(&s,&end)||*s||end<*first) return false;
    *last=end<size?end:size-1; return true;
}
const char *recording_reason_name(recording_reason_t reason) {
    switch(reason) { case REC_SILENCE:return "silence"; case REC_LIMIT:return "30s_limit";
    case REC_GAP:return "capture_gap"; default:return "io_error"; }
}
