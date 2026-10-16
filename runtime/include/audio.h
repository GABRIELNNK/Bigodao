/**
 * @file audio.h
 * @brief GameBoy Audio Processing Unit definitions
 */

#ifndef AUDIO_H
#define AUDIO_H

#include "gbrt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Public Interface
 * ========================================================================== */

/**
 * @brief Create audio subsystem state
 */
void* gb_audio_create(void);

/**
 * @brief Destroy audio subsystem state
 */
void gb_audio_destroy(void* apu);

/**
 * @brief Reset audio state
 */
void gb_audio_reset(void* apu);

/**
 * @brief Read from audio register
 */
uint8_t gb_audio_read(GBContext* ctx, uint16_t addr);

/**
 * @brief Write to audio register
 */
void gb_audio_write(GBContext* ctx, uint16_t addr, uint8_t value);

/**
 * @brief Step audio subsystem
 */
void gb_audio_step(GBContext* ctx, uint32_t cycles);

/**
 * @brief Return system cycles until the next exact output-sample deadline
 *
 * Returns UINT32_MAX while the APU is powered off.
 */
uint32_t gb_audio_cycles_until_sample(const void* apu);

/**
 * @brief Step audio while preserving DIV-APU event ordering across a CPU batch
 *
 * @param old_div Divider value before the CPU batch
 * @param cpu_cycles CPU T-cycles in the batch
 * @param double_speed Whether the CPU batch ran in CGB double-speed mode
 * @param system_cycle_remainder System-cycle half-step carried into the batch
 */
void gb_audio_step_timed(GBContext* ctx,
                         uint16_t old_div,
                         uint32_t cpu_cycles,
                         bool double_speed,
                         uint8_t system_cycle_remainder);

/**
 * @brief Advance frame-sequencer timing from DIV transitions
 * @param old_div Divider value before the CPU step
 * @param new_div Divider value after the CPU step
 */
void gb_audio_div_tick(void* apu, uint16_t old_div, uint16_t new_div, bool double_speed);

/**
 * @brief Handle a write to DIV, including any APU clock edge it causes
 * @param old_div Divider value before it was reset to 0
 */
void gb_audio_div_reset(void* apu, uint16_t old_div, bool double_speed);

/**
 * @brief Get current sample for left/right channels
 * @param apu Audio state
 * @param left Pointer to store left sample
 * @param right Pointer to store right sample
 */
void gb_audio_get_samples(void* apu, int16_t* left, int16_t* right);

/**
 * @brief Read CGB PCM12 digital output register
 * @param apu Audio state
 */
uint8_t gb_audio_read_pcm12(void* apu);

/**
 * @brief Read CGB PCM34 digital output register
 * @param apu Audio state
 */
uint8_t gb_audio_read_pcm34(void* apu);

/**
 * @brief Return the serialized size of the audio state blob
 */
size_t gb_audio_state_size(void);

/**
 * @brief Serialize audio state into a caller-provided buffer
 * @return true on success
 */
bool gb_audio_save_state(const void* apu, void* out_data, size_t size);

/**
 * @brief Restore audio state from a caller-provided buffer
 * @return true on success
 */
bool gb_audio_load_state(void* apu, const void* data, size_t size);

/**
 * @brief Enable/disable audio debug capture
 * @param enabled If true, capture audio to debug_audio.raw
 */
void gb_audio_set_debug(bool enabled);

/**
 * @brief Configure debug capture length
 * @param seconds Number of seconds to capture when debug audio is enabled
 */
void gb_audio_set_debug_capture_seconds(uint32_t seconds);

/**
 * @brief Enable/disable audio text tracing
 * @param enabled If true, write APU activity to debug_audio_trace.log
 */
void gb_audio_set_debug_trace(bool enabled);

/* Passo 6c.1: laboratorio de audio (so para investigar; nao muda o jogo). */
typedef struct {
    bool enabled;       /* canal ligado (NR52) */
    int volume;         /* volume atual (CH3: nivel 0..3) */
    uint16_t freq;      /* frequencia bruta (0..2047); CH4: NR43 */
    uint8_t panning;    /* bits do NR51 deste canal: 1 = direita, 2 = esquerda */
    uint32_t triggers;  /* quantas vezes o canal foi disparado (NRx4 bit 7) */
} GBAudioChannelInfo;

/**
 * @brief Silencia canais na saida (bit 0 = CH1 ... bit 3 = CH4).
 * O som continua sendo emulado; so nao entra na mistura.
 */
void gb_audio_set_channel_mute_mask(uint8_t mask);
uint8_t gb_audio_get_channel_mute_mask(void);

/**
 * @brief Estado atual dos 4 canais (out[0] = CH1 ... out[3] = CH4).
 */
void gb_audio_get_channel_info(const void* apu, GBAudioChannelInfo out[4]);

/* Passo 6c.2: "silenciar musica do GB". Com o portao ligado, cada canal
 * segue um modo: tocar sempre, mudo, ou so quando um byte da RAM do
 * cartucho (o "efeito tocando neste canal" do driver de som) for != 0. */
enum {
    GB_AUDIO_GATE_ALWAYS = 0,
    GB_AUDIO_GATE_MUTE = 1,
    GB_AUDIO_GATE_ON_BYTE = 2
};
void gb_audio_set_gate_enabled(bool enabled);
bool gb_audio_get_gate_enabled(void);
void gb_audio_set_channel_gate(int channel, int mode, uint16_t addr);
void gb_audio_get_channel_gate(int channel, int* mode, uint16_t* addr);
/** Canais que o portao esta deixando passar agora (bit 0 = CH1). */
uint8_t gb_audio_get_gate_open_mask(void);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_H */
