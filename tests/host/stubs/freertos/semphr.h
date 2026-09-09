#pragma once
typedef void *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
static inline int xSemaphoreTake(SemaphoreHandle_t mutex, unsigned timeout)
{
    (void)mutex;
    (void)timeout;
    return 1;
}
static inline void xSemaphoreGive(SemaphoreHandle_t mutex) { (void)mutex; }
