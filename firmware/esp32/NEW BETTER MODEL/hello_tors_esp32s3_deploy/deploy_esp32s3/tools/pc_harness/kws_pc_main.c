// PC harness for the firmware feature front end (kws_features.c), used by
// tools/verify_c_features.py. Not part of the firmware build.
//
//   kws_pc selftest                 - run the boot self-test feature check on the PC
//   kws_pc stream in.raw out.bin    - stream int16 PCM in 320-sample hops; after every
//                                     hop write the 49x40 int8 model input (1960 bytes)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kws_features.h"
#include "kws_test_vectors.h"

// Input quantization of hello_tors_kws_int8.tflite (feature_config.json).
#define INPUT_SCALE 0.5583019852638245f
#define INPUT_ZERO_POINT 93
#define N_FEAT (KWS_NUM_FRAMES * KWS_NUM_MFCC)

static int check(const char *name, const int16_t *pcm, const int8_t *expected) {
    static float audio[KWS_WINDOW_SAMPLES], mfcc[N_FEAT];
    for (int i = 0; i < KWS_WINDOW_SAMPLES; i++) audio[i] = pcm[i] / 32768.0f;
    kws_mfcc_window(audio, mfcc);
    int exact = 0, off1 = 0, worse = 0;
    for (int i = 0; i < N_FEAT; i++) {
        int d = abs(kws_quantize(mfcc[i], INPUT_SCALE, INPUT_ZERO_POINT) - expected[i]);
        if (d == 0) exact++; else if (d == 1) off1++; else worse++;
    }
    printf("%s: %d/%d exact, %d off by 1, %d worse\n", name, exact, N_FEAT, off1, worse);
    return worse == 0 && off1 <= N_FEAT / 100;
}

static int stream(const char *in_path, const char *out_path) {
    FILE *in = fopen(in_path, "rb"), *out = fopen(out_path, "wb");
    if (!in || !out) { perror("open"); return 1; }
    static kws_stream_t s;
    kws_stream_reset(&s);
    int16_t pcm[KWS_FRAME_STEP];
    float hop[KWS_FRAME_STEP];
    int8_t q[N_FEAT];
    long hops = 0;
    while (fread(pcm, sizeof(int16_t), KWS_FRAME_STEP, in) == KWS_FRAME_STEP) {
        for (int i = 0; i < KWS_FRAME_STEP; i++) hop[i] = pcm[i] / 32768.0f;
        kws_stream_push(&s, hop);
        kws_stream_quantize(&s, q, INPUT_SCALE, INPUT_ZERO_POINT);
        fwrite(q, 1, sizeof(q), out);
        hops++;
    }
    fclose(in); fclose(out);
    printf("streamed %ld hops\n", hops);
    return 0;
}

int main(int argc, char **argv) {
    kws_features_init();
    if (argc >= 2 && !strcmp(argv[1], "selftest")) {
        int ok = check("pos", kws_test_pos_pcm, kws_test_pos_features);
        ok &= check("neg", kws_test_neg_pcm, kws_test_neg_features);
        printf("feature self-test: %s\n", ok ? "PASS" : "FAIL");
        return ok ? 0 : 1;
    }
    if (argc == 4 && !strcmp(argv[1], "stream")) return stream(argv[2], argv[3]);
    fprintf(stderr, "usage: kws_pc selftest | kws_pc stream in.raw out.bin\n");
    return 2;
}
