#pragma once

/*
 * Sound output on Intel High Definition Audio controllers (every PC since
 * ~2005, and QEMU's ich9-intel-hda / intel-hda). Programs play 16-bit PCM
 * through it, several at once (mixed); see SYS_AUDIO_* in abi/viralos.h.
 */

#include <stdbool.h>
#include <stdint.h>

struct process;

/* Find and set up the controller and its codec. False if there's no sound card. */
bool audio_init(void);
/* What was found, for the boot log: the codec's vendor/device id and how many outputs it drives. */
void audio_info(uint32_t *codec_id, int *outputs);

int audio_open(uint64_t pid, uint32_t rate, uint32_t channels);
int64_t audio_write(struct process *p, uint64_t user_buf, uint64_t len, uint64_t flags);
int64_t audio_ctl(uint64_t pid, uint64_t op, uint64_t arg);
void audio_process_exited(uint64_t pid);
/* SYS_AUDIO_MIXER: the system volume, mute and output (anyone may change them). */
int64_t audio_mixer(struct process *p, uint64_t op, uint64_t value, uint64_t umixer);
