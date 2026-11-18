#ifndef LAB2_INPUT_CONFIG_H
#define LAB2_INPUT_CONFIG_H

#define INPUT_TIMER_HZ 84000000U
#define INPUT_MIN_HZ 1U
#define INPUT_STANDARD_MAX_HZ 200000U
#define INPUT_EXTENDED_MAX_HZ 500000U
#define INPUT_NO_EDGE_MS 1500U
/* Two 2 KiB rings; half/full interrupt only on the rising-edge stream. */
#define INPUT_DMA_LENGTH 512U
#define INPUT_DMA_IRQ_PRIORITY 4U
#define INPUT_EDGE_HISTORY 4U
#define INPUT_CURSOR_RETRIES 3U
/* Classify quantized endpoint intervals without clamping measurements. */
#define INPUT_RANGE_QUANTIZATION_TICKS 1U

#endif
