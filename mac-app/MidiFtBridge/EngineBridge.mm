// ====================================================================
//  EngineBridge - Implementation. Wraps the C++ Engine and adapts its
//  worker-thread frame callbacks into main-queue delegate calls.
// ====================================================================
#import "EngineBridge.h"

#include "engine.h"
#include "config.h"

#include <memory>
#include <string>
#include <vector>

@implementation MFBEngine {
    std::unique_ptr<Engine> _engine;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _engine = std::make_unique<Engine>();
    }
    return self;
}

- (void)dealloc {
    if (_engine) _engine->stop();
}

#pragma mark - Lifecycle

- (BOOL)startWithConfigPath:(NSString *)configPath statusServer:(BOOL)statusServer {
    if (!_engine) return NO;

    __weak MFBEngine *weakSelf = self;

    // Frame callback runs on the engine worker thread. Convert RGB24 -> RGBA8
    // here (small canvases, cheap) and dispatch to the main queue for delivery.
    _engine->setFrameCallback([weakSelf](const uint8_t *rgb, int w, int h) {
        if (!rgb || w <= 0 || h <= 0) return;
        const size_t pixelCount = (size_t)w * (size_t)h;
        NSMutableData *rgba = [[NSMutableData alloc] initWithLength:pixelCount * 4];
        uint8_t *out = (uint8_t *)rgba.mutableBytes;
        for (size_t i = 0; i < pixelCount; ++i) {
            out[i*4 + 0] = rgb[i*3 + 0];
            out[i*4 + 1] = rgb[i*3 + 1];
            out[i*4 + 2] = rgb[i*3 + 2];
            out[i*4 + 3] = 0xFF;
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            MFBEngine *strong = weakSelf;
            if (!strong) return;
            id<MFBEngineDelegate> d = strong.delegate;
            if ([d respondsToSelector:@selector(engine:didProduceRGBAFrame:width:height:)]) {
                [d engine:strong didProduceRGBAFrame:rgba width:w height:h];
            }
        });
    });

    bool ok = _engine->start(std::string([configPath fileSystemRepresentation]),
                             statusServer ? true : false);
    if (ok) {
        [self notifyStateChange];
    }
    return ok ? YES : NO;
}

- (void)stop {
    if (!_engine) return;
    _engine->stop();
    [self notifyStateChange];
}

- (void)notifyStateChange {
    __weak MFBEngine *weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        MFBEngine *strong = weakSelf;
        if (!strong) return;
        id<MFBEngineDelegate> d = strong.delegate;
        if ([d respondsToSelector:@selector(engineStateDidChange:)]) {
            [d engineStateDidChange:strong];
        }
    });
}

#pragma mark - State accessors

- (BOOL)running {
    return _engine && _engine->isRunning();
}

- (NSString *)activeClipName {
    if (!_engine) return @"";
    return [NSString stringWithUTF8String:_engine->getActiveClipName().c_str()];
}

- (NSString *)midiDeviceName {
    if (!_engine) return @"";
    return [NSString stringWithUTF8String:_engine->getMidiDeviceName().c_str()];
}

- (NSInteger)canvasWidth  { return _engine ? _engine->getConfig().video_width  : 0; }
- (NSInteger)canvasHeight { return _engine ? _engine->getConfig().video_height : 0; }

#pragma mark - Commands

- (void)triggerNote:(NSInteger)note               { if (_engine) _engine->triggerNote((int)note); [self notifyStateChange]; }
- (void)triggerMappingAtIndex:(NSInteger)index    { if (_engine) _engine->triggerMapping((int)index); [self notifyStateChange]; }
- (void)stopActiveClip                            { if (_engine) _engine->stopActiveClip(); [self notifyStateChange]; }
- (void)togglePause                               { if (_engine) _engine->togglePause(); [self notifyStateChange]; }
- (BOOL)isClipPaused                              { return _engine && _engine->isClipPaused(); }
- (void)setAutoPlay:(BOOL)on                      { if (_engine) _engine->setAutoPlay(on); [self notifyStateChange]; }
- (BOOL)isAutoPlay                                { return _engine && _engine->isAutoPlay(); }
- (double)playbackPosition                        { return _engine ? _engine->getPosition() : 0.0; }
- (double)playbackDuration                        { return _engine ? _engine->getDuration() : 0.0; }
- (void)seekTo:(double)seconds                    { if (_engine) _engine->seekTo(seconds); }
- (void)seekBy:(double)seconds                    { if (_engine) _engine->seekBy(seconds); }
- (void)skipToNextClip                            { if (_engine) _engine->skipToNext(); [self notifyStateChange]; }
- (void)skipToPreviousClip                        { if (_engine) _engine->skipToPrevious(); [self notifyStateChange]; }

#pragma mark - Config snapshots

- (NSArray<NSDictionary<NSString *, id> *> *)mappings {
    NSMutableArray *out = [NSMutableArray new];
    if (!_engine) return out;
    const Config &cfg = _engine->getConfig();
    for (const auto &m : cfg.mappings) {
        [out addObject:@{
            @"note":  @(m.note),
            @"clip":  [NSString stringWithUTF8String:m.clip.c_str()],
            @"panel": [NSString stringWithUTF8String:m.panel.c_str()],
        }];
    }
    return out;
}

- (NSArray<NSDictionary<NSString *, id> *> *)panels {
    NSMutableArray *out = [NSMutableArray new];
    if (!_engine) return out;
    const Config &cfg = _engine->getConfig();
    for (const auto &p : cfg.panels) {
        [out addObject:@{
            @"name":   [NSString stringWithUTF8String:p.name.c_str()],
            @"x":      @(p.src_x),
            @"y":      @(p.src_y),
            @"width":  @(p.src_w),
            @"height": @(p.src_h),
            @"type":   [NSString stringWithUTF8String:p.type.c_str()],
        }];
    }
    return out;
}

- (NSArray<NSDictionary<NSString *, id> *> *)panelStatus {
    NSMutableArray *out = [NSMutableArray new];
    if (!_engine) return out;
    for (const auto &p : _engine->getPanelStatus()) {
        [out addObject:@{
            @"name":       [NSString stringWithUTF8String:p.name.c_str()],
            @"ip":         [NSString stringWithUTF8String:p.ip.c_str()],
            @"port":       @(p.port),
            @"framesSent": @(p.framesSent),
            @"bytesSent":  @(p.bytesSent),
            @"connected":  @(p.enabled),
            @"activeClip": [NSString stringWithUTF8String:p.activeClip.c_str()],
            @"type":       [NSString stringWithUTF8String:p.type.c_str()],
        }];
    }
    return out;
}

- (void)shutdownPanels {
    if (_engine) _engine->shutdownPanels();
}

- (void)shutdownPanelNamed:(NSString *)name {
    if (_engine) _engine->shutdownPanel(std::string([name UTF8String]));
}

@end
