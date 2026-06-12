// ====================================================================
//  EngineBridge - Obj-C facade over the C++ Engine, visible to Swift.
//  The Swift side never sees C++ types directly.
// ====================================================================
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@class MFBEngine;

@protocol MFBEngineDelegate <NSObject>
@optional
/// Latest full-canvas frame (RGBA8, width*height*4 bytes). Delivered on the main queue.
- (void)engine:(MFBEngine *)engine
    didProduceRGBAFrame:(NSData *)rgba
                  width:(NSInteger)width
                 height:(NSInteger)height;

/// Engine state changed (running, active clip, MIDI device). Delivered on the main queue.
- (void)engineStateDidChange:(MFBEngine *)engine;
@end

@interface MFBEngine : NSObject

@property (nonatomic, weak, nullable) id<MFBEngineDelegate> delegate;

@property (nonatomic, readonly) BOOL running;
@property (nonatomic, readonly, copy) NSString *activeClipName;
@property (nonatomic, readonly, copy) NSString *midiDeviceName;
@property (nonatomic, readonly) NSInteger canvasWidth;
@property (nonatomic, readonly) NSInteger canvasHeight;

- (instancetype)init;

/// Returns YES on success. statusServer enables the legacy HTTP status page on config.web_port.
- (BOOL)startWithConfigPath:(NSString *)configPath
                statusServer:(BOOL)statusServer;

- (void)stop;

- (void)triggerNote:(NSInteger)note;
- (void)triggerMappingAtIndex:(NSInteger)index;
- (void)stopActiveClip;
- (void)togglePause;
- (BOOL)isClipPaused;

/// Auto-play / test mode: cycle through every mapping endlessly.
- (void)setAutoPlay:(BOOL)on;
- (BOOL)isAutoPlay;

/// Returns an array of dictionaries: @{ @"note": NSNumber, @"clip": NSString, @"panel": NSString }.
- (NSArray<NSDictionary<NSString *, id> *> *)mappings;

/// Returns an array of dictionaries describing each panel from the loaded config:
/// @{ @"name", @"x", @"y", @"width", @"height", @"type" }.
- (NSArray<NSDictionary<NSString *, id> *> *)panels;

/// Returns live per-panel status (updated each engine tick):
/// @{ @"name", @"ip", @"port", @"framesSent", @"bytesSent", @"connected", @"activeClip", @"type" }.
/// Empty when the engine is not running.
- (NSArray<NSDictionary<NSString *, id> *> *)panelStatus;

/// SSH `sudo shutdown now` to all FT panels. No-op for ble/loopback panels.
- (void)shutdownPanels;

/// SSH `sudo shutdown now` to the named FT panel.
- (void)shutdownPanelNamed:(NSString *)name;

@end

NS_ASSUME_NONNULL_END
