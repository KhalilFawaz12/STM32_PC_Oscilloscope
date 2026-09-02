#include "stm32f407xx.h"
#include <stdbool.h>
#include <math.h>


#define FRAME_HEADER 0xAA
#define FRAME_TAIL   0x55
#define STATS_WINDOW 2000 // Calculate stats every 2000 samples (200 ms)

volatile uint8_t max1 = 0, min1 = 255;
volatile uint8_t max2 = 0, min2 = 255;
volatile uint8_t prev1 = 127, prev2 = 127;
volatile uint8_t mid1 = 127, mid2 = 127;
volatile uint16_t sample_count = 0;
volatile bool stats_ready = false;

volatile uint32_t global_tick = 0;
volatile uint32_t first_edge1 = 0, last_edge1 = 0, edge_count1 = 0;
volatile uint32_t first_edge2 = 0, last_edge2 = 0, edge_count2 = 0;


volatile uint8_t ready_max1, ready_min1;
volatile uint8_t ready_max2, ready_min2;
volatile uint16_t ready_freq1, ready_freq2;
volatile uint16_t ready_rms1, ready_rms2;

volatile bool is_paused = 0;

void GPIO_Init(void) {
    // UL: unsigned long (treat the number as a 32-bit unsigned integer)

    RCC->AHB1ENR |= 0b10101UL;     // Enable GPIOA and GPIOE clocks (bits 0 and 4)

    GPIOA->MODER |= 0b11UL;   // Set PA0 as analog mode (analog input) (CH1)

    GPIOC->MODER |= (0b11UL << 8); // Set PC4 as analog mode (CH2)

    GPIOA->MODER &= ~((0b11UL << 2) | (0b11UL << 4) | (0b11UL << 6));
    GPIOA->MODER |= ((0b01UL << 2) | (0b01UL << 4) | (0b01UL << 6));  // Set PA1 (Pause LED), PA2 (CH1 LED) and PA3 (CH2 LED) to output mode

    GPIOA->BSRR = (1UL << 17);                  // Ensure LED on PA1 starts OFF
    GPIOA->BSRR = (1UL << 2) | (1UL << 3);      // Set PA2 and PA3 HIGH initially (Both channels start ON)

    GPIOA->MODER &= ~((0b11UL << 18) | (0b11UL << 20));
    GPIOA->MODER |= ((0b10UL << 18) | (0b10UL << 20));      // Set PA9 (USART1_TX) and PA10 (USART1_RX) to Alternate Function Mode

    GPIOA->AFR[1] &= ~((0b1111UL << 4) | (0b1111UL << 8));
    GPIOA->AFR[1] |= ((0b0111UL << 4) | (0b0111UL << 8));       // Select alternate functions AF7 for PA9 (USART1_TX) and AF7 for PA10 (USART1_RX)

    GPIOE->MODER &= ~((0b11UL << 4) | (0b11UL << 6) | (0b11UL << 8));     // Set PE2 (Pause button), PE3 (CH1 button), PE4 (CH2 button) to Input mode 

    GPIOE->PUPDR &= ~((0b11UL << 4) | (0b11UL << 6) | (0b11UL << 8)); 
    GPIOE->PUPDR |=  ((0b01UL << 4) | (0b01UL << 6) | (0b01UL << 8));    // Enable internal Pull-up resistor for push buttons 
}

void USART1_Init(void) {
    RCC->APB2ENR |= 0b10000;        // Enable USART1 Clock (bit 4)

    // Baud Rate: 921600 (it is a standard for high speed transmission) + 16 MHz PCLK (AHB and APB prescalers are 1)
    USART1->BRR = 0x11;     // baud_rate = f_pclk/(16*USARTDIV) --> USARTDIV=1.085 --> Mantissa=1=0x1 and fraction=0.085*16=1.36->1=0x1

    USART1->CR1 |= ((0b1UL << 2) | (0b1UL << 3) | (0b1UL << 13));       // Enable Transmitter, Receiver, and USART Peripheral
}

void USART1_WriteByte(uint8_t data) {
    while(!(USART1->SR & (0b1UL << 7)));        // Wait until Transmit Data Register is Empty (check TXE flag at bit 7)
    USART1->DR = data;      // Place the data in the Transmit Data Register (TDR)
}

void ADC1_Init(void) {
    RCC->APB2ENR |= (0b1UL << 8);       // Enable ADC1 Clock (bit 8)

    ADC1->CR1 &= ~(0b11UL << 24);
    ADC1->CR1 |= (0b10UL << 24);    // Set ADC resolution to 8-bit (because UART frame could hold only 8 bits)

    ADC1->CR2 |= (0b01UL);      // Enable ADC Peripheral (bit 0)
}

uint8_t ADC1_ReadChannel(uint8_t channel) {
    ADC1->SQR3 = channel;               // 1st channel to be converted is channel "channel" (Only 1 conversion in the sequence by default)
    ADC1->CR2 |= (0b01UL << 30);       // Manually Start ADC Conversion by setting SWSTART bit (bit 30)

    while (!(ADC1->SR & (0b01UL << 1)));        // Wait for End of Conversion (EOC: bit 1)

    return (uint8_t)ADC1->DR;       // Return the ADC result sitting in ADC1->DR (Right Justified By Default)
                                    // Also reading ADC1->DR automatically clears EOC bit
}

void TIM2_Init(void) {
    RCC->APB1ENR |= 0b01UL;         // Enable TIM2 Clock

    TIM2->PSC = 15;     // Timer Frequency = 16 MHz / (PSC + 1) = 1 MHz (1 tick = 1 us)

    TIM2->ARR = 99;    // Auto-reload value = 99 -> Interrupt every (99 + 1) ticks = 100 us (10 kHz Sampling Rate)

    TIM2->DIER |= 0b01UL;         // Enable Update Interrupt (upon overflowing) (UIE: bit 0)

    NVIC_EnableIRQ(TIM2_IRQn);      // Enable TIM2 IRQ in NVIC (Global interrupt bit is set by default)

    TIM2->CR1 |= 0b01UL;    // Start Timer
}

void USART1_WriteCommand(uint8_t cmd, uint8_t val) {
    USART1_WriteByte(0xFF);       // Command Frame Header
    USART1_WriteByte(cmd);        // Command ID
    USART1_WriteByte(val);        // Value (1 = ON/PAUSED, 0 = OFF/RUNNING)
    USART1_WriteByte(FRAME_TAIL); // 0x55
}


void TIM2_IRQHandler(void) {    // Read and send a sample every 100 us --> 10 KHz sampling rate
    if (TIM2->SR & 0b01UL) {    // check if UIF flag (bit 0) is set   
        TIM2->SR &= ~(0b01UL); // Clear UIF

        // Buttons handling
        static bool last_btn_pause = 1, last_btn_ch1 = 1, last_btn_ch2 = 1;
        static uint16_t debounce_pause = 0, debounce_ch1 = 0, debounce_ch2 = 0;
        static bool ch1_active = 1, ch2_active = 1;

        bool current_btn_pause = (GPIOE->IDR & (1UL << 2)) ? 1 : 0; // PE2
        bool current_btn_ch1   = (GPIOE->IDR & (1UL << 3)) ? 1 : 0; // PE3
        bool current_btn_ch2   = (GPIOE->IDR & (1UL << 4)) ? 1 : 0; // PE4

        // Pause Button (PE2)
        if (debounce_pause == 0) {
            if (last_btn_pause == 1 && current_btn_pause == 0) { 
                is_paused = !is_paused; 
                debounce_pause = 2000;  
                if (is_paused) {
                    USART1_WriteCommand(0xC1, 0x01); // Paused
                    GPIOA->BSRR = (1UL << 1);        // PA1 LED ON
                } else {
                    USART1_WriteCommand(0xC1, 0x00); // Running
                    GPIOA->BSRR = (1UL << 17);       // PA1 LED OFF
                }
            }
            last_btn_pause = current_btn_pause;
        } else {
            debounce_pause--;
        }
        
        // Channel 1 Toggle Button (PE3)
        if (debounce_ch1 == 0) {
            if (last_btn_ch1 == 1 && current_btn_ch1 == 0) {
                ch1_active = !ch1_active;
                debounce_ch1 = 2000;
                if (ch1_active) {
                    USART1_WriteCommand(0xE1, 0x01); // CH1 ON
                    GPIOA->BSRR = (1UL << 2);        // PA2 LED ON
                } else {
                    USART1_WriteCommand(0xE1, 0x00); // CH1 OFF
                    GPIOA->BSRR = (1UL << 18);       // PA2 LED OFF
                }
            }
            last_btn_ch1 = current_btn_ch1;
        } else {
            debounce_ch1--;
        }

        // Channel 2 Toggle Button (PE4)
        if (debounce_ch2 == 0) {
            if (last_btn_ch2 == 1 && current_btn_ch2 == 0) {
                ch2_active = !ch2_active;
                debounce_ch2 = 2000;
                if (ch2_active) {
                    USART1_WriteCommand(0xF1, 0x01); // CH2 ON
                    GPIOA->BSRR = (1UL << 3);        // PA3 LED ON
                } else {
                    USART1_WriteCommand(0xF1, 0x00); // CH2 OFF
                    GPIOA->BSRR = (1UL << 19);       // PA3 LED OFF
                }
            }
            last_btn_ch2 = current_btn_ch2;
        } else {
            debounce_ch2--;
        }


        if (!is_paused) {
            uint8_t ch1_sample = ADC1_ReadChannel(0);  
            uint8_t ch2_sample = ADC1_ReadChannel(14); 

            static float sum_sq1 = 0.0f;
            static float sum_sq2 = 0.0f;

            float v1 = ((ch1_sample / 255.0f) * 6.6f) - 3.3f;
            sum_sq1 += (v1 * v1);

            float v2 = ((ch2_sample / 255.0f) * 6.6f) - 3.3f;
            sum_sq2 += (v2 * v2);

            if (ch1_sample > max1) 
                max1 = ch1_sample;
            if (ch1_sample < min1) 
                min1 = ch1_sample;
            if (prev1 <= mid1 && ch1_sample > mid1) {
                if (edge_count1 == 0) 
                    first_edge1 = global_tick;
                last_edge1 = global_tick;
                edge_count1++;
            }

            prev1 = ch1_sample;

            
            if (ch2_sample > max2)
                max2 = ch2_sample;
            if (ch2_sample < min2) 
                min2 = ch2_sample;
            if (prev2 <= mid2 && ch2_sample > mid2) {
                if (edge_count2 == 0) 
                    first_edge2 = global_tick;

                last_edge2 = global_tick;
                edge_count2++;
            }

            prev2 = ch2_sample;

            global_tick++;
            sample_count++;

            if (sample_count >= STATS_WINDOW) {
                ready_max1 = max1; 
                ready_min1 = min1;
                ready_max2 = max2; 
                ready_min2 = min2;

                
                if (edge_count1 > 1 && last_edge1 > first_edge1) {
                    uint32_t total_ticks1 = last_edge1 - first_edge1;
                    uint32_t cycles1 = edge_count1 - 1;
                    ready_freq1 = (uint16_t)(((cycles1 * 10246UL) + (total_ticks1 / 2)) / total_ticks1);    // 10246 Hz is the true timer freq due to non precise clock      // Freq = nb of cycles/(time between first and last edge)   but we added total/2 to the numerator so the final answer of freq is successfully rounded up
                    
                    first_edge1 = last_edge1;
                    edge_count1 = 1;
                } 
                else if((global_tick - last_edge1) > 10246){
                    ready_freq1 = 0;      // Timeout: Force 0 Hz if more than 1 second passes without an edge
                    edge_count1 = 0;
                }


                if (edge_count2 > 1 && last_edge2 > first_edge2) {
                    uint32_t total_ticks2 = last_edge2 - first_edge2;
                    uint32_t cycles2 = edge_count2 - 1;
                    ready_freq2 = (uint16_t)(((cycles2 * 10246UL) + (total_ticks2 / 2)) / total_ticks2);

                    first_edge2 = last_edge2;
                    edge_count2 = 1;

                } 
                else if((global_tick - last_edge2) > 10246){
                    ready_freq2 = 0;    // Timeout: Force 0 Hz if more than 1 second passes without an edge
                    edge_count2 = 0;
                }

                float rms1_float = sqrtf(sum_sq1 / STATS_WINDOW);
                float rms2_float = sqrtf(sum_sq2 / STATS_WINDOW);
                ready_rms1 = (uint16_t)(rms1_float * 100);
                ready_rms2 = (uint16_t)(rms2_float * 100);


                mid1 = (max1 + min1) / 2;
                mid2 = (max2 + min2) / 2;
                
                max1 = 0; 
                min1 = 255; 
                max2 = 0; 
                min2 = 255;
                sum_sq1 = 0.0f;
                sum_sq2 = 0.0f;
                sample_count = 0;
                stats_ready = true;
            }

            USART1_WriteByte(FRAME_HEADER); 
            USART1_WriteByte(ch1_sample);
            USART1_WriteByte(ch2_sample);
            USART1_WriteByte(FRAME_TAIL);   
        }
    }
}

int main(void) {
    GPIO_Init();
    USART1_Init();
    ADC1_Init();
    TIM2_Init();

    while (1) {
        if (stats_ready) {
            NVIC_DisableIRQ(TIM2_IRQn);     // To prevent UART collisions
            
            USART1_WriteByte(0xBB); // Header for Statistics Frame
            USART1_WriteByte(ready_max1);
            USART1_WriteByte(ready_min1);
            USART1_WriteByte((uint8_t)(ready_freq1 >> 8));   // Freq1 High Byte
            USART1_WriteByte((uint8_t)(ready_freq1 & 0xFF)); // Freq1 Low Byte
            USART1_WriteByte((uint8_t)(ready_rms1 >> 8));    // RMS1 High Byte
            USART1_WriteByte((uint8_t)(ready_rms1 & 0xFF));  // RMS1 Low Byte
            
            USART1_WriteByte(ready_max2);
            USART1_WriteByte(ready_min2);
            USART1_WriteByte((uint8_t)(ready_freq2 >> 8));   // Freq2 High Byte
            USART1_WriteByte((uint8_t)(ready_freq2 & 0xFF)); // Freq2 Low Byte
            USART1_WriteByte((uint8_t)(ready_rms2 >> 8));    // RMS2 High Byte
            USART1_WriteByte((uint8_t)(ready_rms2 & 0xFF));  // RMS2 Low Byte
            USART1_WriteByte(0x55); // Frame Tail
            
            stats_ready = false;
            NVIC_EnableIRQ(TIM2_IRQn); // Resume data sampling
        }
    }
}