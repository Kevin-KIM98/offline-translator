#import "OfflineTranslator.h"

#include "translator_c_api.h"

#include <string>
#include <vector>

NSString *const OTErrorDomain = @"com.offlinetranslator";

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static NSString *OTTake(char *s) {
    if (!s) return @"";
    NSString *out = [NSString stringWithUTF8String:s] ?: @"";
    tr_string_free(s);
    return out;
}

static id OTParseJSON(NSString *json) {
    NSData *data = [json dataUsingEncoding:NSUTF8StringEncoding];
    if (!data) return nil;
    return [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
}

static NSDictionary *OTDict(NSString *json) {
    id v = OTParseJSON(json);
    return [v isKindOfClass:[NSDictionary class]] ? v : @{};
}

static int OTProgressTrampoline(uint64_t done, uint64_t total, void *user) {
    OTProgressBlock block = (__bridge OTProgressBlock)user;
    return block ? (block(done, total) ? 1 : 0) : 1;
}

static NSString *OTStr(id v) { return [v isKindOfClass:[NSString class]] ? v : @""; }

// ---------------------------------------------------------------------------
// Config objects
// ---------------------------------------------------------------------------

@implementation OTPipelineConfig
- (instancetype)init {
    if ((self = [super init])) {
        _nmtRootDir = @"";
        _threads = 4;
        _useGPU = YES;
        _enableDenoise = YES;
        _beamSize = 2;
        _maxDecodingLength = 256;
        _pivotLanguages = @[ @"ko", @"en" ];
    }
    return self;
}
@end

@implementation OTSegmenterConfig
- (instancetype)init {
    if ((self = [super init])) {
        tr_segmenter_config c;
        tr_segmenter_config_init(&c);
        _startThreshold = c.start_threshold;
        _endThreshold = c.end_threshold;
        _startFrames = c.start_frames;
        _endSilenceMs = c.end_silence_ms;
        _minUtteranceMs = c.min_utterance_ms;
        _maxUtteranceMs = c.max_utterance_ms;
        _preRollMs = c.pre_roll_ms;
    }
    return self;
}
@end

// ---------------------------------------------------------------------------
// Results
// ---------------------------------------------------------------------------

@implementation OTTranslationResult
- (instancetype)initWithJSON:(NSString *)json {
    if ((self = [super init])) {
        NSDictionary *d = OTDict(json);
        _raw = d;
        _ok = [d[@"ok"] boolValue];
        _error = [d[@"error"] isKindOfClass:[NSString class]] ? d[@"error"] : nil;
        _sourceText = OTStr(d[@"source_text"]);
        _sourceLang = OTStr(d[@"source_lang"]);
        _targetLang = OTStr(d[@"target_lang"]);
        _translatedText = OTStr(d[@"translated_text"]);
        _route = [d[@"route"] isKindOfClass:[NSArray class]] ? d[@"route"] : @[];
        NSDictionary *t = [d[@"timings"] isKindOfClass:[NSDictionary class]] ? d[@"timings"] : @{};
        _sttMs = [t[@"stt_ms"] doubleValue];
        _nmtMs = [t[@"nmt_ms"] doubleValue];
        _totalMs = [t[@"total_ms"] doubleValue];
    }
    return self;
}
- (BOOL)isEmpty {
    return _ok && [[_sourceText stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet] length] == 0;
}
@end

@implementation OTSttResult
- (instancetype)initWithJSON:(NSString *)json {
    if ((self = [super init])) {
        NSDictionary *d = OTDict(json);
        _ok = [d[@"ok"] boolValue];
        _error = [d[@"error"] isKindOfClass:[NSString class]] ? d[@"error"] : nil;
        _text = OTStr(d[@"text"]);
        _detectedLang = OTStr(d[@"detected_lang"]);
        _elapsedMs = [d[@"elapsed_ms"] doubleValue];
    }
    return self;
}
@end

// ---------------------------------------------------------------------------
// Pipeline
// ---------------------------------------------------------------------------

@implementation OTTranslationPipeline {
    tr_pipeline *_p;
}

+ (NSString *)version { return [NSString stringWithUTF8String:tr_version()]; }
+ (NSDictionary *)buildCapabilities { return OTDict(OTTake(tr_build_capabilities())); }
+ (NSArray<NSString *> *)supportedLanguages { return @[ @"ko", @"en", @"ja", @"zh", @"es" ]; }

- (nullable instancetype)initWithConfig:(OTPipelineConfig *)config error:(NSError **)error {
    if ((self = [super init])) {
        tr_pipeline_config c;
        tr_pipeline_config_init(&c);
        std::string whisper = config.whisperModelPath ? config.whisperModelPath.UTF8String : "";
        std::string nmt = config.nmtRootDir.UTF8String;
        std::string pivots = [config.pivotLanguages componentsJoinedByString:@","].UTF8String;
        std::string prompt = config.initialPrompt ? config.initialPrompt.UTF8String : "";
        c.whisper_model_path = whisper.empty() ? nullptr : whisper.c_str();
        c.nmt_root_dir = nmt.c_str();
        c.n_threads = (int)config.threads;
        c.use_gpu = config.useGPU ? 1 : 0;
        c.enable_denoise = config.enableDenoise ? 1 : 0;
        c.beam_size = (int)config.beamSize;
        c.max_decoding_length = (int)config.maxDecodingLength;
        c.pivot_langs = pivots.c_str();
        c.initial_prompt = prompt.empty() ? nullptr : prompt.c_str();
        c.preload_all_pairs = config.preloadAllPairs ? 1 : 0;
        _p = tr_pipeline_create(&c);
        if (!_p) {
            if (error) {
                *error = [NSError errorWithDomain:OTErrorDomain code:1
                                         userInfo:@{NSLocalizedDescriptionKey : [NSString stringWithUTF8String:tr_last_global_error()]}];
            }
            return nil;
        }
    }
    return self;
}

- (void)dealloc {
    if (_p) tr_pipeline_destroy(_p);
}

- (NSString *)lastError { return [NSString stringWithUTF8String:tr_pipeline_last_error(_p)]; }
- (NSDictionary *)capabilities { return OTDict(OTTake(tr_pipeline_capabilities(_p))); }
- (NSArray<NSString *> *)availablePairs {
    id v = OTParseJSON(OTTake(tr_pipeline_available_pairs(_p)));
    return [v isKindOfClass:[NSArray class]] ? v : @[];
}

- (BOOL)setSegmenterConfig:(OTSegmenterConfig *)config {
    tr_segmenter_config c;
    c.start_threshold = config.startThreshold;
    c.end_threshold = config.endThreshold;
    c.start_frames = (int)config.startFrames;
    c.end_silence_ms = (int)config.endSilenceMs;
    c.min_utterance_ms = (int)config.minUtteranceMs;
    c.max_utterance_ms = (int)config.maxUtteranceMs;
    c.pre_roll_ms = (int)config.preRollMs;
    return tr_pipeline_set_segmenter_config(_p, &c) != 0;
}

- (BOOL)feedAudio:(const float *)pcm count:(NSUInteger)count { return tr_pipeline_feed_audio(_p, pcm, count) != 0; }
- (BOOL)feedAudioInt16:(const int16_t *)pcm count:(NSUInteger)count { return tr_pipeline_feed_audio_i16(_p, pcm, count) != 0; }
- (NSInteger)pendingUtteranceCount { return tr_pipeline_pending_count(_p); }
- (BOOL)flushAudio { return tr_pipeline_flush_audio(_p) != 0; }
- (void)resetAudio { tr_pipeline_reset_audio(_p); }

- (OTTranslationResult *)processPendingWithSourceLang:(NSString *)sourceLang targetLang:(NSString *)targetLang {
    return [[OTTranslationResult alloc] initWithJSON:OTTake(tr_pipeline_process_pending(_p, sourceLang.UTF8String, targetLang.UTF8String))];
}

- (OTSttResult *)transcribe:(const float *)pcm count:(NSUInteger)count sourceLang:(NSString *)sourceLang {
    return [[OTSttResult alloc] initWithJSON:OTTake(tr_pipeline_transcribe(_p, pcm, count, sourceLang.UTF8String))];
}

- (OTTranslationResult *)translateText:(NSString *)text sourceLang:(NSString *)sourceLang targetLang:(NSString *)targetLang {
    return [[OTTranslationResult alloc] initWithJSON:OTTake(tr_pipeline_translate_text(_p, text.UTF8String, sourceLang.UTF8String, targetLang.UTF8String))];
}

- (OTTranslationResult *)processSpeech:(const float *)pcm count:(NSUInteger)count
                             sourceLang:(NSString *)sourceLang targetLang:(NSString *)targetLang {
    return [[OTTranslationResult alloc] initWithJSON:OTTake(tr_pipeline_process_speech(_p, pcm, count, sourceLang.UTF8String, targetLang.UTF8String))];
}

- (BOOL)preloadPairFrom:(NSString *)src to:(NSString *)tgt { return tr_pipeline_preload_pair(_p, src.UTF8String, tgt.UTF8String) != 0; }
- (void)unloadPairFrom:(NSString *)src to:(NSString *)tgt { tr_pipeline_unload_pair(_p, src.UTF8String, tgt.UTF8String); }
- (BOOL)canTranslateFrom:(NSString *)src to:(NSString *)tgt { return tr_pipeline_can_translate(_p, src.UTF8String, tgt.UTF8String) != 0; }

@end

// ---------------------------------------------------------------------------
// Model manager
// ---------------------------------------------------------------------------

@implementation OTDownloadItem
- (instancetype)initWithDict:(NSDictionary *)d {
    if ((self = [super init])) {
        _url = [NSURL URLWithString:OTStr(d[@"url"])] ?: [NSURL URLWithString:@"about:blank"];
        _filename = OTStr(d[@"filename"]);
        _sizeBytes = [d[@"size_bytes"] unsignedLongLongValue];
        _sha256 = OTStr(d[@"sha256"]);
        _isArchive = [d[@"archive"] boolValue];
    }
    return self;
}
@end

@implementation OTModelStatus
- (instancetype)initWithDict:(NSDictionary *)d {
    if ((self = [super init])) {
        _identifier = OTStr(d[@"id"]);
        _kind = OTStr(d[@"kind"]);
        _pair = OTStr(d[@"pair"]);
        _version = OTStr(d[@"version"]);
        NSString *s = OTStr(d[@"state"]);
        if ([s isEqualToString:@"ready"]) _state = OTModelStateReady;
        else if ([s isEqualToString:@"missing"]) _state = OTModelStateMissing;
        else if ([s isEqualToString:@"corrupt"]) _state = OTModelStateCorrupt;
        else if ([s isEqualToString:@"update_available"]) _state = OTModelStateUpdateAvailable;
        else _state = OTModelStateUnverified;
        _detail = OTStr(d[@"detail"]);
        _installPath = OTStr(d[@"install_path"]);
        _totalBytes = [d[@"total_bytes"] unsignedLongLongValue];
        NSMutableArray *dl = [NSMutableArray array];
        for (id item in ([d[@"downloads"] isKindOfClass:[NSArray class]] ? d[@"downloads"] : @[])) {
            if ([item isKindOfClass:[NSDictionary class]]) [dl addObject:[[OTDownloadItem alloc] initWithDict:item]];
        }
        _downloads = dl;
    }
    return self;
}
- (BOOL)needsDownload { return _state != OTModelStateReady; }
@end

@implementation OTModelManager {
    tr_model_manager *_m;
}

- (instancetype)initWithModelsRoot:(NSString *)modelsRoot {
    if ((self = [super init])) {
        _modelsRoot = [modelsRoot copy];
        [[NSFileManager defaultManager] createDirectoryAtPath:modelsRoot withIntermediateDirectories:YES attributes:nil error:nil];
        // Never let iCloud back up 1.5 GB of models.
        NSURL *url = [NSURL fileURLWithPath:modelsRoot];
        [url setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:nil];
        _m = tr_mm_create(modelsRoot.UTF8String);
    }
    return self;
}

- (void)dealloc {
    if (_m) tr_mm_destroy(_m);
}

- (NSString *)lastError { return [NSString stringWithUTF8String:tr_mm_last_error(_m)]; }
- (NSString *)manifestVersion { return [NSString stringWithUTF8String:tr_mm_manifest_version(_m)]; }
- (NSString *)sttModelPath { return OTTake(tr_mm_stt_model_path(_m)); }
- (NSString *)nmtRootDir { return OTTake(tr_mm_nmt_root_dir(_m)); }

- (BOOL)loadManifestFile:(NSString *)path { return tr_mm_load_manifest_file(_m, path.UTF8String) != 0; }
- (BOOL)loadManifestJSON:(NSString *)json { return tr_mm_load_manifest_json(_m, json.UTF8String) != 0; }
- (BOOL)loadCachedManifest { return tr_mm_load_cached_manifest(_m) != 0; }
- (BOOL)saveManifest { return tr_mm_save_manifest(_m) != 0; }

- (NSArray<OTModelStatus *> *)parseStatus:(NSString *)json {
    NSMutableArray *out = [NSMutableArray array];
    for (id item in ([OTDict(json)[@"models"] isKindOfClass:[NSArray class]] ? OTDict(json)[@"models"] : @[])) {
        if ([item isKindOfClass:[NSDictionary class]]) [out addObject:[[OTModelStatus alloc] initWithDict:item]];
    }
    return out;
}

- (NSArray<OTModelStatus *> *)statusWithDeepVerify:(BOOL)deepVerify {
    return [self parseStatus:OTTake(tr_mm_status_json(_m, deepVerify ? 1 : 0))];
}

- (NSArray<OTModelStatus *> *)statusForLanguages:(NSArray<NSString *> *)languages deepVerify:(BOOL)deepVerify {
    NSString *csv = [languages componentsJoinedByString:@","];
    return [self parseStatus:OTTake(tr_mm_status_for_languages_json(_m, csv.UTF8String, deepVerify ? 1 : 0))];
}

- (uint64_t)pendingBytesForLanguages:(nullable NSArray<NSString *> *)languages {
    NSArray *st = languages ? [self statusForLanguages:languages deepVerify:NO] : [self statusWithDeepVerify:NO];
    uint64_t total = 0;
    for (OTModelStatus *s in st)
        if (s.needsDownload) total += s.totalBytes;
    return total;
}

- (NSString *)stagingDirForModel:(NSString *)identifier { return OTTake(tr_mm_staging_dir(_m, identifier.UTF8String)); }
- (BOOL)clearStagingForModel:(nullable NSString *)identifier { return tr_mm_clear_staging(_m, identifier ? identifier.UTF8String : nullptr) != 0; }

- (OTVerifyResult)verifyFile:(NSString *)path sha256:(NSString *)sha256 expectedSize:(uint64_t)size progress:(nullable OTProgressBlock)progress {
    return (OTVerifyResult)tr_mm_verify_file(_m, path.UTF8String, sha256.UTF8String, size,
                                             progress ? OTProgressTrampoline : nullptr, (__bridge void *)progress);
}

- (BOOL)installModel:(NSString *)identifier stagedPath:(NSString *)stagedPath verifyHashes:(BOOL)verifyHashes progress:(nullable OTProgressBlock)progress {
    return tr_mm_install(_m, identifier.UTF8String, stagedPath.UTF8String, verifyHashes ? 1 : 0,
                         progress ? OTProgressTrampoline : nullptr, (__bridge void *)progress) != 0;
}

- (BOOL)removeModel:(NSString *)identifier { return tr_mm_remove(_m, identifier.UTF8String) != 0; }

- (OTPipelineConfig *)pipelineConfig {
    OTPipelineConfig *c = [OTPipelineConfig new];
    NSString *stt = self.sttModelPath;
    c.whisperModelPath = stt.length ? stt : nil;
    c.nmtRootDir = self.nmtRootDir;
    c.threads = MAX(2, MIN(6, (NSInteger)[NSProcessInfo processInfo].activeProcessorCount));
    return c;
}

@end
