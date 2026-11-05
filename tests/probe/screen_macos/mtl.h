/* The probe's own CAMetalLayer presenter, shared by the share, links,
 * present and bits sections. */
#ifndef YSP_SCREEN_MACOS_MTL_H
#define YSP_SCREEN_MACOS_MTL_H

#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include "probe.h"

enum { PM_ASAP = 0, PM_AT = 1, PM_MINDUR = 2 };

typedef struct frame_rec {
    int      run, frame, lead, late_inj, mode;
    double   frac;
    int64_t  t_req, t_got;       /* around nextDrawable, mach ns            */
    int64_t  t_commit;           /* after commit                            */
    int64_t  target;             /* presentAtTime target, 0 = none          */
    int64_t  planned;            /* planned vblank time, 0 = none           */
    int64_t  live_vblank;        /* newest CVDisplayLink vblank at commit   */
    int64_t  sched, done;        /* handler arrivals                        */
    int64_t  gpu_start, gpu_end; /* GPUStartTime, GPUEndTime                */
    int64_t  presented;          /* presentedTime, 0 = not shown            */
    int64_t  pres_arrive;        /* presented handler arrival               */
    uint64_t drawable_id;
    uint32_t readback;           /* share: the pixel read before present    */
    uint32_t expect;
    volatile int complete;
} frame_rec;

@interface Presenter : NSObject
@property(nonatomic) SDL_Window* window;
@property(nonatomic) SDL_MetalView view;
@property(nonatomic, strong) CAMetalLayer* layer;
@property(nonatomic, strong) id<MTLDevice> device;
@property(nonatomic, strong) id<MTLCommandQueue> queue;
@property(nonatomic, strong) id<MTLTexture> patchOn;
@property(nonatomic, strong) id<MTLTexture> patchOff;
@property(nonatomic) cv_link* cv;
@property(nonatomic) dispatch_semaphore_t presentedSem;
@property(nonatomic) volatile int outstanding;

/* Opens a window of g_opt.kind with a CAMetalLayer on `device` (NULL: the
 * system default), in `format`. Starts the CVDisplayLink grid. */
- (instancetype)initWithDevice:(id<MTLDevice>)device format:(MTLPixelFormat)format title:(const char*)title;
- (void)close;
/* Clears the frame to `level`, toggles the corner patch, presents by
 * `r->mode` (asap, at r->target, or after `mindur` seconds) and commits.
 * Fills r from the handlers. */
- (void)frame:(frame_rec*)r level:(float)level mindur:(double)mindur;
/* Adds the handlers to `cb`, presents `d` by r->mode and commits. */
- (void)commit:(id<MTLCommandBuffer>)cb drawable:(id<CAMetalDrawable>)d rec:(frame_rec*)r mindur:(double)mindur;
/* Waits until every committed frame has its presented handler, or 1 s. */
- (void)drain;
- (void)printLayer;
@end

/* Runs `work` on a new thread while the main thread pumps SDL and the run
 * loop, so the window stays responsive and fullscreen transitions finish. */
void run_worker(void (^work)(void));

/* Depth etc. for one run of frame records against the CVDisplayLink grid. */
void analyze_run(const char* name, frame_rec* r, int n, const grid* g);
grid presented_grid(const grid* g, const frame_rec* r, int n, double* phase_us);
void write_frames(FILE* csv, const char* name, const frame_rec* r, int n, const grid* g);
const char* frames_header(void);

#endif
