#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "recording_format.h"
int main(void) {
    uint8_t wav[44], meta[REC_META_BYTES];
    recording_info_t in = {.id=42,.stream_id=7,.pcm_bytes=640,.pcm_crc=123,.reason=REC_SILENCE};
    recording_info_t out;
    recording_wav_header(wav,640);
    assert(!memcmp(wav,"RIFF",4) && !memcmp(wav+8,"WAVEfmt ",8));
    assert(wav[22]==1 && wav[24]==0x80 && wav[25]==0x3e && wav[34]==16);
    assert(wav[40]==0x80 && wav[41]==2);
    assert(recording_crc32(0,"123456789",9)==0xcbf43926u);
    uint32_t crc=recording_crc32(0,"1234",4);
    assert(recording_crc32(crc,"56789",5)==0xcbf43926u);
    recording_encode(meta,&in);
    assert(recording_decode(meta,&out) && out.id==42 && out.pcm_bytes==640);
    meta[12]^=1; assert(!recording_decode(meta,&out));
    memset(meta,255,sizeof(meta)); assert(!recording_decode(meta,&out));
    uint32_t id,first,last;
    assert(recording_parse_path("/recordings/42.wav", &id) && id==42);
    assert(!recording_parse_path("/recordings/../42.wav", &id));
    assert(!recording_parse_path("/recordings/4294967296.wav", &id));
    assert(!recording_parse_path("/recordings/0.wav", &id));
    assert(recording_parse_range(NULL,684,&first,&last) && first==0 && last==683);
    assert(recording_parse_range("bytes=0-43",684,&first,&last) && last==43);
    assert(recording_parse_range("bytes=44-",684,&first,&last) && first==44 && last==683);
    assert(recording_parse_range("bytes=-10",684,&first,&last) && first==674);
    assert(recording_parse_range("bytes=20-999",684,&first,&last) && last==683);
    assert(!recording_parse_range("bytes=684-",684,&first,&last));
    assert(!recording_parse_range("bytes=0-1,3-4",684,&first,&last));
    assert(!recording_parse_range("bytes=9-4",684,&first,&last));
    assert(!recording_parse_range("bytes=42949672960-",684,&first,&last));
    in.pcm_bytes=REC_MAX_PCM_BYTES+640; recording_encode(meta,&in);
    assert(!recording_decode(meta,&out));
    in.pcm_bytes=640; in.reason=(recording_reason_t)-1; recording_encode(meta,&in);
    assert(!recording_decode(meta,&out));
    puts("Recording format: WAV, metadata CRC, bounds, paths and ranges passed");
}
