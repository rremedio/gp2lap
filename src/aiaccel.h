#ifndef _AIACCEL_H
#define _AIACCEL_H

// 2026: AI low-HP "slow launch" fix. Fades the AI accel HP multiplier in with speed so a
// carset edited below stock HP no longer launches / exits slow corners disproportionately
// slowly. See docs/ai-behavior.md sect 4.2.

extern unsigned long AILowHpLaunchFix;      // 0 = off (set on once the patch is installed)
extern unsigned long AILaunchFadeBuckets;   // K: crossover speed in speed-buckets (the asm stub reads this)

void AiLaunchFixInit(void);                 // read cfg + install the 0x22F25 trampoline (call once, late init)

#endif
