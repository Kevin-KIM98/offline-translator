// Objective-C interface over translator_c_api.h — usable from Swift via the bridging header
// (or as the public header of an OfflineTranslator.framework).
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

typedef BOOL (^OTProgressBlock)(uint64_t bytesDone, uint64_t bytesTotal); // return NO to abort

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

@interface OTPipelineConfig : NSObject
@property (nonatomic, copy, nullable) NSString *whisperModelPath;
@property (nonatomic, copy) NSString *nmtRootDir;
@property (nonatomic) NSInteger threads;            // default 4
@property (nonatomic) BOOL useGPU;                  // Metal (default YES)
@property (nonatomic) BOOL enableDenoise;           // RNNoise (default YES)
@property (nonatomic) NSInteger beamSize;           // default 2
@property (nonatomic) NSInteger maxDecodingLength;  // default 256
@property (nonatomic, copy) NSArray<NSString *> *pivotLanguages; // default @[@"ko", @"en"]
@property (nonatomic, copy, nullable) NSString *initialPrompt;
@property (nonatomic) BOOL preloadAllPairs;
@end

@interface OTSegmenterConfig : NSObject
@property (nonatomic) float startThreshold;   // 0.60
@property (nonatomic) float endThreshold;     // 0.35
@property (nonatomic) NSInteger startFrames;  // 3
@property (nonatomic) NSInteger endSilenceMs; // 700
@property (nonatomic) NSInteger minUtteranceMs; // 400
@property (nonatomic) NSInteger maxUtteranceMs; // 15000
@property (nonatomic) NSInteger preRollMs;    // 300
@end

// ---------------------------------------------------------------------------
// Results
// ---------------------------------------------------------------------------

@interface OTTranslationResult : NSObject
@property (nonatomic, readonly) BOOL ok;
@property (nonatomic, readonly, copy, nullable) NSString *error;
@property (nonatomic, readonly, copy) NSString *sourceText;
@property (nonatomic, readonly, copy) NSString *sourceLang;
@property (nonatomic, readonly, copy) NSString *targetLang;
@property (nonatomic, readonly, copy) NSString *translatedText;
@property (nonatomic, readonly, copy) NSArray<NSString *> *route;
@property (nonatomic, readonly) double sttMs;
@property (nonatomic, readonly) double nmtMs;
@property (nonatomic, readonly) double totalMs;
/// YES when STT heard nothing (silence) — not an error.
@property (nonatomic, readonly) BOOL isEmpty;
@property (nonatomic, readonly, copy) NSDictionary *raw;
@end

@interface OTSttResult : NSObject
@property (nonatomic, readonly) BOOL ok;
@property (nonatomic, readonly, copy, nullable) NSString *error;
@property (nonatomic, readonly, copy) NSString *text;
@property (nonatomic, readonly, copy) NSString *detectedLang;
@property (nonatomic, readonly) double elapsedMs;
@end

// ---------------------------------------------------------------------------
// Pipeline
// ---------------------------------------------------------------------------

@interface OTTranslationPipeline : NSObject

+ (NSString *)version;
+ (NSDictionary *)buildCapabilities;
+ (NSArray<NSString *> *)supportedLanguages;

/// Returns nil and sets *error on failure (missing model, etc.).
- (nullable instancetype)initWithConfig:(OTPipelineConfig *)config error:(NSError **)error;

@property (nonatomic, readonly) NSString *lastError;
@property (nonatomic, readonly) NSDictionary *capabilities;
@property (nonatomic, readonly) NSArray<NSString *> *availablePairs;

- (BOOL)setSegmenterConfig:(OTSegmenterConfig *)config;

// Streaming — call from the audio thread. Returns YES when an utterance is ready.
- (BOOL)feedAudio:(const float *)pcm count:(NSUInteger)count;
- (BOOL)feedAudioInt16:(const int16_t *)pcm count:(NSUInteger)count;
@property (nonatomic, readonly) NSInteger pendingUtteranceCount;
- (BOOL)flushAudio;
- (void)resetAudio;
/// Blocking: run on a background queue.
- (OTTranslationResult *)processPendingWithSourceLang:(NSString *)sourceLang targetLang:(NSString *)targetLang;

// One-shot (blocking).
- (OTSttResult *)transcribe:(const float *)pcm count:(NSUInteger)count sourceLang:(NSString *)sourceLang;
- (OTTranslationResult *)translateText:(NSString *)text sourceLang:(NSString *)sourceLang targetLang:(NSString *)targetLang;
- (OTTranslationResult *)processSpeech:(const float *)pcm count:(NSUInteger)count
                             sourceLang:(NSString *)sourceLang targetLang:(NSString *)targetLang;

// NMT management.
- (BOOL)preloadPairFrom:(NSString *)src to:(NSString *)tgt;
- (void)unloadPairFrom:(NSString *)src to:(NSString *)tgt;
- (BOOL)canTranslateFrom:(NSString *)src to:(NSString *)tgt;

@end

// ---------------------------------------------------------------------------
// Model manager
// ---------------------------------------------------------------------------

typedef NS_ENUM(NSInteger, OTModelState) {
    OTModelStateReady,
    OTModelStateMissing,
    OTModelStateCorrupt,
    OTModelStateUpdateAvailable,
    OTModelStateUnverified,
};

@interface OTDownloadItem : NSObject
@property (nonatomic, readonly, copy) NSURL *url;
@property (nonatomic, readonly, copy) NSString *filename;
@property (nonatomic, readonly) uint64_t sizeBytes;
@property (nonatomic, readonly, copy) NSString *sha256;
@property (nonatomic, readonly) BOOL isArchive;
@end

@interface OTModelStatus : NSObject
@property (nonatomic, readonly, copy) NSString *identifier;
@property (nonatomic, readonly, copy) NSString *kind;      // stt | nmt | tokenizer
@property (nonatomic, readonly, copy) NSString *pair;      // "ko-en" for nmt
@property (nonatomic, readonly, copy) NSString *version;
@property (nonatomic, readonly) OTModelState state;
@property (nonatomic, readonly, copy) NSString *detail;
@property (nonatomic, readonly, copy) NSString *installPath;
@property (nonatomic, readonly) uint64_t totalBytes;
@property (nonatomic, readonly, copy) NSArray<OTDownloadItem *> *downloads;
@property (nonatomic, readonly) BOOL needsDownload;
@end

typedef NS_ENUM(NSInteger, OTVerifyResult) {
    OTVerifyResultOk = 1,
    OTVerifyResultMismatch = 0,
    OTVerifyResultIOError = -1,
    OTVerifyResultAborted = -2,
};

@interface OTModelManager : NSObject

/// modelsRoot: e.g. <Application Support>/models (created if needed).
- (instancetype)initWithModelsRoot:(NSString *)modelsRoot;

@property (nonatomic, readonly) NSString *modelsRoot;
@property (nonatomic, readonly) NSString *lastError;
@property (nonatomic, readonly) NSString *manifestVersion;
@property (nonatomic, readonly) NSString *sttModelPath;
@property (nonatomic, readonly) NSString *nmtRootDir;

- (BOOL)loadManifestFile:(NSString *)path;
- (BOOL)loadManifestJSON:(NSString *)json;
- (BOOL)loadCachedManifest;
- (BOOL)saveManifest;

/// deepVerify re-hashes installed files — slow, run on a background queue.
- (NSArray<OTModelStatus *> *)statusWithDeepVerify:(BOOL)deepVerify;
- (NSArray<OTModelStatus *> *)statusForLanguages:(NSArray<NSString *> *)languages deepVerify:(BOOL)deepVerify;
- (uint64_t)pendingBytesForLanguages:(nullable NSArray<NSString *> *)languages;

- (NSString *)stagingDirForModel:(NSString *)identifier;
- (BOOL)clearStagingForModel:(nullable NSString *)identifier;
- (OTVerifyResult)verifyFile:(NSString *)path sha256:(NSString *)sha256 expectedSize:(uint64_t)size
                    progress:(nullable OTProgressBlock)progress;
- (BOOL)installModel:(NSString *)identifier stagedPath:(NSString *)stagedPath verifyHashes:(BOOL)verifyHashes
            progress:(nullable OTProgressBlock)progress;
- (BOOL)removeModel:(NSString *)identifier;

- (OTPipelineConfig *)pipelineConfig;

@end

NS_ASSUME_NONNULL_END
